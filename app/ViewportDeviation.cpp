// Viewport, Deviation's parts (DeviationArea.cpp): whether a body's vertex paint is drawn, and the labels pinned at the worst
// spots and probed points. The paints themselves go through the tinted arrays (ViewportCompare.cpp applyTints).
#include <AIS_InteractiveObject.hxx>
#include <AIS_TextLabel.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_TextAspect.hxx>
#include <SelectMgr_Selection.hxx>
#include <TCollection_ExtendedString.hxx>

#include "Viewport.hpp"
#include "opad/geometry.hpp"

namespace {
// The pin's point: a round marker in its colour, over everything.
class PinDot : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(PinDot, AIS_InteractiveObject)
 public:
  PinDot(const gp_Pnt& at, const Quantity_Color& color, double scale) : m_at(at), m_color(color), m_scale(scale) {}

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    Handle(Graphic3d_ArrayOfPoints) points = new Graphic3d_ArrayOfPoints(1);
    points->AddVertex(m_at);
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, m_color, 2.2 * m_scale));
    g->AddPrimitiveArray(points);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  gp_Pnt m_at;
  Quantity_Color m_color;
  double m_scale;
};

Quantity_Color occColor(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
}  // namespace

bool Viewport::vertexPaintDrawn(const std::string& id) const {
  const auto tint = m_faceTints.find(id);
  const auto item = m_items.find(id);
  return tint != m_faceTints.end() && tint->second.paint && item != m_items.end() && tint->second.ais && tint->second.ais == item->second.ais.get();
}

void Viewport::setDeviationPins(const std::vector<Pin>& pins) {
  if (!m_initialised) return;
  for (const auto& o : m_deviationPins) m_ctx->Remove(o, Standard_False);
  m_deviationPins.clear();
  const double scale = displayScale();
  for (const Pin& pin : pins) {
    const gp_Pnt at(pin.at[0], pin.at[1], pin.at[2]);
    Handle(PinDot) dot = new PinDot(at, occColor(pin.color), scale);
    Handle(AIS_TextLabel) label = new AIS_TextLabel();
    label->SetText(TCollection_ExtendedString(("  " + pin.text).toUtf8().constData(), Standard_True));
    label->SetPosition(at);
    label->SetColor(Quantity_Color(1, 1, 1, Quantity_TOC_sRGB));
    label->SetHeight(12 * scale);
    label->SetHJustification(Graphic3d_HTA_LEFT);
    label->SetVJustification(Graphic3d_VTA_BOTTOM);
    label->SetDisplayType(Aspect_TODT_SUBTITLE);  // on a plate: readable over the coloured map
    label->SetColorSubTitle(Quantity_Color(0.13, 0.14, 0.16, Quantity_TOC_sRGB));
    for (const Handle(AIS_InteractiveObject)& o : {Handle(AIS_InteractiveObject)(dot), Handle(AIS_InteractiveObject)(label)}) {
      o->SetInfiniteState(Standard_True);  // Fit never frames it
      o->SetZLayer(Graphic3d_ZLayerId_Topmost);
      m_ctx->Display(o, 0, -1, Standard_False);  // never picked: no selection mode
      m_deviationPins.push_back(o);
    }
  }
  redrawScene();
}

opad::json Viewport::benchVertexPaint(const std::string& id, size_t samples) const {
  const auto tint = m_faceTints.find(id);
  const auto item = m_items.find(id);
  opad::json out{{"drawn", vertexPaintDrawn(id)}, {"vertices", opad::json::array()}};
  if (tint == m_faceTints.end() || !tint->second.arrays || tint->second.arrays->triangles.IsNull() || item == m_items.end()) return out;
  const Handle(Graphic3d_ArrayOfTriangles)& tris = tint->second.arrays->triangles;
  const gp_Trsf placed = opad::trsf_from_mat(item->second.world);
  const int n = tris->VertexNumber();
  if (n <= 0 || !tris->HasVertexColors()) return out;
  for (size_t k = 0; k < samples; ++k) {
    const int i = 1 + int((double(k) + 0.5) * n / double(samples));
    if (i > n) break;
    const gp_Pnt p = tris->Vertice(i).Transformed(placed);
    double r = 0, g = 0, b = 0;
    tris->VertexColor(i).Values(r, g, b, Quantity_TOC_sRGB);
    out["vertices"].push_back({{"at", {p.X(), p.Y(), p.Z()}}, {"color", {r, g, b}}});
  }
  return out;
}
