#include "Viewport.hpp"
#include "Units.hpp"

#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_SequenceOfHClipPlane.hxx>
#include <Prs3d_Arrow.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Precision.hxx>
#include <QFontMetricsF>
#include <algorithm>
#include <cmath>

namespace {
Quantity_Color color(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
gp_Pnt point(const opad::json& j) { return gp_Pnt(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()); }
bool samePoint(const gp_Pnt& a, const gp_Pnt& b) { return a.SquareDistance(b) <= 1e-14; }
int componentCount(const gp_Pnt& a, const gp_Pnt& b) {
  int count = 0;
  for (int axis = 1; axis <= 3; ++axis)
    if (std::abs(b.Coord(axis) - a.Coord(axis)) > Precision::Confusion()) ++count;
  return count;
}

// Small, shaded primitive arrays: no BREP construction or tessellation on camera movement.
class InspectGraphic : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(InspectGraphic, AIS_InteractiveObject)
 public:
  struct Arrow { gp_Pnt a, b; QColor color; bool twoHeads; double pixel; };
  struct Line { gp_Pnt a, b; QColor color; bool dashed = true; };
  std::vector<Arrow> arrows;
  std::vector<Line> lines;
  std::vector<gp_Pnt> endpoints;
  std::vector<gp_Pnt> snapPoints;
  QColor endpointColor, snapPointColor;
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    for (const auto& l : lines) {
      auto group = prs->NewGroup();
      Handle(Prs3d_LineAspect) style = new Prs3d_LineAspect(color(l.color), l.dashed ? Aspect_TOL_DASH : Aspect_TOL_SOLID, l.dashed ? 1.0 : 2.0);
      group->SetGroupPrimitivesAspect(style->Aspect());
      Handle(Graphic3d_ArrayOfSegments) vertices = new Graphic3d_ArrayOfSegments(2);
      vertices->AddVertex(l.a); vertices->AddVertex(l.b);
      group->AddPrimitiveArray(vertices);
    }
    for (const auto& a : arrows) {
      const double length = a.a.Distance(a.b);
      if (length < 1e-9) continue;
      auto group = prs->NewGroup();
      Handle(Prs3d_ShadingAspect) style = new Prs3d_ShadingAspect();
      group->SetClosed(true);
      style->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
      style->SetColor(color(a.color));
      group->SetGroupPrimitivesAspect(style->Aspect());
      const double head = std::min(14.0 * a.pixel, length * (a.twoHeads ? 0.22 : 0.35));
      const double radius = std::min(1.65 * a.pixel, head * 0.18);
      const gp_Pnt middle((a.a.X() + a.b.X()) / 2, (a.a.Y() + a.b.Y()) / 2, (a.a.Z() + a.b.Z()) / 2);
      auto draw = [&](const gp_Pnt& start, const gp_Pnt& end) {
        group->AddPrimitiveArray(Prs3d_Arrow::DrawShaded(gp_Ax1(start, gp_Dir(gp_Vec(start, end))), radius,
                                                       start.Distance(end), head * 0.38, head, 24));
      };
      if (a.twoHeads) { draw(middle, a.a); draw(middle, a.b); }
      else draw(a.a, a.b);
    }
    if (!endpoints.empty()) {
      auto group = prs->NewGroup();
      Handle(Prs3d_PointAspect) style = new Prs3d_PointAspect(Aspect_TOM_O, color(endpointColor), 4);
      group->SetGroupPrimitivesAspect(style->Aspect());
      Handle(Graphic3d_ArrayOfPoints) points = new Graphic3d_ArrayOfPoints(static_cast<int>(endpoints.size()));
      for (const auto& p : endpoints) points->AddVertex(p);
      group->AddPrimitiveArray(points);
    }
    if (!snapPoints.empty()) {
      auto group = prs->NewGroup();
      Handle(Prs3d_PointAspect) style = new Prs3d_PointAspect(Aspect_TOM_RING2, color(snapPointColor), 5);
      group->SetGroupPrimitivesAspect(style->Aspect());
      Handle(Graphic3d_ArrayOfPoints) points = new Graphic3d_ArrayOfPoints(static_cast<int>(snapPoints.size()));
      for (const auto& p : snapPoints) points->AddVertex(p);
      group->AddPrimitiveArray(points);
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}
};
}  // namespace

