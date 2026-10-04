#include "PrimitivePlacer.hpp"

#include <AIS_Shape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Circ.hxx>

#include <QMouseEvent>
#include <QPointer>

#include <algorithm>
#include <cmath>

#include "AppDocument.hpp"
#include "DesignPanels.hpp"
#include "DimensionHandle.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "ToolValues.hpp"
#include "Units.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/sketch_geom.hpp"

using opad::json;
using Stage = PrimitivePlacer::Stage;

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
constexpr double kSnapPixels = 10;   // a vertex, a round edge's centre or the plane's origin this close to the pointer takes it
constexpr size_t kTargets = 4000;    // of one hovered face: enough for any face a primitive is put on
gp_Pnt pnt(const opad::Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }
}  // namespace

PrimitivePlacer::PrimitivePlacer(AppDocument* doc, Viewport* view, JobRunner* jobs, FeaturePanel* form, ToolValues* values, QObject* parent)
    : QObject(parent), m_doc(doc), m_view(view), m_jobs(jobs), m_form(form), m_values(values) {
  view->installEventFilter(this);
}

PrimitivePlacer::~PrimitivePlacer() {
  if (m_job) m_job->cancel();
}

bool PrimitivePlacer::ring() const { return m_spec && m_spec->footprint == "ring"; }

bool PrimitivePlacer::hasHeight() const { return m_form->input("height") != nullptr; }

QStringList PrimitivePlacer::sizeKeys() const {
  if (!m_spec) return {};
  if (m_spec->footprint == "rect") return {"length", "width"};
  if (m_stage == Stage::Section) return {"section"};
  return {"diameter"};
}

void PrimitivePlacer::start(const opad::design::FeatureSpec& spec) {
  stop();
  m_spec = &spec;
  m_locked.clear();
  m_written.clear();
  for (const char* key : {"x", "y", "length", "width", "diameter", "section"})
    if (m_form->input(key)) m_written[key] = m_form->valueText(key);
  enter(Stage::Place);
}

void PrimitivePlacer::stop() {
  if (m_stage == Stage::Off) return;
  const Stage was = m_stage;
  m_stage = Stage::Off;
  ++m_serial;
  if (m_job) m_job->cancel();
  m_job = nullptr;
  m_down = false;
  m_release = {};
  clearMarker();
  clearOutline();
  if (was == Stage::Place) m_view->clearCandidates();
  if (was == Stage::Size || was == Stage::Section) m_values->reset();  // the sizes' boxes by the pointer go
  m_planes.clear();
  m_form->setPickNote("plane", QString());
  m_form->setGuideStep(0, 0);
  setStatus(QString());
  emit stageChanged();
}

void PrimitivePlacer::setStatus(const QString& text) {
  if (text == m_status) return;
  m_status = text;
  emit status(text);
}

// The stage's look: the planes to click and the plane box's note (Place), the outline and the boxes by the pointer (Size,
// Section), the guide's step and the prompt.
void PrimitivePlacer::enter(Stage stage) {
  const Stage was = m_stage;
  m_stage = stage;
  m_sized = stage == Stage::Height || stage == Stage::Done || (was != Stage::Place && was != Stage::Off);  // back from a later stage: sized already
  clearMarker();
  if (stage == Stage::Place) {
    m_sized = false;
    showPlanes();
    if (m_view->selectionFilter() != Viewport::SelFilter::Face) m_view->setSelectionFilter(Viewport::SelFilter::Face);  // planar faces to click
    m_form->setPickNote("plane", tr("Click in the view"));
    clearOutline();
  } else {
    if (was == Stage::Place) m_view->clearCandidates();
    m_form->setPickNote("plane", QString());
  }
  if (stage == Stage::Size || stage == Stage::Section) {
    for (const QString& key : sizeKeys()) m_locked.erase(key);  // sized again from the pointer, unless typed again
    m_values->reset();
    m_values->showNear(m_last.toPoint(), {});
    drawOutline();
  } else {
    m_values->reset();
    if (stage != Stage::Place) clearOutline();
  }
  m_form->setGuideStep(guideStep(), guideCount());
  setStatus(prompt());
  emit stageChanged();
}

