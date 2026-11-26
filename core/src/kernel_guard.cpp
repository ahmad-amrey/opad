// Hardware faults inside guarded kernel calls become errors (opad/kernel_guard.hpp).
//
// A guard registers an OCCT error handler of its own (what OCC_CATCH_SIGNALS declares), so the fault always has an
// OCCT handler to go to on this thread: the kernel's nearest one inside the call, else the guard's.
// Windows: a vectored exception handler (it runs before any frame's handler and before the unhandled-exception filter
// that writes the crash log) sees the fault on the faulting thread. When that thread is inside a guard, it raises the
// fault as OCCT's own handler would (Standard_Failure::Jump: a jump to the nearest Standard_ErrorHandler, which turns it
// into a Standard_Failure there).
// POSIX: a SIGSEGV/SIGBUS/SIGILL/SIGFPE handler does the same Jump from the handler, as OCCT's does; other threads'
// signals go on to the handler that was there before (or the default action).
// An OCCT built without signal conversion (MSVC) jumps nowhere (Jump throws): there the guard jumps back into itself
// (`fallback`), and the kernel has no handlers of its own to leave behind.
#include "opad/kernel_guard.hpp"

#include <OSD_Exception.hxx>
#include <OSD_Exception_ACCESS_VIOLATION.hxx>
#include <OSD_Exception_ILLEGAL_INSTRUCTION.hxx>
#include <OSD_Exception_INT_OVERFLOW.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>

#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#endif

namespace opad {
namespace {
std::atomic<unsigned long> g_faults{0};
bool g_jumps = false;  // Standard_Failure::Jump reaches the nearest Standard_ErrorHandler (OCC_CONVERT_SIGNALS)

struct Guard {
#if defined(_WIN32)
  void* fallback[5];  // __builtin_setjmp's buffer
#else
  sigjmp_buf fallback;
#endif
  Guard* outer;
  KernelFault fault;
};

// Raises the fault at the nearest OCCT handler. Returns only when Jump throws instead (no conversion), never here.
void raise_in_kernel(const KernelFault& fault) {
  const std::string what = fault.text();
  const std::string text = "OPAD kernel guard: " + what;
  Handle(Standard_Failure) failure;
  if (what.rfind("access violation", 0) == 0) failure = new OSD_Exception_ACCESS_VIOLATION(text.c_str());
  else if (what.rfind("illegal", 0) == 0) failure = new OSD_Exception_ILLEGAL_INSTRUCTION(text.c_str());
  else if (what.rfind("integer", 0) == 0 || what.rfind("arithmetic", 0) == 0) failure = new OSD_Exception_INT_OVERFLOW(text.c_str());
  else failure = new OSD_Exception(text.c_str());
  failure->Jump();
}

// Whether this OCCT converts: a Jump inside a handler of our own lands in it (it longjmps) or throws.
bool probe_jumps() {
  Standard_ErrorHandler handler;
  if (setjmp(handler.Label())) {
    handler.Catches(STANDARD_TYPE(Standard_Failure));
    return true;
  }
  try {
    Standard_Failure::NewInstance("OPAD kernel guard probe")->Jump();
  } catch (const Standard_Failure&) {
  }
  return false;
}

#if defined(_WIN32)
// The thread's innermost guard. Win32 TLS rather than thread_local: MinGW's emulated TLS may allocate on a thread's first
// access, which must not happen inside the exception handler.
DWORD slot() {
  static const DWORD s = TlsAlloc();
  return s;
}
Guard* current() { return static_cast<Guard*>(TlsGetValue(slot())); }
void set_current(Guard* g) { TlsSetValue(slot(), g); }

LONG CALLBACK on_fault(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;  // C++ exceptions, stack overflow, breakpoints, ...
  }
  Guard* g = current();
  if (!g) return EXCEPTION_CONTINUE_SEARCH;
  g->fault.code = code;
  g->fault.address = info->ExceptionRecord->ExceptionAddress;
  ++g_faults;
  // From inside the dispatch, as OCCT's own Windows handler does from its filter: the jump's unwind goes up through the
  // dispatcher's frame, which holds the faulting thread's context, and on from the faulting frame.
  if (g_jumps) raise_in_kernel(g->fault);
  __builtin_longjmp(g->fallback, 1);
}

void install() {
  static std::once_flag once;
  std::call_once(once, [] {
    slot();
    g_jumps = probe_jumps();
    AddVectoredExceptionHandler(1, on_fault);
  });
}
#else
thread_local Guard* t_guard = nullptr;
Guard* current() { return t_guard; }
void set_current(Guard* g) { t_guard = g; }

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
constexpr size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
struct sigaction g_previous[kSignalCount];

void on_signal(int sig, siginfo_t* info, void* context) {
  if (Guard* g = current()) {
    g->fault.code = static_cast<unsigned long>(sig);
    g->fault.address = info ? info->si_addr : nullptr;
    ++g_faults;
    if (g_jumps) raise_in_kernel(g->fault);  // SA_NODEFER: the signal is not left blocked by the jump
    siglongjmp(g->fallback, 1);
  }
  for (size_t i = 0; i < kSignalCount; ++i) {
    if (kSignals[i] != sig) continue;
    const struct sigaction& before = g_previous[i];
    if ((before.sa_flags & SA_SIGINFO) && before.sa_sigaction) {
      before.sa_sigaction(sig, info, context);
      return;
    }
    if (before.sa_handler != SIG_DFL && before.sa_handler != SIG_IGN && before.sa_handler) {
      before.sa_handler(sig);
      return;
    }
  }
  // Nobody else wanted it: the default action, as if this handler had never been there (the fault happens again).
  signal(sig, SIG_DFL);
}

void install() {
  static std::once_flag once;
  std::call_once(once, [] {
    g_jumps = probe_jumps();
    for (size_t i = 0; i < kSignalCount; ++i) {
      struct sigaction action;
      std::memset(&action, 0, sizeof(action));
      action.sa_sigaction = on_signal;
      action.sa_flags = SA_SIGINFO | SA_NODEFER;
      sigemptyset(&action.sa_mask);
      sigaction(kSignals[i], &action, &g_previous[i]);
    }
  });
}
#endif

struct Pop {
  Guard* outer;
  ~Pop() { set_current(outer); }
};
}  // namespace

