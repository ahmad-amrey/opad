// A native window shown again that the system never exposed (no part of it was invalid, so no WM_PAINT): Qt paints such a
// window only once exposed, and a view drawn by OpenGL (Viewport) kept whatever the window's surface held there before, the
// start page's picture (UI-56 follow-up). Its whole area invalidated: the system paints it, Qt exposes it, the view draws.
#ifdef _WIN32
#include <windows.h>
#endif
#include <QWidget>

namespace native {
bool invalidate(QWidget* w) {
#ifdef _WIN32
  if (!w || !w->internalWinId()) return false;
  return RedrawWindow(reinterpret_cast<HWND>(w->internalWinId()), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN) != 0;
#else
  if (w) w->update();
  return false;
#endif
}
}  // namespace native
