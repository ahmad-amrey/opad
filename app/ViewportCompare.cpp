// Viewport, Compare (UI-58): another version's bodies and the moves between the two, drawn with the model and never
// picked. The parts are shaded from arrays built off the UI thread (BodyShape), so showing them walks no triangulation
// here; a new style changes their aspects in place (SynchronizeAspects), never Redisplay.
#include "Viewport.hpp"

#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Arrow.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_Presentation.hxx>
#include <TopLoc_Location.hxx>

#include <algorithm>
#include <cmath>

#include "opad/geometry.hpp"

namespace {
Quantity_Color rgb(const std::array<double, 3>& c) {
  return Quantity_Color(std::clamp(c[0], 0.0, 1.0), std::clamp(c[1], 0.0, 1.0), std::clamp(c[2], 0.0, 1.0), Quantity_TOC_sRGB);
}

// Dashed segments from each old centre to the new one, with a solid head at the new end. Sized in the world: the head is
// a fifth of the arrow, so a short move keeps a head that fits it.
class CompareArrows : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(CompareArrows, AIS_InteractiveObject)
 public:
  CompareArrows(const std::vector<Viewport::CompareArrow>& arrows, const Quantity_Color& color) : m_arrows(arrows), m_color(color) {}
  void setArrows(const std::vector<Viewport::CompareArrow>& arrows) { m_arrows = arrows; }
  size_t shown() const { return size_t(std::count_if(m_arrows.begin(), m_arrows.end(), [](const auto& a) { return a.visible; })); }

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    std::vector<std::pair<gp_Pnt, gp_Pnt>> lines;
    for (const auto& a : m_arrows) {
      const gp_Pnt from(a.from[0], a.from[1], a.from[2]), to(a.to[0], a.to[1], a.to[2]);
      if (a.visible && from.Distance(to) > 1e-9) lines.emplace_back(from, to);
    }
    if (lines.empty()) return;
    Handle(Graphic3d_ArrayOfSegments) dashes = new Graphic3d_ArrayOfSegments(int(lines.size()) * 2);
    for (const auto& [from, to] : lines) {
      dashes->AddVertex(from);
      dashes->AddVertex(to);
    }
    Handle(Graphic3d_Group) group = prs->NewGroup();
    group->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_color, Aspect_TOL_DASH, 2.0));
    group->AddPrimitiveArray(dashes);
    Handle(Graphic3d_Group) heads = prs->NewGroup();
    heads->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_color, Aspect_TOL_SOLID, 2.0));
    for (const auto& [from, to] : lines) {
      const double length = from.Distance(to);
      heads->AddPrimitiveArray(Prs3d_Arrow::DrawSegments(to, gp_Dir(gp_Vec(from, to)), 0.35, length * 0.2, 12));
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  std::vector<Viewport::CompareArrow> m_arrows;
  Quantity_Color m_color;
};
}  // namespace

void Viewport::styleComparePart(const Handle(AIS_Shape)& ais, const ComparePart& part) {
  ais->SetColor(rgb(part.color));
  ais->SetTransparency(1.0 - std::clamp(part.opacity, 0.0, 1.0));
  // Line aspects ignore alpha: a ghost's edges are blended towards the background instead, so they fade with it.
  const auto edge = looks::mix(looks::mix(part.color, {0, 0, 0}, 0.35), {m_tokens.vp.redF(), m_tokens.vp.greenF(), m_tokens.vp.blueF()},
                               1 - std::clamp(part.opacity, 0.0, 1.0));
  ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
  ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(rgb(edge), Aspect_TOL_SOLID, 1.0));
}

