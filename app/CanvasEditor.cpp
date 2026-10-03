// On-canvas handles of an image canvas (UI-70, CanvasEditor.hpp).
#include "CanvasEditor.hpp"

#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <SelectMgr_Selection.hxx>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <QKeyEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

#include "AppDocument.hpp"
#include "Jobs.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
gp_Pnt pnt(const opad::Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }

// The handles: the outline (dashed while locked), the knob's stem, dots at the corners, a ring on the knob, a cross at the
// centre. Markers keep their size on screen; widths follow the display scale.
class CanvasGrips : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(CanvasGrips, AIS_InteractiveObject)
 public:
  CanvasGrips(const QColor& color, double scale) : m_color(occ(color)), m_scale(scale) {}
  std::vector<gp_Pnt> outline, stem, corners, knob, centre;
  bool locked = false;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    auto segments = [&](const std::vector<gp_Pnt>& ends, Aspect_TypeOfLine type, double width, const Quantity_Color& color) {
      if (ends.size() < 2) return;
      Handle(Graphic3d_ArrayOfSegments) array = new Graphic3d_ArrayOfSegments(int(ends.size()));
      for (const auto& p : ends) array->AddVertex(p);
      auto group = prs->NewGroup();
      group->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(color, type, width * m_scale));
      group->AddPrimitiveArray(array);
    };
    auto markers = [&](const std::vector<gp_Pnt>& at, Aspect_TypeOfMarker type, double size, const Quantity_Color& color) {
      if (at.empty()) return;
      Handle(Graphic3d_ArrayOfPoints) points = new Graphic3d_ArrayOfPoints(int(at.size()));
      for (const auto& p : at) points->AddVertex(p);
      auto group = prs->NewGroup();
      group->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(type, color, size * m_scale));
      group->AddPrimitiveArray(points);
    };
    segments(outline, Aspect_TOL_SOLID, 3.0, Quantity_NOC_WHITE);  // a light band under it: the line reads on any picture
    segments(outline, locked ? Aspect_TOL_DASH : Aspect_TOL_SOLID, 1.5, m_color);
    if (locked) return;
    segments(stem, Aspect_TOL_SOLID, 1.5, m_color);
    markers(corners, Aspect_TOM_BALL, 3.2, Quantity_NOC_WHITE);
    markers(corners, Aspect_TOM_O, 3.0, m_color);
    markers(knob, Aspect_TOM_O_POINT, 3.0, m_color);
    markers(centre, Aspect_TOM_PLUS, 3.0, m_color);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}  // never picked: the editor hit-tests

 private:
  Quantity_Color m_color;
  double m_scale;
};

// A place's corners (bottom left, bottom right, top right, top left) and centre in its plane.
std::array<std::pair<double, double>, 5> planeCorners(const opad::CanvasPlace& p) {
  const double c = std::cos(p.angle), s = std::sin(p.angle), w = p.width / 2, h = p.height / 2;
  auto at = [&](double dx, double dy) { return std::make_pair(p.x + dx * c - dy * s, p.y + dx * s + dy * c); };
  return {at(-w, -h), at(w, -h), at(w, h), at(-w, h), at(0, 0)};
}
}  // namespace

CanvasEditor::CanvasEditor(AppDocument* doc, Viewport* view, JobRunner* jobs, QObject* parent) : QObject(parent), m_doc(doc), m_view(view), m_jobs(jobs) {
  view->installEventFilter(this);
  connect(view, &Viewport::notesMoved, this, [this] {  // the camera moved: the knob keeps its distance on screen
    if (active() && !dragging()) show(m_place);
  });
}

CanvasEditor::~CanvasEditor() { hideOverlay(); }

void CanvasEditor::start(const std::string& canvas) {
  stop();
  m_canvas = canvas;
  refresh();
}

