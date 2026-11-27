// Viewport, Align's point pairs (DesignController "point pairs"): a numbered dot at each picked point, the bodies' points in
// one colour and the target's in another, and a line from each From point (where the preview draws it) to its To point.
#include <AIS_InteractiveObject.hxx>
#include <AIS_TextLabel.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Prs3d_Presentation.hxx>
#include <SelectMgr_Selection.hxx>
#include <TCollection_ExtendedString.hxx>

#include "BodyShape.hpp"
#include "Theme.hpp"
#include "Viewport.hpp"

namespace {

Quantity_Color occColor(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }

// Every pair's dots and lines as one object: dots by colour, the lines dashed between them.
class PairMarks : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(PairMarks, AIS_InteractiveObject)
 public:
  struct Dot {
    gp_Pnt at;
    Quantity_Color color;
  };
  PairMarks(std::vector<Dot> dots, std::vector<std::pair<gp_Pnt, gp_Pnt>> lines, const Quantity_Color& lineColor, double scale)
      : m_dots(std::move(dots)), m_lines(std::move(lines)), m_lineColor(lineColor), m_scale(scale) {}

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    if (!m_lines.empty()) {
      Handle(Graphic3d_ArrayOfSegments) segs = new Graphic3d_ArrayOfSegments(int(2 * m_lines.size()));
      for (const auto& [a, b] : m_lines) segs->AddVertex(a), segs->AddVertex(b);
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_lineColor, Aspect_TOL_DASH, 1.5 * m_scale));
      g->AddPrimitiveArray(segs);
    }
    for (const Dot& d : m_dots) {  // a group per dot: each its own colour
      Handle(Graphic3d_ArrayOfPoints) points = new Graphic3d_ArrayOfPoints(1);
      points->AddVertex(d.at);
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, d.color, 2.4 * m_scale));
      g->AddPrimitiveArray(points);
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  std::vector<Dot> m_dots;
  std::vector<std::pair<gp_Pnt, gp_Pnt>> m_lines;
  Quantity_Color m_lineColor;
  double m_scale;
};

}  // namespace

void Viewport::setPairMarkers(const std::vector<PairMarker>& pairs) {
  if (!m_initialised) return;
  for (const auto& o : m_pairMarks) m_ctx->Remove(o, Standard_False);
  m_pairMarks.clear();
  m_pairMarkers = pairs;
  const Tokens& t = theme::current();
  const double scale = displayScale();
  std::vector<PairMarks::Dot> dots;
  std::vector<std::pair<gp_Pnt, gp_Pnt>> lines;
  auto label = [&](const gp_Pnt& at, const QString& text, const QColor& plate) {
    Handle(AIS_TextLabel) l = new AIS_TextLabel();
    l->SetText(TCollection_ExtendedString((" " + text + " ").toUtf8().constData(), Standard_True));
    l->SetPosition(at);
    l->SetColor(Quantity_Color(1, 1, 1, Quantity_TOC_sRGB));
    l->SetHeight(12 * scale);
    l->SetHJustification(Graphic3d_HTA_LEFT);
    l->SetVJustification(Graphic3d_VTA_BOTTOM);
    l->SetDisplayType(Aspect_TODT_SUBTITLE);  // on a plate in the side's colour: which side and which pair at a glance
    l->SetColorSubTitle(occColor(plate.darker(140)));
    m_pairMarks.push_back(l);
  };
  for (const PairMarker& p : pairs) {
    const QColor from = p.stray ? t.red : t.sel, to = p.stray ? t.red : t.green;
    if (p.from) {
      const gp_Pnt a((*p.from)[0], (*p.from)[1], (*p.from)[2]);
      dots.push_back({a, occColor(from)});
      label(a, p.label, from);
    }
    if (p.to) {
      const gp_Pnt b((*p.to)[0], (*p.to)[1], (*p.to)[2]);
      dots.push_back({b, occColor(to)});
      label(b, p.label, to);
    }
    if (p.from && p.to) lines.push_back({gp_Pnt((*p.from)[0], (*p.from)[1], (*p.from)[2]), gp_Pnt((*p.to)[0], (*p.to)[1], (*p.to)[2])});
  }
  if (!dots.empty()) m_pairMarks.insert(m_pairMarks.begin(), new PairMarks(std::move(dots), std::move(lines), occColor(t.fg2), scale));
  for (const auto& o : m_pairMarks) {
    o->SetInfiniteState(Standard_True);  // Fit never frames it
    o->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(o, 0, -1, Standard_False);  // never picked: no selection mode
  }
  redrawScene();
}

void Viewport::setGuideLine(const std::optional<std::pair<opad::Vec3, opad::Vec3>>& line) {
  if (!m_initialised || line == m_guideLineEnds) return;
  if (!m_guideLine.IsNull()) m_ctx->Remove(m_guideLine, Standard_False);
  m_guideLine.Nullify();
  m_guideLineEnds = line;
  if (line) {
    const Tokens& t = theme::current();
    const gp_Pnt a(line->first[0], line->first[1], line->first[2]), b(line->second[0], line->second[1], line->second[2]);
    Handle(PairMarks) l = new PairMarks({{a, occColor(t.sel)}, {b, occColor(t.sel)}}, {{a, b}}, occColor(t.sel), displayScale() * 1.6);
    l->SetInfiniteState(Standard_True);
    l->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(l, 0, -1, Standard_False);
    m_guideLine = l;
  }
  redrawScene();
}

bool Viewport::hoveredMeshSide(std::string& body, int& side) const {
  if (!m_initialised || !m_ctx->HasDetected()) return false;
  const auto found = m_nodeOf.find(m_ctx->DetectedInteractive().get());
  if (found == m_nodeOf.end()) return false;
  const auto owner = Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner());
  if (owner.IsNull() || owner->kind() != opad::Ref::Kind::Edge) return false;
  body = found->second;
  side = owner->index();
  return true;
}

opad::json Viewport::benchPairMarkers() const {
  opad::json out = opad::json::array();
  for (const PairMarker& p : m_pairMarkers) {
    opad::json j = {{"label", p.label.toStdString()}, {"stray", p.stray}};
    if (p.from) j["from"] = *p.from;
    if (p.to) j["to"] = *p.to;
    if (p.from && p.to) j["line"] = std::hypot((*p.from)[0] - (*p.to)[0], (*p.from)[1] - (*p.to)[1], (*p.from)[2] - (*p.to)[2]);
    out.push_back(j);
  }
  return out;
}
