// Viewport, hidden edges visible (UI-48): the faces are drawn as in Hidden line (the background's colour) and every body's
// edges twice, in world coordinates built on a worker: dashed and dim with no depth test in a layer of their own after the
// faces (all of them, so the hidden ones show), then solid in the Top layer against the faces' depth (the ones in sight,
// over the dashes). Two objects for the whole model; built again when the scene, a look or the style changes. No outline
// where a face turns away here: OCCT's silhouette pass leaves depth in front of the rims it outlines, which hid the solid
// lines drawn in a later layer (a cylinder's near rim came out dashed).
#include "Viewport.hpp"

#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_ZLayerSettings.hxx>
#include <Prs3d_Presentation.hxx>

#include "Jobs.hpp"

namespace {
class EdgeOverlay : public AIS_InteractiveObject {
 public:
  EdgeOverlay(std::shared_ptr<const std::vector<Handle(Graphic3d_ArrayOfSegments)>> arrays, Handle(Graphic3d_AspectLine3d) aspect)
      : m_arrays(std::move(arrays)), m_aspect(std::move(aspect)) {}

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(m_aspect);
    for (const auto& array : *m_arrays) g->AddPrimitiveArray(array);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  std::shared_ptr<const std::vector<Handle(Graphic3d_ArrayOfSegments)>> m_arrays;
  Handle(Graphic3d_AspectLine3d) m_aspect;
};

constexpr int kChunk = 1 << 21;  // vertices an array
}  // namespace

void Viewport::scheduleEdgeOverlay() {
  if (!m_initialised) return;
  if (m_style != Style::HiddenEdges) return clearEdgeOverlay();
  m_edgeTimer.start();
}

void Viewport::clearEdgeOverlay() {
  m_edgeTimer.stop();
  ++m_edgeSerial;  // a build on its way is dropped
  for (auto* overlay : {&m_edgesBehind, &m_edgesSeen})
    if (!overlay->IsNull()) {
      m_ctx->Remove(*overlay, Standard_False);
      overlay->Nullify();
    }
  redrawScene();
}

void Viewport::buildEdgeOverlay() {
  if (!m_initialised || m_style != Style::HiddenEdges || !m_jobs) return;
  if (m_hiddenLayer == Graphic3d_ZLayerId_UNKNOWN) {
    Graphic3d_ZLayerSettings behind;
    behind.SetName("hidden edges");
    behind.SetEnableDepthTest(Standard_False);
    behind.SetEnableDepthWrite(Standard_False);
    behind.SetClearDepth(Standard_False);
    m_viewer->InsertLayerBefore(m_hiddenLayer, behind, Graphic3d_ZLayerId_Top);
  }
  struct Part { std::shared_ptr<const BodyPrs> prs; gp_Trsf placed; };
  auto parts = std::make_shared<std::vector<Part>>();
  for (const auto& [id, item] : m_items) {
    const auto body = Handle(BodyShape)::DownCast(item.ais);
    if (body.IsNull() || !body->prs() || body->prs()->triangles.IsNull() || !body->hiddenLine() || !item.look.visible || !m_ctx->IsDisplayed(item.ais)) continue;
    parts->push_back({body->prs(), item.ais->Transformation()});
  }
  auto arrays = std::make_shared<std::vector<Handle(Graphic3d_ArrayOfSegments)>>();
  const unsigned serial = ++m_edgeSerial;
  m_jobs->async(tr("Drawing hidden edges"), [parts, arrays](Progress progress) {
    std::vector<gp_Pnt> chunk;
    auto flush = [&] {
      if (chunk.empty()) return;
      Handle(Graphic3d_ArrayOfSegments) array = new Graphic3d_ArrayOfSegments(int(chunk.size()));
      for (const gp_Pnt& p : chunk) array->AddVertex(p);
      arrays->push_back(array);
      chunk.clear();
    };
    for (const Part& part : *parts) {
      if (progress.cancelled()) return;
      for (const auto& lines : {part.prs->boundaries, part.prs->freeEdges}) {
        if (lines.IsNull()) continue;
        for (int i = 1; i + 1 <= lines->VertexNumber(); i += 2) {
          chunk.push_back(lines->Vertice(i).Transformed(part.placed));
          chunk.push_back(lines->Vertice(i + 1).Transformed(part.placed));
          if (chunk.size() >= size_t(kChunk)) flush();
        }
      }
    }
    flush();
  }, [this, arrays, serial](bool ok, const QString&) {
    if (!ok || serial != m_edgeSerial || m_style != Style::HiddenEdges) return;
    for (auto* overlay : {&m_edgesBehind, &m_edgesSeen})
      if (!overlay->IsNull()) m_ctx->Remove(*overlay, Standard_False);
    const QColor bg = backgroundColor(), fg = m_tokens.fg;
    const QColor dim = QColor::fromRgbF(fg.redF() * 0.35 + bg.redF() * 0.65, fg.greenF() * 0.35 + bg.greenF() * 0.65, fg.blueF() * 0.35 + bg.blueF() * 0.65);
    auto colour = [](const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); };
    m_edgesBehind = new EdgeOverlay(arrays, new Graphic3d_AspectLine3d(colour(dim), Aspect_TOL_DASH, 1.0));
    m_edgesSeen = new EdgeOverlay(arrays, new Graphic3d_AspectLine3d(colour(fg), Aspect_TOL_SOLID, 1.0));
    m_edgesBehind->SetZLayer(m_hiddenLayer);
    m_edgesSeen->SetZLayer(Graphic3d_ZLayerId_Top);
    for (const auto& overlay : {m_edgesBehind, m_edgesSeen}) m_ctx->Display(overlay, 0, -1, Standard_False);  // never pickable
    redrawScene();
  }, JobKind::Background);
}