void PrimitivePlacer::showPlanes() {
  if (m_stage != Stage::Place) return;
  m_planes = planes ? planes() : std::vector<Viewport::Candidate>{};
  m_view->showCandidates(m_planes);
}

int PrimitivePlacer::guideCount() const { return ring() ? 4 : 3; }

int PrimitivePlacer::guideStep() const {
  switch (m_stage) {
    case Stage::Place: return 0;
    case Stage::Size: return 1;
    case Stage::Section: return 2;
    case Stage::Height: return 2;
    case Stage::Done: return ring() ? 3 : 2;
    default: return 0;
  }
}

QString PrimitivePlacer::prompt() const {
  if (!m_spec) return {};
  const QString what = i18n::t(QString::fromStdString(m_spec->label)).toLower();
  auto label = [this](const char* key) {
    const opad::design::InputSpec* in = m_form->input(key);
    return in ? i18n::t(QString::fromStdString(in->label)) : QString(key);
  };
  switch (m_stage) {
    case Stage::Place: return tr("Click a plane or a planar face to place the %1 · Enter adds it where the panel says").arg(what);
    case Stage::Size:
      return tr("%1: move the pointer and click, or type it (Tab: the next box) · Enter adds the %2")
          .arg(m_spec->footprint == "rect" ? tr("Length and width") : label("diameter"), what);
    case Stage::Section: return tr("%1: move the pointer and click, or type it (Tab: the next box) · Enter adds the %2").arg(label("section"), what);
    case Stage::Height: return tr("Drag the arrow or type the height · Enter adds the %1").arg(what);
    case Stage::Done: return tr("The panel has every value to change · Enter adds the %1").arg(what);
    default: return {};
  }
}

bool PrimitivePlacer::tracking() const { return (m_stage == Stage::Size || m_stage == Stage::Section) && m_sized; }

bool PrimitivePlacer::previewShown() const {
  if (m_stage == Stage::Off) return true;
  if (m_stage == Stage::Place) return false;
  return m_sized || m_stage == Stage::Section;
}

bool PrimitivePlacer::arrowShown() const { return m_stage == Stage::Off || m_stage == Stage::Height || m_stage == Stage::Done; }

bool PrimitivePlacer::markerShown(opad::Vec3* at) const {
  if (at && m_markerShown) *at = m_markerAt;
  return m_markerShown;
}

QList<DynamicInput::Field> PrimitivePlacer::fields() const {
  QList<DynamicInput::Field> out;
  if (m_stage != Stage::Size && m_stage != Stage::Section) return out;
  for (const QString& key : sizeKeys())
    if (const opad::design::InputSpec* in = m_form->input(key)) out << ToolValues::box(key, i18n::t(QString::fromStdString(in->label)), m_form->valueText(key));
  return out;
}

QStringList PrimitivePlacer::arrowExtras() const {
  QStringList out;
  if (m_form->input("top_diameter")) out << "top_diameter";  // a cone's: what its guide's card shows next
  if (m_spec && m_spec->footprint == "rect") out << "length" << "width";
  else if (m_form->input("diameter")) out << "diameter";
  return out;
}

void PrimitivePlacer::typed(const QString& key) {
  if (!m_values->input() || m_stage == Stage::Off) return;
  if (!m_values->input()->text(key).isEmpty()) m_locked.insert(key);
  else m_locked.erase(key);
  m_written[key] = m_form->valueText(key);
  drawOutline();
}

void PrimitivePlacer::inputsChanged() {
  if (m_writing || m_stage == Stage::Off) return;
  for (auto& [key, text] : m_written)
    if (m_form->valueText(key) != text) {
      m_locked.insert(key);  // typed into the panel (or a box): the pointer leaves it
      text = m_form->valueText(key);
    }
  drawOutline();
}

void PrimitivePlacer::write(const std::vector<std::pair<QString, json>>& values) {
  m_writing = true;
  for (const auto& [key, value] : values)
    if (m_written.count(key) && value.is_string()) m_written[key] = QString::fromStdString(value.get<std::string>());
  m_form->setValues(values);
  m_writing = false;
}

