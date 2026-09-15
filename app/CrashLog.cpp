// Writes a symbolised backtrace of the faulting thread to the trace sink (OPAD_TRACE) and stderr when the
// process crashes, so a segfault in a release build is diagnosable from one run.
#include "CrashLog.hpp"

#include <QString>
#include <cstdio>

#include "Jobs.hpp"

#if defined(_WIN32)
#include <windows.h>
// clang-format off
#include <dbghelp.h>
// clang-format on

namespace {
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
  static volatile LONG entered = 0;
  if (InterlockedIncrement(&entered) > 1) return EXCEPTION_CONTINUE_SEARCH;
  HANDLE proc = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(proc, nullptr, TRUE);
  auto out = [](const QString& s) {
    trace::log(s);
    std::fputs((s + "\n").toUtf8().constData(), stderr);
    std::fflush(stderr);
  };
  out(QStringLiteral("CRASH: exception 0x%1 at 0x%2 on thread %3")
          .arg(static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode), 8, 16, QChar('0'))
          .arg(reinterpret_cast<quintptr>(ep->ExceptionRecord->ExceptionAddress), 0, 16)
          .arg(GetCurrentThreadId()));
  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 frame = {};
  DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
  machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrStack.Offset = ctx.Rsp;
#else
  machine = IMAGE_FILE_MACHINE_I386;
  frame.AddrPC.Offset = ctx.Eip;
  frame.AddrFrame.Offset = ctx.Ebp;
  frame.AddrStack.Offset = ctx.Esp;
#endif
  frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(machine, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    if (frame.AddrPC.Offset == 0) break;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    QString name = SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym) ? QString::fromLatin1(sym->Name) + QStringLiteral("+0x%1").arg(disp, 0, 16) : QStringLiteral("?");
    char modPath[MAX_PATH] = {};
    HMODULE mod = reinterpret_cast<HMODULE>(SymGetModuleBase64(proc, frame.AddrPC.Offset));
    if (mod) GetModuleFileNameA(mod, modPath, MAX_PATH);
    QString modName = QString::fromLocal8Bit(modPath);
    modName = modName.mid(modName.lastIndexOf('\\') + 1);
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    QString where = SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &ldisp, &line) ? QStringLiteral(" (%1:%2)").arg(QString::fromLocal8Bit(line.FileName)).arg(line.LineNumber) : QString();
    out(QStringLiteral("  #%1 0x%2 %3!%4%5").arg(i, 2).arg(frame.AddrPC.Offset, 0, 16).arg(modName, name, where));
  }
  return EXCEPTION_CONTINUE_SEARCH;  // let the default handler terminate the process
}
}  // namespace

void installCrashHandler() { SetUnhandledExceptionFilter(crashFilter); }
#else
void installCrashHandler() {}
#endif