void Viewport::clearDimension() {
  m_measurement = opad::json();
  m_measureAnchors.clear();
  m_measureSelectionLocked = false;
  unsetCursor();
  setToolTip(QString());
  m_measureCamera.Reset();
  if (!m_initialised) return;
  for (const auto& o : m_dimension) m_ctx->Remove(o, Standard_False);
  m_dimension.clear();
  m_measureCaptions.clear();
  refreshMeasurement(true);
  redrawScene();
}

void Viewport::showMeasurement(const opad::json& result) {
  const bool sameMeasurement = !m_measurement.is_null() && m_measurement.value("refs", opad::json::array()) == result.value("refs", opad::json::array());
  m_measurement = result;
  m_measureAnchors.clear();
  if (result.contains("anchors")) {
    for (const auto& option : result["anchors"]) {
      for (int side = 0; side < 2; ++side) {
        const opad::Vec3 candidate = option[side == 0 ? "point_a" : "point_b"].get<opad::Vec3>();
        const gp_Pnt p(candidate[0], candidate[1], candidate[2]);
        if (std::none_of(m_measureAnchors.begin(), m_measureAnchors.end(), [&](const MeasurementAnchor& other) {
              return other.side == side && samePoint(p, gp_Pnt(other.point[0], other.point[1], other.point[2]));
            })) m_measureAnchors.push_back({side, candidate});
      }
    }
  }
  // New diagonal measurements show the distance and its components together.
  // An aligned measurement always uses its one axis arrow instead.
  if (!sameMeasurement) m_measureComponents = true;
  // Finished markers belong at the exact extrema, rather than the approximate mouse hits.
  showPickMarkers({});
  clearPreview();
  refreshMeasurement(true);
  redrawScene();
}

void Viewport::setMeasurementComponents(bool on) {
  m_measureComponents = on;
  refreshMeasurement(true);
  redrawScene();
}

bool Viewport::measurementHasMultipleAxes() const {
  return !m_measurement.is_null() && m_measurement.value("kind", "distance") == "distance"
      && m_measurement.contains("point_a") && m_measurement.contains("point_b")
      && componentCount(point(m_measurement["point_a"]), point(m_measurement["point_b"])) > 1;
}

int Viewport::measurementAnchorAt(const QPointF& position) const {
  if (!m_initialised || !m_measureSelectionLocked) return -1;
  int nearest = -1;
  double best = 12.0 * 12.0;  // logical pixels: stable at every zoom and display scale
  for (size_t i = 0; i < m_measureAnchors.size(); ++i) {
    const auto& candidate = m_measureAnchors[i];
    const QPointF at = widgetPoint(candidate.point);
    const QPointF delta = at - position;
    double score = QPointF::dotProduct(delta, delta);
    // At a shared projected position, prefer an anchor which would change an endpoint.
    const auto& selected = m_measurement[candidate.side == 0 ? "point_a" : "point_b"];
    if (samePoint(point(selected), gp_Pnt(candidate.point[0], candidate.point[1], candidate.point[2]))) score += 0.25;
    if (score < best) { best = score; nearest = static_cast<int>(i); }
  }
  return nearest;
}