double PrimitivePlacer::value(const QString& key, double fallback) const {
  try {
    std::vector<opad::design::ParamDef> defs;
    for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    return opad::design::ParamTable(defs, m_doc->scene.units).length(m_form->valueText(key).trimmed().toStdString());
  } catch (const std::exception&) {
    return fallback;
  }
}

QString PrimitivePlacer::lengthText(double mm, double step) const { return DimensionHandle::pulledText(mm, step); }

double PrimitivePlacer::gridOr(double fallback, bool free) const { return m_view->gridSnap() && !free && m_view->gridStep() > 0 ? m_view->gridStep() : fallback; }

bool PrimitivePlacer::snapTo(const QPointF& pos, const std::vector<opad::Vec3>& targets, opad::Vec3& out) const {
  double best = kSnapPixels * kSnapPixels;
  bool found = false;
  for (const auto& t : targets) {
    const QPointF d = QPointF(m_view->widgetPoint(t)) - pos;
    const double dd = QPointF::dotProduct(d, d);
    if (dd <= best) best = dd, out = t, found = true;
  }
  return found;
}

std::vector<opad::Vec3> PrimitivePlacer::faceTargets(const TopoDS_Face& face) const {
  std::vector<opad::Vec3> out;
  for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More() && out.size() < kTargets; v.Next()) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
    out.push_back({p.X(), p.Y(), p.Z()});
  }
  for (TopExp_Explorer e(face, TopAbs_EDGE); e.More() && out.size() < 2 * kTargets; e.Next()) {
    const BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
    if (c.GetType() != GeomAbs_Circle) continue;
    const gp_Pnt p = c.Circle().Location();
    out.push_back({p.X(), p.Y(), p.Z()});
  }
  return out;
}

// What the pointer is over and where a click would put the primitive: an origin or construction plane (its frame known), a
// planar face (its frame resolved on a worker when clicked), or nothing: the plane the panel holds.
PrimitivePlacer::Hit PrimitivePlacer::hitAt(const QPointF& pos, bool free) {
  Hit h;
  std::string candidate;
  TopoDS_Face face;
  opad::Vec3 at{0, 0, 0};
  const double step = DimensionHandle::pullStep(m_view->pixelSize());
  // A known frame: the point where the pointer meets it, snapped to the plane's origin, else the grid (or a round step).
  auto onFrame = [&](const json& plane, const opad::Frame& frame) {
    double u = 0, v = 0;
    if (!m_view->planePoint(pos, frame, u, v) || !std::isfinite(u) || !std::isfinite(v) || std::fabs(u) > 1e6 || std::fabs(v) > 1e6) return false;
    opad::Vec3 snapped;
    if (snapTo(pos, {frame.origin}, snapped)) u = v = 0;
    else {
      const double s = gridOr(step, free);
      u = std::round(u / s) * s;
      v = std::round(v / s) * s;
    }
    h.ok = h.frameKnown = true;
    h.plane = plane;
    h.frame = frame;
    h.u = u;
    h.v = v;
    h.at = frame.to_world(u, v);
    return true;
  };
  if (m_view->surfaceAt(pos, candidate, face, at)) {
    if (!candidate.empty()) {
      const json id = json::parse(candidate, nullptr, false);
      if (id.is_object() && id.contains("base")) onFrame(id, opad::design::base_frame(id.value("base", std::string("xy"))));
      else if (id.is_object() && id.contains("feature"))
        if (const opad::Feature* f = m_doc->scene.feature(id.value("feature", std::string())); f && f->result.contains("plane")) onFrame(id, opad::Frame::from_json(f->result.at("plane")));
      return h;
    }
    if (!face.IsNull()) {
      const BRepAdaptor_Surface surface(face);
      if (surface.GetType() != GeomAbs_Plane) {
        h.why = tr("A curved face: click a planar face, an origin plane or a construction plane");
        return h;
      }
      opad::Vec3 snapped;
      if (snapTo(pos, faceTargets(face), snapped)) at = snapped, h.snapped = true;
      gp_Ax3 ax = surface.Plane().Position();
      gp_Dir n = ax.Direction();
      if (!ax.Direct()) n.Reverse();
      if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
      h.ok = true;
      h.frame = opad::design::frame_from_ax3(gp_Ax3(pnt(at), n, ax.XDirection()));
      h.at = at;
      return h;
    }
  }
  // Nothing under the pointer: the plane the panel holds, or the origin plane that faces the view most when that one is seen
  // edge-on (XY from the front).
  const json held = m_form->picks("plane");
  std::vector<std::pair<json, opad::Frame>> choices;
  if (held.is_object() && held.contains("base")) choices.push_back({json{{"base", held["base"]}}, opad::design::base_frame(held.value("base", std::string("xy")))});
  else if (held.is_object() && held.contains("feature"))
    if (const opad::Feature* f = m_doc->scene.feature(held.value("feature", std::string())); f && f->result.contains("plane")) choices.push_back({held, opad::Frame::from_json(f->result.at("plane"))});
  if (choices.empty()) choices.push_back({json{{"base", "xy"}}, opad::design::base_frame("xy")});
  const opad::Vec3 d = m_view->viewDirection();
  auto facing = [&d](const opad::Frame& f) {
    const opad::Vec3 n = f.normal();
    return std::fabs(n[0] * d[0] + n[1] * d[1] + n[2] * d[2]);
  };
  if (facing(choices.front().second) < 0.2) {
    std::pair<json, opad::Frame> best{json{{"base", "xy"}}, opad::design::base_frame("xy")};
    for (const char* base : {"xy", "xz", "yz"})
      if (facing(opad::design::base_frame(base)) > facing(best.second)) best = {json{{"base", base}}, opad::design::base_frame(base)};
    choices.front() = best;
  }
  onFrame(choices.front().first, choices.front().second);
  return h;
}

