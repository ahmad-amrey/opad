// Object snap (UI-90): while a tool picks points, the drawings' and sketches' ends, midpoints, centres, quadrants,
// intersections, nearest points and, from the point picked before, perpendicular and tangent points (opad/snap2d.hpp, the
// sketch's snap set and settings) under the mouse, with a marker of the kind's shape; a click picks that point. Indexes
// per body key (a sketch's per version of it), built on a worker the first time a pick needs them.
#include "Viewport.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <Prs3d_LineAspect.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Circ.hxx>

#include <QApplication>
#include <QMouseEvent>
#include <QSettings>

#include <cmath>
#include <map>
#include <set>

#include "Jobs.hpp"
#include "Units.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/snap2d.hpp"

struct ObjectSnapState {
  std::map<std::string, std::shared_ptr<const opad::snap2d::Index>> indexes;  // by body key, or a sketch's version key
  struct Version {
    std::string stamp, key;
  };
  std::map<std::string, Version> sketches;  // by sketch id: the stamp its index was asked for and that index's key
  int versions = 0;
  std::set<std::string> declined;  // keys whose indexing was cancelled: not asked again until snapping starts anew
  std::optional<opad::Vec3> from;  // the point picked before
  Job* job = nullptr;
  bool shown = false;
  opad::Vec3 point{0, 0, 0};
  QString kind;
  QPointF cursor{-1e9, -1e9};
  Graphic3d_WorldViewProjState camera;
  Handle(AIS_Shape) glyph;
};

namespace {
constexpr double kAperture = 10;  // widget points around the cursor
// The drawings in one plane: each body's x and y placed in the plane's frame.
struct Group {
  opad::Frame frame;
  std::vector<opad::snap2d::Placed> sources;
};
gp_Vec vec(const opad::Vec3& v) { return gp_Vec(v[0], v[1], v[2]); }
}  // namespace

ObjectSnapState& Viewport::snapState() {
  if (!m_osnap) m_osnap = std::make_shared<ObjectSnapState>();
  return *m_osnap;
}

void Viewport::setSnapFrom(const std::optional<opad::Vec3>& from) {
  ObjectSnapState& s = snapState();
  if (s.from == from) return;
  s.from = from;
  s.cursor = {-1e9, -1e9};
}

void Viewport::setObjectSnap(bool on) {
  m_objectSnap = on;
  QSettings().setValue("view/objectSnap", on);
  if (m_osnap) m_osnap->cursor = {-1e9, -1e9}, m_osnap->declined.clear();
  redrawScene();
}

void Viewport::setSnapPicks(SnapPicks picks) {
  if (m_snapPicks == picks) return;
  m_snapPicks = picks;
  if (m_osnap) m_osnap->cursor = {-1e9, -1e9}, m_osnap->declined.clear();  // a new pick asks again for what was cancelled
  requestRedraw();  // the marker comes or goes with the next frame
}

// Indexes of what the scene no longer has (deleted bodies and sketches, another document, a viewer's next keys); hidden
// bodies keep theirs (a layer walk shows them again).
void Viewport::pruneSnapIndexes() {
  if (!m_osnap || (m_osnap->indexes.empty() && m_osnap->sketches.empty() && m_osnap->declined.empty())) return;
  ObjectSnapState& s = *m_osnap;
  std::set<std::string> live;
  for (const auto& id : m_doc->scene.all_bodies())
    if (const opad::Node* n = m_doc->scene.node(id); n && n->representation == "drawing2d") live.insert(n->body_key);
  for (auto it = s.sketches.begin(); it != s.sketches.end();)
    if (m_doc->scene.sketch(it->first)) live.insert(it++->second.key);
    else it = s.sketches.erase(it);
  for (auto it = s.indexes.begin(); it != s.indexes.end();) it = live.count(it->first) ? std::next(it) : s.indexes.erase(it);
  for (auto it = s.declined.begin(); it != s.declined.end();) it = live.count(*it) ? std::next(it) : s.declined.erase(it);
}

