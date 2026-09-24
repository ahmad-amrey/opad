#pragma once
#include <QPoint>
#include <QRect>

// Ignore queued pre-warp events until the pointer reaches the destination side.
struct CursorWarpGate {
  QPoint from, to;
  bool pending = false;
  void begin(QPoint a, QPoint b) { from=a; to=b; pending=true; }
  bool accept(QPoint p) {
    if (!pending) return true;
    if ((p-to).manhattanLength() >= (p-from).manhattanLength()) return false;
    pending=false; return true;
  }
};

inline QPoint wrappedCursor(const QPoint& p, const QRect& screen) {
  QPoint target = p;
  if (p.x() <= screen.left()) target.setX(screen.right() - 2);
  else if (p.x() >= screen.right()) target.setX(screen.left() + 2);
  if (p.y() <= screen.top()) target.setY(screen.bottom() - 2);
  else if (p.y() >= screen.bottom()) target.setY(screen.top() + 2);
  return target;
}