void PrimitivePlacer::clearMarker() {
  if (!m_marker.IsNull()) m_view->removeOverlay(m_marker);
  m_marker.Nullify();
  m_markerShown = false;
}

// A cross in a ring on the plane where a click would put the primitive (amber, as the plane picker's origin).
void PrimitivePlacer::showMarker(const Hit& hit) {
  clearMarker();
  if (!hit.ok) return;
  const opad::Frame& f = hit.frame;
  const double s = m_view->pixelSize() * 12;
  double u0 = 0, v0 = 0;
  f.to_local(hit.at, u0, v0);
  auto p = [&](double du, double dv) { return pnt(f.to_world(u0 + du, v0 + dv)); };
  TopoDS_Compound shape;
  BRep_Builder b;
  b.MakeCompound(shape);
  b.Add(shape, BRepBuilderAPI_MakeEdge(p(-s, 0), p(s, 0)).Edge());
  b.Add(shape, BRepBuilderAPI_MakeEdge(p(0, -s), p(0, s)).Edge());
  const opad::Vec3 n = f.normal();
  gp_Ax2 axes(pnt(hit.at), gp_Dir(n[0], n[1], n[2]), gp_Dir(f.x[0], f.x[1], f.x[2]));
  b.Add(shape, BRepBuilderAPI_MakeEdge(gp_Circ(axes, s * 0.6)).Edge());
  Handle(AIS_Shape) ais = new AIS_Shape(shape);
  ais->SetInfiniteState(true);
  ais->SetColor(occ(theme::current().amber));
  ais->SetWidth(2);
  m_marker = ais;
  m_view->showOverlay(m_marker);
  m_markerShown = true;
  m_markerAt = hit.at;
}

void PrimitivePlacer::clearOutline() {
  if (!m_outline.IsNull()) m_view->removeOverlay(m_outline);
  m_outline.Nullify();
}

