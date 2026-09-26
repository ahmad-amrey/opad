#include "opad/design/sketch_edit.hpp"
#include "opad/design/sketch_pattern.hpp"
#include "opad/design/sketch_modify.hpp"
// SketchEditor, the tools: what a click means for each of them, constraints, dimensions, fillet, trim, mirror.
#include "SketchEditor.hpp"
#include "DimensionHandle.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <QInputDialog>
#include <QKeyEvent>
#include <QApplication>
#include <cmath>

#include "I18n.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/sketch_geom.hpp"

using namespace opad::design;
using CT = SkConstraint::Type;
using ET = SkEntity::Type;

namespace {

ParamTable paramTable(const opad::Scene& scene) {
  std::vector<ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return ParamTable(defs,scene.units);
}

QString trimmedNumber(double v, int decimals) {
  QString s = QString::number(v, 'f', decimals);
  if (s.contains('.')) {
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
  }
  return s == "-0" ? QStringLiteral("0") : s;
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
  m_clicks.clear();
  m_chain.clear();
  m_picked.clear();
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
  const QStringList preserve={"mirror","offset","node","move","rotate","scale","copy","rect_pattern","polar_pattern","explode","chamfer","break","break_link"};
  if(!preserve.contains(tool))m_sel.clear();
  if((tool=="rect_pattern"||tool=="polar_pattern")&&!m_sel.empty()) {
    const int id=pattern_of(m_sk,m_sel.front(),true);
    for(const auto& p:m_sk.patterns)if(p.at("id").get<int>()==id)for(const auto& [key,value]:p.at("inputs").items())if(key!="polar")m_options[QString::fromStdString(key)]=QString::fromStdString(value.is_string()?value.get<std::string>():value.dump());
  }
  emit toolChanged(m_tool);
  toolPrompt();
  rebuild();
  scheduleToolPreview();
}

void SketchEditor::toolPrompt() {
  QString t;
  const int n = static_cast<int>(m_clicks.size());
  if (m_tool == "select") t = tr("Select or drag geometry · double-click a dimension to change it · Del deletes · X construction");
  else if (m_tool == "line") t = m_chain.empty() ? tr("Line: click the start point") : tr("Line: click the next point · Enter or double-click ends the chain · Esc");
  else if (m_tool == "rect") t = n == 0 ? tr("Rectangle: click the first corner") : tr("Rectangle: click the opposite corner");
  else if (m_tool == "crect") t = n == 0 ? tr("Centre rectangle: click the centre") : tr("Centre rectangle: click a corner");
  else if (m_tool == "circle") t = n == 0 ? tr("Circle: click the centre") : tr("Circle: click a point on the circle");
  else if (m_tool == "circle3") t = tr("3-point circle: click point %1 of 3").arg(n + 1);
  else if (m_tool == "arc3") t = n == 0 ? tr("3-point arc: click the start") : n == 1 ? tr("3-point arc: click the end") : tr("3-point arc: click a point on the arc");
  else if (m_tool == "arcc") t = n == 0 ? tr("Centre arc: click the centre") : n == 1 ? tr("Centre arc: click the start") : tr("Centre arc: click the end");
  else if (m_tool == "polygon") t = n == 0 ? tr("Polygon: click the centre") : tr("Polygon: click a corner");
  else if (m_tool == "slot") t = n == 0 ? tr("Slot: click the first centre") : n == 1 ? tr("Slot: click the second centre") : tr("Slot: click to set the width");
  else if (m_tool == "ellipse") t = n == 0 ? tr("Ellipse: click the centre") : n == 1 ? tr("Ellipse: click the end of the first axis") : tr("Ellipse: click to set the second axis");
  else if (m_tool == "point") t = tr("Point: click to place (holes are drilled at sketch points)");
  else if (m_tool == "spline") t = tr("Spline: click nodes; Enter finishes. Alt-click a finished spline to insert a node; double-click a node to edit weights.");
  else if (m_tool == "fillet") t = tr("Sketch fillet: click the corner where two lines meet");
  else if (m_tool == "trim") t = tr("Trim: click the part of a curve to remove");
  else if (m_tool == "mirror") t = tr("Mirror: click the mirror line");
  else if (m_tool == "project") t = tr("Project: click straight or circular edges of bodies; they become fixed reference curves");
  else if (m_tool == "dimension") t = m_placingDim ? tr("Dimension: click where the value should sit (or pick a second entity)") : tr("Dimension: pick a line, a circle, an arc, or two points");
  else if (m_tool.startsWith("c:")) t = tr("%1: pick the geometry it applies to").arg(i18n::t(m_tool.mid(2).left(1).toUpper() + m_tool.mid(3)));
  emit status(t);
  emit workflowChanged();
}

// ---------------------------------------------------------------- clicks
void SketchEditor::finishChain() {
  if (m_tool == "spline" && m_chain.size() >= 2) {
    begin_change();
    add_cubic_spline(m_sk,m_chain);
    end_change(tr("Spline"));
  } else if (m_chain.size() == 1) {
    // A start point that never got its segment.
    m_sk.remove(m_chain.front());
  }
  if(!m_chain.empty() && m_undo.size()>m_chainUndoStart+1)m_undo.erase(m_undo.begin()+m_chainUndoStart+1,m_undo.end());
  m_chain.clear();
  m_clicks.clear();
  toolPrompt();
  rebuild();
}

void SketchEditor::click(const Snap& s, Qt::KeyboardModifiers) {
  invalidatePreview();
  if(imageClick(s.u,s.v))return;
  if(m_tool=="project"||m_tool=="intersect_body"||m_tool=="silhouette"||m_tool=="include3d")return pickReference();
  if(modifyClick(s.u,s.v))return;
  if(primitiveClick(s.u,s.v))return;
  const Hit hit = hitTest(s.u, s.v);
  if (m_tool.startsWith("c:")) return constraintClick(hit);
  if (m_tool == "dimension") return dimensionClick(hit, s.u, s.v);
  if (m_tool == "project") return projectHovered();
  if (m_tool == "offset" || m_tool == "node") {
    const Hit h=hitTest(s.u,s.v);
    if(h.kind!=Hit::None) {
      auto it=std::find(m_sel.begin(),m_sel.end(),h.id);
      if(it==m_sel.end())m_sel.push_back(h.id);else m_sel.erase(it);
      if(m_tool=="offset" && option("chain","1")=="1")selectConnected();
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
      end_change(tr("Point"));
      return;
    }
    const int line = m_sk.add_line(m_chain.back(), p);
    if (s.horizontal) m_sk.add_constraint(CT::Horizontal, {line});
    if (s.vertical) m_sk.add_constraint(CT::Vertical, {line});
    const bool closes = p == m_chain.front() && m_chain.size() > 1;
    if (!end_change(tr("Line"))) return;
    m_chain.push_back(p);
    if (closes) {  // back at the start: the profile is closed, the chain is done
      finishChain();
    }
    return;
  }

  m_clicks.push_back(s);
  const size_t n = m_clicks.size();
  auto done = [&](const QString& what) {
    const bool accepted=end_change(what);
    m_clicks.clear();
    if(accepted)toolPrompt();
  };
  auto rectangle = [&](double x0, double y0, double x1, double y1, const Snap* first, const Snap* second) {
    const int a = first ? pointFor(*first) : m_sk.add_point(x0, y0);
    const int b = m_sk.add_point(x1, y0);
    const int c = second ? pointFor(*second) : m_sk.add_point(x1, y1);
    const int d = m_sk.add_point(x0, y1);
    const int l0 = m_sk.add_line(a, b), l1 = m_sk.add_line(b, c), l2 = m_sk.add_line(c, d), l3 = m_sk.add_line(d, a);
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
    rectangle(a.u, a.v, c.u, c.v, &a, &c);
    return done(tr("Rectangle"));
  }
  if (m_tool == "crect" && n == 2) {
    const Snap &o = m_clicks[0], &c = m_clicks[1];
    const double w = std::fabs(c.u - o.u), h = std::fabs(c.v - o.v);
    if (w < 1e-6 || h < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    const auto corners = rectangle(o.u - w, o.v - h, o.u + w, o.v + h, nullptr, nullptr);
    const int centre = pointFor(o);
    const int diagonal = m_sk.add_line(corners[0], corners[2], true);
    m_sk.add_constraint(CT::Midpoint, {centre, diagonal});
    return done(tr("Rectangle"));
  }
  if (m_tool == "circle" && n == 2) {
    const double r = std::hypot(s.u - m_clicks[0].u, s.v - m_clicks[0].v);
    if (r < 1e-6) { m_clicks.pop_back(); return; }
    begin_change();
    const int circle = m_sk.add_circle(pointFor(m_clicks[0]), r);
    if (s.point) m_sk.add_constraint(CT::Coincident, {s.point, circle});
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
      return done(tr("Circle"));
    }
    // Start and end were clicked first; the third point says which way round the arc goes.
    const double a0 = std::atan2(ay - uy, ax - ux), a1 = std::atan2(by - uy, bx - ux), am = std::atan2(cy - uy, cx - ux);
    const bool ccw = norm_angle(am - a0) < norm_angle(a1 - a0);
    const int ps = pointFor(m_clicks[0]), pe = pointFor(m_clicks[1]);
    m_sk.add_arc(centre, ccw ? ps : pe, ccw ? pe : ps);
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
    m_sk.add_arc(centre, sweep > 0 ? ps : pe, sweep > 0 ? pe : ps);
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
    m_sk.add_line(p1, p2, true);
    for (int line : {top, bottom})
      for (int cap : {cap1, cap2}) m_sk.add_constraint(CT::Tangent, {line, cap});
    m_sk.add_constraint(CT::Equal, {cap1, cap2});
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
    return done(tr("Ellipse"));
  }
  toolPrompt();
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
    case CT::Curvature:
      if(splines.size()==2)sets.push_back(splines);
      break;
    case CT::Tangent:
      if(splines.size()==2)sets.push_back(splines);
      else if(splines.size()==1 && lines.size()==1)sets.push_back({lines[0],splines[0]});
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
  } else if (m_picked.size() >= 3) {
    m_picked.clear();
    emit status(tr("That constraint does not fit what was picked; pick again."));
  }
  rebuild();
}

