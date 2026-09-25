// Notes in the viewport: the anchor dot of every open note and the pointer from it to the note's card (a widget
// over the view, placed by NoteCards). Hidden notes (Show notes off) leave nothing in the view. The pointer is drawn in the view, in the camera plane at the anchor's depth,
// so it follows the model through the depth buffer like the measurement graphics do.
#include "Viewport.hpp"

#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_SequenceOfHClipPlane.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>

#include "Jobs.hpp"
#include "Notes.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }

class NoteGraphic : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(NoteGraphic, AIS_InteractiveObject)
 public:
  struct Leader { gp_Pnt from, to; QColor color; Aspect_TypeOfLine type; double width; };
  struct Dot { gp_Pnt at; QColor color; };
  std::vector<Leader> leaders;
  std::vector<Dot> dots;
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    for (const auto& l : leaders) {
      auto group = prs->NewGroup();
      Handle(Prs3d_LineAspect) style = new Prs3d_LineAspect(occ(l.color), l.type, l.width);
      group->SetGroupPrimitivesAspect(style->Aspect());
      Handle(Graphic3d_ArrayOfSegments) v = new Graphic3d_ArrayOfSegments(2);
      v->AddVertex(l.from); v->AddVertex(l.to);
      group->AddPrimitiveArray(v);
    }
    for (const auto& d : dots) {
      auto group = prs->NewGroup();
      Handle(Prs3d_PointAspect) ring = new Prs3d_PointAspect(Aspect_TOM_O, occ(d.color), 9);
      group->SetGroupPrimitivesAspect(ring->Aspect());
      Handle(Graphic3d_ArrayOfPoints) p = new Graphic3d_ArrayOfPoints(1);
      p->AddVertex(d.at);
      group->AddPrimitiveArray(p);
      auto centre = prs->NewGroup();
      Handle(Prs3d_PointAspect) ball = new Prs3d_PointAspect(Aspect_TOM_BALL, occ(d.color), 4);
      centre->SetGroupPrimitivesAspect(ball->Aspect());
      Handle(Graphic3d_ArrayOfPoints) q = new Graphic3d_ArrayOfPoints(1);
      q->AddVertex(d.at);
      centre->AddPrimitiveArray(q);
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}
};
}  // namespace

// Where each open note is anchored, from the scene; the cards follow through notesMoved.
void Viewport::updateAnnotations() {
  if (!m_initialised) return;
  refreshMeasurement(true);
  m_notes.clear();
  for (const auto& a : m_doc->scene.annotations) {
    if (a.unresolved) continue;
    gp_Pnt at(a.anchor.point[0], a.anchor.point[1], a.anchor.point[2]);
    if (a.anchor.kind != opad::Ref::Kind::Point) {
      try {
        opad::json info = opad::inspect_ref(m_doc->doc, m_doc->scene, a.anchor);
        opad::json c = info.contains("center") ? info["center"] : info.contains("point") ? info["point"] : info.contains("start") ? info["start"] : info["bbox"]["center"];
        at = gp_Pnt(c[0].get<double>(), c[1].get<double>(), c[2].get<double>());
      } catch (const std::exception&) {
        continue;
      }
    }
    m_notes[a.id] = {at, a.style};
  }
  m_noteCamera.Reset();  // so the next frame lays the cards out again
  QMetaObject::invokeMethod(this, [this] { emit notesMoved(); }, Qt::QueuedConnection);
}

bool Viewport::noteAnchor(const std::string& id, QPoint& out) const {
  auto it = m_notes.find(id);
  if (it == m_notes.end() || !m_initialised) return false;
  const gp_Pnt& p = it->second.at;
  if (!m_view->Camera()->IsOrthographic() && gp_Vec(m_view->Camera()->Eye(), p).Dot(gp_Vec(m_view->Camera()->Direction())) <= 0) return false;  // behind the eye
  out = widgetPoint({p.X(), p.Y(), p.Z()});
  return true;
}

void Viewport::setNoteLeaders(const std::map<std::string, QPoint>& ends, bool shown) {
  if (!m_initialised) return;
  for (const auto& o : m_labels) m_ctx->Remove(o, Standard_False);
  m_labels.clear();
  if (m_notes.empty() || !shown) return redrawScene();
  const auto& camera = m_view->Camera();
  const gp_Vec up(camera->OrthogonalizedUp());
  const gp_Vec right = gp_Vec(camera->Direction()).Crossed(up);
  Handle(NoteGraphic) g = new NoteGraphic();
  for (const auto& [id, note] : m_notes) {
    const notes::Style& look = notes::style(note.style);
    const QColor color = m_tokens.*look.color;
    g->dots.push_back({note.at, color});
    auto end = ends.find(id);
    if (end == ends.end()) continue;
    // World units per widget pixel at the anchor (perspective: at its depth, not the camera target's).
    double px = pixelSize();
    if (!camera->IsOrthographic()) px *= std::max(gp_Vec(camera->Eye(), note.at).Dot(gp_Vec(camera->Direction())), camera->Distance() * 0.01) / camera->Distance();
    const QPoint from = widgetPoint({note.at.X(), note.at.Y(), note.at.Z()});
    const gp_Pnt to = note.at.Translated(right * ((end->second.x() - from.x()) * px) + up * ((from.y() - end->second.y()) * px));
    g->leaders.push_back({note.at, to, color, look.line == Qt::SolidLine ? Aspect_TOL_SOLID : look.line == Qt::DashLine ? Aspect_TOL_DASH : Aspect_TOL_DOT, look.width});
  }
  Handle(Graphic3d_SequenceOfHClipPlane) noClip = new Graphic3d_SequenceOfHClipPlane();
  noClip->SetOverrideGlobal(Standard_True);
  g->SetZLayer(Graphic3d_ZLayerId_TopOSD);
  g->SetInfiniteState(Standard_True);  // never part of Fit All
  g->SetClipPlanes(noClip);
  m_ctx->Display(g, 0, -1, Standard_False);
  m_labels.push_back(g);
  if (trace::enabled()) trace::log(QStringLiteral("notes: %1 anchors, %2 leaders").arg(m_notes.size()).arg(g->leaders.size()));
  redrawScene();
}

// Called from handleViewRedraw: after a camera move the cards need a new place (queued, the frame is being drawn).
void Viewport::noteCameraMoved() {
  const auto state = m_view->Camera()->WorldViewProjState();
  const QSize pixels(qRound(width() * devicePixelRatioF()), qRound(height() * devicePixelRatioF()));
  if (m_noteCamera == state && m_noteSize == pixels) return;
  m_noteCamera = state;
  m_noteSize = pixels;
  QMetaObject::invokeMethod(this, [this] { emit notesMoved(); }, Qt::QueuedConnection);
}