int Viewport::snapIndexCount() const { return m_osnap ? int(m_osnap->indexes.size()) : 0; }

bool Viewport::objectSnapActive() const {
  if (!m_objectSnap || m_snapPicks == SnapPicks::None || m_sketchInput || m_blocked || !m_bodiesPickable || m_ctrlCenterPick) return false;
  return m_snapPicks == SnapPicks::Always || (m_pickAccumulate && m_filter == SelFilter::Vertex && !m_measureSelectionLocked);
}

QString Viewport::snapWord(const QString& kind) {
  if (kind == "endpoint") return tr("Endpoint");
  if (kind == "midpoint") return tr("Midpoint");
  if (kind == "center") return tr("Center");
  if (kind == "quadrant") return tr("Quadrant");
  if (kind == "intersection") return tr("Intersection");
  if (kind == "nearest") return tr("Nearest");
  if (kind == "perpendicular") return tr("Perpendicular");
  if (kind == "tangent") return tr("Tangent");
  if (kind == "origin") return tr("Origin");
  return kind;
}

// The index key of a displayed sketch's present version ("" when it is not shown).
std::string Viewport::sketchSnapKey(const std::string& id) {
  const auto wire = m_sketchWires.find(id);
  if (wire == m_sketchWires.end() || !m_ctx->IsDisplayed(wire->second.ais)) return {};
  ObjectSnapState& s = snapState();
  ObjectSnapState::Version& v = s.sketches[id];
  if (v.key.empty() || v.stamp != wire->second.stamp) {
    if (!v.key.empty()) s.indexes.erase(v.key);
    v = {wire->second.stamp, "sketch:" + id + ":" + std::to_string(++s.versions)};
  }
  return v.key;
}

// The displayed drawings and sketches with an index, grouped by plane; those still without one are indexed on a worker
// (once): a drawing from its shape, a sketch from its curves and points in its own plane.
bool Viewport::snapIndexesReady() {
  if (!m_initialised) return false;
  ObjectSnapState& s = snapState();
  std::vector<std::pair<std::string, TopoDS_Shape>> missing;
  std::vector<std::pair<std::string, opad::json>> sketches;
  std::set<std::string> wanted;
  for (const auto& [id, item] : m_items) {
    const opad::Node* node = m_doc->scene.node(id);
    if (!node || node->representation != "drawing2d" || !node->raster.is_null() || !item.rigid || !m_ctx->IsDisplayed(item.ais)) continue;
    if (!s.indexes.count(item.key) && !s.declined.count(item.key) && wanted.insert(item.key).second) missing.push_back({item.key, item.ais->Shape()});
  }
  for (const auto& sketch : m_doc->scene.sketches)
    if (const std::string key = sketchSnapKey(sketch.id); !key.empty() && !s.indexes.count(key) && !s.declined.count(key) && wanted.insert(key).second)
      sketches.push_back({key, sketch.geometry});
  if (missing.empty() && sketches.empty()) return true;
  if (s.job || !m_jobs) return false;
  auto built = std::make_shared<std::vector<std::pair<std::string, std::shared_ptr<const opad::snap2d::Index>>>>();
  s.job = m_jobs->async(tr("Preparing object snaps"), [missing, sketches, built](Progress p) {
    const size_t total = missing.size() + sketches.size();
    for (size_t i = 0; i < missing.size() && !p.cancelled(); ++i) {
      built->push_back({missing[i].first, opad::snap2d::index_shape(missing[i].second)});
      p.setOverall(int(100 * (i + 1) / total));
    }
    for (size_t i = 0; i < sketches.size() && !p.cancelled(); ++i) {
      TopoDS_Compound shape;
      BRep_Builder builder;
      builder.MakeCompound(shape);
      try {
        const auto sk = opad::design::Sketch::from_json(sketches[i].second);
        for (const auto& e : opad::design::sketch_edges(sk, opad::Frame(), true)) builder.Add(shape, e);
        for (const auto& e : sk.entities)
          if (e.type == opad::design::SkEntity::Type::Point)
            if (const auto* q = sk.point(e.p.empty() ? 0 : e.p[0])) builder.Add(shape, BRepBuilderAPI_MakeVertex(gp_Pnt(q->x, q->y, 0)).Vertex());
      } catch (const std::exception&) {
      }
      built->push_back({sketches[i].first, opad::snap2d::index_shape(shape)});
      p.setOverall(int(100 * (missing.size() + i + 1) / total));
    }
  }, [this, built, wanted, alive = std::weak_ptr<ObjectSnapState>(m_osnap)](bool ok, const QString&) {
    const auto state = alive.lock();
    if (!state) return;
    state->job = nullptr;
    if (!ok) {  // cancelled (its Cancel button) or failed: the worker may still be filling `built`; not asked again for now
      state->declined.insert(wanted.begin(), wanted.end());
      return;
    }
    for (auto& [key, index] : *built) state->indexes[key] = std::move(index);
    state->cursor = {-1e9, -1e9};  // ask again at the next frame
    redrawScene();
  });
  return false;
}

