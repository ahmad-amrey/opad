#pragma once
// A hardware fault (access violation, illegal instruction, integer division by zero) inside a kernel call, turned into
// an error on the thread that made it instead of the end of the process. Nothing installs OCCT's own conversion
// (OSD::SetSignal): an access violation in BRepFilletAPI_MakeFillet ended the whole app, unsaved document and all (a
// live-MCP fillet preview, 2026-11-26).
//
// How (kernel_guard.cpp): the fault is handed to OCCT as an OSD_Exception at the nearest OCC_CATCH_SIGNALS, the way
// OCCT's own signal handler does on Linux (OCCT built by its CMake with any compiler but MSVC converts signals): the
// kernel's try/catch then takes it like any Standard_Failure (BRepFilletAPI_MakeFillet just reports not done), and its
// handler list and the mutexes registered with it stay right. With no OCC_CATCH_SIGNALS nearer, the guard's own handler
// takes it. The frames the jump leaves behind do not run their destructors (that memory leaks: the price of carrying
// on), so a guarded call must hold no OPAD lock and touch no shared OPAD state: kernel calls on shapes the caller owns.
// Guards nest; each thread has its own.
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace opad {

struct KernelFault {
  unsigned long code = 0;         // the system's exception code (0xc0000005 = access violation) or the POSIX signal
  const void* address = nullptr;  // where it happened
  std::string text() const;       // "access violation (0xc0000005) at 0x..."
};

namespace detail {
// Runs fn(arg) under the guard: false (and `fault` filled) when it faulted and the guard's own handler took the fault.
// C++ exceptions from fn propagate (a fault the kernel's own OCC_CATCH_SIGNALS took and did not catch comes out as a
// Standard_Failure).
bool run_kernel_guarded(void (*fn)(void*), void* arg, KernelFault& fault);
}  // namespace detail

// Faults seen inside guards since start (for tests and traces).
unsigned long kernel_faults();

// f() under the guard: its value, or nullopt with `fault` filled when the kernel faulted outside any try block of its
// own. Exceptions from f propagate, a Standard_Failure from a fault the kernel converted included.
template <class F>
auto kernel_guarded(F&& f, KernelFault* fault = nullptr) -> std::optional<std::invoke_result_t<F&>> {
  using R = std::invoke_result_t<F&>;
  // No try/catch between the guard and f: the jump out of a fault may unwind, and a catch (...) there could stop it.
  struct Call {
    F* f;
    std::optional<R>* value;
  };
  std::optional<R> value;
  Call call{&f, &value};
  KernelFault local;
  const bool ok = detail::run_kernel_guarded(
      [](void* p) {
        auto* c = static_cast<Call*>(p);
        c->value->emplace((*c->f)());
      },
      &call, local);
  if (!ok) {
    if (fault) *fault = local;
    return std::nullopt;
  }
  return value;
}

}  // namespace opad