// The footprint as the panel holds it, drawn on the plane at once (the preview body follows when its plan is back).
void PrimitivePlacer::drawOutline() {
  if (m_stage != Stage::Size && m_stage != Stage::Section) return clearOutline();
  if (!m_sized) return clearOutline();
  const opad::Frame& f = m_frame;
  const double cu = value("x", m_cu), cv = value("y", m_cv);
  TopoDS_Compound shape;
  BRep_Builder b;
  b.MakeCompound(shape);
  const opad::Vec3 n = f.normal();
  auto circle = [&](double r) {
    if (r <= 1e-9) return;
    const gp_Ax2 axes(pnt(f.to_world(cu, cv)), gp_Dir(n[0], n[1], n[2]), gp_Dir(f.x[0], f.x[1], f.x[2]));
    b.Add(shape, BRepBuilderAPI_MakeEdge(gp_Circ(axes, r)).Edge());
  };
  try {
    if (m_spec->footprint == "rect") {
      const double l = value("length", 0), w = value("width", 0);
      const bool centred = m_form->inputs().value("centered", true);
      const double u0 = centred ? cu - l / 2 : cu, v0 = centred ? cv - w / 2 : cv;
      if (l <= 1e-9 || w <= 1e-9) return clearOutline();
      const gp_Pnt c[4] = {pnt(f.to_world(u0, v0)), pnt(f.to_world(u0 + l, v0)), pnt(f.to_world(u0 + l, v0 + w)), pnt(f.to_world(u0, v0 + w))};
      for (int i = 0; i < 4; ++i) b.Add(shape, BRepBuilderAPI_MakeEdge(c[i], c[(i + 1) % 4]).Edge());
    } else if (m_spec->footprint == "ring") {
      const double r = value("diameter", 0) / 2;
      if (m_stage == Stage::Section) {
        const double s = value("section", 0) / 2;
        circle(r - s);
        circle(r + s);
      } else {
        circle(r);
      }
    } else {
      circle(value("diameter", 0) / 2);
    }
  } catch (const Standard_Failure&) {
    return clearOutline();
  }
  clearOutline();
  Handle(AIS_Shape) ais = new AIS_Shape(shape);
  ais->SetInfiniteState(true);
  ais->SetColor(occ(theme::current().sel));
  ais->SetWidth(2);
  m_outline = ais;
  m_view->showOverlay(m_outline);
}

// The plane clicked: Plane and Position X/Y written as the panel writes them; the pointer sizes the footprint next.
void PrimitivePlacer::placeAt(const json& plane, const opad::Frame& frame, double u, double v) {
  if (m_stage != Stage::Place) return;
  m_frame = frame;
  m_cu = u;
  m_cv = v;
  // Written to the decimals this zoom tells apart (a vertex's place too, as a pull of the arrows is).
  const double step = DimensionHandle::pullStep(m_view->pixelSize());
  m_locked.erase("x");  // the click is the position, whatever was typed before
  m_locked.erase("y");
  m_form->setPicks("plane", plane);
  write({{"x", lengthText(u, step).toStdString()}, {"y", lengthText(v, step).toStdString()}});
  enter(Stage::Size);
}