// ---------------------------------------------------------------- dimensions
QString SketchEditor::dimensionText(const SkConstraint& c) const {
  QString value = c.type == CT::Angle ? trimmedNumber(c.value * 180.0 / M_PI, 2) + QString::fromUtf8("°") : trimmedNumber(c.value / ParamTable({},m_doc->scene.units).length("1"), 3) + " " + QString::fromStdString(m_doc->scene.units);
  if (c.type == CT::Radius) value = "R" + value;
  if (c.type == CT::Diameter) value = QString::fromUtf8("Ø") + value;
  bool plain = false;
  QString::fromStdString(c.expr).toDouble(&plain);
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
  if (!m_dimEdit) m_dimEdit = new QLineEdit(m_viewport);
  if (m_tool != "dimension") setTool("dimension");

  m_dimEditing = id;
  m_dimFresh = fresh;
  const QString shown = !c->expr.empty() ? QString::fromStdString(c->expr) : c->type == CT::Angle ? trimmedNumber(c->value * 180 / M_PI, 4) + " deg" : trimmedNumber(c->value, 4) + " mm";
  m_dimEdit->setText(shown);
  m_options["expression"] = shown;
  m_options["reference"] = c->reference ? "1" : "0";
  m_panelFieldsDirty = true;
  emit toolChanged(m_tool);
  emit workflowChanged();

  rebuild();
}