void Viewport::setCompare(const std::vector<ComparePart>& parts, const std::vector<CompareArrow>& arrows, const QColor& arrowColor) {
  if (!m_initialised) return;
  clearCompare();
  for (const auto& part : parts) {  // one entry per part, null when it cannot be drawn, so restyleCompare keeps the order
    Handle(AIS_Shape) ais;
    if (!part.shape.IsNull()) try {
        gp_Trsf placed;
        if (!part.world.is_identity()) placed = opad::trsf_from_mat(part.world);  // throws when not rigid: such a part comes baked
        ais = part.prs ? Handle(AIS_Shape)(new BodyShape(part.shape, part.prs)) : new AIS_Shape(part.shape);
        ais->Attributes()->SetAutoTriangulation(Standard_False);  // meshed already
        ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
        if (placed.Form() != gp_Identity) ais->SetLocalTransformation(placed);
        styleComparePart(ais, part);
        if (part.visible) m_ctx->Display(ais, AIS_Shaded, -1, Standard_False);  // mode -1: never picked
      } catch (const std::exception&) {
        ais.Nullify();
      }
    m_compareParts.emplace_back(part.id, ais);
  }
  if (!arrows.empty()) {
    m_compareArrows = new CompareArrows(arrows, Quantity_Color(arrowColor.redF(), arrowColor.greenF(), arrowColor.blueF(), Quantity_TOC_sRGB));
    m_ctx->Display(m_compareArrows, 0, -1, Standard_False);
    m_ctx->SetZLayer(m_compareArrows, Graphic3d_ZLayerId_Topmost);  // over the parts they join
  }
  redrawScene();
}

void Viewport::restyleCompare(const std::vector<ComparePart>& parts, const std::vector<CompareArrow>& arrows) {
  if (!m_initialised) return;
  for (size_t k = 0; k < parts.size() && k < m_compareParts.size(); ++k) {
    const ComparePart& part = parts[k];
    const Handle(AIS_Shape)& ais = m_compareParts[k].second;
    if (ais.IsNull()) continue;
    if (!part.visible) {
      if (m_ctx->IsDisplayed(ais)) m_ctx->Erase(ais, Standard_False);
      continue;
    }
    styleComparePart(ais, part);
    ais->SynchronizeAspects();
    if (!m_ctx->IsDisplayed(ais)) m_ctx->Display(ais, AIS_Shaded, -1, Standard_False);
  }
  if (const auto drawn = Handle(CompareArrows)::DownCast(m_compareArrows); !drawn.IsNull()) {
    drawn->setArrows(arrows);
    m_ctx->Redisplay(drawn, Standard_False);  // a few segments, nothing to pick
  }
  redrawScene();
}

void Viewport::clearCompare() {
  if (!m_initialised || (m_compareParts.empty() && m_compareArrows.IsNull())) return;
  for (const auto& [id, ais] : m_compareParts)
    if (!ais.IsNull()) m_ctx->Remove(ais, Standard_False);
  m_compareParts.clear();
  if (!m_compareArrows.IsNull()) m_ctx->Remove(m_compareArrows, Standard_False);
  m_compareArrows.Nullify();
  redrawScene();
}

std::shared_ptr<const BodyPrs> Viewport::displayArrays(const std::string& key) const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_meshMu));
  const auto it = m_prs.find(key);
  return it == m_prs.end() ? nullptr : it->second;
}

void Viewport::fitBox(const Bnd_Box& box) {
  if (!m_initialised) return;
  if (box.IsVoid()) return fitAll();
  m_needFit = false;
  m_view->FitAll(box, 0.02, Standard_False);
  m_view->Invalidate();
  requestRedraw();
}

opad::json Viewport::benchCompareState() const {
  opad::json parts = opad::json::array();
  for (const auto& [id, ais] : m_compareParts) {
    if (ais.IsNull()) {
      parts.push_back({{"id", id}, {"displayed", false}});
      continue;
    }
    Quantity_Color c;
    ais->Color(c);
    double r = 0, g = 0, b = 0;
    c.Values(r, g, b, Quantity_TOC_sRGB);
    parts.push_back({{"id", id}, {"displayed", m_initialised && m_ctx->IsDisplayed(ais)}, {"color", {r, g, b}}, {"transparency", ais->Transparency()}});
  }
  const auto arrows = Handle(CompareArrows)::DownCast(m_compareArrows);
  return {{"parts", parts}, {"arrows", arrows.IsNull() ? 0 : arrows->shown()}};
}
