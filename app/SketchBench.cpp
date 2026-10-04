// The sketch input benches (TODO 11 T1) through the registry; cases in tools/bench_cases/sketch.py. Each opens Sketch1 on
// XY in the Design workspace, as OPAD_BENCH_DESIGN does, and runs its SketchEditor bench (Sketch*Bench.cpp), which logs
// its PASS/FAIL lines and quits; one that has not ended after 150 s fails (gui_benches gives a case 240 s).
#include <QCoreApplication>
#include <QTimer>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "SketchEditor.hpp"

#define OPAD_SKETCH_BENCH(variable, id, method)                                                                          \
  OPAD_BENCH(variable, id) {                                                                                             \
    w.setWorkspace("design");                                                                                            \
    w.m_design->benchSketch([&w] { w.m_design->sketch()->method(); });                                                   \
    QTimer::singleShot(150000, qApp, [] { trace::log("bench: sketch: " #id " did not end FAIL"); QCoreApplication::exit(2); }); \
    return true;                                                                                                         \
  }

OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_GRID, sketchGrid, benchGrid)                      // UI-18
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_GRIDCURSOR, sketchGridCursor, benchGridCursor)    // grid snapping's drawing cursor
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_LADDER, sketchLadder, benchLadder)                // UI-20
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_KEYS, sketchKeys, benchKeys)                      // UI-16
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_SHAPES, sketchShapes, benchShapes)                // UI-17
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_CROSSLOCK, sketchCrossLock, benchCrossLock)       // UI-19
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_SNAPS, sketchSnaps, benchSnaps)                   // UI-21, UI-23
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_STEPS, sketchSteps, benchSteps)                   // UI-25
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_CONSTRAINTS, sketchConstraints, benchConstraints)  // UI-24
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_COMMANDLINE, sketchCommandLine, benchCommandLine)  // UI-133
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_CLIPBOARD, sketchClipboard, benchClipboard)        // UI-129
OPAD_SKETCH_BENCH(OPAD_BENCH_SKETCH_EDITS, sketchEdits, benchEdits)                    // UI-28