void SketchEditor::commitDimensionEdit() {
  if (!m_dimEdit || !m_dimEditing) return;
  const QString text = m_dimEdit->text().trimmed();
  m_dimEdit->hide();
  m_viewport->setFocus();
  SkConstraint* c = m_sk.constraint(m_dimEditing);
  if (!c || text.isEmpty()) return rebuild();
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
  bool plain = false;
  text.toDouble(&plain);
  const std::string expr = plain ? std::string() : c->type==CT::Angle?text.toStdString():sketch_parameters(m_sk,paramTable(m_doc->scene)).explicit_length(text.toStdString());
  if (std::fabs(value - c->value) < 1e-12 && expr == c->expr && c->reference == (option("reference", "0") == "1")) return rebuild();
  const int id = c->id;
  begin_change();
  c = m_sk.constraint(id);
  c->value = value;
  c->expr = expr;
  c->reference = option("reference", "0") == "1";
  if (c->reference) c->expr.clear();
  if(end_change(tr("Dimension"))) {
    if(m_dimFresh && m_undo.size()>1)m_undo.pop_back(); // placement and its value are one user edit
    m_dimFresh=false;m_dimEditing=0;
  }
  m_panelFieldsDirty = true; emit workflowChanged();
}

// ---------------------------------------------------------------- sketch fillet
void SketchEditor::filletAt(const Hit& h, double, double) {
  if (h.kind != Hit::Point) return emit status(tr("Sketch fillet: click the corner point where two lines meet"));
  std::vector<SkEntity*> lines;
  for (auto& e : m_sk.entities)
    if (e.type == ET::Line && (e.p[0] == h.id || e.p[1] == h.id)) lines.push_back(&e);
  if (lines.size() != 2) return emit status(tr("Sketch fillet: exactly two lines must meet at that point"));
  const QString text = option("radius", "2 mm");
  double r = 0;
  try {
    r = paramTable(m_doc->scene).length(text.toStdString());
  } catch (const std::exception& e) {
    return emit status(i18n::t(QString::fromUtf8(e.what())));
  }
  const SkPoint corner = *m_sk.point(h.id);
  auto other = [&](const SkEntity* l) { return m_sk.point(l->p[0] == h.id ? l->p[1] : l->p[0]); };
  const SkPoint *A = other(lines[0]), *B = other(lines[1]);
  const double l1 = std::hypot(A->x - corner.x, A->y - corner.y), l2 = std::hypot(B->x - corner.x, B->y - corner.y);
  if (l1 < 1e-9 || l2 < 1e-9) return;
  const double d1x = (A->x - corner.x) / l1, d1y = (A->y - corner.y) / l1, d2x = (B->x - corner.x) / l2, d2y = (B->y - corner.y) / l2;
  const double theta = std::acos(std::clamp(d1x * d2x + d1y * d2y, -1.0, 1.0));
  if (theta < 1e-6 || theta > M_PI - 1e-6) return emit status(tr("Sketch fillet: the lines are parallel"));
  const double t = r / std::tan(theta / 2);
  if (r <= 0 || t >= l1 || t >= l2) return emit status(tr("Sketch fillet: that radius does not fit these lines"));
  double bx = d1x + d2x, by = d1y + d2y;
  const double bl = std::hypot(bx, by);
  bx /= bl;
  by /= bl;
  const double cd = r / std::sin(theta / 2);
  const int line1 = lines[0]->id, line2 = lines[1]->id;
  begin_change();
  const int t1 = m_sk.add_point(corner.x + d1x * t, corner.y + d1y * t), t2 = m_sk.add_point(corner.x + d2x * t, corner.y + d2y * t);
  const int centre = m_sk.add_point(corner.x + bx * cd, corner.y + by * cd);
  for (const auto& [line, tp] : {std::pair{line1, t1}, std::pair{line2, t2}}) {
    SkEntity* l = m_sk.entity(line);
    (l->p[0] == h.id ? l->p[0] : l->p[1]) = tp;
  }
  const double a1 = std::atan2(m_sk.point(t1)->y - m_sk.point(centre)->y, m_sk.point(t1)->x - m_sk.point(centre)->x);
  const double a2 = std::atan2(m_sk.point(t2)->y - m_sk.point(centre)->y, m_sk.point(t2)->x - m_sk.point(centre)->x);
  const bool ccw = norm_angle(a2 - a1) < M_PI;
  const int arc = m_sk.add_arc(centre, ccw ? t1 : t2, ccw ? t2 : t1);
  m_sk.add_constraint(CT::Tangent, {line1, arc});
  m_sk.add_constraint(CT::Tangent, {line2, arc});
  bool plain = false;
  text.toDouble(&plain);
  m_sk.add_constraint(CT::Radius, {arc}, r, plain ? std::string() : text.toStdString());
  m_sk.remove(h.id);  // the old corner, unless something else still uses it
  end_change(tr("Sketch fillet"));
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
}  // namespace

