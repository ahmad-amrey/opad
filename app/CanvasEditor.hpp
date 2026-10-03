#pragma once
// On-canvas handles of an image canvas (UI-70, opad/canvas.hpp): the picture itself moves it, the four corner dots scale it
// about the opposite corner (Ctrl: about its centre), the knob above its top edge turns it about its centre (Shift: in 15
// degree steps). A drag shows the canvas where it would go through its local transformation (Viewport::previewPlacement:
// no remesh, every frame) and the release appends one transform op through the canvas command (one undo step). Moving snaps
// its corners and centre to the corners and centres of the other canvases within a few pixels and its centre to the grid
// while grid snap is on; Alt drags free. A locked canvas shows its outline and no handles. Calibrate and Align to model ask
// for points through pick(): a point on the canvas, or a vertex or circle centre of the model.
#include <QObject>
#include <QPointF>
#include <array>
#include <string>
#include <vector>

#include <AIS_InteractiveObject.hxx>

#include "opad/canvas.hpp"

class AppDocument;
class Viewport;

class CanvasEditor : public QObject {
  Q_OBJECT
 public:
  enum class Grip { None, Move, Corner0, Corner1, Corner2, Corner3, Turn };
  enum class Pick { None, Canvas, Model };
  CanvasEditor(AppDocument* doc, Viewport* view, QObject* parent);
  ~CanvasEditor() override;
  void start(const std::string& canvas);  // shows its handles; another canvas's are dropped
  void stop();
  bool active() const { return !m_canvas.empty(); }
  const std::string& canvas() const { return m_canvas; }
  bool locked() const;
  void refresh();  // the document changed (undo, an edit, the panel): the handles go where the canvas is
  void pick(Pick what);  // the next left click on the view is a point, not a drag (picked(); Esc: cancelled)
  // The view picks vertices (and circle centres) until the picking ends: Align switches it on at its start, so the filter
  // has reached every body by the time a model point is asked for (Pick::Model switches it on too).
  void vertexFilter(bool on);
  Pick picking() const { return m_pick; }
  bool dragging() const { return m_drag != Grip::None; }
  // Benches: where a handle is on the widget, and a drag through the real mouse handlers' path (press, moves, release).
  bool gripPoint(Grip g, QPointF& at) const;
  opad::CanvasPlace place() const { return m_place; }
 signals:
  void dragged(const opad::CanvasPlace& place);  // every step of a drag (the panel shows the numbers)
  void placed(const opad::CanvasPlace& place);   // a drag let go: the place to commit
  void picked(const opad::Vec3& point, bool model);
  void pickCancelled();
  void status(const QString& text);
 protected:
  bool eventFilter(QObject* object, QEvent* event) override;
 private:
  Grip gripAt(const QPointF& at) const;
  void show(const opad::CanvasPlace& place);  // the handles' overlay at this place
  void dragTo(const QPointF& at, Qt::KeyboardModifiers mods);
  bool planeAt(const QPointF& at, double& u, double& v) const;  // in m_place.plane
  bool snapMove(opad::CanvasPlace& place) const;
  void hideOverlay();
  AppDocument* m_doc;
  Viewport* m_view;
  std::string m_canvas;
  opad::CanvasPlace m_place, m_start;
  Grip m_drag = Grip::None, m_hover = Grip::None;
  Pick m_pick = Pick::None;
  QPointF m_press;
  double m_pressU = 0, m_pressV = 0;
  bool m_moved = false;
  std::vector<opad::Vec3> m_targets;  // the other canvases' corners and centres (snapping), taken when a move starts
  Handle(AIS_InteractiveObject) m_overlay;
  int m_oldFilter = -1;
};
