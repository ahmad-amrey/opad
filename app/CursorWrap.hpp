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
#include <vector>
// Wrap only at the outer edge of the desktop along the pointer's row/column.
// A neighboring screen remains reachable without a warp, including offset layouts.
inline QPoint wrappedDesktopCursor(const QPoint& p,const std::vector<QRect>& screens) {
  auto contains=[&](QPoint q){ for(const auto& r:screens) if(r.contains(q)) return true; return false; };
  if(!contains(p)) return p;
  int left=p.x(),right=p.x(),top=p.y(),bottom=p.y();
  for(const auto& r:screens) {
    if(p.y()>=r.top() && p.y()<=r.bottom()) {left=std::min(left,r.left());right=std::max(right,r.right());}
    if(p.x()>=r.left() && p.x()<=r.right()) {top=std::min(top,r.top());bottom=std::max(bottom,r.bottom());}
  }
  QPoint to=p;
  if(!contains(p+QPoint(-1,0))) to.setX(right-2);
  else if(!contains(p+QPoint(1,0))) to.setX(left+2);
  if(!contains(p+QPoint(0,-1))) to.setY(bottom-2);
  else if(!contains(p+QPoint(0,1))) to.setY(top+2);
  // At an L-shaped desktop corner the two opposite extremes may be a gap.
  if(!contains(to)) { to.setY(p.y()); if(!contains(to)) to=QPoint(p.x(),top+2); }
  return contains(to)?to:p;
}
