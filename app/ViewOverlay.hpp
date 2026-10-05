#pragma once
// Native widgets over the OCCT view (the value boxes beside the pointer, the sketch's value box, chips, toasts, prompt bars,
// badges, note cards) are child windows of the view's own native window, whose pixels only OCCT draws, and OCCT draws a
// frame only when something in the scene, the camera or the hover changed. A part of the view such an overlay leaves (it
// moved, shrank, went, or its mask changed) kept the overlay's last image until the next frame: the value boxes that follow
// the pointer left a trail of copies of themselves wherever no frame came (in a sketch with grid snapping a move within one
// cell draws none). Viewport shows its last frame again for such a part: it watches its native overlays' moves, sizes and
// hides and its own window's exposes; a mask change has no event, so the overlay says so here.
#include <QCoreApplication>
#include <QEvent>
#include <QWidget>

namespace viewoverlay {
inline QEvent::Type uncoveredEvent() {
  static const auto type = QEvent::Type(QEvent::registerEventType());
  return type;
}
// An overlay over `view` no longer covers part of what it covered (its mask changed): the view draws that part again.
inline void uncovered(QWidget* view) {
  if (!view) return;
  QEvent event(uncoveredEvent());
  QCoreApplication::sendEvent(view, &event);
}
}  // namespace viewoverlay
