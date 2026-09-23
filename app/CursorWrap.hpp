#pragma once
#include <QPoint>
#include <QRect>

inline QPoint wrappedCursor(const QPoint& p, const QRect& screen) {
  QPoint target = p;
  if (p.x() <= screen.left()) target.setX(screen.right() - 2);
  else if (p.x() >= screen.right()) target.setX(screen.left() + 2);
  if (p.y() <= screen.top()) target.setY(screen.bottom() - 2);
  else if (p.y() >= screen.bottom()) target.setY(screen.top() + 2);
  return target;
}