void PrimitivePlacer::click(const QPointF& pos, bool free) {
  if (m_job) return;  // the face clicked before is being resolved
  const Hit hit = hitAt(pos, free);
  if (!hit.ok) {
    if (!hit.why.isEmpty()) setStatus(hit.why);
    return;
  }
  m_placedAt = pos;
  const bool snapped = hit.snapped;
  if (hit.frameKnown) return placeAt(hit.plane, hit.frame, hit.u, hit.v);
  // A face: its frame as the feature will resolve it (from the face's corner, kernel work) on a worker, then the click's
  // point in it.
  opad::Ref ref;
  if (!m_view->hoveredReference(ref) || ref.kind != opad::Ref::Kind::Face) return;
  const auto copied = copies ? copies() : std::make_pair(std::shared_ptr<const opad::Document>(std::make_shared<opad::Document>(m_doc->doc)),
                                                         std::shared_ptr<const opad::Scene>(std::make_shared<opad::Scene>(m_doc->scene)));
  auto doc = copied.first;
  auto scene = copied.second;
  auto plane = std::make_shared<json>(json{{"face", ref.to_json()}});
  auto frame = std::make_shared<opad::Frame>();
  const int serial = ++m_serial;
  const opad::Vec3 at = hit.at;
  m_release = {};
  showMarker(hit);
  m_job = m_jobs->async(tr("Resolving the plane"), [doc, scene, plane, frame](Progress p) {
    if (p.cancelled()) return;
    *frame = opad::design::resolve_plane(*doc, *scene, *plane);
    (*plane)["face"] = opad::design::make_ref(*doc, *scene, opad::Ref::from_json(plane->at("face")));
  }, [this, serial, plane, frame, at, free, snapped](bool ok, const QString& error) {
    if (serial != m_serial || m_stage != Stage::Place) return;
    m_job = nullptr;
    if (!ok) {
      if (error != "cancelled") setStatus(i18n::t(error));
      return;
    }
    double u = 0, v = 0;
    frame->to_local(at, u, v);
    // A vertex or a centre it snapped to stays where it is; anything else is rounded as on an origin plane.
    if (!snapped) {
      const double s = gridOr(DimensionHandle::pullStep(m_view->pixelSize()), free);
      u = std::round(u / s) * s;
      v = std::round(v / s) * s;
    }
    placeAt(*plane, *frame, u, v);
    if (m_release.pending) {  // pressed, dragged and let go while the face was resolved: the footprint is that drag
      sizeFrom(m_release.at, m_release.free);
      fixSize();
      m_release = {};
    } else if (QLineF(m_last, m_placedAt).length() > 3) {
      sizeFrom(m_last, free);
    }
  });
}

// The sizes where the pointer is (`pos` on the plane): the ones not typed, rounded at this zoom or to the grid.
void PrimitivePlacer::sizeFrom(const QPointF& pos, bool free) {
  if (m_stage != Stage::Size && m_stage != Stage::Section) return;
  double u = 0, v = 0;
  if (!m_view->planePoint(pos, m_frame, u, v) || !std::isfinite(u) || !std::isfinite(v)) return;
  // On a body's face: its vertices and round edges' centres take the pointer (seen where they are, put on the plane).
  std::string candidate;
  TopoDS_Face face;
  opad::Vec3 at{0, 0, 0}, snapped{0, 0, 0};
  bool exact = false;
  if (m_view->surfaceAt(pos, candidate, face, at) && !face.IsNull() && snapTo(pos, faceTargets(face), snapped)) {
    m_frame.to_local(snapped, u, v);
    exact = true;
  }
  const double pull = DimensionHandle::pullStep(m_view->pixelSize()), step = gridOr(pull, free);
  if (!exact && step != pull) {  // the grid: the pointer goes to its nodes
    u = std::round(u / step) * step;
    v = std::round(v / step) * step;
  }
  auto size = [&](double mm) { return std::max(pull, std::round(mm / pull) * pull); };  // grid nodes give whole steps already
  const double du = u - m_cu, dv = v - m_cv;
  std::vector<std::pair<QString, json>> out;
  auto put = [&](const char* key, double mm) {
    if (!m_locked.count(key)) out.push_back({key, lengthText(mm, pull).toStdString()});
  };
  if (m_spec->footprint == "rect") {
    const bool centred = m_form->inputs().value("centered", true);
    const double l = m_locked.count("length") ? value("length", 0) : size(centred ? 2 * std::fabs(du) : std::fabs(du));
    const double w = m_locked.count("width") ? value("width", 0) : size(centred ? 2 * std::fabs(dv) : std::fabs(dv));
    put("length", l);
    put("width", w);
    // Centred on the click, or (Centred off) from it as a corner towards the pointer: written each time, so turning Centred
    // on or off while sizing moves the position with it.
    if (!m_locked.count("x")) out.push_back({"x", lengthText(centred || du >= 0 ? m_cu : m_cu - l, pull).toStdString()});
    if (!m_locked.count("y")) out.push_back({"y", lengthText(centred || dv >= 0 ? m_cv : m_cv - w, pull).toStdString()});
  } else if (m_stage == Stage::Section) {
    const double r = value("diameter", 0) / 2, s = size(2 * std::fabs(std::hypot(du, dv) - r));
    put("section", std::min(s, std::max(pull, 2 * r - pull)));  // thinner than the ring
  } else {
    put("diameter", size(2 * std::hypot(du, dv)));
  }
  m_sized = true;
  write(out);
  drawOutline();
}

