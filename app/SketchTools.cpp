#include "opad/design/sketch_edit.hpp"
#include "opad/design/sketch_pattern.hpp"
#include "opad/design/sketch_modify.hpp"
// SketchEditor, the tools: what a click means for each of them, constraints, dimensions, fillet, trim, mirror.
#include "CurveSamples.hpp"
#include "SketchEditor.hpp"
#include "SketchGeometryCache.hpp"
#include "SketchPanel.hpp"
#include "DimensionHandle.hpp"
#include "ShapeInput.hpp"
#include "SketchSteps.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <QInputDialog>
#include <QKeyEvent>
#include <QSettings>
#include <QApplication>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

#include "I18n.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/sketch_geom.hpp"

using namespace opad::design;
using CT = SkConstraint::Type;
using ET = SkEntity::Type;

namespace {

// A value with nothing to evaluate: "12", "12.5 mm", "30 deg". Only anything else is kept as an expression (shown with
// "fx:"); "50 mm", the form the fields suggest, used to be.
bool plainValue(const QString& text) {
  static const QRegularExpression number(QStringLiteral(R"(^\s*[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?\s*(mm|cm|m|um|in|ft|deg|rad|°)?\s*$)"));
  return number.match(text).hasMatch();
}

ParamTable paramTable(const opad::Scene& scene) {
  std::vector<ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return ParamTable(defs,scene.units);
}

double norm_angle(double a) {  // into [0, 2 pi)
  a = std::fmod(a, 2 * M_PI);
  return a < 0 ? a + 2 * M_PI : a;
}

}  // namespace

// ---------------------------------------------------------------- tool selection
void SketchEditor::setTool(const QString& tool) {
  // The offset's arrow and value box go with the tool at once, also when a running job makes this call return early.
  if(tool!="offset")m_dimensionHandle->hide();
  if(m_editJob && m_previewComputing){invalidatePreview();m_editJob->cancel();m_editJob=nullptr;m_previewComputing=false;}
  if (!m_active || m_editJob) return;
  invalidatePreview();
  if (!m_chain.empty()) finishChain();
  cancel_change();
  m_tracked.clear();m_dwellPoint=0;m_dwellTimer.stop();unlock();m_pointer.onLine=false;m_snapChoice=0;  // tracking points, their guides and the lock are the tool's
  m_clicks.clear();
  m_chain.clear();
  m_picked.clear();
  m_sources.clear();
  m_placingDim = false;
  // A constraint button with a fitting selection acts at once, the way Fusion's constraint palette does.
  if (tool.startsWith("c:") && !m_sel.empty()) {
    const CT type = SkConstraint::type_from_name(tool.mid(2).toStdString());
    const std::vector<int> ids = m_sel;
    if (applyConstraint(type, ids, true)) {
      m_sel.clear();
      rebuild();
      return;
    }
  }

  m_dimEditing = 0;
  m_panelFieldsDirty = true;
  m_tool = tool;referenceHover();
  if(tool=="mirror")m_options["mirrorStage"]=m_sel.empty()?"seed":"axis";
  const QStringList preserve={"mirror","offset","node","move","rotate","scale","copy","rect_pattern","polar_pattern","explode","chamfer","break","break_link","copybase"};
  if(tool!="paste")m_clip.reset();
  if(!preserve.contains(tool))m_sel.clear();
  if((tool=="rect_pattern"||tool=="polar_pattern")&&!m_sel.empty()) {
    const int id=pattern_of(m_sk,m_sel.front(),true);
    for(const auto& p:m_sk.patterns)if(p.at("id").get<int>()==id)for(const auto& [key,value]:p.at("inputs").items())if(key!="polar")m_options[QString::fromStdString(key)]=QString::fromStdString(value.is_string()?value.get<std::string>():value.dump());
  }
  emit toolChanged(m_tool);
  toolPrompt();
  rebuild();
  if (placing()) resnap();  // where the pointer is, snapped for this tool: the drawing cursor jumps there at once
  scheduleToolPreview();
}

// The step that waits, as the prompt bar and the tool panel list it (SketchSteps.hpp, UI-25), and the tool's note.
void SketchEditor::toolPrompt() {
  emit status(prompt(true));
  emit workflowChanged();
  updateInput();  // the boxes of the step that waits now
}

QString SketchEditor::prompt(bool note) const {
  const auto* entry = sketchsteps::find(m_tool.toStdString());
  if (!entry) return {};
  const QList<ToolStep> steps = toolSteps();
  int waiting = 0;
  while (waiting + 1 < steps.size() && !steps[waiting].picked.isEmpty()) ++waiting;
  QString t = tr("%1: %2").arg(i18n::t(entry->name), steps[waiting].label);
  if (note && *entry->note) t += QStringLiteral(" · ") + i18n::t(entry->note);
  return t;
}

// "close" on the command line: the polyline's last segment back to its first point, which ends it.
bool SketchEditor::closeChain() {
  if (!m_active || m_editJob || m_tool != "line" || m_chain.size() < 3) return false;
  const SkPoint* first = m_sk.point(m_chain.front());
  if (!first) return false;
  Snap s;
  s.u = first->x;
  s.v = first->y;
  s.point = first->id;
  click(s, Qt::AltModifier);
  return true;
}

// ---------------------------------------------------------------- clicks
void SketchEditor::finishChain() {
  if (m_tool == "spline" && m_chain.size() >= 2) {
    begin_change();
    add_cubic_spline(m_sk,m_chain);
    end_change(tr("Spline"));
  } else if (m_chain.size() == 1 && m_undo.size() > m_chainUndoStart) {
    // A start point that never got its segment: back to before that click. (Removing the point by id deleted an
    // existing point the chain had started on, a rectangle corner with its sides.)
    m_sk = m_undo[m_chainUndoStart].geometry;
    m_undo.erase(m_undo.begin() + static_cast<std::ptrdiff_t>(m_chainUndoStart), m_undo.end());
    analyseSketch();
    scheduleFill();
  }
  if(!m_chain.empty() && m_undo.size()>m_chainUndoStart+1)m_undo.erase(m_undo.begin()+m_chainUndoStart+1,m_undo.end());
  m_chain.clear();
  m_clicks.clear();
  toolPrompt();
  rebuild();
}