void CanvasEditor::stop() {
  if (dragging()) m_view->endPlacementPreview(m_canvas);
  m_drag = m_hover = Grip::None;
  if (m_pick != Pick::None) pick(Pick::None);
  if (m_modelJob) m_modelJob->cancel();
  m_model.reset();
  m_modelStamp.clear();
  hideOverlay();
  if (!m_canvas.empty()) m_view->unsetCursor();
  m_canvas.clear();
}

bool CanvasEditor::locked() const {
  const opad::Node* n = m_doc->scene.node(m_canvas);
  return n && n->locked;
}

void CanvasEditor::refresh() {
  if (!active()) return;
  const opad::Node* n = m_doc->scene.node(m_canvas);
  if (!n || !opad::is_canvas(*n) || n->body_missing) return stop();
  if (dragging()) return;  // the drag's own place is shown until it is let go
  m_place = opad::canvas_place(m_doc->scene, m_canvas);
  show(m_place);
  findModelPoints();
}

void CanvasEditor::findModelPoints() {
  // What they depend on: the plane, and every other visible body's key and place (O(bodies), no geometry).
  struct Body {
    TopoDS_Shape shape;
    opad::Mat4 world;
  };
  std::string stamp = m_place.plane.to_json().dump();
  std::vector<std::pair<std::string, opad::Mat4>> found;
  for (const auto& id : m_doc->scene.all_bodies())
    if (const opad::Node* n = m_doc->scene.node(id); n && id != m_canvas && !n->body_missing && !opad::is_canvas(*n) && m_doc->scene.effectively_visible(id)) {
      found.push_back({n->body_key, m_doc->scene.world(id)});
      stamp += n->body_key.substr(0, 8) + opad::json(found.back().second.m).dump();
    }
  if (stamp == m_modelStamp) return;
  m_modelStamp = stamp;
  if (m_modelJob) m_modelJob->cancel();
  auto bodies = std::make_shared<std::vector<Body>>();
  for (const auto& [key, world] : found) bodies->push_back({opad::body_shape(m_doc->doc, key), world});  // cached since the load (as meshing takes them)
  auto points = std::make_shared<PlanePoints>();
  const opad::Frame plane = m_place.plane;
  QPointer<CanvasEditor> self(this);
  m_modelJob = m_jobs->async(tr("Finding snap points"), [bodies, points, plane](Progress progress) {
    constexpr size_t kMax = 300000;
    auto add = [&](const gp_Pnt& p, const opad::Mat4& world) {
      double u, v;
      plane.to_local(world.apply({p.X(), p.Y(), p.Z()}), u, v);
      points->push_back({u, v});
    };
    for (const auto& b : *bodies) {
      if (progress.cancelled() || points->size() >= kMax) break;
      TopTools_IndexedMapOfShape vertices, edges;
      TopExp::MapShapes(b.shape, TopAbs_VERTEX, vertices);
      for (int i = 1; i <= vertices.Extent() && points->size() < kMax; ++i) add(BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))), b.world);
      TopExp::MapShapes(b.shape, TopAbs_EDGE, edges);
      for (int i = 1; i <= edges.Extent() && points->size() < kMax; ++i) {
        if (BRep_Tool::Degenerated(TopoDS::Edge(edges(i)))) continue;
        const BRepAdaptor_Curve curve(TopoDS::Edge(edges(i)));
        if (curve.GetType() == GeomAbs_Circle) add(curve.Circle().Location(), b.world);
      }
    }
    std::sort(points->begin(), points->end());
  }, [self, points, plane](bool ok, const QString&) {
    if (!self || !ok) return;
    self->m_modelJob = nullptr;
    self->m_model = points;
    self->m_modelPlane = plane;
  });
}

void CanvasEditor::hideOverlay() {
  if (!m_overlay.IsNull()) m_view->removeOverlay(m_overlay);
  m_overlay.Nullify();
}