void PrimitivePlacer::fixSize() {
  if (m_stage == Stage::Size) {
    m_sized = true;
    enter(ring() ? Stage::Section : hasHeight() ? Stage::Height : Stage::Done);
  } else if (m_stage == Stage::Section) {
    enter(Stage::Done);
  }
}

bool PrimitivePlacer::escape() {
  switch (m_stage) {
    case Stage::Size:
      if (m_job) m_job->cancel();
      enter(Stage::Place);
      return true;
    case Stage::Section:
    case Stage::Height:
      enter(Stage::Size);
      return true;
    case Stage::Done:
      enter(ring() ? Stage::Section : Stage::Size);
      return true;
    case Stage::Place:
      if (m_job) {  // the clicked face is still being resolved: that click goes
        ++m_serial;
        m_job->cancel();
        m_job = nullptr;
        clearMarker();
        return true;
      }
      return false;
    default:
      return false;
  }
}

void PrimitivePlacer::hover(const QPointF& pos, bool free) {
  if (m_stage == Stage::Place) {
    if (m_job) return;
    const Hit hit = hitAt(pos, free);
    showMarker(hit);
    setStatus(hit.ok || hit.why.isEmpty() ? prompt() : hit.why);
  } else if (m_stage == Stage::Size || m_stage == Stage::Section) {
    if (QLineF(pos, m_placedAt).length() <= 3 && !m_sized) return;  // still on the click: no size yet
    sizeFrom(pos, free);
    DynamicInput* boxes = m_values->input();
    if (boxes && boxes->isVisible() && !boxes->typed() && !boxes->editing()) boxes->placeNear(pos.toPoint());
    else if (boxes && !boxes->isVisible()) m_values->showNear(pos.toPoint(), {});
  }
}

// The left button in the view while placing: a click places, fixes the footprint or the section; nothing else is picked
// meanwhile (a pick box of the panel being filled, the cube and the arrow keep theirs). Moves go on to the view too (its
// hover and navigation).
bool PrimitivePlacer::eventFilter(QObject* watched, QEvent* event) {
  if (watched != m_view || m_stage == Stage::Off) return false;
  const QEvent::Type type = event->type();
  if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonDblClick && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease) return false;
  if (!m_form->activeInput().isEmpty()) return false;  // the panel's pick box takes the clicks (the bodies a cut changes)
  auto* e = static_cast<QMouseEvent*>(event);
  const QPointF pos = e->position();
  const bool free = e->modifiers().testFlag(Qt::AltModifier);
  if (type == QEvent::MouseMove) {
    m_last = pos;
    if (e->buttons() == Qt::NoButton || (m_down && e->buttons() == Qt::LeftButton)) hover(pos, free);
    return false;
  }
  if (e->button() != Qt::LeftButton) return false;
  if (type != QEvent::MouseButtonRelease) {
    if ((e->modifiers() & ~Qt::AltModifier) != Qt::NoModifier || m_view->cubeAt(pos)) return false;
    m_down = true;
    m_press = pos;
    m_last = pos;
    m_pressStage = m_stage;
    if (m_stage == Stage::Place) {
      click(pos, free);
    } else if (m_stage == Stage::Size || m_stage == Stage::Section) {
      if (!m_sized && QLineF(pos, m_placedAt).length() <= 3) return true;  // a double click on the place: no size yet
      sizeFrom(pos, free);
      fixSize();
    }
    return true;
  }
  if (!m_down) return false;
  m_down = false;
  // Pressed on the plane, dragged and let go: the footprint is that drag (when the face was still resolving, once it is).
  if (m_pressStage == Stage::Place && QLineF(pos, m_press).length() > 6) {
    if (m_job) m_release = {true, pos, free};
    else if (m_stage == Stage::Size || m_stage == Stage::Section) {
      sizeFrom(pos, free);
      fixSize();
    }
  }
  return true;
}