void SketchEditor::click(const Snap& s, Qt::KeyboardModifiers) {
  invalidatePreview();
  unlock();  // a lock lasts until the point it placed
  m_snapChoice = 0;  // the next point starts from the nearest snap
  if(clipClick(s))return;
  if(imageClick(s.u,s.v))return;
  if(m_tool=="project"||m_tool=="intersect_body"||m_tool=="silhouette"||m_tool=="include3d")return pickReference();
  if(modifyClick(s.u,s.v))return;
  if(primitiveClick(s))return;
  const Hit hit = hitTest(s.u, s.v);
  if (m_tool.startsWith("c:")) return constraintClick(hit);
  if (m_tool == "dimension") return dimensionClick(hit, s.u, s.v);
  if (m_tool == "offset" || m_tool == "node") {
    const Hit h=hitTest(s.u,s.v);
    if(h.kind!=Hit::None) {
      if(h.kind==Hit::Entity)pickCurve(h.id);  // the offset: its connected chain (the tool's option, on by default)
      else if(auto it=std::find(m_sel.begin(),m_sel.end(),h.id);it==m_sel.end())m_sel.push_back(h.id);
      else m_sel.erase(it);
      invalidatePreview();
      rebuild();emit changed();toolPrompt();
      scheduleToolPreview();
    }
    return;
  }
  if (m_tool == "fillet") return filletAt(hit, s.u, s.v);
  if (m_tool == "trim") return trimAt(hit, s.u, s.v);
  if (m_tool == "mirror") {
    const SkEntity* e = hit.kind == Hit::Entity ? m_sk.entity(hit.id) : nullptr;
    if (!e || e->type != ET::Line) return emit status(tr("Mirror: click a line to mirror about"));
    return mirrorSelection(hit.id);
  }

  if (m_tool == "line" || m_tool == "spline") {
    if(m_chain.empty())m_chainUndoStart=m_undo.size();
    begin_change();
    const int p = pointFor(s);
    if (m_chain.empty()) {
      m_chain.push_back(p);
      end_change(tr("Point"));
      return toolPrompt();
    }
    if (p == m_chain.back()) return cancel_change();
    if (m_tool == "spline") {
      m_chain.push_back(p);
      if (end_change(tr("Point")) && m_chain.size() == 2) toolPrompt();
      return;
    }
    const int line = m_sk.add_line(m_chain.back(), p);
    if (s.horizontal) m_sk.add_constraint(CT::Horizontal, {line});
    if (s.vertical) m_sk.add_constraint(CT::Vertical, {line});
    for (const auto& h : s.segment)  // square to a line or a circle (through its centre), touching a circle or an arc (UI-23)
      if (m_sk.point(h.ref) || m_sk.entity(h.ref)) m_sk.add_constraint(h.type, h.type == CT::Coincident ? std::vector<int>{h.ref, line} : std::vector<int>{line, h.ref});
    if (m_tool == "line") {  // a typed length and angle hold the line (the angle along an axis, or against the line before)
      keepTyped(s, "length", CT::Distance, {line});
      int previous = 0;
      if (m_chain.size() >= 2)
        for (const auto& e : m_sk.entities)
          if (e.type == ET::Line && e.p.size() == 2 && e.p[0] == m_chain[m_chain.size() - 2] && e.p[1] == m_chain.back()) previous = e.id;
      const SkPoint *from = m_sk.point(m_chain.back()), *to = m_sk.point(p);
      keepDirection(s, "angle", {line}, std::atan2(to->y - from->y, to->x - from->x), line, previous);
      if (const auto it = s.typed.find("dx"); it != s.typed.end() && std::fabs(it->second.first) < 1e-12 && !s.vertical) keepDirection(s, "dx", {line}, M_PI / 2);
      if (const auto it = s.typed.find("dy"); it != s.typed.end() && std::fabs(it->second.first) < 1e-12 && !s.horizontal) keepDirection(s, "dy", {line}, 0);
      for (const char* key : {"dx", "dy"})  // a typed ΔX (ΔY) that is not 0: the signed horizontal (vertical) distance from the last point
        if (const auto it = s.typed.find(key); it != s.typed.end() && std::fabs(it->second.first) >= 1e-12)
          if (SkConstraint* c = m_sk.constraint(keepTyped(s, key, key[1] == 'x' ? CT::HDistance : CT::VDistance, {m_chain.back(), p}))) {
            c->is_signed = true;
            c->value = it->second.first;
            const SkPoint *a = m_sk.point(m_chain.back()), *b = m_sk.point(p);
            const double out = 24 * m_viewport->pixelSize();  // ΔX under the segment, ΔY right of it
            if (key[1] == 'x') labelAt(c->id, (a->x + b->x) / 2, std::min(a->y, b->y) - out);
            else labelAt(c->id, std::max(a->x, b->x) + out, (a->y + b->y) / 2);
          }
    }
    const bool closes = p == m_chain.front() && m_chain.size() > 1;
    if (!end_change(tr("Line"))) return;
    m_chain.push_back(p);
    if (closes) finishChain();  // back at the start: the profile is closed, the chain is done
    else if (m_chain.size() == 2) toolPrompt();  // more points, or Enter ends it
    return;
  }

  m_clicks.push_back(s);
  const size_t n = m_clicks.size();
  auto done = [&](const QString& what) {
    const bool accepted=end_change(what);
    m_clicks.clear();
    if(accepted)toolPrompt();
  };
  int sides[2] = {0, 0};  // a rectangle's bottom (top) and side: its typed width and height
  // Corners (x0, y0), (x1, y0), (x1, y1), (x0, y1); a clicked one is the click's point (with what it snapped to, UI-21).
  auto rectangle = [&](double x0, double y0, double x1, double y1, std::array<const Snap*, 4> at) {
    auto corner = [&](size_t i, double x, double y) { return at[i] ? pointFor(*at[i]) : m_sk.add_point(x, y); };
    const int a = corner(0, x0, y0), b = corner(1, x1, y0), c = corner(2, x1, y1), d = corner(3, x0, y1);
    const int l0 = m_sk.add_line(a, b), l1 = m_sk.add_line(b, c), l2 = m_sk.add_line(c, d), l3 = m_sk.add_line(d, a);
    sides[0] = l0;
    sides[1] = l1;
    m_sk.add_constraint(CT::Horizontal, {l0});
    m_sk.add_constraint(CT::Horizontal, {l2});
    m_sk.add_constraint(CT::Vertical, {l1});
    m_sk.add_constraint(CT::Vertical, {l3});
    return std::array<int, 4>{a, b, c, d};
  };

  if (m_tool == "point") {
    begin_change();
    SkEntity e;
    e.type = ET::Point;
    e.p = {pointFor(s)};
    e.id = m_sk.next_id();
    m_sk.entities.push_back(e);
    return done(tr("Point"));
  }
  if (m_tool == "rect" && n == 2) {
    const Snap &a = m_clicks[0], &c = m_clicks[1];
    if (std::fabs(a.u - c.u) < 1e-6 || std::fabs(a.v - c.v) < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    rectangle(a.u, a.v, c.u, c.v, {&a, nullptr, &c, nullptr});
    const double mu = (a.u + c.u) / 2, mv = (a.v + c.v) / 2, out = 24 * m_viewport->pixelSize();
    // Its width and height typed, or the corner typed as ΔX and ΔY from the first ('@'): its sides.
    const int width = keepTyped(c, "width", CT::Distance, {sides[0]}), height = keepTyped(c, "height", CT::Distance, {sides[1]});
    labelOff(width ? width : keepTyped(c, "dx", CT::Distance, {sides[0]}), sides[0], mu, mv, out);
    labelOff(height ? height : keepTyped(c, "dy", CT::Distance, {sides[1]}), sides[1], mu, mv, out);
    return done(tr("Rectangle"));
  }
  if (m_tool == "crect" && n == 2) {
    const Snap &o = m_clicks[0], &c = m_clicks[1];
    const double w = std::fabs(c.u - o.u), h = std::fabs(c.v - o.v);
    if (w < 1e-6 || h < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    std::array<const Snap*, 4> at{};  // the clicked corner: the one on its side of the centre
    at[c.u > o.u ? (c.v > o.v ? 2 : 1) : (c.v > o.v ? 3 : 0)] = &c;
    const auto corners = rectangle(o.u - w, o.v - h, o.u + w, o.v + h, at);
    const int centre = pointFor(o);
    const int diagonal = m_sk.add_line(corners[0], corners[2], true);
    m_sk.add_constraint(CT::Midpoint, {centre, diagonal});
    const int width = keepTyped(c, "width", CT::Distance, {sides[0]}), height = keepTyped(c, "height", CT::Distance, {sides[1]});
    labelOff(width ? width : keepTyped(c, "dx", CT::Distance, {sides[0]}, 2), sides[0], o.u, o.v, 24 * m_viewport->pixelSize());  // ΔX from the centre: half of it
    labelOff(height ? height : keepTyped(c, "dy", CT::Distance, {sides[1]}, 2), sides[1], o.u, o.v, 24 * m_viewport->pixelSize());
    return done(tr("Rectangle"));
  }
  if (m_tool == "circle" && n == 2) {
    const double r = std::hypot(s.u - m_clicks[0].u, s.v - m_clicks[0].v);
    if (r < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    const int circle = m_sk.add_circle(pointFor(m_clicks[0]), r);
    if (s.point) m_sk.add_constraint(CT::Coincident, {s.point, circle});
    keepTyped(s, "diameter", CT::Diameter, {circle});
    keepTyped(s, "radius", CT::Radius, {circle});
    return done(tr("Circle"));
  }
  if ((m_tool == "circle3" || m_tool == "arc3") && n == 3) {
    // Circle through three points.
    const double ax = m_clicks[0].u, ay = m_clicks[0].v, bx = m_clicks[1].u, by = m_clicks[1].v, cx = m_clicks[2].u, cy = m_clicks[2].v;
    const double dd = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::fabs(dd) < 1e-9) { m_clicks.pop_back(); return emit status(tr("Those three points are on one line")); }
    const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / dd;
    const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / dd;
    begin_change();
    const int centre = m_sk.add_point(ux, uy);
    if (m_tool == "circle3") {
      const int circle = m_sk.add_circle(centre, std::hypot(ax - ux, ay - uy));
      for (const Snap& k : m_clicks)
        if (k.point) m_sk.add_constraint(CT::Coincident, {k.point, circle});
      keepTyped(s, "radius", CT::Radius, {circle});
      return done(tr("Circle"));
    }
    // Start and end were clicked first; the third point says which way round the arc goes.
    const double a0 = std::atan2(ay - uy, ax - ux), a1 = std::atan2(by - uy, bx - ux), am = std::atan2(cy - uy, cx - ux);
    const bool ccw = norm_angle(am - a0) < norm_angle(a1 - a0);
    const int ps = pointFor(m_clicks[0]), pe = pointFor(m_clicks[1]);
    const int arc = m_sk.add_arc(centre, ccw ? ps : pe, ccw ? pe : ps);
    keepTyped(s, "radius", CT::Radius, {arc});
    if (s.point && s.point != ps && s.point != pe) m_sk.add_constraint(CT::Coincident, {s.point, arc});  // through a point it snapped to
    keepAligned(m_clicks[1], {ps, pe});
    keepTyped(m_clicks[1], "length", CT::Distance, {ps, pe});  // the chord
    keepDirection(m_clicks[1], "angle", {ps, pe}, std::atan2(by - ay, bx - ax));
    return done(tr("Arc"));
  }
  if (m_tool == "arcc" && n == 3) {
    const Snap &c = m_clicks[0], &a = m_clicks[1];
    const double r = std::hypot(a.u - c.u, a.v - c.v), de = std::hypot(s.u - c.u, s.v - c.v);
    if (r < 1e-6 || de < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    const int centre = pointFor(c), ps = pointFor(a);
    const int pe = s.point ? s.point : m_sk.add_point(c.u + (s.u - c.u) * r / de, c.v + (s.v - c.v) * r / de);
    double sweep = std::atan2(s.v - c.v, s.u - c.u) - std::atan2(a.v - c.v, a.u - c.u);
    while (sweep > M_PI) sweep -= 2 * M_PI;
    while (sweep <= -M_PI) sweep += 2 * M_PI;
    if (const auto typed = s.typed.find("sweep"); typed != s.typed.end()) sweep = typed->second.first;  // past half a turn too
    const int arc = m_sk.add_arc(centre, sweep > 0 ? ps : pe, sweep > 0 ? pe : ps);
    const int radius = keepTyped(a, "radius", CT::Radius, {arc});
    keepDirection(a, "angle", {centre, ps}, std::atan2(a.v - c.v, a.u - c.u));  // the start along an axis
    keepAligned(a, {centre, ps});
    if (pe != centre) keepAligned(s, {centre, pe});
    // The sweep typed with the radius: held as the arc's length, that radius times the sweep (a radius edited later keeps the
    // sweep, past half a turn too); without it, its end along an axis.
    if (!keepSweep(s, arc, radius, r)) keepDirection(s, "sweep", {centre, pe}, std::atan2(m_sk.point(pe)->y - c.v, m_sk.point(pe)->x - c.u));
    return done(tr("Arc"));
  }
  if (m_tool == "polygon" && n == 2) {
    bool valid=false;const int sides=option("sides","6").toInt(&valid);
    if(!valid || sides<3 || sides>256) {m_clicks.pop_back();emit status(tr("Use between 3 and 256 polygon sides."));return;}
    m_polygonSides=sides;
    const Snap& o = m_clicks[0];
    const double r = std::hypot(s.u - o.u, s.v - o.v), a0 = std::atan2(s.v - o.v, s.u - o.u);
    if (r < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    const int centre = pointFor(o);
    const int guide = m_sk.add_circle(centre, r, true);
    std::vector<int> pts, lines;
    for (int i = 0; i < m_polygonSides; ++i) {
      const double a = a0 + 2 * M_PI * i / m_polygonSides;
      pts.push_back(i == 0 ? pointFor(s) : m_sk.add_point(o.u + r * std::cos(a), o.v + r * std::sin(a)));
      if (!(i == 0 && s.point)) m_sk.add_constraint(CT::Coincident, {pts.back(), guide});
    }
    for (int i = 0; i < m_polygonSides; ++i) lines.push_back(m_sk.add_line(pts[static_cast<size_t>(i)], pts[static_cast<size_t>((i + 1) % m_polygonSides)]));
    for (size_t i = 1; i < lines.size(); ++i) m_sk.add_constraint(CT::Equal, {lines[0], lines[i]});
    keepTyped(s, "diameter", CT::Diameter, {guide});
    keepDirection(s, "angle", {centre, pts[0]}, a0);
    keepAligned(s, {centre, pts[0]});
    return done(tr("Polygon"));
  }
  if (m_tool == "slot" && n == 3) {
    const Snap &c1 = m_clicks[0], &c2 = m_clicks[1];
    const double dx = c2.u - c1.u, dy = c2.v - c1.v, len = std::hypot(dx, dy);
    if (len < 1e-6) { m_clicks.clear(); return toolPrompt(); }
    const double nx = -dy / len, ny = dx / len;
    const double r = std::fabs((s.u - c1.u) * nx + (s.v - c1.v) * ny);
    if (r < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    const int p1 = pointFor(c1), p2 = pointFor(c2);
    const int a1 = m_sk.add_point(c1.u + nx * r, c1.v + ny * r), b1 = m_sk.add_point(c1.u - nx * r, c1.v - ny * r);
    const int a2 = m_sk.add_point(c2.u + nx * r, c2.v + ny * r), b2 = m_sk.add_point(c2.u - nx * r, c2.v - ny * r);
    const int top = m_sk.add_line(a1, a2), bottom = m_sk.add_line(b2, b1);
    const int cap2 = m_sk.add_arc(p2, b2, a2), cap1 = m_sk.add_arc(p1, a1, b1);  // counter-clockwise round each end
    const int centres = m_sk.add_line(p1, p2, true);
    for (int line : {top, bottom})
      for (int cap : {cap1, cap2}) m_sk.add_constraint(CT::Tangent, {line, cap});
    m_sk.add_constraint(CT::Equal, {cap1, cap2});
    const double px = m_viewport->pixelSize();
    labelOff(keepTyped(c2, "length", CT::Distance, {centres}), centres, c1.u - nx, c1.v - ny, r + 20 * px);  // above it
    keepDirection(c2, "angle", {centres}, std::atan2(dy, dx));
    keepAligned(c2, {centres});
    if (const int width = keepTyped(s, "width", CT::Distance, {top, bottom})) labelAt(width, c1.u - dx / len * (r + 30 * px), c1.v - dy / len * (r + 30 * px));  // past the first cap
    return done(tr("Slot"));
  }
  if (m_tool == "ellipse" && n == 3) {
    const Snap &c = m_clicks[0], &m = m_clicks[1];
    const double dx = m.u - c.u, dy = m.v - c.v, len = std::hypot(dx, dy);
    if (len < 1e-6) { m_clicks.clear(); return toolPrompt(); }
    const double minor = std::fabs((s.u - c.u) * (-dy / len) + (s.v - c.v) * (dx / len));
    if (minor < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    SkEntity e;
    e.type = ET::Ellipse;
    e.p = {pointFor(c), pointFor(m)};
    e.r = minor;
    e.id = m_sk.next_id();
    m_sk.entities.push_back(e);
    keepTyped(m, "radius", CT::Distance, {e.p[0], e.p[1]});  // the major radius (nothing holds the minor one)
    keepDirection(m, "angle", {e.p[0], e.p[1]}, std::atan2(dy, dx));
    keepAligned(m, {e.p[0], e.p[1]});
    return done(tr("Ellipse"));
  }
  toolPrompt();
}

// ---------------------------------------------------------------- typed values kept (UI-17)
// A size typed for a shape becomes the driving dimension of what the click made (setting sketch/input/addDimensions, on
// by default), inside the shape's own change: one undo step, and the solver sees a value the geometry already has. An
// expression stays one (times `scale`); a plain number is kept as its value.
int SketchEditor::keepTyped(const Snap& s, const char* key, CT type, std::vector<int> refs, double scale) {
  const auto it = s.typed.find(key);
  if (it == s.typed.end() || !QSettings().value("sketch/input/addDimensions", true).toBool()) return 0;
  const double value = std::fabs(it->second.first * scale);
  if (value < 1e-9) return 0;
  std::string expr;
  const QString text = it->second.second;
  try {
    if (!plainValue(text) && it->second.first > 0) expr = sketch_parameters(m_sk, paramTable(m_doc->scene)).explicit_length(text.toStdString());
  } catch (const std::exception&) {
    expr.clear();  // evaluated when it was typed; kept as its value
  }
  if (!expr.empty() && scale != 1) expr = "(" + expr + ") * " + QString::number(scale, 'g', 17).toStdString();
  return m_sk.add_constraint(type, std::move(refs), value, expr);
}

// Where a kept dimension's value sits: outside the shape (left alone a rectangle's sat inside it, a slot's two on one spot).
void SketchEditor::labelOff(int id, int line, double u, double v, double offset) {
  const SkEntity* e = m_sk.entity(line);
  if (!id || !e || e->p.size() < 2) return;
  const SkPoint *a = m_sk.point(e->p[0]), *b = m_sk.point(e->p[1]);
  const double dx = b->x - a->x, dy = b->y - a->y, length = std::hypot(dx, dy), mu = (a->x + b->x) / 2, mv = (a->y + b->y) / 2;
  if (length < 1e-12) return;
  const double side = (u - mu) * -dy + (v - mv) * dx > 0 ? -1 : 1;
  labelAt(id, mu - side * dy / length * offset, mv + side * dx / length * offset);
}

void SketchEditor::labelAt(int id, double u, double v) {
  if (SkConstraint* c = id ? m_sk.constraint(id) : nullptr) {
    c->pos[0] = u;
    c->pos[1] = v;
  }
}

// The pointer's horizontal or vertical from the step's last point (UI-23), on what the click made from there: an arc's
// chord, a centre to its arc's end or a polygon's corner, a slot's centres, an ellipse's axis.
void SketchEditor::keepAligned(const Snap& s, std::vector<int> axis) {
  if (s.horizontal) m_sk.add_constraint(CT::Horizontal, axis);
  if (s.vertical) m_sk.add_constraint(CT::Vertical, std::move(axis));
}

void SketchEditor::keepDirection(const Snap& s, const char* key, std::vector<int> axis, double angle, int line, int previous) {
  if (!s.typed.count(key) || !QSettings().value("sketch/input/addDimensions", true).toBool()) return;
  double between = 0;
  const SkEntity* before = previous ? m_sk.entity(previous) : nullptr;
  if (before) {
    const SkPoint *a = m_sk.point(before->p[0]), *b = m_sk.point(before->p[1]);
    between = angle - std::atan2(b->y - a->y, b->x - a->x);
  }
  // A line's angle typed from the line before (the box's switch) is held against it; a polyline's first segment is not.
  const bool relative = key == std::string("angle") && m_angleRelative && before;
  using shapeinput::Hold;
  switch (shapeinput::angleHold(angle, before != nullptr, between, relative)) {
    case Hold::Horizontal:
      if (!s.horizontal) m_sk.add_constraint(CT::Horizontal, std::move(axis));
      break;
    case Hold::Vertical:
      if (!s.vertical) m_sk.add_constraint(CT::Vertical, std::move(axis));
      break;
    case Hold::Parallel: m_sk.add_constraint(CT::Parallel, {previous, line}); break;
    case Hold::Perpendicular: m_sk.add_constraint(CT::Perpendicular, {previous, line}); break;
    case Hold::Angle: {  // its value on an arc at the corner (left alone it sat on the first line's middle)
      const int id = m_sk.add_constraint(CT::Angle, {previous, line}, shapeinput::between(between));
      const SkPoint *corner = m_sk.point(m_sk.entity(line)->p[0]), *end = m_sk.point(m_sk.entity(line)->p[1]);
      const double reach = std::min(40 * m_viewport->pixelSize(), 0.6 * std::hypot(end->x - corner->x, end->y - corner->y));
      const double middle = angle - between + std::remainder(between, 2 * M_PI) / 2;
      SkConstraint* c = m_sk.constraint(id);
      c->pos[0] = corner->x + reach * std::cos(middle);
      c->pos[1] = corner->y + reach * std::sin(middle);
      break;
    }
    case Hold::None: {  // the first line: against the sketch's X axis (a fixed reference line, made once)
      const SkEntity* l = axis.size() == 1 ? m_sk.entity(axis[0]) : nullptr;
      if (!l || l->type != ET::Line || key != std::string("angle")) break;
      const SkPoint *corner = m_sk.point(l->p[0]), *end = m_sk.point(l->p[1]);
      const int id = m_sk.add_constraint(CT::Angle, {referenceX(), axis[0]}, shapeinput::between(angle));
      const double reach = std::min(40 * m_viewport->pixelSize(), 0.6 * std::hypot(end->x - corner->x, end->y - corner->y)), middle = std::remainder(angle, 2 * M_PI) / 2;
      labelAt(id, corner->x + reach * std::cos(middle), corner->y + reach * std::sin(middle));
      break;
    }
  }
}

// A fixed line along +X that angles typed from the X axis are held against: one already there (Mirror's X axis, a projected
// horizontal edge), else one made beside the origin as Mirror makes it.
int SketchEditor::referenceX() {
  for (const auto& e : m_sk.entities)
    if (e.type == ET::Line && e.fixed && e.p.size() == 2) {
      const SkPoint *a = m_sk.point(e.p[0]), *b = m_sk.point(e.p[1]);
      if (a && b && a->fixed && b->fixed && b->x - a->x > 1e-9 && std::fabs(b->y - a->y) < 1e-12) return e.id;
    }
  const int line = m_sk.add_line(m_sk.add_point(0, 0, true), m_sk.add_point(1, 0, true), true);
  m_sk.entity(line)->fixed = true;
  return line;
}

// A centre arc's typed sweep, with its radius kept as dimension `radius`: the arc's length, that radius times the sweep
// (an expression as typed). False: not kept.
bool SketchEditor::keepSweep(const Snap& s, int arc, int radius, double r) {
  const auto it = s.typed.find("sweep");
  if (!radius || it == s.typed.end() || !QSettings().value("sketch/input/addDimensions", true).toBool()) return false;
  const double turn = std::fabs(it->second.first);
  QString expr = QStringLiteral("d%1 * %2 deg").arg(radius).arg(turn * 180 / M_PI, 0, 'g', 15);
  if (!plainValue(it->second.second) && it->second.first > 0) try {
      const Quantity q = sketch_parameters(m_sk, paramTable(m_doc->scene)).eval(it->second.second.toStdString());
      expr = QStringLiteral("d%1 * (%2)%3").arg(radius).arg(it->second.second, q.angle ? QString() : QStringLiteral(" * 1 deg"));
    } catch (const std::exception&) {  // evaluated when it was typed; kept as its value
    }
  m_sk.add_constraint(CT::ArcLength, {arc}, r * turn, expr.toStdString());
  return true;
}

// ---------------------------------------------------------------- constraints
bool SketchEditor::applyConstraint(CT type, const std::vector<int>& ids, bool quiet) {
  std::vector<int> points, lines, rounds, splines, others;
  for (int id : ids) {
    if (m_sk.point(id)) points.push_back(id);
    else if (const SkEntity* e = m_sk.entity(id)) {
      if (e->type == ET::Line) lines.push_back(id);
      else if (e->type == ET::Circle || e->type == ET::Arc) rounds.push_back(id);
      else if (e->type == ET::Point && !e->p.empty()) points.push_back(e->p[0]);
      else if(e->type==ET::Spline)splines.push_back(id);
      else others.push_back(id);
    }
  }
  std::vector<std::vector<int>> sets;  // one constraint per entry
  switch (type) {
    case CT::Horizontal:
    case CT::Vertical:
      for (int l : lines) sets.push_back({l});
      if (lines.empty() && points.size() == 2) sets.push_back(points);
      break;
    case CT::Coincident:
      if (points.size() == 2 && lines.empty() && rounds.empty()) sets.push_back(points);
      else if (points.size() == 1 && lines.size() + rounds.size() == 1) sets.push_back({points[0], lines.empty() ? rounds[0] : lines[0]});
      break;
    case CT::Parallel:
    case CT::Perpendicular:
    case CT::Collinear:
      if (lines.size() >= 2 && (type == CT::Perpendicular ? lines.size() == 2 : true))
        for (size_t i = 1; i < lines.size(); ++i) sets.push_back({lines[0], lines[i]});
      break;
    case CT::Equal:
      if (lines.size() >= 2 && rounds.empty())
        for (size_t i = 1; i < lines.size(); ++i) sets.push_back({lines[0], lines[i]});
      else if (rounds.size() >= 2 && lines.empty())
        for (size_t i = 1; i < rounds.size(); ++i) sets.push_back({rounds[0], rounds[i]});
      break;
    case CT::Smooth:
    case CT::Curvature:  // a spline with a spline, a line or an arc (TODO 11 wave 3, P6: the guides join a line and a spline)
      if(splines.size()==2)sets.push_back(splines);
      else if(splines.size()==1 && lines.size()+rounds.size()==1)sets.push_back({lines.empty()?rounds[0]:lines[0],splines[0]});
      break;
    case CT::Tangent:
      if(splines.size()==2)sets.push_back(splines);
      else if(splines.size()==1 && lines.size()+rounds.size()==1)sets.push_back({lines.empty()?rounds[0]:lines[0],splines[0]});
      if (lines.size() == 1 && rounds.size() == 1) sets.push_back({lines[0], rounds[0]});
      else if (lines.empty() && rounds.size() == 2) sets.push_back(rounds);
      break;
    case CT::Concentric:
      if (rounds.size() == 2 && lines.empty()) sets.push_back(rounds);
      break;
    case CT::Midpoint:
      if (points.size() == 1 && lines.size() == 1) sets.push_back({points[0], lines[0]});
      break;
    case CT::Symmetric:
      if (points.size() == 2 && lines.size() == 1) sets.push_back({points[0], points[1], lines[0]});
      break;
    case CT::Fix:
      for (int id : ids) sets.push_back({id});
      break;
    default:
      break;
  }
  if (sets.empty()) {
    if (!quiet) emit status(tr("That constraint does not fit what is picked."));
    return false;
  }
  begin_change();
  try {
    for (const auto& refs : sets) m_sk.add_constraint(type, refs);
  } catch (const std::exception& e) {
    cancel_change();
    emit status(i18n::t(QString::fromUtf8(e.what())));
    return false;
  }
  const QString name = i18n::t(QString::fromLatin1(SkConstraint::type_name(type)));
  return end_change(name.left(1).toUpper() + name.mid(1));
}

void SketchEditor::constraintClick(const Hit& h) {
  if (h.kind == Hit::None || h.kind == Hit::Dimension) return;
  if (std::find(m_picked.begin(), m_picked.end(), h.id) != m_picked.end()) return;
  m_picked.push_back(h.id);
  const CT type = SkConstraint::type_from_name(m_tool.mid(2).toStdString());
  const std::vector<int> ids = m_picked;
  // Try after every pick: a line is enough for Horizontal, Symmetric needs three picks.
  const bool lineOnly = (type == CT::Horizontal || type == CT::Vertical) && m_sk.point(h.id) != nullptr && ids.size() < 2;
  if (!lineOnly && applyConstraint(type, ids, true)) {
    m_picked.clear();
  } else if (m_picked.size() >= 3 || !m_conflicts.empty()) {  // the reason stays on the status line
    if (m_conflicts.empty()) emit status(tr("That constraint does not fit what was picked; pick again."));
    m_picked.clear();
    return rebuild();
  }
  rebuild();
  toolPrompt();  // the next pick, or the next constraint
}

// ---------------------------------------------------------------- dimensions
QString SketchEditor::option(const QString& key, const QString& fallback) const {
  const auto it = m_options.constFind(key);
  return it != m_options.cend() ? *it : units::presetText(fallback);
}

QString SketchEditor::dimensionText(const SkConstraint& c) const {
  QString value = c.type == CT::Angle ? units::compact(units::Kind::Angle, c.value * 180.0 / M_PI) : units::compact(units::Kind::Length, c.value);
  if (c.type == CT::Radius) value = "R" + value;
  if (c.type == CT::Diameter) value = QString::fromUtf8("Ø") + value;
  const bool plain = plainValue(QString::fromStdString(c.expr));
  if (!c.expr.empty() && !plain) value = QStringLiteral("fx: ") + value;
  if (c.reference) value = "(" + value + ")";
  return value;
}

void SketchEditor::labelPosition(const SkConstraint& c, double& u, double& v) const {
  if (c.pos[0] != 0 || c.pos[1] != 0) {
    u = c.pos[0];
    v = c.pos[1];
    return;
  }
  // Never placed (made by a command, or by a sketch tool): next to the first thing it measures.
  u = v = 0;
  const double off = 24 * m_viewport->pixelSize();
  if (c.refs.empty()) return;
  if (const SkEntity* e = m_sk.entity(c.refs[0])) {
    const auto pts = sampled(*e);
    if (e->type == ET::Line && pts.size() == 2) {
      const double dx = pts[1].first - pts[0].first, dy = pts[1].second - pts[0].second, len = std::max(1e-9, std::hypot(dx, dy));
      u = (pts[0].first + pts[1].first) / 2 - dy / len * off;
      v = (pts[0].second + pts[1].second) / 2 + dx / len * off;
    } else if (const SkPoint* p = m_sk.point(e->p.empty() ? 0 : e->p[0])) {
      const double r = e->type == ET::Circle ? e->r : pts.empty() ? 0 : std::hypot(pts[0].first - p->x, pts[0].second - p->y);
      u = p->x + (r + off) * 0.7071;
      v = p->y + (r + off) * 0.7071;
    }
  } else if (const SkPoint* p = m_sk.point(c.refs[0])) {
    const SkPoint* q = c.refs.size() > 1 ? m_sk.point(c.refs[1]) : nullptr;
    u = q ? (p->x + q->x) / 2 : p->x + off;
    v = (q ? (p->y + q->y) / 2 : p->y) + off;
  }
}

void SketchEditor::dimensionClick(const Hit& h, double u, double v) {
  const bool onGeometry = h.kind == Hit::Point || h.kind == Hit::Entity;
  if (h.kind == Hit::Dimension && !m_placingDim) return editDimension(h.id, false);
  if (onGeometry && m_picked.size() < 2 && std::find(m_picked.begin(), m_picked.end(), h.id) == m_picked.end()) {
    // A Point entity stands for its point.
    int id = h.id;
    if (const SkEntity* e = m_sk.entity(id); e && e->type == ET::Point && !e->p.empty()) id = e->p[0];
    m_picked.push_back(id);
    // What do the picks measure?
    auto kindOf = [&](int k) { const SkEntity* e = m_sk.entity(k); return e ? e->type : ET::Point; };
    SkConstraint c;
    bool ready = true;
    if (m_picked.size() == 1) {
      if (m_sk.point(id)) ready = false;  // a point alone measures nothing: wait for the second pick
      else if (kindOf(id) == ET::Line) { c.type = CT::Distance; c.refs = {id}; }
      else if (kindOf(id) == ET::Circle) { c.type = CT::Diameter; c.refs = {id}; }
      else if (kindOf(id) == ET::Arc) { c.type = CT::Radius; c.refs = {id}; }
      else { m_picked.clear(); ready = false; emit status(tr("Dimension: that curve cannot be dimensioned; use its points")); }
    } else {
      const int a = m_picked[0], b = m_picked[1];
      const bool pa = m_sk.point(a) != nullptr, pb = m_sk.point(b) != nullptr;
      if (pa && pb) { c.type = CT::Distance; c.refs = {a, b}; }
      else if (pa != pb && kindOf(pa ? b : a) == ET::Line) { c.type = CT::Distance; c.refs = {pa ? a : b, pa ? b : a}; }
      else if (!pa && !pb && kindOf(a) == ET::Line && kindOf(b) == ET::Line) {
        const SkEntity *l1 = m_sk.entity(a), *l2 = m_sk.entity(b);
        const SkPoint *p0 = m_sk.point(l1->p[0]), *p1 = m_sk.point(l1->p[1]), *q0 = m_sk.point(l2->p[0]), *q1 = m_sk.point(l2->p[1]);
        const double cross = (p1->x - p0->x) * (q1->y - q0->y) - (p1->y - p0->y) * (q1->x - q0->x);
        const double scale = std::hypot(p1->x - p0->x, p1->y - p0->y) * std::hypot(q1->x - q0->x, q1->y - q0->y);
        c.type = std::fabs(cross) < 1e-6 * std::max(scale, 1e-12) ? CT::Distance : CT::Angle;
        c.refs = {a, b};
      } else {
        m_picked.clear();
        ready = false;
        emit status(tr("Dimension: pick a line, a circle, an arc, two points, a point and a line, or two lines"));
      }
    }
    if (ready) {
      m_pendingDim = c;
      m_placingDim = true;
    }
    toolPrompt();
    rebuild();
    return;
  }
  if (m_placingDim) placeDimension(u, v);
}

void SketchEditor::placeDimension(double u, double v) {
  SkConstraint c = m_pendingDim;
  m_placingDim = false;
  m_picked.clear();
  auto P = [&](int id) { return m_sk.point(id); };
  // Two points: where the label goes says whether the distance is meant along u, along v, or straight.
  if (option("dimensionType","auto")=="auto" && c.type == CT::Distance && c.refs.size() == 2 && P(c.refs[0]) && P(c.refs[1])) {
    const SkPoint *a = P(c.refs[0]), *b = P(c.refs[1]);
    const double x0 = std::min(a->x, b->x), x1 = std::max(a->x, b->x), y0 = std::min(a->y, b->y), y1 = std::max(a->y, b->y);
    const bool insideX = u > x0 && u < x1, insideY = v > y0 && v < y1;
    if (insideX && !insideY && x1 - x0 > 1e-9) c.type = CT::HDistance;
    else if (insideY && !insideX && y1 - y0 > 1e-9) c.type = CT::VDistance;
  }
  const QString requested=option("dimensionType","auto");
  if(requested!="auto") {
    const auto type=SkConstraint::type_from_name(requested.toStdString());
    if((type==CT::HDistance||type==CT::VDistance) && c.refs.size()==1) {
      const auto* line=m_sk.entity(c.refs[0]);
      if(line && line->type==ET::Line)c.refs=line->p;
    }
    c.type=type;
  }
  double value=0;
  try {
    Sketch check=m_sk;
    check.add_constraint(c.type,c.refs,1);
    value=dimension_value(m_sk,c);
  } catch(const std::exception& e) {emit status(i18n::t(QString::fromUtf8(e.what())));return rebuild();}
  if (value < 1e-9) {
    emit status(tr("Dimension: that measures zero; nothing to drive"));
    return rebuild();
  }
  begin_change();
  int id = 0;
  try {
    id = m_sk.add_constraint(c.type, c.refs, value);
  } catch (const std::exception& e) {
    cancel_change();
    return emit status(i18n::t(QString::fromUtf8(e.what())));
  }
  if (SkConstraint* made = m_sk.constraint(id)) {
    made->reference=option("reference","0")=="1";
    made->pos[0] = u;
    made->pos[1] = v;
  }
  if (!end_change(tr("Dimension"))) return;
  editDimension(id, true);
  toolPrompt();
}

void SketchEditor::editDimension(int id, bool fresh) {
  const SkConstraint* c = m_sk.constraint(id);
  if (!c || !c->is_dimension()) return;
  if (!m_dimEdit) {
    // Typed where the value is, right after placing a dimension (or double-clicking one): Enter applies, Esc keeps the
    // measured value. The box existed but was never shown, so values could only be typed in the panel.
    m_dimEdit = new QLineEdit(m_viewport);
    m_dimEdit->setObjectName("sketchDimensionValue");
    m_dimEdit->setAttribute(Qt::WA_NativeWindow);  // over the OCCT window, like the drag handles' value box
    m_dimEdit->setAutoFillBackground(true);
    m_dimEdit->setAlignment(Qt::AlignCenter);
    m_dimEdit->setToolTip(tr("Type a value or an expression · Enter applies · Esc keeps the measured value"));
    connect(m_dimEdit, &QLineEdit::textEdited, this, [this](const QString& text) { m_options["expression"] = text; });
    connect(m_dimEdit, &QLineEdit::returnPressed, this, [this] { m_options["expression"] = m_dimEdit->text(); commitDimensionEdit(); });
  }
  if (m_tool != "dimension") setTool("dimension");

  m_dimEditing = id;
  m_dimFresh = fresh;
  const QString shown = !c->expr.empty() ? QString::fromStdString(c->expr) : c->type == CT::Angle ? units::editable(units::Kind::Angle, c->value * 180 / M_PI) : units::editable(units::Kind::Length, c->value);
  m_dimEdit->setText(shown);
  m_dimShown = shown;
  m_options["expression"] = shown;
  m_options["reference"] = c->reference ? "1" : "0";
  m_panelFieldsDirty = true;
  emit toolChanged(m_tool);
  toolPrompt();  // the value step waits (the status line still asked for the geometry)

  const Tokens& t = theme::current();
  m_dimEdit->setStyleSheet(QString("#sketchDimensionValue { background: %1; color: %2; border: 1px solid %3; border-radius: 4px; padding: 1px 6px; selection-background-color: %4; }")
                               .arg(theme::css(t.bg2), theme::css(t.fg), theme::css(t.sel), theme::css(t.selbg)));
  m_dimEdit->setFont(theme::ui(13));
  m_dimEdit->setFixedWidth(std::clamp(m_dimEdit->fontMetrics().horizontalAdvance(shown + "    ") + 16, 90, 260));
  m_dimEdit->adjustSize();
  double lu, lv;
  labelPosition(*c, lu, lv);
  const QPoint at = m_viewport->widgetPoint(m_frame.to_world(lu, lv));
  m_dimEdit->move(std::clamp(at.x() - m_dimEdit->width() / 2, 0, std::max(0, m_viewport->width() - m_dimEdit->width())),
                  std::clamp(at.y() - m_dimEdit->height() / 2, 0, std::max(0, m_viewport->height() - m_dimEdit->height())));
  m_dimEdit->show();
  m_dimEdit->raise();
  m_dimEdit->setFocus();
  m_dimEdit->selectAll();
  rebuild();
}

void SketchEditor::commitDimensionEdit() {
  if (!m_dimEdit || !m_dimEditing) return;
  const QString text = m_dimEdit->text().trimmed();
  m_dimEdit->hide();
  m_viewport->setFocus();
  SkConstraint* c = m_sk.constraint(m_dimEditing);
  if (!c || text.isEmpty()) return rebuild();
  // "1.181102 in" as shown for 30 mm reads back as 29.99999 mm: Enter on the box as it opened keeps the value.
  if (text == m_dimShown.trimmed() && c->expr.empty() && c->reference == (option("reference", "0") == "1")) return rebuild();
  double value = 0;
  try {
    value = sketch_parameters(m_sk, paramTable(m_doc->scene)).as(c->type == CT::Angle ? Dim::Angle : Dim::Length, text.toStdString());
  } catch (const std::exception& e) {
    emit status(i18n::t(QString::fromUtf8(e.what())));
    return rebuild();
  }
  // Signed distances and coordinates (gap log #11) may be negative; sizes may not.
  const bool mayBeNegative = c->type == CT::Angle || ((c->type == CT::HDistance || c->type == CT::VDistance) && (c->is_signed || c->refs.size() == 1));
  if (value <= 0 && !mayBeNegative) {
    emit status(tr("A dimension must be positive"));
    return rebuild();
  }
  const bool plain = plainValue(text);
  const std::string expr = plain ? std::string() : c->type==CT::Angle?text.toStdString():sketch_parameters(m_sk,paramTable(m_doc->scene)).explicit_length(text.toStdString());
  if (std::fabs(value - c->value) < 1e-12 && expr == c->expr && c->reference == (option("reference", "0") == "1")) return rebuild();
  const int id = c->id;
  begin_change();
  c = m_sk.constraint(id);
  c->value = value;
  c->expr = expr;
  c->reference = option("reference", "0") == "1";
  if (c->reference) c->expr.clear();
  m_panelFieldsDirty = true;
  if(end_change(tr("Dimension"))) {
    if(m_dimFresh && m_undo.size()>1)m_undo.pop_back(); // placement and its value are one user edit
    m_dimFresh=false;m_dimEditing=0;
    return toolPrompt();  // the next dimension (a refusal's reason stays on the status line)
  }
  emit workflowChanged();
}

// ---------------------------------------------------------------- sketch fillet
// A corner where two lines or arcs end (UI-28, core fillet_corner): the nearest arc of the radius set that fits, tangent to
// both; the curves end where it touches them.
void SketchEditor::filletAt(const Hit& h, double, double) {
  if (h.kind != Hit::Point) return emit status(tr("Sketch fillet: click the corner point where two lines or arcs meet"));
  const QString text = option("radius", "2 mm");
  double r = 0;
  try {
    r = sketch_parameters(m_sk, paramTable(m_doc->scene)).length(text.toStdString());
  } catch (const std::exception& e) {
    return emit status(i18n::t(QString::fromUtf8(e.what())));
  }
  FilletCorner corner;
  if (!fillet_geometry(m_sk, h.id, r, corner)) return emit status(tr("Sketch fillet: no fillet of that radius fits there; pick a corner where two lines or arcs end, with room for it"));
  begin_change();
  try {
    fillet_corner(m_sk, h.id, r, plainValue(text) ? std::string() : text.toStdString());
  } catch (const std::exception& e) {
    cancel_change();
    return emit status(i18n::t(QString::fromUtf8(e.what())));
  }
  end_change(tr("Sketch fillet"));
}

// The arc a click on that corner rounds it with, at the radius set (shown while the corner is hovered, from the tool's
// start); empty where none fits, as filletAt refuses.
std::vector<std::pair<double, double>> SketchEditor::filletPreview(int id) const {
  std::vector<std::pair<double, double>> arc;
  double r = 0;
  try {
    r = paramTable(m_doc->scene).length(option("radius", "2 mm").toStdString());
  } catch (const std::exception&) {
    return arc;
  }
  FilletCorner f;
  if (!fillet_geometry(m_sk, id, r, f)) return arc;
  const double a1 = std::atan2(f.ay - f.cy, f.ax - f.cx), a2 = std::atan2(f.by - f.cy, f.bx - f.cx);
  const double sweep = f.ccw ? norm_angle(a2 - a1) : -norm_angle(a1 - a2);
  const int n = std::max(8, int(std::ceil(std::fabs(sweep) / (2 * M_PI) * 96)));
  for (int i = 0; i <= n; ++i) arc.push_back({f.cx + r * std::cos(a1 + sweep * i / n), f.cy + r * std::sin(a1 + sweep * i / n)});
  return arc;
}

// ---------------------------------------------------------------- trim
namespace {
struct Cut {
  double t;   // parameter on the trimmed curve: 0..1 along a line, angle for circles and arcs
  int other;  // the curve that cuts it
};

// Points where a line a->b meets a circle (c, r): parameters along the line.
std::vector<double> line_circle(double ax, double ay, double bx, double by, double cx, double cy, double r) {
  const double dx = bx - ax, dy = by - ay, fx = ax - cx, fy = ay - cy;
  const double A = dx * dx + dy * dy, B = 2 * (fx * dx + fy * dy), C = fx * fx + fy * fy - r * r;
  const double disc = B * B - 4 * A * C;
  if (A < 1e-18 || disc < 0) return {};
  const double s = std::sqrt(disc);
  return {(-B - s) / (2 * A), (-B + s) / (2 * A)};
}

struct Arc2 { double cx, cy, r, a0, sweep; };  // sweep 2 pi = full circle

bool on_round(const Arc2& k, double x, double y) { return k.sweep >= 2 * M_PI - 1e-12 || norm_angle(std::atan2(y - k.cy, x - k.cx) - k.a0) <= k.sweep + 1e-9; }

// Where the other curves cross `target`, sorted along it. A trim at a point removes the span between the cuts on
// either side of it (or the whole curve when nothing crosses it); the hover shows that span before the click.
struct Crossings {
  bool line = true;
  double ax = 0, ay = 0, bx = 0, by = 0;  // a line's ends
  Arc2 self{};                           // a circle's or an arc's
  std::vector<Cut> cuts;
};
// `samples`: a preview's, a spline's or an ellipse's polyline (nullptr: the kernel's crossings, as a trim asks).
using Samples = std::function<const std::vector<std::pair<double, double>>*(const SkEntity&)>;
Crossings crossings(const Sketch& sk, const SkEntity& target, const Samples& samples = {}) {
  auto P = [&](int id) { return sk.point(id); };
  auto round_of = [&](const SkEntity& e) {
    Arc2 k{P(e.p[0])->x, P(e.p[0])->y, e.r, 0, 2 * M_PI};
    if (e.type == ET::Arc) {
      k.r = std::hypot(P(e.p[1])->x - k.cx, P(e.p[1])->y - k.cy);
      k.a0 = std::atan2(P(e.p[1])->y - k.cy, P(e.p[1])->x - k.cx);
      k.sweep = norm_angle(std::atan2(P(e.p[2])->y - k.cy, P(e.p[2])->x - k.cx) - k.a0);
      if (k.sweep < 1e-12) k.sweep = 2 * M_PI;
    }
    return k;
  };
  const double eps = 1e-7;
  Crossings out;
  out.line = target.type == ET::Line;
  if (out.line) {
    out.ax = P(target.p[0])->x, out.ay = P(target.p[0])->y, out.bx = P(target.p[1])->x, out.by = P(target.p[1])->y;
  } else {
    out.self = round_of(target);
  }
  const bool isLine = out.line;
  const Arc2& self = out.self;
  const double ax = out.ax, ay = out.ay, bx = out.bx, by = out.by;
  std::vector<Cut>& cuts = out.cuts;
  for (const auto& o : sk.entities) {
    if (o.id == target.id) continue;
    std::vector<std::pair<double, double>> hits;  // intersection points
    if (o.type == ET::Line) {
      const double cx = P(o.p[0])->x, cy = P(o.p[0])->y, dx = P(o.p[1])->x, dy = P(o.p[1])->y;
      if (isLine) {
        const double den = (bx - ax) * (dy - cy) - (by - ay) * (dx - cx);
        if (std::fabs(den) < 1e-14) continue;
        const double t = ((cx - ax) * (dy - cy) - (cy - ay) * (dx - cx)) / den, s = ((cx - ax) * (by - ay) - (cy - ay) * (bx - ax)) / den;
        if (s >= -eps && s <= 1 + eps) hits.push_back({ax + t * (bx - ax), ay + t * (by - ay)});
      } else {
        for (double s : line_circle(cx, cy, dx, dy, self.cx, self.cy, self.r))
          if (s >= -eps && s <= 1 + eps) hits.push_back({cx + s * (dx - cx), cy + s * (dy - cy)});
      }
    } else if (o.type == ET::Circle || o.type == ET::Arc) {
      const Arc2 k = round_of(o);
      if (isLine) {
        for (double t : line_circle(ax, ay, bx, by, k.cx, k.cy, k.r)) {
          const double x = ax + t * (bx - ax), y = ay + t * (by - ay);
          if (on_round(k, x, y)) hits.push_back({x, y});
        }
      } else {
        const double d = std::hypot(k.cx - self.cx, k.cy - self.cy);
        if (d < 1e-12 || d > self.r + k.r || d < std::fabs(self.r - k.r)) continue;
        const double a = (self.r * self.r - k.r * k.r + d * d) / (2 * d), hh = std::sqrt(std::max(0.0, self.r * self.r - a * a));
        const double mx = self.cx + a * (k.cx - self.cx) / d, my = self.cy + a * (k.cy - self.cy) / d;
        for (double sgn : {1.0, -1.0}) {
          const double x = mx + sgn * hh * (k.cy - self.cy) / d, y = my - sgn * hh * (k.cx - self.cx) / d;
          if (on_round(k, x, y)) hits.push_back({x, y});
        }
      }
    } else if (o.type == ET::Ellipse || o.type == ET::Spline) {  // the kernel's crossings (UI-28), a preview's on the samples
      if (const auto* poly = samples ? samples(o) : nullptr) {
        auto within = [&](double s, size_t i) { return s >= -1e-9 && (s < 1 - 1e-9 || (i + 1 == poly->size() && s <= 1 + 1e-9)); };  // a vertex once, the ends too
        for (size_t i = 1; i < poly->size(); ++i) {
          const double cx = (*poly)[i - 1].first, cy = (*poly)[i - 1].second, dx = (*poly)[i].first, dy = (*poly)[i].second;
          if (isLine) {
            const double den = (bx - ax) * (dy - cy) - (by - ay) * (dx - cx);
            if (std::fabs(den) < 1e-14) continue;
            const double t = ((cx - ax) * (dy - cy) - (cy - ay) * (dx - cx)) / den, s = ((cx - ax) * (by - ay) - (cy - ay) * (bx - ax)) / den;
            if (within(s, i)) hits.push_back({ax + t * (bx - ax), ay + t * (by - ay)});
          } else
            for (double s : line_circle(cx, cy, dx, dy, self.cx, self.cy, self.r))
              if (within(s, i)) hits.push_back({cx + s * (dx - cx), cy + s * (dy - cy)});
        }
      } else
        try {
          for (const auto& [x, y] : curve_crossings(sk, target, o)) hits.push_back({x, y});
        } catch (...) {
        }
    } else {
      continue;
    }
    for (const auto& [x, y] : hits) {
      if (isLine) {
        const double len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
        const double t = ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / len2;
        if (t > eps && t < 1 - eps) cuts.push_back({t, o.id});
      } else if (on_round(self, x, y)) {
        const double t = norm_angle(std::atan2(y - self.cy, x - self.cx) - self.a0);
        if (self.sweep >= 2 * M_PI - 1e-12 || (t > eps && t < self.sweep - eps)) cuts.push_back({t, o.id});
      }
    }
  }
  std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.t < b.t; });
  return out;
}
}  // namespace

struct SketchEditor::TrimCrossings {
  Crossings c;
};

// Whether the samples of two curves come near each other (UI-28: only those go to the kernel; a spline's box from its points
// is wide). A crossing lies within the samples' deflection of both polylines; without samples, they may cross.
bool SketchEditor::mayCross(const SkEntity& a, const SkEntity& b) const {
  const auto* pa = m_geometry ? m_geometry->samples(m_sk, a) : nullptr;
  const auto* pb = pa ? m_geometry->samples(m_sk, b) : nullptr;
  if (!pa || !pb || pa->empty() || pb->empty()) return true;
  const double gap = 8 * m_geometry->deflection() + 1e-9;
  using P = std::pair<double, double>;
  auto toSegment = [](const P& p, const P& a, const P& b) {
    const double dx = b.first - a.first, dy = b.second - a.second, len2 = dx * dx + dy * dy;
    const double t = len2 < 1e-30 ? 0 : std::clamp(((p.first - a.first) * dx + (p.second - a.second) * dy) / len2, 0.0, 1.0);
    return std::hypot(a.first + t * dx - p.first, a.second + t * dy - p.second);
  };
  auto side = [](const P& a, const P& b, const P& p) { return (b.first - a.first) * (p.second - a.second) - (b.second - a.second) * (p.first - a.first); };
  auto close = [&](const P& a0, const P& a1, const P& b0, const P& b1) {
    if (std::min(a0.first, a1.first) > std::max(b0.first, b1.first) + gap || std::min(b0.first, b1.first) > std::max(a0.first, a1.first) + gap ||
        std::min(a0.second, a1.second) > std::max(b0.second, b1.second) + gap || std::min(b0.second, b1.second) > std::max(a0.second, a1.second) + gap)
      return false;
    if ((side(a0, a1, b0) > 0) != (side(a0, a1, b1) > 0) && (side(b0, b1, a0) > 0) != (side(b0, b1, a1) > 0)) return true;
    return std::min({toSegment(b0, a0, a1), toSegment(b1, a0, a1), toSegment(a0, b0, b1), toSegment(a1, b0, b1)}) <= gap;
  };
  const size_t na = std::max<size_t>(1, pa->size() - 1), nb = std::max<size_t>(1, pb->size() - 1);  // segments (a point: one)
  for (size_t i = 0; i < na; ++i)
    for (size_t j = 0; j < nb; ++j)
      if (close((*pa)[i], (*pa)[std::min(i + 1, pa->size() - 1)], (*pb)[j], (*pb)[std::min(j + 1, pb->size() - 1)])) return true;
  return false;
}

// What a trim click at (u, v) on curve `id` removes, as a polyline (empty: nothing it could trim).
std::vector<std::pair<double, double>> SketchEditor::trimPreview(int id, double u, double v) const {
  std::vector<std::pair<double, double>> piece;
  const SkEntity* target = m_sk.entity(id);
  if (m_trimCutsRevision != m_modelRevision) m_trimCuts.clear(), m_trimCrossings.clear(), m_trimCutsRevision = m_modelRevision;
  if (target && (target->type == ET::Spline || target->type == ET::Ellipse)) {  // the kernel's cuts, once per curve and edit
    auto& cuts = m_trimCuts[id];
    std::vector<TrimPiece> keep, gone;
    try {
      if (!cuts) cuts = std::make_shared<const CurveCuts>(curve_cuts(m_sk, id, [this, target](const SkEntity& o) { return mayCross(*target, o); }));
      if (!trim_pieces(*cuts, u, v, keep, gone)) return piece;
      for (const auto& g : gone)
        for (const auto& p : curveSamples(piece_edge(*cuts, g), std::max(1e-7, m_viewport->pixelSize() * 0.25))) piece.push_back({p.X(), p.Y()});
    } catch (...) {
      piece.clear();
    }
    return piece;
  }
  if (!target || (target->type != ET::Line && target->type != ET::Circle && target->type != ET::Arc)) return piece;
  auto& cached = m_trimCrossings[id];
  if (!cached) cached = std::make_shared<const TrimCrossings>(TrimCrossings{crossings(m_sk, *target, [this](const SkEntity& o) { return m_geometry ? m_geometry->samples(m_sk, o) : nullptr; })});
  const Crossings& c = cached->c;
  if (c.line) {
    const double len2 = (c.bx - c.ax) * (c.bx - c.ax) + (c.by - c.ay) * (c.by - c.ay);
    if (len2 < 1e-18) return piece;
    const double tc = ((u - c.ax) * (c.bx - c.ax) + (v - c.ay) * (c.by - c.ay)) / len2;
    double lo = 0, hi = 1;
    for (const auto& k : c.cuts) {
      if (k.t < tc) lo = k.t;
      else { hi = k.t; break; }
    }
    piece = {{c.ax + lo * (c.bx - c.ax), c.ay + lo * (c.by - c.ay)}, {c.ax + hi * (c.bx - c.ax), c.ay + hi * (c.by - c.ay)}};
    return piece;
  }
  const Arc2& k = c.self;
  const bool full = target->type == ET::Circle;
  if (full && c.cuts.size() < 2) return piece;  // trimAt refuses: crossed once at most
  const double tc = norm_angle(std::atan2(v - k.cy, u - k.cx) - k.a0);
  double from = 0, to = k.sweep;
  if (full) {
    size_t hi = 0;
    while (hi < c.cuts.size() && c.cuts[hi].t < tc) ++hi;
    from = c.cuts[(hi + c.cuts.size() - 1) % c.cuts.size()].t;
    to = c.cuts[hi % c.cuts.size()].t;
    if (to <= from) to += 2 * M_PI;
  } else {
    for (const auto& cut : c.cuts) {
      if (cut.t < tc) from = cut.t;
      else { to = cut.t; break; }
    }
  }
  const int n = std::max(8, int(std::ceil((to - from) / (2 * M_PI) * 96)));
  for (int i = 0; i <= n; ++i) {
    const double a = k.a0 + from + (to - from) * i / n;
    piece.push_back({k.cx + k.r * std::cos(a), k.cy + k.r * std::sin(a)});
  }
  return piece;
}

void SketchEditor::trimAt(const Hit& h, double u, double v) {
  QString why;
  begin_change();
  if (!trimPiece(h.kind == Hit::Entity ? h.id : 0, u, v, why)) {
    cancel_change();
    return emit status(why);
  }
  end_change(tr("Trim"));
}

// The piece of curve `id` about (u, v) between the curves that cross it goes (all of it when none does), inside a change;
// false and why: nothing trimmed, nothing changed.
bool SketchEditor::trimPiece(int id, double u, double v, QString& why) {
  SkEntity* target = m_sk.entity(id);
  if (target && (target->type == ET::Spline || target->type == ET::Ellipse)) {  // the kernel's trim (core trim_curve), as it was when it fails
    try {
      trim_curve(m_sk, id, u, v);
      return true;
    } catch (const std::exception& e) {
      why = tr("Trim: %1").arg(i18n::t(QString::fromUtf8(e.what())));
    } catch (const Standard_Failure& e) {
      why = tr("Trim: %1").arg(QString::fromUtf8(e.GetMessageString()));
    }
    return false;
  }
  if (!target || (target->type != ET::Line && target->type != ET::Circle && target->type != ET::Arc)) {
    why = tr("Trim: click a curve");
    return false;
  }
  const Crossings found = crossings(m_sk, *target);
  const bool isLine = found.line;
  const Arc2 self = found.self;
  const double ax = found.ax, ay = found.ay, bx = found.bx, by = found.by;
  const std::vector<Cut>& cuts = found.cuts;
  if (target->type == ET::Circle && cuts.size() == 1) {
    why = tr("Trim: the circle is crossed only once; nothing to cut between");
    return false;
  }
  // A length dimension on the trimmed curve would now mean something else.
  std::vector<int> stale;
  for (const auto& c : m_sk.constraints)
    if (c.is_dimension() && c.type == CT::Distance && c.refs.size() == 1 && c.refs[0] == id) stale.push_back(c.id);
  for (int c : stale) m_sk.remove(c);
  auto cut_point = [&](double x, double y, int other) {  // on the cutting curve (a point can be held on a line, a circle or an arc)
    const int p = m_sk.add_point(x, y);
    if (const SkEntity* by = m_sk.entity(other); by && (by->type == ET::Line || by->type == ET::Circle || by->type == ET::Arc)) m_sk.add_constraint(CT::Coincident, {p, other});
    return p;
  };
  if (cuts.empty()) {
    m_sk.remove(id);
    return true;
  }
  if (isLine) {
    const double len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
    const double tc = ((u - ax) * (bx - ax) + (v - ay) * (by - ay)) / len2;
    const Cut *lo = nullptr, *hi = nullptr;
    for (const auto& c : cuts) {
      if (c.t < tc) lo = &c;
      else if (!hi) hi = &c;
    }
    const int oldStart = target->p[0], oldEnd = target->p[1];
    const bool construction = target->construction;
    if (lo && hi) {
      const int p1 = cut_point(ax + lo->t * (bx - ax), ay + lo->t * (by - ay), lo->other), p2 = cut_point(ax + hi->t * (bx - ax), ay + hi->t * (by - ay), hi->other);
      m_sk.entity(id)->p[1] = p1;
      const int rest = m_sk.add_line(p2, oldEnd, construction);
      m_sk.add_constraint(CT::Collinear, {id, rest});
    } else if (lo) {
      m_sk.entity(id)->p[1] = cut_point(ax + lo->t * (bx - ax), ay + lo->t * (by - ay), lo->other);
      m_sk.remove(oldEnd);
    } else {
      m_sk.entity(id)->p[0] = cut_point(ax + hi->t * (bx - ax), ay + hi->t * (by - ay), hi->other);
      m_sk.remove(oldStart);
    }
    return true;
  }
  const double tc = norm_angle(std::atan2(v - self.cy, u - self.cx) - self.a0);
  auto at = [&](double t, double& x, double& y) { x = self.cx + self.r * std::cos(self.a0 + t); y = self.cy + self.r * std::sin(self.a0 + t); };
  if (target->type == ET::Circle) {
    // The clicked span lies between two neighbouring cuts; what is left runs the other way round.
    size_t hi = 0;
    while (hi < cuts.size() && cuts[hi].t < tc) ++hi;
    const Cut& end = cuts[(hi + cuts.size() - 1) % cuts.size()];  // the span starts here ...
    const Cut& start = cuts[hi % cuts.size()];                    // ... and ends here: the arc kept starts here
    double x, y;
    at(start.t, x, y);
    const int ps = cut_point(x, y, start.other);
    at(end.t, x, y);
    const int pe = cut_point(x, y, end.other);
    SkEntity* e = m_sk.entity(id);
    e->type = ET::Arc;
    e->p = {e->p[0], ps, pe};
    e->r = 0;
    return true;
  }
  const Cut *lo = nullptr, *hi = nullptr;
  for (const auto& c : cuts) {
    if (c.t < tc) lo = &c;
    else if (!hi) hi = &c;
  }
  const int centre = target->p[0], oldStart = target->p[1], oldEnd = target->p[2];
  const bool construction = target->construction;
  double x, y;
  if (lo && hi) {
    at(lo->t, x, y);
    const int p1 = cut_point(x, y, lo->other);
    at(hi->t, x, y);
    const int p2 = cut_point(x, y, hi->other);
    m_sk.entity(id)->p[2] = p1;
    const int rest = m_sk.add_arc(centre, p2, oldEnd, construction);
    m_sk.add_constraint(CT::Equal, {id, rest});
  } else if (lo) {
    at(lo->t, x, y);
    m_sk.entity(id)->p[2] = cut_point(x, y, lo->other);
    m_sk.remove(oldEnd);
  } else {
    at(hi->t, x, y);
    m_sk.entity(id)->p[1] = cut_point(x, y, hi->other);
    m_sk.remove(oldStart);
  }
  return true;
}

// Where the fence from (au, av) to (bu, bv) crosses the curves near it (UI-28), in order along it.
std::vector<std::tuple<int, double, double>> SketchEditor::fenceHits(double au, double av, double bu, double bv) const {
  std::vector<std::tuple<double, int, double, double>> along;
  if (!m_geometry || m_geometryJob) return {};
  const double dx = bu - au, dy = bv - av, len2 = dx * dx + dy * dy;
  if (len2 < 1e-18) return {};
  for (size_t index : m_geometry->query(std::min(au, bu), std::min(av, bv), std::max(au, bu), std::max(av, bv)).entities) {
    if (index >= m_sk.entities.size()) continue;
    const SkEntity& e = m_sk.entities[index];
    if (e.type == ET::Line) {
      const SkPoint *c = m_sk.point(e.p[0]), *d = m_sk.point(e.p[1]);
      const double ex = d->x - c->x, ey = d->y - c->y, den = dx * ey - dy * ex;
      if (std::fabs(den) < 1e-15) continue;
      const double t = ((c->x - au) * ey - (c->y - av) * ex) / den, s = ((c->x - au) * dy - (c->y - av) * dx) / den;
      if (t >= 0 && t <= 1 && s > 1e-9 && s < 1 - 1e-9) along.push_back({t, e.id, au + t * dx, av + t * dy});
    } else if (e.type == ET::Circle || e.type == ET::Arc) {
      const SkPoint* c = m_sk.point(e.p[0]);
      Arc2 k{c->x, c->y, e.r, 0, 2 * M_PI};
      if (e.type == ET::Arc) {
        const SkPoint *s0 = m_sk.point(e.p[1]), *s1 = m_sk.point(e.p[2]);
        k.r = std::hypot(s0->x - c->x, s0->y - c->y);
        k.a0 = std::atan2(s0->y - c->y, s0->x - c->x);
        k.sweep = norm_angle(std::atan2(s1->y - c->y, s1->x - c->x) - k.a0);
      }
      for (double t : line_circle(au, av, bu, bv, k.cx, k.cy, k.r))
        if (t >= 0 && t <= 1 && on_round(k, au + t * dx, av + t * dy)) along.push_back({t, e.id, au + t * dx, av + t * dy});
    } else if (e.type == ET::Ellipse || e.type == ET::Spline) {  // on its samples: the trim finds the curve there
      const auto poly = sampled(e);
      for (size_t i = 1; i < poly.size(); ++i) {
        const double cx = poly[i - 1].first, cy = poly[i - 1].second, ex = poly[i].first - cx, ey = poly[i].second - cy, den = dx * ey - dy * ex;
        if (std::fabs(den) < 1e-15) continue;
        const double t = ((cx - au) * ey - (cy - av) * ex) / den, s = ((cx - au) * dy - (cy - av) * dx) / den;
        if (t >= 0 && t <= 1 && s >= 0 && s < 1) along.push_back({t, e.id, au + t * dx, av + t * dy});
      }
    }
  }
  std::sort(along.begin(), along.end());
  std::vector<std::tuple<int, double, double>> out;
  for (const auto& [t, id, x, y] : along) out.push_back({id, x, y});
  return out;
}

// The curve through (u, v) as the sketch is now (a trim may have split or taken the one there), 0: none.
int SketchEditor::curveThrough(double u, double v) const {
  const double eps = 1e-7 * (1 + std::fabs(u) + std::fabs(v));
  for (const auto& e : m_sk.entities) {
    if (e.type == ET::Line) {
      const SkPoint *a = m_sk.point(e.p[0]), *b = m_sk.point(e.p[1]);
      const double dx = b->x - a->x, dy = b->y - a->y, len2 = dx * dx + dy * dy;
      const double t = len2 < 1e-18 ? 0 : std::clamp(((u - a->x) * dx + (v - a->y) * dy) / len2, 0.0, 1.0);
      if (std::hypot(a->x + t * dx - u, a->y + t * dy - v) < eps) return e.id;
    } else if (e.type == ET::Circle || e.type == ET::Arc) {
      const SkPoint* c = m_sk.point(e.p[0]);
      Arc2 k{c->x, c->y, e.r, 0, 2 * M_PI};
      if (e.type == ET::Arc) {
        const SkPoint *s0 = m_sk.point(e.p[1]), *s1 = m_sk.point(e.p[2]);
        k.r = std::hypot(s0->x - c->x, s0->y - c->y);
        k.a0 = std::atan2(s0->y - c->y, s0->x - c->x);
        k.sweep = norm_angle(std::atan2(s1->y - c->y, s1->x - c->x) - k.a0);
      }
      if (std::fabs(std::hypot(u - k.cx, v - k.cy) - k.r) < eps && on_round(k, u, v)) return e.id;
    } else if (e.type == ET::Ellipse || e.type == ET::Spline) {  // near its samples (a fence's hit is on them)
      const auto poly = sampled(e);
      const double reach = std::max(eps, 2 * m_viewport->pixelSize());
      for (size_t i = 1; i < poly.size(); ++i) {
        const double ax = poly[i - 1].first, ay = poly[i - 1].second, dx = poly[i].first - ax, dy = poly[i].second - ay, len2 = dx * dx + dy * dy;
        const double t = len2 < 1e-18 ? 0 : std::clamp(((u - ax) * dx + (v - ay) * dy) / len2, 0.0, 1.0);
        if (std::hypot(ax + t * dx - u, ay + t * dy - v) < reach) return e.id;
      }
    }
  }
  return 0;
}

// A fence dragged with the trim tool (UI-28): every piece it crosses goes, as a click where it crosses would take it, in
// one undo step.
void SketchEditor::fenceTrim(double au, double av, double bu, double bv) {
  const auto hits = fenceHits(au, av, bu, bv);
  if (hits.empty()) return emit status(tr("Trim: the fence crosses no curve"));
  begin_change();
  int trimmed = 0;
  QString why;
  for (const auto& [id, x, y] : hits)
    if (const int now = curveThrough(x, y); now && trimPiece(now, x, y, why)) ++trimmed;
  if (!trimmed) {
    cancel_change();
    return emit status(why.isEmpty() ? tr("Trim: nothing to trim along the fence") : why);
  }
  if (end_change(tr("Trim"))) emit status(tr("Trimmed %1 pieces").arg(trimmed));
}

// A circle's or an arc's centre, radius, start angle and sweep.
static Arc2 roundOf(const Sketch& sk, const SkEntity& f) {
  const SkPoint* c = sk.point(f.p[0]);
  Arc2 k{c->x, c->y, f.r, 0, 2 * M_PI};
  if (f.type == ET::Arc) {
    const SkPoint *s0 = sk.point(f.p[1]), *s1 = sk.point(f.p[2]);
    k.r = std::hypot(s0->x - c->x, s0->y - c->y);
    k.a0 = std::atan2(s0->y - c->y, s0->x - c->x);
    k.sweep = norm_angle(std::atan2(s1->y - c->y, s1->x - c->x) - k.a0);
    if (k.sweep < 1e-12) k.sweep = 2 * M_PI;
  }
  return k;
}

// One-click extend's preview (UI-28): the end of the line or arc nearer (u, v) run on to the first curve it meets (a spline
// or an ellipse by its samples; the click asks the kernel: core extend_entity), as a polyline from the end; empty: none.
std::vector<std::pair<double, double>> SketchEditor::extendPreview(int id, double u, double v) {
  const SkEntity* e = m_sk.entity(id);
  if (!e || e->fixed || (e->type != ET::Line && e->type != ET::Arc)) return {};
  const bool line = e->type == ET::Line;
  const SkPoint *p = m_sk.point(e->p[line ? 0 : 1]), *q = m_sk.point(e->p[line ? 1 : 2]);
  const bool fromStart = std::hypot(u - p->x, v - p->y) < std::hypot(u - q->x, v - q->y);
  const std::tuple<int, bool, int> key{id, fromStart, m_modelRevision};
  if (key == m_extendKey) return m_extendShown;
  m_extendKey = key;
  m_extendShown.clear();
  const SkPoint* end = fromStart ? p : q;
  double best = 1e300;
  if (line) {  // along the line past its end: the nearest crossing
    const double length = std::hypot(q->x - p->x, q->y - p->y);
    if (length < 1e-12) return {};
    const double dx = (fromStart ? p->x - q->x : q->x - p->x) / length, dy = (fromStart ? p->y - q->y : q->y - p->y) / length;
    for (const auto& f : m_sk.entities) {
      if (f.id == id) continue;
      if (f.type == ET::Line) {
        const SkPoint *c = m_sk.point(f.p[0]), *d = m_sk.point(f.p[1]);
        const double ex = d->x - c->x, ey = d->y - c->y, den = dx * ey - dy * ex;
        if (std::fabs(den) < 1e-15) continue;
        const double t = ((c->x - end->x) * ey - (c->y - end->y) * ex) / den, s = ((c->x - end->x) * dy - (c->y - end->y) * dx) / den;
        if (t > 1e-9 && s >= -1e-9 && s <= 1 + 1e-9) best = std::min(best, t);
      } else if (f.type == ET::Circle || f.type == ET::Arc) {
        const Arc2 k = roundOf(m_sk, f);
        for (double t : line_circle(end->x, end->y, end->x + dx, end->y + dy, k.cx, k.cy, k.r))
          if (t > 1e-9 && on_round(k, end->x + t * dx, end->y + t * dy)) best = std::min(best, t);
      } else if (f.type == ET::Ellipse || f.type == ET::Spline) {  // on its samples (the click asks the kernel)
        const auto poly = sampled(f);
        for (size_t i = 1; i < poly.size(); ++i) {
          const double cx = poly[i - 1].first, cy = poly[i - 1].second, ex = poly[i].first - cx, ey = poly[i].second - cy, den = dx * ey - dy * ex;
          if (std::fabs(den) < 1e-15) continue;
          const double t = ((cx - end->x) * ey - (cy - end->y) * ex) / den, s = ((cx - end->x) * dy - (cy - end->y) * dx) / den;
          if (t > 1e-9 && s >= 0 && s <= 1) best = std::min(best, t);
        }
      }
    }
    if (best < 1e300) m_extendShown = {{end->x, end->y}, {end->x + best * dx, end->y + best * dy}};
    return m_extendShown;
  }
  // An arc: on round its circle past its end (counter-clockwise past its last point, clockwise past its first).
  const Arc2 self = roundOf(m_sk, *e);
  const double from = std::atan2(end->y - self.cy, end->x - self.cx), way = fromStart ? -1 : 1;
  auto consider = [&](double x, double y) {
    const double turn = norm_angle(way * (std::atan2(y - self.cy, x - self.cx) - from));
    if (turn > 1e-9 && turn + self.sweep < 2 * M_PI - 1e-8) best = std::min(best, turn);
  };
  for (const auto& f : m_sk.entities) {
    if (f.id == id) continue;
    if (f.type == ET::Line) {
      const SkPoint *c = m_sk.point(f.p[0]), *d = m_sk.point(f.p[1]);
      for (double s : line_circle(c->x, c->y, d->x, d->y, self.cx, self.cy, self.r))
        if (s >= -1e-9 && s <= 1 + 1e-9) consider(c->x + s * (d->x - c->x), c->y + s * (d->y - c->y));
    } else if (f.type == ET::Circle || f.type == ET::Arc) {
      const Arc2 k = roundOf(m_sk, f);
      const double dist = std::hypot(k.cx - self.cx, k.cy - self.cy);
      if (dist < 1e-12 || dist > self.r + k.r || dist < std::fabs(self.r - k.r)) continue;
      const double a = (self.r * self.r - k.r * k.r + dist * dist) / (2 * dist), h = std::sqrt(std::max(0.0, self.r * self.r - a * a));
      const double mx = self.cx + a * (k.cx - self.cx) / dist, my = self.cy + a * (k.cy - self.cy) / dist;
      for (double sgn : {1.0, -1.0}) {
        const double x = mx + sgn * h * (k.cy - self.cy) / dist, y = my - sgn * h * (k.cx - self.cx) / dist;
        if (on_round(k, x, y)) consider(x, y);
      }
    } else if (f.type == ET::Ellipse || f.type == ET::Spline) {
      const auto poly = sampled(f);
      for (size_t i = 1; i < poly.size(); ++i)
        for (double s : line_circle(poly[i - 1].first, poly[i - 1].second, poly[i].first, poly[i].second, self.cx, self.cy, self.r))
          if (s >= 0 && s <= 1) consider(poly[i - 1].first + s * (poly[i].first - poly[i - 1].first), poly[i - 1].second + s * (poly[i].second - poly[i - 1].second));
    }
  }
  if (best < 1e300)
    for (int i = 0, n = std::max(8, int(std::ceil(best / (2 * M_PI) * 96))); i <= n; ++i)
      m_extendShown.push_back({self.cx + self.r * std::cos(from + way * best * i / n), self.cy + self.r * std::sin(from + way * best * i / n)});
  return m_extendShown;
}

// What a dragged point is held to (UI-28): another point in reach (not one a curve of the dragged point ends on: that would
// collapse it), else the line, circle or arc in reach that the dragged point is not on; (x, y) where it lands.
bool SketchEditor::dropTarget(int dragged, double u, double v, double& x, double& y) {
  m_dropPoint = m_dropCurve = 0;
  // The index as it is (a large sketch's is rebuilt on a worker while the drag moves its curves): it finds what is near,
  // the distances below are the sketch's own.
  if (!m_geometry) return false;
  const double t = tol();
  std::set<int> joined{dragged};
  for (size_t index : m_geometry->curvesAt(dragged))
    if (index < m_sk.entities.size()) joined.insert(m_sk.entities[index].p.begin(), m_sk.entities[index].p.end());
  const auto around = m_geometry->query(u - t, v - t, u + t, v + t);
  double best = t;
  for (size_t index : around.points) {
    if (index >= m_sk.points.size()) continue;
    const SkPoint& p = m_sk.points[index];
    if (joined.count(p.id) || std::hypot(p.x - u, p.y - v) >= best) continue;
    best = std::hypot(p.x - u, p.y - v);
    m_dropPoint = p.id, x = p.x, y = p.y;
  }
  if (m_dropPoint) return true;
  best = t;
  for (size_t index : around.entities) {
    if (index >= m_sk.entities.size()) continue;
    const SkEntity& e = m_sk.entities[index];
    if (std::count(e.p.begin(), e.p.end(), dragged)) continue;
    double fx = u, fy = v;
    if (e.type == ET::Line) {
      const SkPoint *a = m_sk.point(e.p[0]), *b = m_sk.point(e.p[1]);
      const double dx = b->x - a->x, dy = b->y - a->y, len2 = dx * dx + dy * dy;
      const double k = len2 < 1e-18 ? 0 : std::clamp(((u - a->x) * dx + (v - a->y) * dy) / len2, 0.0, 1.0);
      fx = a->x + k * dx, fy = a->y + k * dy;
    } else if (e.type == ET::Circle || e.type == ET::Arc) {
      const Arc2 k = roundOf(m_sk, e);
      const double d = std::hypot(u - k.cx, v - k.cy);
      if (d < 1e-12) continue;
      fx = k.cx + (u - k.cx) * k.r / d, fy = k.cy + (v - k.cy) * k.r / d;
      if (!on_round(k, fx, fy)) continue;  // past the arc's ends
    } else {
      continue;
    }
    if (std::hypot(fx - u, fy - v) >= best) continue;
    best = std::hypot(fx - u, fy - v);
    m_dropCurve = e.id, x = fx, y = fy;
  }
  return m_dropCurve != 0;
}

// ---------------------------------------------------------------- mirror
void SketchEditor::mirrorSelection(int axisLine) {
  std::vector<int> sources;for(int id:m_sel)if(id!=axisLine && m_sk.entity(id))sources.push_back(id);
  const QString base=option("mirrorAxis","picked");
  runSketchEdit(tr("Mirror"),[sources,axisLine,base](Sketch& sk) {
    int axis=axisLine;
    if(!axis) {const int a=sk.add_point(0,0,true),b=sk.add_point(base=="y"?0:1,base=="y"?1:0,true);axis=sk.add_line(a,b,true);sk.entity(axis)->fixed=true;}
    const auto* line=sk.entity(axis);if(!line || line->type!=ET::Line)throw opad::Error("pick a mirror line");
    const auto a=*sk.point(line->p[0]),b=*sk.point(line->p[1]);SketchTransform t;t.mirror=true;t.angle=2*std::atan2(b.y-a.y,b.x-a.x);t.cx=a.x;t.cy=a.y;
    const auto copies=transform_entities(sk,sources,t,true);std::set<std::pair<int,int>> points;
    for(size_t i=0;i<sources.size();++i) {const auto source=*sk.entity(sources[i]),copy=*sk.entity(copies[i]);
      for(size_t k=0;k<source.p.size();++k)points.insert({source.p[k],copy.p[source.type==ET::Arc&&k>0?3-k:k]});
      if(source.type==ET::Circle)sk.add_constraint(CT::Equal,{source.id,copy.id});
    }
    for(auto [a,b]:points)sk.add_constraint(CT::Symmetric,{a,b,axis});
  });
}

// ---------------------------------------------------------------- offset, project
// Offsets the selected chain of lines and arcs by a distance (negative = the other side). The copy is plain
// geometry: dimension it, or constrain it to the original, as needed.
void SketchEditor::offsetSelection() {
  std::vector<int> ids;
  for(int id:m_sel)if(const auto* e=m_sk.entity(id);e && e->type!=ET::Point)ids.push_back(id);
  try {
    if(ids.empty())throw opad::Error("select a connected curve chain first");
    const double distance=sketch_parameters(m_sk,paramTable(m_doc->scene)).length(option("distance","5 mm").toStdString());
    const bool round=option("corners","round")=="round";
    runSketchEdit(tr("Offset"),[ids,distance,round](Sketch& sk){offset_entities(sk,ids,distance,round);});
  }catch(const std::exception& e){emit status(QString::fromUtf8(e.what()));}
}

bool SketchEditor::eventFilter(QObject* o, QEvent* e) {
  // Shift locks the pointer onto a guide (UI-19): its press and release wherever the view's keys go (seen as often as the
  // event travels up, hence the state); any other key while it is down, a value box's too, makes it no tap.
  if(m_active && (e->type()==QEvent::KeyPress || e->type()==QEvent::KeyRelease)) {
    const auto* key=static_cast<QKeyEvent*>(e);
    // Alt frees the point: pressed or let go with the mouse still, the cursor drawn (or the pointer shown) follows at once,
    // so a click goes where it is shown.
    if(key->key()==Qt::Key_Alt && !key->isAutoRepeat() && DynamicInput::takesKeysFrom(m_viewport,o))altKey(e->type()==QEvent::KeyPress);
    if(key->key()!=Qt::Key_Shift){if(e->type()==QEvent::KeyPress)m_shiftUsed=true;}
    else if(!key->isAutoRepeat() && DynamicInput::takesKeysFrom(m_viewport,o))shiftKey(e->type()==QEvent::KeyPress);
  }
  if(m_active && (e->type()==QEvent::ShortcutOverride || e->type()==QEvent::KeyPress)){
    auto* widget=qobject_cast<QWidget*>(o);auto* key=static_cast<QKeyEvent*>(e);
    if(widget && (widget==m_viewport || m_viewport->window()->isAncestorOf(widget)) && key->key()==Qt::Key_Z && key->modifiers().testFlag(Qt::ControlModifier)){
      key->accept();if(e->type()==QEvent::KeyPress){const bool forward=key->modifiers().testFlag(Qt::ShiftModifier);QTimer::singleShot(0,this,[this,forward]{if(m_active){if(forward)redo();else undo();}});}return true;
    }
    // A value typed while a tool panel (or another part of the window that is not a text field) has the keyboard is the
    // tool's too, never a window shortcut: Qt hands a tool window's unclaimed keys to the main window's shortcuts. Tab
    // stays the panel's; the view routes its own keys (Viewport::event).
    if(o!=m_viewport && key->key()!=Qt::Key_Tab && key->key()!=Qt::Key_Backtab && typingKey(key) && DynamicInput::takesKeysFrom(m_viewport,o)){
      key->accept();if(e->type()==QEvent::KeyPress)sketchType(key);return true;
    }
    // Esc in the sketch's tool panel: the view's ladder. Left to Qt, the panel's Esc and the main window's (which Qt also
    // offers a tool window's keys to) were ambiguous, and neither ran.
    if(widget && key->key()==Qt::Key_Escape && !(key->modifiers()&~Qt::KeypadModifier) && widget->window()!=m_viewport->window() && widget->window()->findChild<SketchPanel*>()){
      key->accept();if(e->type()==QEvent::KeyPress)escape();return true;
    }
  }
  if (o == m_dimEdit && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
    m_dimEdit->hide();
    m_dimEditing = 0;
    m_viewport->setFocus();
    rebuild();
    return true;
  }
  return QObject::eventFilter(o, e);
}

// ---------------------------------------------------------------- bench
// Drives the tools the way the mouse does (sketch coordinates instead of pixels), so a headless run covers the
// same code as a user: a 40 x 25 rectangle from the origin with a hole, width and height dimensioned.
void SketchEditor::bench(const QString&) {
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_HANDLES"))return benchHandles();
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_REFERENCE"))return benchWorkflow();
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_DRAG"))return benchDrag();
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_MODIFY"))return benchModify();
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_PRIMITIVES"))return benchPrimitives();
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_WORKFLOW"))return benchWorkflow();
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_SPLINE");!shot.isEmpty()) {
    m_viewport->setCameraJson({{"eye",{20,8,100}},{"target",{20,8,0}},{"up",{0,1,0}},{"scale",65},{"projection","orthographic"},{"absolute",true}});
    m_viewport->setGridSnap(false);setTool("spline");
    for(const auto& p:std::vector<std::pair<double,double>>{{0,0},{12,18},{30,3}}) {sketchMove(p.first,p.second,Qt::NoModifier,false);sketchPress(p.first,p.second,Qt::NoModifier);}
    sketchMove(45,14,Qt::NoModifier,false);m_viewport->grabImage().save(shot+".live.png");
    finishChain();const auto id=m_sk.entities.back().id;const auto count=m_sk.entity(id)->p.size();
    BRepAdaptor_Curve curve(entity_edge(m_sk,*m_sk.entity(id),opad::Frame{}));auto at=curve.Value(0.6);
    setTool("select");sketchPress(at.X(),at.Y(),Qt::AltModifier);
    const bool inserted=m_sk.entity(id)->p.size()>count;
    m_sk.entity(id)->weights[1]=0.6;m_sk.entity(id)->weights[2]=1.8;rebuild();
    m_viewport->grabImage().save(shot+".nodes.png");findOpenVertices();
    trace::log(QString("bench: spline live preview / insert node / asymmetric weights %1").arg(inserted?"PASS":"FAIL"));
    QTimer::singleShot(500,this,[this,inserted]{const bool ends=m_dangling.size()==2;trace::log(QString("bench: spline open ends %1").arg(ends?"PASS":"FAIL"));m_doc->newDocument();const bool reset=!m_active;trace::log(QString("bench: active sketch document reset %1").arg(reset?"PASS":"FAIL"));QCoreApplication::exit(inserted && ends && reset?0:2);});
    return;
  }

  setTool("rect");  // grid snapping takes the points a tool places (a pick, the select tool's, is where the pointer is)
  const bool grid=m_viewport->gridSnap(); const double step=m_viewport->gridStep();
  m_viewport->setGridSnap(true); const auto snapped=snap(1.24*step,2.34*step);
  if(std::abs(snapped.u-step)>1e-9 || std::abs(snapped.v-2*step)>1e-9) throw opad::Error("grid snap missed its lattice");
  m_viewport->setGridSnap(false); const auto free=snap(1.24*step,2.34*step);
  if(std::abs(free.u-1.24*step)>1e-9) throw opad::Error("disabled grid snap changed a free point");
  m_viewport->setGridSnap(grid);
  trace::log(QStringLiteral("bench: grid snapping toggle PASS"));
  auto press = [this](double u, double v) {
    sketchMove(u, v, Qt::NoModifier, false);
    sketchPress(u, v, Qt::NoModifier);
    sketchRelease(u, v, Qt::NoModifier);
  };
  auto type = [this](const QString& text) {
    if (!m_dimEdit || !m_dimEditing) return trace::log(QStringLiteral("bench: sketch: no dimension editor open"));
    m_dimEdit->setText(text);
    commitDimensionEdit();
  };
  setTool("rect");
  press(0, 0);
  press(37, 22);
  setTool("circle");
  press(20, 12);
  press(26, 12);
  setTool("dimension");
  press(18, 0);   // the bottom line
  press(18, -9);  // place
  type("40");
  press(37, 10);  // the right line (after the solve it is at u = 40; still within reach of the click? re-pick by hit test)
  if (!m_placingDim) press(40, 10);
  press(49, 10);
  type("25 mm");
  press(26, 12);  // the circle's rim
  if (!m_placingDim) press(20 + 6, 12);
  press(32, 20);
  type("12");
  // Enter on the value box as it opened changes nothing, also where the shown unit rounds (12 mm reads 0.472441 in).
  for (auto it = m_sk.constraints.rbegin(); it != m_sk.constraints.rend(); ++it) {
    if (!it->is_dimension()) continue;
    const int id = it->id;
    const double before = it->value;
    const size_t steps = m_undo.size();
    units::setSessionUnit("in");
    editDimension(id, false);
    commitDimensionEdit();
    units::setSessionUnit({});
    const bool kept = m_sk.constraint(id)->value == before && m_undo.size() == steps;
    trace::log(QStringLiteral("bench: sketch: Enter on a dimension as shown in inches keeps %1 mm %2").arg(m_sk.constraint(id)->value, 0, 'g', 17).arg(kept ? "PASS" : "FAIL"));
    break;
  }
  setTool("select");
  trace::log(QStringLiteral("bench: sketch: %1 entities, %2 constraints, dof %3").arg(m_sk.entities.size()).arg(m_sk.constraints.size()).arg(m_solved.dof));
}
