#pragma once
// In-app benches in their own files. MainWindow::runBench asks the registry first, so a new bench is a new .cpp (and a
// tools/bench_cases/<area>.py case), with no edit of MainWindow:
//   OPAD_BENCH(OPAD_BENCH_FOO, foo) { ... w.m_viewport ...; return true; }
// The body is a static member of a friend of MainWindow: `w` is the window (its private members are in reach), `value`
// the variable's value. It runs once the file given on the command line has loaded (--bench-select), when the variable
// is set; returning false lets runBench go on to the built-in benches. A bench logs "bench: ... PASS" / "FAIL" lines
// (trace::log) for tools/gui_benches.py and quits the app itself (QCoreApplication::exit).
#include <QString>
#include <QStringList>

class MainWindow;
template <class Tag>
struct MainWindowBench;  // befriended by MainWindow, specialised by OPAD_BENCH

namespace bench {
using Handler = bool (*)(MainWindow& w, const QString& value);
bool add(const char* variable, Handler handler);  // static registration (OPAD_BENCH)
QStringList variables();  // registered, sorted
QStringList clashes();  // variables registered twice (two files claim the same switch: the seams bench fails)
QString pending();  // the first registered variable (sorted) that is set in the environment, else empty
bool run(MainWindow& w);  // runs the pending bench; false when there is none or it declined
}  // namespace bench

#define OPAD_BENCH(variable, id)                                                                    \
  namespace {                                                                                       \
  struct id##_bench;                                                                                \
  }                                                                                                 \
  template <>                                                                                       \
  struct MainWindowBench<id##_bench> {                                                              \
    static bool run(MainWindow& w, const QString& value);                                           \
  };                                                                                                \
  [[maybe_unused]] static const bool id##_bench_added = bench::add(#variable, &MainWindowBench<id##_bench>::run); \
  bool MainWindowBench<id##_bench>::run([[maybe_unused]] MainWindow& w, [[maybe_unused]] const QString& value)