std::string KernelFault::text() const {
  const char* what = "fault";
#if defined(_WIN32)
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: what = "access violation"; break;
    case EXCEPTION_IN_PAGE_ERROR: what = "access violation (page error)"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: what = "illegal instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW: what = "integer division fault"; break;
    default: break;
  }
#else
  switch (code) {
    case SIGSEGV:
    case SIGBUS: what = "access violation"; break;
    case SIGILL: what = "illegal instruction"; break;
    case SIGFPE: what = "arithmetic fault"; break;
    default: break;
  }
#endif
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s (0x%lx) at %p", what, code, address);
  return buf;
}

unsigned long kernel_faults() { return g_faults.load(); }

namespace detail {
bool run_kernel_guarded(void (*fn)(void*), void* arg, KernelFault& fault) {
  // OPAD_NO_KERNEL_GUARD=1: faults crash as they would unguarded (to see one in a debugger or the crash log).
  static const bool off = std::getenv("OPAD_NO_KERNEL_GUARD") != nullptr;
  if (off) {
    fn(arg);
    return true;
  }
  install();
  Guard g{};
  g.outer = current();
  // Read back through volatile pointers after a jump: nothing cached in a register from before it is trusted.
  Guard* volatile self = &g;
  KernelFault* volatile out = &fault;
  Pop pop{g.outer};  // also when fn throws
  // Where a fault lands when the kernel has no OCC_CATCH_SIGNALS of its own nearer: the macro spelled out, but taking
  // the failure here instead of raising it again (and not empty when OCCT's CMake package leaves OCC_CONVERT_SIGNALS out).
  Standard_ErrorHandler handler;
  if (setjmp(handler.Label())) {
    handler.Catches(STANDARD_TYPE(Standard_Failure));  // taken: its destructor must not pass the failure on
    *out = self->fault;
    return false;
  }
  if (!g_jumps) {
#if defined(_WIN32)
    if (__builtin_setjmp(g.fallback)) {
#else
    if (sigsetjmp(g.fallback, 1)) {
#endif
      *out = self->fault;
      return false;
    }
  }
  set_current(self);
  fn(arg);
  return true;
}
}  // namespace detail

}  // namespace opad
