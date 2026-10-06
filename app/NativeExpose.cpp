// The 3D view's native window (Viewport): invalidated when shown again but not exposed (no part of it was invalid, so no
// WM_PAINT: Qt paints such a window only once exposed, and an OpenGL view kept whatever the window's surface held there
// before, the start page's picture), and its state and paint messages described for trace::traceFrames.
#ifdef _WIN32
#include <windows.h>
#endif
#include <QString>
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

QString styleText(WId window) {
#ifdef _WIN32
  return QStringLiteral(", WS_VISIBLE %1").arg(bool(GetWindowLongPtrW(reinterpret_cast<HWND>(window), GWL_STYLE) & WS_VISIBLE));
#else
  (void)window;
  return {};
#endif
}

QString messageText(const QByteArray& type, void* message) {
#ifdef _WIN32
  if (type != "windows_generic_MSG") return {};
  const MSG* m = static_cast<const MSG*>(message);
  switch (m->message) {
    case WM_PAINT: return QStringLiteral("WM_PAINT");
    case WM_ERASEBKGND: return QStringLiteral("WM_ERASEBKGND");
    case WM_SHOWWINDOW: return QStringLiteral("WM_SHOWWINDOW %1").arg(m->wParam ? "show" : "hide");
    case WM_WINDOWPOSCHANGED: {
      const auto* pos = reinterpret_cast<const WINDOWPOS*>(m->lParam);
      if (pos->flags & SWP_SHOWWINDOW) return QStringLiteral("WM_WINDOWPOSCHANGED show");
      if (pos->flags & SWP_HIDEWINDOW) return QStringLiteral("WM_WINDOWPOSCHANGED hide");
      return {};
    }
    default: return {};
  }
#else
  (void)type;
  (void)message;
  return {};
#endif
}
}  // namespace native