void CanvasEditor::show(const opad::CanvasPlace& place) {
  const bool fresh = m_overlay.IsNull();
  Handle(CanvasGrips) grips = fresh ? new CanvasGrips(m_view->tokens().sel, m_view->displayScale()) : Handle(CanvasGrips)::DownCast(m_overlay);
  grips->outline.clear(), grips->stem.clear(), grips->corners.clear(), grips->knob.clear(), grips->centre.clear();
  grips->locked = locked();
  const auto c = planeCorners(place);
  std::array<gp_Pnt, 5> w;
  for (int i = 0; i < 5; ++i) w[size_t(i)] = pnt(place.plane.to_world(c[size_t(i)].first, c[size_t(i)].second));
  for (int i = 0; i < 4; ++i) grips->outline.insert(grips->outline.end(), {w[size_t(i)], w[size_t((i + 1) % 4)]}), grips->corners.push_back(w[size_t(i)]);
  const gp_Pnt top((w[2].XYZ() + w[3].XYZ()) / 2);
  gp_Vec up(w[0], w[3]);
  if (up.Magnitude() > 1e-12) up.Normalize();
  const gp_Pnt knob = top.Translated(up * (28 * m_view->pixelSize()));
  grips->stem = {top, knob};
  grips->knob = {knob};
  grips->centre = {w[4]};
  m_overlay = grips;
  if (fresh) m_view->showOverlay(m_overlay);
  else m_view->updateOverlay(m_overlay);
}

bool CanvasEditor::planeAt(const QPointF& at, double& u, double& v) const { return m_view->planePoint(at, m_place.plane, u, v); }

bool CanvasEditor::gripPoint(Grip g, QPointF& at) const {
  if (!active() || g == Grip::None) return false;
  const auto c = planeCorners(m_place);
  auto widget = [&](double u, double v) { return QPointF(m_view->widgetPoint(m_place.plane.to_world(u, v))); };
  if (g == Grip::Move) {  // halfway between the bottom left corner and the centre: on the picture, off the centre cross
    at = widget((c[0].first + c[4].first) / 2, (c[0].second + c[4].second) / 2);
    return true;
  }
  if (g == Grip::Turn) {
    const QPointF top = (widget(c[2].first, c[2].second) + widget(c[3].first, c[3].second)) / 2, bottom = (widget(c[0].first, c[0].second) + widget(c[1].first, c[1].second)) / 2;
    const QPointF up = top - bottom;
    const double l = std::hypot(up.x(), up.y());
    at = l > 1e-9 ? top + up * (28 / l) : top;
    return true;
  }
  const int i = int(g) - int(Grip::Corner0);
  at = widget(c[size_t(i)].first, c[size_t(i)].second);
  return true;
}

CanvasEditor::Grip CanvasEditor::gripAt(const QPointF& at) const {
  if (!active() || locked()) return Grip::None;
  auto hit = [&](Grip g) {
    QPointF p;
    return gripPoint(g, p) && std::hypot(p.x() - at.x(), p.y() - at.y()) <= 9;
  };
  if (hit(Grip::Turn)) return Grip::Turn;
  for (Grip g : {Grip::Corner0, Grip::Corner1, Grip::Corner2, Grip::Corner3})
    if (hit(g)) return g;
  double u, v;
  if (!planeAt(at, u, v)) return Grip::None;
  const double c = std::cos(m_place.angle), s = std::sin(m_place.angle), dx = u - m_place.x, dy = v - m_place.y;
  return std::fabs(dx * c + dy * s) <= m_place.width / 2 && std::fabs(-dx * s + dy * c) <= m_place.height / 2 ? Grip::Move : Grip::None;
}

