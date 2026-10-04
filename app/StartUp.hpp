#pragma once
// Startup order (UI-44): the window's shell is painted first, on its first expose; then the 3D viewer is made (the OpenGL
// context: 0.2-0.8 s), its first frame drawn (the shaders: 0.2-0.4 s) and the file given on the command line opened, each
// on an event-loop turn of its own. Before, the viewer was made and drawn before the window had ever been painted: the
// launch froze for 0.8-1.5 s on a window with nothing in it. A window started hidden (the benches') is never exposed: it
// goes on after kExposeWaitMs.
#include <QString>
#include <QtGlobal>

class MainWindow;

namespace startup {
constexpr int kExposeWaitMs = 250;
// Times since main started (ms; -1: not yet), for the startup bench and the trace ("startup: first interactive frame").
struct Marks {
  qint64 shown = -1, exposed = -1, viewer = -1, viewerDone = -1, frame = -1, frameDone = -1, opened = -1;
  bool byExpose = false;  // went on at the window's first expose, not after the wait
};
void begin();                                   // the clock: first thing in main
// Once the application exists: OCCT's list of the system's fonts (Font_FontMgr, ~0.1 s, the view cube's labels need it in
// the first frame) is read on a worker while the window is built.
void prepare();
void run(MainWindow* window, const QString& file);  // after window->show()
const Marks& marks();
}  // namespace startup