bool Viewport::snapAt(const QPointF& widgetPos, opad::Vec3& world, QString* kind) {
  if (!m_initialised || !snapIndexesReady()) return false;
  ObjectSnapState& s = snapState();
  // Group the drawings and sketches by plane: the first one of a plane gives the frame, the others are placed in it.
  std::vector<Group> groups;
  auto place = [&](const opad::snap2d::Index* index, const gp_Pnt& o, const gp_Vec& x, const gp_Vec& y) {
    Group* group = nullptr;
    for (auto& g : groups) {
      const gp_Vec n = vec(g.frame.normal());
      if (std::abs(x.Dot(n)) < 1e-9 * x.Magnitude() && std::abs(y.Dot(n)) < 1e-9 * y.Magnitude() && std::abs(gp_Vec(gp_Pnt(g.frame.origin[0], g.frame.origin[1], g.frame.origin[2]), o).Dot(n)) < 1e-6) {
        group = &g;
        break;
      }
    }
    if (!group) {
      opad::Frame f;
      f.origin = {o.X(), o.Y(), o.Z()};
      const gp_Vec ux = x.Normalized(), uy = y.Normalized();
      f.x = {ux.X(), ux.Y(), ux.Z()};
      f.y = {uy.X(), uy.Y(), uy.Z()};
      groups.push_back({f, {}});
      group = &groups.back();
    }
    const gp_Vec gx = vec(group->frame.x), gy = vec(group->frame.y);
    const gp_Vec offset(gp_Pnt(group->frame.origin[0], group->frame.origin[1], group->frame.origin[2]), o);
    group->sources.push_back({index, x.Dot(gx), y.Dot(gx), offset.Dot(gx), x.Dot(gy), y.Dot(gy), offset.Dot(gy)});
  };
  for (const auto& [id, item] : m_items) {
    const auto found = s.indexes.find(item.key);
    if (found == s.indexes.end() || !m_ctx->IsDisplayed(item.ais) || found->second->empty()) continue;
    const opad::Node* node = m_doc->scene.node(id);
    if (!node || node->representation != "drawing2d" || !item.rigid) continue;
    const gp_Trsf t = item.ais->Transformation();
    place(found->second.get(), gp_Pnt(0, 0, found->second->z).Transformed(t), gp_Vec(1, 0, 0).Transformed(t), gp_Vec(0, 1, 0).Transformed(t));
  }
  for (const auto& sketch : m_doc->scene.sketches) {
    const auto found = s.indexes.find(sketchSnapKey(sketch.id));
    if (found == s.indexes.end() || found->second->empty()) continue;
    const opad::Frame& f = sketch.frame;
    place(found->second.get(), gp_Pnt(f.origin[0], f.origin[1], f.origin[2]), vec(f.x), vec(f.y));
  }
  QSettings settings;
  opad::snap2d::Kinds kinds;
  auto on = [&](const char* name) { return settings.value(QString("sketch/snap/") + name, true).toBool(); };  // the sketch's switches
  kinds.endpoint = on("endpoint"), kinds.midpoint = on("midpoint"), kinds.center = on("center"), kinds.quadrant = on("quadrant");
  kinds.intersection = on("intersection"), kinds.nearest = on("nearest"), kinds.perpendicular = on("perpendicular"), kinds.tangent = on("tangent");
  const double aperture = kAperture * pixelSize();
  double best = 1e300;
  int bestClass = 3;
  for (const auto& g : groups) {
    double u, v;
    if (!planePoint(widgetPos, g.frame, u, v)) continue;
    opad::snap2d::P from;
    if (s.from) g.frame.to_local(*s.from, from.x, from.y);
    const opad::snap2d::Snap hit = opad::snap2d::snap(g.sources, {u, v}, aperture, kinds, s.from ? &from : nullptr);
    if (!hit) continue;
    const int cls = hit.kind == opad::snap2d::Kind::Endpoint || hit.kind == opad::snap2d::Kind::Center ? 0 : hit.kind == opad::snap2d::Kind::Nearest ? 2 : 1;
    const opad::Vec3 at = g.frame.to_world(hit.at.x, hit.at.y);
    const double d = QLineF(widgetPoint(at), widgetPos).length();
    if (cls > bestClass || (cls == bestClass && d >= best)) continue;
    best = d, bestClass = cls, world = at;
    if (kind) *kind = opad::snap2d::kind_name(hit.kind);
  }
  return bestClass < 3;
}