bool CanvasEditor::snapMove(opad::CanvasPlace& place) const {
  const auto mine = planeCorners(place);
  std::array<QPointF, 5> at;
  for (size_t i = 0; i < 5; ++i) at[i] = m_view->widgetPoint(place.plane.to_world(mine[i].first, mine[i].second));
  double best = 10 * 10, du = 0, dv = 0;  // squared pixels
  for (const auto& t : m_targets) {  // the other canvases' corners and centres
    const QPointF ts = m_view->widgetPoint(t);
    double tu, tv;
    place.plane.to_local(t, tu, tv);
    for (size_t i = 0; i < 5; ++i)
      if (const double d = (ts.x() - at[i].x()) * (ts.x() - at[i].x()) + (ts.y() - at[i].y()) * (ts.y() - at[i].y()); d < best)
        best = d, du = tu - mine[i].first, dv = tv - mine[i].second;
  }
  const opad::Frame& p = place.plane;
  const double px = m_view->pixelSize(), r = 10 * px;
  if (m_model && px > 0 && m_modelPlane.origin == p.origin && m_modelPlane.x == p.x && m_modelPlane.y == p.y)  // the model, projected on the plane
    for (const auto& [u, v] : mine)
      for (auto it = std::lower_bound(m_model->begin(), m_model->end(), std::array<double, 2>{u - r, -1e300}); it != m_model->end() && (*it)[0] <= u + r; ++it)
        if (const double d = ((*it)[0] - u) * ((*it)[0] - u) / (px * px) + ((*it)[1] - v) * ((*it)[1] - v) / (px * px); d < best)
          best = d, du = (*it)[0] - u, dv = (*it)[1] - v;
  if (best < 100) {
    place.x += du, place.y += dv;
    return true;
  }
  if (!m_view->gridSnap() || !(m_view->gridStep() > 0)) return false;
  const double step = m_view->gridStep();
  place.x = std::round(place.x / step) * step, place.y = std::round(place.y / step) * step;
  return true;
}

void CanvasEditor::dragTo(const QPointF& at, Qt::KeyboardModifiers mods) {
  double u, v;
  if (!planeAt(at, u, v)) return;
  opad::CanvasPlace p = m_start;
  if (m_drag == Grip::Move) {
    p.x += u - m_pressU, p.y += v - m_pressV;
    if (!(mods & Qt::AltModifier) && snapMove(p)) emit status(tr("Snapped"));
  } else if (m_drag == Grip::Turn) {
    double turn = std::atan2(v - m_start.y, u - m_start.x) - std::atan2(m_pressV - m_start.y, m_pressU - m_start.x);
    p.angle = m_start.angle + turn;
    if (mods & Qt::ShiftModifier) p.angle = std::round(p.angle / (M_PI / 12)) * (M_PI / 12);
    p.angle = std::remainder(p.angle, 2 * M_PI);
  } else {
    const int i = int(m_drag) - int(Grip::Corner0);
    const auto c = planeCorners(m_start);
    const auto fixed = (mods & Qt::ControlModifier) ? c[4] : c[size_t((i + 2) % 4)];
    const double ax = c[size_t(i)].first - fixed.first, ay = c[size_t(i)].second - fixed.second, l2 = ax * ax + ay * ay;
    if (l2 < 1e-18) return;
    const double k = std::max(0.01, ((u - fixed.first) * ax + (v - fixed.second) * ay) / l2);
    p.width = m_start.width * k, p.height = m_start.height * k;
    p.x = fixed.first + (m_start.x - fixed.first) * k, p.y = fixed.second + (m_start.y - fixed.second) * k;
  }
  m_moved = true;
  m_place = p;
  m_view->previewPlacement(m_canvas, opad::canvas_world(p));
  show(p);
  emit dragged(p);
}

void CanvasEditor::vertexFilter(bool on) {
  if (on && m_oldFilter < 0) {
    m_oldFilter = int(m_view->selectionFilter());
    m_view->setSelectionFilter(Viewport::SelFilter::Vertex);
  } else if (!on && m_oldFilter >= 0) {
    m_view->setSelectionFilter(Viewport::SelFilter(m_oldFilter));
    m_oldFilter = -1;
  }
}

void CanvasEditor::pick(Pick what) {
  if (what == Pick::Model) vertexFilter(true);  // vertices and circle centres are what a model point snaps to
  if (what == Pick::None) vertexFilter(false);
  m_pick = what;
  m_view->setCursor(what == Pick::None ? Qt::ArrowCursor : Qt::CrossCursor);
  if (what == Pick::None) m_view->unsetCursor();
}