void Viewport::refreshMeasurement(bool force) {
  if (!m_initialised) return;
  const auto& camera = m_view->Camera();
  const auto state = camera->WorldViewProjState();
  const QSize pixels(qRound(width() * devicePixelRatioF()), qRound(height() * devicePixelRatioF()));
  if (!force && m_measureCamera == state && m_measureSize == pixels) return;
  m_measureCamera = state;
  m_measureSize = pixels;
  for (const auto& o : m_dimension) m_ctx->Remove(o, Standard_False);
  m_dimension.clear();

  Handle(Graphic3d_SequenceOfHClipPlane) noClip = new Graphic3d_SequenceOfHClipPlane();
  noClip->SetOverrideGlobal(Standard_True);
  auto display = [&](const Handle(AIS_InteractiveObject)& object) {
    object->SetZLayer(Graphic3d_ZLayerId_TopOSD);
    object->SetInfiniteState(Standard_True);  // annotations must never change Fit All
    object->SetClipPlanes(noClip);
    m_ctx->Display(object, 0, -1, Standard_False);
    m_dimension.push_back(object);
  };
  std::vector<opad::json> results;
  for (const auto& saved : m_doc->scene.measurements)
    if (!saved.unresolved && saved.result != m_measurement) results.push_back(saved.result);
  if (!m_measurement.is_null()) results.push_back(m_measurement);
  for (const auto& r : results) {
  Handle(InspectGraphic) graphic = new InspectGraphic();
  graphic->endpointColor = m_tokens.fg;
  graphic->snapPointColor = m_tokens.hov;
  const gp_Vec up(camera->OrthogonalizedUp());
  const gp_Vec right = gp_Vec(camera->Direction()).Crossed(up);
  // Perspective scale is evaluated at the annotation, not at the camera target.
  auto pixelAt = [&](const gp_Pnt& p) {
    double px = pixelSize();
    if (!camera->IsOrthographic()) {
      const double depth = gp_Vec(camera->Eye(), p).Dot(gp_Vec(camera->Direction()));
      px *= std::max(depth, camera->Distance() * 0.01) / camera->Distance();
    }
    return std::max(px, 1e-12);
  };
  auto arrow = [&](const gp_Pnt& a, const gp_Pnt& b, const QColor& c, bool twoHeads = false) {
    graphic->arrows.push_back({a, b, c, twoHeads, pixelAt(gp_Pnt((a.X()+b.X())/2, (a.Y()+b.Y())/2, (a.Z()+b.Z())/2))});
  };
  std::vector<QRectF> occupied;
  std::vector<Handle(AIS_TextLabel)> labels;
  auto screenNormal = [&](const gp_Pnt& a, const gp_Pnt& b) {
    const QPoint pa = widgetPoint({a.X(), a.Y(), a.Z()}), pb = widgetPoint({b.X(), b.Y(), b.Z()});
    QPointF normal(-(pb.y() - pa.y()), pb.x() - pa.x());
    const double length = std::hypot(normal.x(), normal.y());
    if (length < 1) return QPointF(0, -1);
    normal /= length;
    if (normal.y() > 0) normal = -normal;
    return normal;
  };
  auto label = [&](const gp_Pnt& anchor, const QString& caption, const QColor& c, double dx, double dy) {
    const QPoint screen = widgetPoint({anchor.X(), anchor.Y(), anchor.Z()});
    const QFontMetricsF metrics(theme::ui(13));
    const double w = metrics.horizontalAdvance(caption) + 18, h = 25;
    QRectF box(screen.x() + dx - w / 2, screen.y() - dy - h / 2, w, h);
    box.moveLeft(std::clamp(box.left(), 8.0, std::max(8.0, width() - w - 8)));
    box.moveTop(std::clamp(box.top(), 8.0, std::max(8.0, height() - h - 8)));
    // Stack only colliding labels; keeps zeros and end-on axes legible in standard views.
    const QRectF preferred = box;
    for (int attempt = 0; attempt < 24; ++attempt) {
      if (std::none_of(occupied.begin(), occupied.end(), [&](const QRectF& other) { return other.intersects(box); })) break;
      const int step = attempt / 2 + 1;
      box = preferred.translated(0, (attempt % 2 ? step : -step) * 28);
      box.moveTop(std::clamp(box.top(), 8.0, std::max(8.0, height() - h - 8)));
    }
    occupied.push_back(box);
    const double px = pixelAt(anchor);
    gp_Pnt at = anchor.Translated(right * ((box.center().x() - screen.x()) * px) + up * ((screen.y() - box.center().y()) * px));
    if (anchor.Distance(at) > 22 * px) graphic->lines.push_back({anchor, at, m_tokens.fg3});
    m_measureCaptions << caption;
    Handle(AIS_TextLabel) text = new AIS_TextLabel();
    text->SetText(TCollection_ExtendedString((" " + caption + " ").toUtf8().constData(), Standard_True));
    text->SetPosition(at);
    text->SetHeight(13 * viewScale().x());
    text->SetFont(theme::ui().family().toUtf8().constData());
    text->SetHJustification(Graphic3d_HTA_CENTER);
    text->SetVJustification(Graphic3d_VTA_CENTER);
    const bool endpoint = caption == tr("1") || caption == tr("2") || caption == tr("1 = 2");
    text->SetColor(color(endpoint ? m_tokens.onsel : c));
    text->SetDisplayType(Aspect_TODT_SUBTITLE);
    text->SetColorSubTitle(color(endpoint ? m_tokens.sel : m_tokens.bg2));
    labels.push_back(text);
  };
  auto beside = [&](const gp_Pnt& anchor, const QString& caption, const QColor& c, const QPointF& normal, double gap) {
    const double halfWidth = (QFontMetricsF(theme::ui(13)).horizontalAdvance(caption) + 18) / 2;
    const double distance = gap + halfWidth * std::abs(normal.x()) + 12.5 * std::abs(normal.y());
    label(anchor, caption, c, normal.x() * distance, -normal.y() * distance);
  };
  const QColor axes[] = {m_tokens.red, m_tokens.green, m_tokens.dark ? QColor("#76b5ff") : QColor("#2067be")};
  const std::string kind = r.value("kind", "distance");
  if (kind == "distance" || kind == "radius") {
    if (!r.contains("point_a") || !r.contains("point_b")) continue;
    const gp_Pnt a = point(r["point_a"]), b = point(r["point_b"]);
    const int components = componentCount(a, b);
    const bool aligned = kind == "distance" && components == 1;
    graphic->endpoints = {a, b};
    if (r == m_measurement) for (const auto& anchor : m_measureAnchors) {
      const gp_Pnt candidate(anchor.point[0], anchor.point[1], anchor.point[2]);
      if (samePoint(candidate, a) || samePoint(candidate, b)) continue;
      if (std::none_of(graphic->snapPoints.begin(), graphic->snapPoints.end(), [&](const gp_Pnt& p) { return samePoint(p, candidate); }))
        graphic->snapPoints.push_back(candidate);
    }
    if (!aligned) {
      // Both tips terminate at the measured points; only the label is offset.
      QPointF normal = screenNormal(a, b);
      auto clearance = [&](const QPointF& direction) {
        double space = 1e20;
        for (const auto& p : {a, b}) {
          const QPointF at = QPointF(widgetPoint({p.X(), p.Y(), p.Z()})) + direction * 24;
          space = std::min({space, at.x(), at.y(), width() - at.x(), height() - at.y()});
        }
        return space;
      };
      if (clearance(normal) < 8 && clearance(-normal) > clearance(normal)) normal = -normal;
      arrow(a, b, m_tokens.fg, kind == "distance");
      beside(gp_Pnt((a.X()+b.X())/2, (a.Y()+b.Y())/2, (a.Z()+b.Z())/2),
            (kind == "distance" ? tr("Distance %1") : tr("R %1")).arg(units::format(units::Kind::Length, r["value"].get<double>())), m_tokens.fg, normal, kind == "distance" ? 36 : 16);
    }
    label(a, kind == "distance" ? (a.Distance(b) < 1e-9 ? tr("1 = 2") : tr("1")) : tr("Center"), m_tokens.fg2, -16, -19);
    if (a.Distance(b) > 1e-9) label(b, kind == "distance" ? tr("2") : tr("Radius"), m_tokens.fg2, 16, -19);
    if (kind == "distance" && (aligned || (components > 1 && m_measureComponents))) {
      gp_Pnt start = a;
      for (int i = 0; i < 3; ++i) {
        gp_Pnt end = start;
        end.SetCoord(i + 1, b.Coord(i + 1));
        const double delta = b.Coord(i + 1) - a.Coord(i + 1);
        // Zero components stay in the result table, without extra viewport labels.
        if (std::abs(delta) <= Precision::Confusion()) { start = end; continue; }
        arrow(start, end, axes[i]);
        const QString value = (delta > 0 && units::number(units::Kind::Length, delta) != units::number(units::Kind::Length, 0) ? "+" : "") + units::format(units::Kind::Length, delta);
        const QPointF normal = screenNormal(start, end);
        beside(gp_Pnt((start.X()+end.X())/2, (start.Y()+end.Y())/2, (start.Z()+end.Z())/2),
              QString("Δ%1 %2").arg(QChar("XYZ"[i])).arg(value), axes[i], -normal, 12);
        start = end;
      }
    }
  } else if (kind == "bbox") {
    const gp_Pnt lo = point(r["min"]), hi = point(r["max"]);
    for (int mask = 0; mask < 8; ++mask) {
      gp_Pnt a((mask & 1) ? hi.X() : lo.X(), (mask & 2) ? hi.Y() : lo.Y(), (mask & 4) ? hi.Z() : lo.Z());
      for (int i = 0; i < 3; ++i) if (!(mask & (1 << i))) {
        gp_Pnt b = a; b.SetCoord(i + 1, hi.Coord(i + 1));
        graphic->lines.push_back({a, b, m_tokens.fg3});
      }
    }
    for (int i = 0; i < 3; ++i) {
      gp_Pnt end = lo; end.SetCoord(i + 1, hi.Coord(i + 1));
      arrow(lo, end, axes[i], true);
      const QPointF normal = screenNormal(lo, end);
      beside(gp_Pnt((lo.X()+end.X())/2, (lo.Y()+end.Y())/2, (lo.Z()+end.Z())/2),
            QString("%1 %2").arg(QChar("XYZ"[i])).arg(units::format(units::Kind::Length, hi.Coord(i+1)-lo.Coord(i+1))), axes[i], -normal, 12);
    }
  } else if (kind == "angle" && r.contains("origin")) {
    // With a construction from the core the diagram sits on the objects: rays from the vertex where they meet,
    // along each of them, and the arc through the nearer one. Otherwise (parallel, or a line against a normal;
    // results pinned before the construction existed) the two directions are compared at the first object.
    const bool built = r.contains("vertex");
    const gp_Pnt origin = point(r[built ? "vertex" : "origin"]);
    const gp_Vec a(point(r[built ? "ray_a" : "direction_a"]).XYZ()), b(point(r[built ? "ray_b" : "direction_b"]).XYZ());
    const double angle = a.Angle(b);
    double radius = 70 * pixelAt(origin);
    if (built) {
      const double reachA = r["reach_a"].get<double>(), reachB = r["reach_b"].get<double>(), glyph = radius;
      if (std::min(reachA, reachB) > 40 * pixelAt(origin)) radius = std::min(reachA, reachB);
      // Solid as far as the arc, dashed on to the object itself.
      auto ray = [&](const gp_Pnt& from, const gp_Vec& dir, double reach, const QColor& c) {
        const double solid = std::max(radius * 1.15, glyph * 0.5);
        graphic->lines.push_back({from, from.Translated(dir * solid), c, false});
        if (reach > solid) graphic->lines.push_back({from.Translated(dir * solid), from.Translated(dir * reach), c});
      };
      ray(origin, a, reachA, m_tokens.hov);
      const gp_Pnt originB = point(r["vertex_b"]);
      if (!samePoint(origin, originB)) graphic->lines.push_back({origin, originB, m_tokens.fg3});  // skew lines: b is compared moved over
      // The angle may be against b's extension beyond the vertex; the dashes still lead to b itself.
      const bool direct = samePoint(origin, originB) && !r.value("ray_b_extended", false);
      ray(origin, b, direct ? reachB : 0, m_tokens.amber);
      if (!direct && reachB > 0) graphic->lines.push_back({originB, point(r["point_b"]), m_tokens.amber});
    } else {
      arrow(origin, origin.Translated(a * radius * 1.3), m_tokens.hov);
      arrow(origin, origin.Translated(b * radius * 1.3), m_tokens.amber);
      if (r.contains("point_b")) graphic->lines.push_back({point(r["point_b"]), origin, m_tokens.fg3});
    }
    gp_Vec tangent = b - a * a.Dot(b);
    if (tangent.SquareMagnitude() < 1e-12) {
      tangent = up - a * a.Dot(up);
      if (tangent.SquareMagnitude() < 1e-12) tangent = right - a * a.Dot(right);
    }
    tangent.Normalize();
    gp_Pnt last = origin.Translated(a * radius);
    for (int i = 1; i <= 40; ++i) {
      const double t = angle * i / 40;
      gp_Pnt next = origin.Translated((a * std::cos(t) + tangent * std::sin(t)) * radius);
      graphic->lines.push_back({last, next, m_tokens.hov, false});
      last = next;
    }
    if (angle > 1e-6) {
      const double before = std::max(0.0, angle - 0.3);
      arrow(origin.Translated((a * std::cos(before) + tangent * std::sin(before)) * radius), last, m_tokens.hov);
    }
    label(origin.Translated((a * std::cos(angle / 2) + tangent * std::sin(angle / 2)) * radius),
          units::format(units::Kind::Angle, r["value"].get<double>()), m_tokens.fg, 0, 24);
    label(built ? point(r["point_a"]) : origin.Translated(a * radius * 1.3), tr("1"), m_tokens.sel, -15, -18);
    label(built ? point(r["point_b"]) : origin.Translated(b * radius * 1.3), tr("2"), m_tokens.amber, 15, -18);
  }
  display(graphic);
  for (const auto& text : labels) display(text);
  }
  m_view->Invalidate();
}