bool Viewport::shownSnap(opad::Vec3& world, QString* kind) const {
  if (!m_osnap || !m_osnap->shown) return false;
  world = m_osnap->point;
  if (kind) *kind = m_osnap->kind;
  return true;
}

// The kind's shape around the point, facing the camera, 7 points across: AutoCAD's square end, triangle midpoint, circle
// centre, diamond quadrant, cross intersection, right angle perpendicular, circle under a line tangent, hourglass nearest.
TopoDS_Shape Viewport::snapGlyph(const QString& kind, const opad::Vec3& at) const {
  const double r = 6 * pixelSize();
  const gp_Pnt c(at[0], at[1], at[2]);
  const gp_Dir n(-vec(viewDirection()));
  const gp_Ax2 axes(c, n);
  const gp_Vec ux(axes.XDirection()), uy(axes.YDirection());
  auto p = [&](double x, double y) { return c.Translated(ux * (x * r) + uy * (y * r)); };
  BRep_Builder builder;
  TopoDS_Compound shape;
  builder.MakeCompound(shape);
  auto loop = [&](std::initializer_list<std::pair<double, double>> pts, bool closed = true) {
    std::vector<gp_Pnt> v;
    for (const auto& [x, y] : pts) v.push_back(p(x, y));
    for (size_t i = 0; i + 1 < v.size() + (closed ? 1 : 0); ++i) builder.Add(shape, BRepBuilderAPI_MakeEdge(v[i], v[(i + 1) % v.size()]).Edge());
  };
  if (kind == "endpoint") loop({{-1, -1}, {1, -1}, {1, 1}, {-1, 1}});
  else if (kind == "midpoint") loop({{-1, -0.8}, {1, -0.8}, {0, 1}});
  else if (kind == "center") builder.Add(shape, BRepBuilderAPI_MakeEdge(gp_Circ(axes, r)).Edge());
  else if (kind == "quadrant") loop({{0, -1.2}, {1.2, 0}, {0, 1.2}, {-1.2, 0}});
  else if (kind == "intersection") loop({{-1, -1}, {1, 1}}, false), loop({{-1, 1}, {1, -1}}, false);
  else if (kind == "perpendicular") loop({{-1, 1}, {-1, -1}, {1, -1}}, false), loop({{-1, 0}, {0, 0}, {0, -1}}, false);
  else if (kind == "tangent") builder.Add(shape, BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(p(0, -0.15), n), 0.8 * r)).Edge()), loop({{-1, 0.65}, {1, 0.65}}, false);
  else loop({{-1, 1}, {1, 1}, {-1, -1}, {1, -1}});  // nearest
  return shape;
}