void SketchEditor::trimAt(const Hit& h, double u, double v) {
  SkEntity* target = h.kind == Hit::Entity ? m_sk.entity(h.id) : nullptr;
  if (!target || (target->type != ET::Line && target->type != ET::Circle && target->type != ET::Arc)) return emit status(tr("Trim: click a line, a circle or an arc"));
  auto P = [&](int id) { return m_sk.point(id); };
  struct Round { double cx, cy, r, a0, sweep; };  // sweep 2 pi = full circle
  auto round_of = [&](const SkEntity& e) {
    Round k{P(e.p[0])->x, P(e.p[0])->y, e.r, 0, 2 * M_PI};
    if (e.type == ET::Arc) {
      k.r = std::hypot(P(e.p[1])->x - k.cx, P(e.p[1])->y - k.cy);
      k.a0 = std::atan2(P(e.p[1])->y - k.cy, P(e.p[1])->x - k.cx);
      k.sweep = norm_angle(std::atan2(P(e.p[2])->y - k.cy, P(e.p[2])->x - k.cx) - k.a0);
      if (k.sweep < 1e-12) k.sweep = 2 * M_PI;
    }
    return k;
  };
  auto on_round = [&](const Round& k, double x, double y) { return k.sweep >= 2 * M_PI - 1e-12 || norm_angle(std::atan2(y - k.cy, x - k.cx) - k.a0) <= k.sweep + 1e-9; };
  const double eps = 1e-7;

  std::vector<Cut> cuts;
  const bool isLine = target->type == ET::Line;
  const Round self = isLine ? Round{} : round_of(*target);
  const double ax = isLine ? P(target->p[0])->x : 0, ay = isLine ? P(target->p[0])->y : 0, bx = isLine ? P(target->p[1])->x : 0, by = isLine ? P(target->p[1])->y : 0;
  for (const auto& o : m_sk.entities) {
    if (o.id == target->id) continue;
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
      const Round k = round_of(o);
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

  begin_change();
  const int id = target->id;
  // A length dimension on the trimmed curve would now mean something else.
  std::vector<int> stale;
  for (const auto& c : m_sk.constraints)
    if (c.is_dimension() && c.type == CT::Distance && c.refs.size() == 1 && c.refs[0] == id) stale.push_back(c.id);
  for (int c : stale) m_sk.remove(c);
  auto cut_point = [&](double x, double y, int other) {
    const int p = m_sk.add_point(x, y);
    m_sk.add_constraint(CT::Coincident, {p, other});
    return p;
  };
  if (cuts.empty()) {
    m_sk.remove(id);
    end_change(tr("Trim"));
    return;
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
  } else {
    const double tc = norm_angle(std::atan2(v - self.cy, u - self.cx) - self.a0);
    auto at = [&](double t, double& x, double& y) { x = self.cx + self.r * std::cos(self.a0 + t); y = self.cy + self.r * std::sin(self.a0 + t); };
    const bool full = target->type == ET::Circle;
    if (full) {
      if (cuts.size() < 2) { cancel_change(); return emit status(tr("Trim: the circle is crossed only once; nothing to cut between")); }
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
    } else {
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
    }
  }
  end_change(tr("Trim"));
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

// Body edges as fixed reference curves in the sketch (not associative: project again after the body changes).
void SketchEditor::projectHovered() {
  TopoDS_Shape shape;
  if (!m_viewport->hoveredEdge(shape)) return emit status(tr("Project: point at an edge of a body and click"));
  BRepAdaptor_Curve c(TopoDS::Edge(shape));
  auto local = [&](const gp_Pnt& p, double& u, double& v) { m_frame.to_local({p.X(), p.Y(), p.Z()}, u, v); };
  const opad::Vec3 n = m_frame.normal();
  double au, av, bu, bv;
  local(c.Value(c.FirstParameter()), au, av);
  local(c.Value(c.LastParameter()), bu, bv);
  begin_change();
  if (c.GetType() == GeomAbs_Line) {
    if (std::hypot(bu - au, bv - av) < 1e-7) {
      cancel_change();
      return emit status(tr("Project: that edge is perpendicular to the sketch plane"));
    }
    const int line = m_sk.add_line(m_sk.add_point(au, av, true), m_sk.add_point(bu, bv, true));
    m_sk.entity(line)->fixed = true;
  } else if (c.GetType() == GeomAbs_Circle && std::fabs(std::fabs(c.Circle().Axis().Direction().Dot(gp_Dir(n[0], n[1], n[2]))) - 1.0) < 1e-9) {
    double cu, cv;
    local(c.Circle().Location(), cu, cv);
    const int centre = m_sk.add_point(cu, cv, true);
    int made = 0;
    if (std::hypot(bu - au, bv - av) < 1e-7) {
      made = m_sk.add_circle(centre, c.Circle().Radius());
    } else {
      double mu, mv;  // which way round: the arc's mid point tells
      local(c.Value((c.FirstParameter() + c.LastParameter()) / 2), mu, mv);
      const double a0 = std::atan2(av - cv, au - cu), a1 = std::atan2(bv - cv, bu - cu), am = std::atan2(mv - cv, mu - cu);
      const bool ccw = norm_angle(am - a0) < norm_angle(a1 - a0);
      const int ps = m_sk.add_point(au, av, true), pe = m_sk.add_point(bu, bv, true);
      made = m_sk.add_arc(centre, ccw ? ps : pe, ccw ? pe : ps);
    }
    m_sk.entity(made)->fixed = true;
  } else {
    cancel_change();
    return emit status(tr("Project: only straight edges, and circles parallel to the sketch plane, can be projected"));
  }
  end_change(tr("Project"));
}

bool SketchEditor::eventFilter(QObject* o, QEvent* e) {
  if(m_active && (e->type()==QEvent::ShortcutOverride || e->type()==QEvent::KeyPress)){
    auto* widget=qobject_cast<QWidget*>(o);auto* key=static_cast<QKeyEvent*>(e);
    if(widget && (widget==m_viewport || m_viewport->window()->isAncestorOf(widget)) && key->key()==Qt::Key_Z && key->modifiers().testFlag(Qt::ControlModifier)){
      key->accept();if(e->type()==QEvent::KeyPress){const bool forward=key->modifiers().testFlag(Qt::ShiftModifier);QTimer::singleShot(0,this,[this,forward]{if(m_active){if(forward)redo();else undo();}});}return true;
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
  setTool("select");
  trace::log(QStringLiteral("bench: sketch: %1 entities, %2 constraints, dof %3").arg(m_sk.entities.size()).arg(m_sk.constraints.size()).arg(m_solved.dof));
}
