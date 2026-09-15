#pragma once
// Installs a process-wide crash handler that logs a symbolised backtrace (Windows; a no-op elsewhere).
// Output goes to the OPAD_TRACE sink and stderr; see Jobs.hpp for the trace switch.
void installCrashHandler();