// After a frame's detection (paintEvent): the snap under the cursor, its marker and its name in the status bar.
void Viewport::updateObjectSnap() {
  if (!m_initialised || (!m_osnap && !objectSnapActive())) return;
  ObjectSnapState& s = snapState();
  const bool active = objectSnapActive() && QApplication::mouseButtons() == Qt::NoButton && rect().contains(m_trackingCursor.toPoint());
  if (!active && !s.shown) return;
  const auto camera = m_view->Camera()->WorldViewProjState();
  if (active && s.cursor == m_trackingCursor && s.camera == camera) return;  // asked once per move or camera change, never per frame
  s.cursor = m_trackingCursor;
  s.camera = camera;
  opad::Vec3 at;
  QString kind;
  const bool found = active && snapAt(m_trackingCursor, at, &kind);
  if (!found && !s.shown) return;
  if (!s.glyph.IsNull()) {
    m_ctx->Remove(s.glyph, false);
    s.glyph.Nullify();
  }
  const bool was = s.shown;
  s.shown = found;
  if (!found) {
    if (was) {  // the status says what is hovered again
      m_hoverOwner = nullptr;
      updateHover();
    }
    redrawScene();
    return;
  }
  s.point = at;
  s.kind = kind;
  s.glyph = new AIS_Shape(snapGlyph(kind, at));
  const QColor colour = m_tokens.candidate;
  s.glyph->Attributes()->SetWireAspect(new Prs3d_LineAspect(Quantity_Color(colour.redF(), colour.greenF(), colour.blueF(), Quantity_TOC_sRGB), Aspect_TOL_SOLID, lineWidth(2)));
  s.glyph->SetZLayer(Graphic3d_ZLayerId_Topmost);
  s.glyph->SetInfiniteState(true);  // never part of Fit
  m_ctx->Display(s.glyph, 0, -1, false);  // never pickable: a click is taken in objectSnapPress
  const QString text = tr("%1 · click to pick this point (F3 turns object snap off)").arg(snapWord(kind));
  if (text != m_hover) {
    m_hover = text;
    emit hoverChanged(text);
  }
  emit hoverPoint(true, at);
  redrawScene();
}

// A left press while a snap is shown picks its point: a free point (Point ref) selected like a tracking point.
bool Viewport::objectSnapPress(QMouseEvent* e) {
  if (!m_osnap || !m_osnap->shown || e->button() != Qt::LeftButton || m_cubeGesture || m_snapPicks != SnapPicks::Points || !objectSnapActive()) return false;
  if ((QPointF(widgetPoint(m_osnap->point)) - e->position()).manhattanLength() > 2 * kAperture + 4) return false;
  opad::Ref ref;
  ref.kind = opad::Ref::Kind::Point;
  ref.point = m_osnap->point;
  centerMarker(ref, gp_Pnt(ref.point[0], ref.point[1], ref.point[2]));
  m_snapClick = ref.str();
  e->accept();
  return true;
}

bool Viewport::pointUnder(const QPointF& widgetPos, opad::Vec3& world) {
  gp_Pnt p;
  if (!m_initialised || !navigationPoint(devicePos(widgetPos), p)) return false;
  world = {p.X(), p.Y(), p.Z()};
  return true;
}

bool Viewport::benchSnap(const QPointF& widgetPos) {
  if (!m_initialised) return false;
  m_view->Redraw();  // the camera range a frame sets
  m_trackingCursor = widgetPos;
  if (m_osnap) m_osnap->cursor = {-1e9, -1e9};
  updateObjectSnap();
  return m_osnap && m_osnap->shown;
}