bool CanvasEditor::eventFilter(QObject* object, QEvent* event) {
  if (object != m_view || !active()) return false;
  const QEvent::Type type = event->type();
  if (type == QEvent::ShortcutOverride && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && (dragging() || m_pick != Pick::None)) {
    event->accept();  // ours (the drag goes back, the pick steps back), not the window's Esc
    return true;
  }
  if (type == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
    if (dragging()) {  // the drag goes back where it started
      m_drag = Grip::None;
      m_place = m_start;
      m_view->endPlacementPreview(m_canvas);
      show(m_place);
      emit dragged(m_place);
      return true;
    }
    if (m_pick != Pick::None) {
      pick(Pick::None);
      emit pickCancelled();
      return true;
    }
    return false;
  }
  if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease) return false;
  auto* e = static_cast<QMouseEvent*>(event);
  const QPointF at = e->position();
  if (type == QEvent::MouseButtonPress) {
    if (e->button() != Qt::LeftButton || m_view->cubeAt(at)) return false;
    if (m_pick == Pick::Canvas) {
      double u, v;
      const double c = std::cos(m_place.angle), s = std::sin(m_place.angle);
      if (planeAt(at, u, v) && std::fabs((u - m_place.x) * c + (v - m_place.y) * s) <= m_place.width * 0.51 &&
          std::fabs(-(u - m_place.x) * s + (v - m_place.y) * c) <= m_place.height * 0.51)
        emit picked(m_place.plane.to_world(u, v), false);
      else
        emit status(tr("Click a point on the picture"));
      return true;
    }
    if (m_pick == Pick::Model) {
      opad::Ref ref;
      if (m_view->originReferenceAt(at, ref) && ref.body != m_canvas) {
        try {
          const opad::json info = opad::inspect_ref(m_doc->doc, m_doc->scene, ref);
          const opad::json& p = info.contains("point") ? info["point"] : info.at("center");
          emit picked({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}, true);
          return true;
        } catch (const std::exception&) {
        }
      }
      emit status(tr("Click a vertex or a circle's centre of the model"));
      return true;
    }
    const Grip g = gripAt(at);
    if (g == Grip::None) return false;  // off the canvas: the view's (navigation, selection)
    m_drag = g;
    m_start = m_place;
    m_press = at;
    m_moved = false;
    planeAt(at, m_pressU, m_pressV);
    m_targets.clear();
    if (g == Grip::Move)
      for (const auto& id : m_doc->scene.all_bodies())
        if (const opad::Node* n = m_doc->scene.node(id); id != m_canvas && n && opad::is_canvas(*n) && m_doc->scene.effectively_visible(id)) try {
            const opad::CanvasPlace other = opad::canvas_place(m_doc->scene, id);
            for (const auto& p : opad::canvas_points(opad::canvas_world(other), other.body_w, other.body_h)) m_targets.push_back(p);
          } catch (const std::exception&) {
          }
    return true;
  }
  if (type == QEvent::MouseMove) {
    if (dragging()) {
      if (m_moved || (at - m_press).manhattanLength() >= 3) dragTo(at, e->modifiers());
      return true;
    }
    if (e->buttons() != Qt::NoButton || m_pick != Pick::None) return false;
    const Grip g = gripAt(at);
    if (g != m_hover) {
      m_hover = g;
      if (g == Grip::None) m_view->unsetCursor();
      else m_view->setCursor(g == Grip::Move ? Qt::SizeAllCursor : g == Grip::Turn ? Qt::PointingHandCursor : Qt::SizeFDiagCursor);
    }
    return false;  // hovering still highlights in the view
  }
  if (!dragging() || e->button() != Qt::LeftButton) return false;
  const Grip was = m_drag;
  m_drag = Grip::None;
  if (!m_moved) {  // a click: nothing moves
    if (was != Grip::None) m_view->endPlacementPreview(m_canvas);
    return true;
  }
  emit placed(m_place);
  return true;
}
