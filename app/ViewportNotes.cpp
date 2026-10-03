// Notes in the viewport: the anchor dot of every open note and the pointer from it to the note's card (a widget
// over the view, placed by NoteCards). Hidden notes (Show notes off) leave nothing in the view. The pointer is drawn in the view, in the camera plane at the anchor's depth,
// so it follows the model through the depth buffer like the measurement graphics do. Also the annotation editor's
// view parts: the target it pins a note or drawing to, and the drawing's strokes while they are drawn.
#include "Viewport.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Graphic3d_SequenceOfHClipPlane.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "Jobs.hpp"
#include "Notes.hpp"
#include "opad/geometry.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }

class NoteGraphic : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(NoteGraphic, AIS_InteractiveObject)
 public:
  struct Leader { gp_Pnt from, to; QColor color; Aspect_TypeOfLine type; double width; };
  struct Dot { gp_Pnt at; QColor color; };
  std::vector<Leader> leaders;
  std::vector<Dot> dots;
  struct Stroke {std::vector<gp_Pnt> points;QColor color;double width;};
  std::vector<Stroke> strokes;
  // scale: backing pixels per widget point, so a stroke is as wide on screen as its sample in the editor
  void addDrawing(const opad::json& drawing, double scale, const gp_Vec& offset = gp_Vec()) {
    if(drawing.is_null()) return;
    for(const auto& stroke:drawing.at("strokes")) {
      const auto frame=opad::Frame::from_json(stroke.value("plane",drawing.at("plane")));
      const auto name=stroke.at("color").get<std::string>();
      const QColor color=notes::penColor(name);
      const auto& points=stroke.at("points");
      Stroke line{{},color,stroke.at("width").get<double>()*scale};line.points.reserve(points.size());
      for(const auto& point:points){const auto p=frame.to_world(point[0],point[1]);line.points.push_back(gp_Pnt(p[0],p[1],p[2]).Translated(offset));}
      strokes.push_back(std::move(line));
    }
  }
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    for(const auto& stroke:strokes) {
      if(stroke.points.size()<2)continue;
      auto group=prs->NewGroup();
      Handle(Prs3d_LineAspect) style=new Prs3d_LineAspect(occ(stroke.color),Aspect_TOL_SOLID,stroke.width);
      group->SetGroupPrimitivesAspect(style->Aspect());
      Handle(Graphic3d_ArrayOfSegments) vertices=new Graphic3d_ArrayOfSegments(int(2*(stroke.points.size()-1)));
      for(size_t i=1;i<stroke.points.size();++i){vertices->AddVertex(stroke.points[i-1]);vertices->AddVertex(stroke.points[i]);}
      group->AddPrimitiveArray(vertices);
    }
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

// What a note's place depends on: its reference, and the body keys and placements of what it is pinned to (every body of
// a component; a sketch's plane and size). The same signature, the same place: nothing is measured again.
size_t Viewport::anchorSignature(const opad::Ref& ref) const {
  const opad::Scene& scene = m_doc->scene;
  size_t h = std::hash<std::string>{}(ref.str());
  auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
  auto body = [&](const std::string& id) {
    const opad::Node* n = scene.node(id);
    if (!n || n->kind != opad::Node::Kind::Body) return;
    mix(std::hash<std::string>{}(id));
    mix(std::hash<std::string>{}(n->body_missing ? std::string() : n->body_key));
    for (double v : scene.world(id).m) mix(std::hash<double>{}(v));
  };
  if (const opad::SketchItem* s = scene.sketch(ref.body)) {
    mix(std::hash<std::string>{}(s->frame.to_json().dump()));
    for (const char* k : {"points", "entities"}) {
      const auto it = s->geometry.find(k);
      mix(it == s->geometry.end() ? 0 : it->size());
    }
  } else if (const opad::Node* n = scene.node(ref.body); n && n->kind == opad::Node::Kind::Component) {
    for (const auto& b : scene.bodies_under(ref.body)) body(b);
  } else {
    body(ref.body);
  }
  return h;
}

// Where each open note is anchored, from the scene; the cards follow through notesMoved. A note pinned to a body,
// component, sketch or sub-shape shows once its anchor has been measured on a worker (UI-03: exact mass properties here
// held a sync of the Engine for 15 s per note); the cache keeps it until what it depends on changes.
void Viewport::updateAnnotations() {
  if (!m_initialised) return;
  trace::Scope scope("Viewport::updateAnnotations");
  m_notesRevision = m_doc->revision;
  refreshMeasurement(true);
  m_notes.clear();
  struct Measure { std::string id; opad::Ref ref; size_t signature; };
  std::vector<Measure> measure;
  std::set<std::string> pinned;
  for (const auto& a : m_doc->scene.annotations) {
    if (a.unresolved) continue;
    gp_Pnt at(a.anchor.point[0], a.anchor.point[1], a.anchor.point[2]);
    if (!a.drawing.is_null()) {
      const auto& p = a.drawing.at("plane").at("origin");
      at = gp_Pnt(p[0], p[1], p[2]);
    } else if (a.anchor.kind != opad::Ref::Kind::Point) {
      pinned.insert(a.id);
      const size_t signature = anchorSignature(a.anchor);
      auto [it, added] = m_noteAnchors.try_emplace(a.id);
      NoteAnchor& anchor = it->second;
      if (added || anchor.signature != signature) anchor = NoteAnchor{signature};
      if (!anchor.ready) {
        if (!anchor.queued) measure.push_back({a.id, a.anchor, signature});
        anchor.queued = true;
        continue;
      }
      if (!anchor.found) continue;
      at = anchor.at;
    }
    m_notes[a.id] = {at, a.style, a.drawing, a.anchor.body};
  }
  for (auto it = m_noteAnchors.begin(); it != m_noteAnchors.end();) it = pinned.count(it->first) ? std::next(it) : m_noteAnchors.erase(it);
  if (!measure.empty()) {
    // The worker reads a copy of the scene and a document that only shares the shape cache (a copy of the document would
    // copy every BREP text): the shapes are cached here first, a hit for every body loaded or displayed.
    auto document = std::make_shared<opad::Document>();
    document->shape_cache = m_doc->doc.shape_cache;
    const opad::Scene& scene = m_doc->scene;
    for (const auto& m : measure)
      for (const auto& id : scene.node(m.ref.body) ? scene.bodies_under(m.ref.body) : std::vector<std::string>{})
        if (const opad::Node* n = scene.node(id); n && !n->body_missing) try { opad::body_shape(m_doc->doc, n->body_key); } catch (const std::exception&) {}
    auto copy = std::make_shared<opad::Scene>(scene);
    auto found = std::make_shared<std::vector<std::pair<bool, opad::Vec3>>>(measure.size());
    const auto generation = m_doc->generation;
    ++m_anchorJobs;
    m_jobs->async(tr("Placing notes"), [document, copy, measure, found](Progress p) {
      for (size_t i = 0; i < measure.size() && !p.cancelled(); ++i) {
        try {
          (*found)[i] = {true, opad::annotation_anchor(*document, *copy, measure[i].ref)};
        } catch (const std::exception&) {
        } catch (const Standard_Failure&) {
        }
      }
    }, [this, measure, found, generation](bool ok, const QString&) {
      --m_anchorJobs;
      if (generation != m_doc->generation) return;
      bool placed = false;
      for (size_t i = 0; i < measure.size(); ++i) {
        auto it = m_noteAnchors.find(measure[i].id);
        if (it == m_noteAnchors.end() || it->second.signature != measure[i].signature) continue;  // moved on meanwhile
        NoteAnchor& anchor = it->second;
        anchor.queued = false;
        if (!ok) continue;  // cancelled: measured again after the next change
        const auto& [hit, at] = (*found)[i];
        anchor.ready = true;
        anchor.found = hit;
        anchor.at = gp_Pnt(at[0], at[1], at[2]);
        placed = placed || hit;
        ++m_anchorsMeasured;
      }
      if (placed) updateAnnotations();
    });
  }
  m_noteCamera.Reset();  // so the next frame lays the cards out again
  QMetaObject::invokeMethod(this, [this] { emit notesMoved(); }, Qt::QueuedConnection);
}

bool Viewport::noteAnchorPoint(const std::string& id, opad::Vec3& out) const {
  auto it = m_notes.find(id);
  if (it == m_notes.end()) return false;
  out = {it->second.at.X(), it->second.at.Y(), it->second.at.Z()};
  return true;
}

bool Viewport::noteAnchor(const std::string& id, QPoint& out) const {
  auto it = m_notes.find(id);
  if (it == m_notes.end() || !m_initialised) return false;
  const gp_Pnt p = it->second.at.Translated(lookOffset(it->second.node));
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
    if(!m_noteTypeFilter.empty() && note.style!=m_noteTypeFilter) continue;
    const gp_Vec offset = lookOffset(note.node);
    const gp_Pnt at = note.at.Translated(offset);
    g->addDrawing(note.drawing, m_cubeScale, offset);
    const notes::Style& look = notes::style(note.style);
    const QColor color = m_tokens.*look.color;
    g->dots.push_back({at, color});
    auto end = ends.find(id);
    if (end == ends.end()) continue;
    // World units per widget pixel at the anchor (perspective: at its depth, not the camera target's).
    double px = pixelSize();
    if (!camera->IsOrthographic()) px *= std::max(gp_Vec(camera->Eye(), at).Dot(gp_Vec(camera->Direction())), camera->Distance() * 0.01) / camera->Distance();
    const QPoint from = widgetPoint({at.X(), at.Y(), at.Z()});
    const gp_Pnt to = at.Translated(right * ((end->second.x() - from.x()) * px) + up * ((from.y() - end->second.y()) * px));
    g->leaders.push_back({at, to, color, look.line == Qt::SolidLine ? Aspect_TOL_SOLID : look.line == Qt::DashLine ? Aspect_TOL_DASH : Aspect_TOL_DOT, look.width});
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

// ---------------------------------------------------------------- annotation editor
namespace {
// The target of the note or drawing being edited, in the selection blue: a face or body tinted and ringed by a
// dashed outline (over a faint band, so the gaps stay blue), an edge drawn thick, a vertex ringed. Built in the body's
// own frame from the meshes it is displayed with, and in the Topmost layer so it shows through like a selection.
class TargetHighlight : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(TargetHighlight, AIS_InteractiveObject)
 public:
  explicit TargetHighlight(const QColor& color) : m_color(occ(color)) {}
  std::vector<Handle(Graphic3d_ArrayOfTriangles)> fills;
  std::vector<gp_Pnt> outline, lines;  // segment end pairs
  std::vector<gp_Pnt> rings;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    if (!fills.empty()) {
      Handle(Graphic3d_AspectFillArea3d) fill = new Graphic3d_AspectFillArea3d();
      fill->SetInteriorStyle(Aspect_IS_SOLID);
      fill->SetInteriorColor(Quantity_ColorRGBA(m_color, 0.32f));
      fill->SetAlphaMode(Graphic3d_AlphaMode_Blend);
      fill->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // a flat tint: the arrays carry no normals
      auto group = prs->NewGroup();
      group->SetGroupPrimitivesAspect(fill);
      for (const auto& a : fills) group->AddPrimitiveArray(a);
    }
    auto segments = [&](const std::vector<gp_Pnt>& ends, Aspect_TypeOfLine type, double width, const Quantity_Color& color) {
      if (ends.size() < 2) return;
      Handle(Graphic3d_AspectLine3d) aspect = new Graphic3d_AspectLine3d(color, type, width);
      Handle(Graphic3d_ArrayOfSegments) array = new Graphic3d_ArrayOfSegments(int(ends.size()));
      for (const auto& p : ends) array->AddVertex(p);
      auto group = prs->NewGroup();
      group->SetGroupPrimitivesAspect(aspect);
      group->AddPrimitiveArray(array);
    };
    // Line colours are drawn opaque here: a white band under the dashes makes their gaps light (translucent blue
    // underneath would fill them in the same blue and read as one solid line).
    segments(outline, Aspect_TOL_SOLID, 3.5, Quantity_NOC_WHITE);
    segments(outline, Aspect_TOL_DASH, 2.0, m_color);
    segments(lines, Aspect_TOL_SOLID, 4.0, m_color);
    if (!rings.empty()) {
      Handle(Graphic3d_ArrayOfPoints) points = new Graphic3d_ArrayOfPoints(int(rings.size()));
      for (const auto& p : rings) points->AddVertex(p);
      auto ring = prs->NewGroup();
      ring->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_O, m_color, 7.0));
      ring->AddPrimitiveArray(points);
      auto centre = prs->NewGroup();
      centre->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, m_color, 3.0));
      centre->AddPrimitiveArray(points);
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}

 private:
  Quantity_Color m_color;
};

// An edge as segment end pairs: the polyline it is drawn with, else a sampling of its curve.
void edgeSegments(const TopoDS_Edge& edge, std::vector<gp_Pnt>& ends) {
  std::vector<gp_Pnt> line;
  TopLoc_Location loc;
  Handle(Poly_PolygonOnTriangulation) polygon;
  Handle(Poly_Triangulation) mesh;
  BRep_Tool::PolygonOnTriangulation(edge, polygon, mesh, loc);
  if (!polygon.IsNull() && !mesh.IsNull()) {
    for (int n = 1; n <= polygon->NbNodes(); ++n) line.push_back(mesh->Node(polygon->Node(n)).Transformed(loc.Transformation()));
  } else if (Handle(Poly_Polygon3D) p3 = BRep_Tool::Polygon3D(edge, loc); !p3.IsNull()) {
    for (int n = 1; n <= p3->NbNodes(); ++n) line.push_back(p3->Nodes().Value(n).Transformed(loc.Transformation()));
  } else if (!BRep_Tool::Degenerated(edge)) {
    BRepAdaptor_Curve curve(edge);
    constexpr int kSamples = 32;
    for (int n = 0; n <= kSamples; ++n) line.push_back(curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * n / kSamples));
  }
  for (size_t n = 1; n < line.size(); ++n) {
    ends.push_back(line[n - 1]);
    ends.push_back(line[n]);
  }
}

void boxSegments(const Bnd_Box& box, std::vector<gp_Pnt>& ends) {
  if (box.IsVoid()) return;
  const gp_Pnt lo = box.CornerMin(), hi = box.CornerMax();
  auto corner = [&](int i) { return gp_Pnt(i & 1 ? hi.X() : lo.X(), i & 2 ? hi.Y() : lo.Y(), i & 4 ? hi.Z() : lo.Z()); };
  for (int i = 0; i < 8; ++i)
    for (int axis : {1, 2, 4})
      if (!(i & axis)) {
        ends.push_back(corner(i));
        ends.push_back(corner(i | axis));
      }
}
}  // namespace

bool Viewport::annotationPick(const QPointF& point, opad::Ref& target, bool& hit) {
  hit = false;
  if (!m_initialised || m_blocked || !referenceAt(point, target) || !m_items.count(target.body)) return false;
  const auto& selector = m_ctx->MainSelector();
  const Handle(SelectMgr_EntityOwner) owner = m_ctx->DetectedOwner();
  for (int i = 1; i <= selector->NbPicked(); ++i)
    if (selector->Picked(i) == owner) {  // where the mouse met the target: every stroke's plane passes through it
      const gp_Pnt p = selector->PickedPoint(i);
      target.point = {p.X(), p.Y(), p.Z()};
      hit = true;
      break;
    }
  return true;
}

// Cheap on purpose (it runs in a click): one sub-shape's mesh, or the arrays the body is already drawn with.
bool Viewport::showAnnotationTarget(const opad::Ref& target, opad::Vec3* centre) {
  clearAnnotationTarget();
  if (!m_initialised) return false;
  const auto item = m_items.find(target.body);
  if (item == m_items.end() || !m_ctx->IsDisplayed(item->second.ais)) return false;
  const Handle(AIS_Shape)& ais = item->second.ais;
  const bool rigid = item->second.world.is_identity() || opad::mat_is_rigid(item->second.world);  // see displayBody
  Handle(TargetHighlight) mark = new TargetHighlight(m_tokens.sel);
  Bnd_Box box;  // in the body's own frame, like the arrays
  try {
    if (target.kind == opad::Ref::Kind::Body) {
      std::shared_ptr<BodyPrs> prs;
      {
        std::lock_guard<std::mutex> lock(m_meshMu);
        if (auto p = m_prs.find(item->second.key); p != m_prs.end()) prs = p->second;
      }
      if (rigid && prs && !prs->triangles.IsNull()) {
        mark->fills.push_back(prs->triangles);
      } else if (rigid) {  // drawn without the worker's arrays: build them there too, the tint follows
        auto shape = std::make_shared<TopoDS_Shape>(ais->Shape());
        auto built = std::make_shared<std::shared_ptr<BodyPrs>>();
        m_targetJob = m_jobs->async(tr("Highlighting %1").arg(m_doc->nodeName(target.body)), [shape, built](Progress) {
          Bnd_Box bounds;
          BRepBndLib::Add(*shape, bounds, Standard_True);
          *built = BodyPrs::build(*shape, bounds);
        }, [this, mark, built](bool ok, const QString&) {
          if (m_annotationTarget == mark) m_targetJob = nullptr;
          if (!ok || m_annotationTarget != mark || !*built || (*built)->triangles.IsNull()) return;
          mark->fills.push_back((*built)->triangles);
          m_ctx->Redisplay(mark, Standard_False);
          redrawScene();
        });
      }
      box = rigid ? opad::body_bbox(m_doc->doc, item->second.key) : opad::node_world_bbox(m_doc->doc, m_doc->scene, target.body);
      boxSegments(box, mark->outline);
    } else {
      const TopoDS_Shape sub = opad::subshape(ais->Shape(), target.kind, target.index);
      if (sub.IsNull()) return false;
      BRepBndLib::Add(sub, box, Standard_True);
      if (sub.ShapeType() == TopAbs_FACE) {
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(TopoDS::Face(sub), loc);
        if (!mesh.IsNull() && mesh->NbTriangles() > 0) {
          Handle(Graphic3d_ArrayOfTriangles) fill = new Graphic3d_ArrayOfTriangles(mesh->NbNodes(), 3 * mesh->NbTriangles());
          for (int n = 1; n <= mesh->NbNodes(); ++n) fill->AddVertex(mesh->Node(n).Transformed(loc.Transformation()));
          for (int k = 1; k <= mesh->NbTriangles(); ++k) {
            int a, b, c;
            mesh->Triangle(k).Get(a, b, c);
            fill->AddEdges(a, b, c);
          }
          mark->fills.push_back(fill);
        }
        for (TopExp_Explorer e(sub, TopAbs_EDGE); e.More(); e.Next()) edgeSegments(TopoDS::Edge(e.Current()), mark->outline);
      } else if (sub.ShapeType() == TopAbs_EDGE) {
        edgeSegments(TopoDS::Edge(sub), mark->lines);
      } else if (sub.ShapeType() == TopAbs_VERTEX) {
        mark->rings.push_back(BRep_Tool::Pnt(TopoDS::Vertex(sub)));
      } else {
        boxSegments(box, mark->outline);
      }
    }
  } catch (const Standard_Failure&) {
    return false;
  } catch (const std::exception&) {  // the ordinal is gone (the body changed)
    return false;
  }
  const gp_Trsf trsf = ais->Transformation();  // identity for a non-rigid body: its shape is already placed
  mark->SetLocalTransformation(trsf);
  mark->SetZLayer(Graphic3d_ZLayerId_Topmost);
  mark->SetInfiniteState(Standard_True);  // never part of Fit All
  mark->SetClipPlanes(ais->ClipPlanes());  // a section cuts it like the body
  m_ctx->Display(mark, 0, -1, Standard_False);
  if (trace::enabled()) trace::log(QStringLiteral("annotation target %1: %2 fills, %3 outline, %4 lines, rigid %5").arg(QString::fromStdString(target.str())).arg(mark->fills.size()).arg(mark->outline.size()).arg(mark->lines.size()).arg(rigid));
  m_ctx->ClearDetected(Standard_False);  // the pick's hover highlight
  m_annotationTarget = mark;
  if (!box.IsVoid()) {
    const gp_Pnt lo = box.CornerMin(), hi = box.CornerMax();
    for (int i = 0; i < 8; ++i) {
      const gp_Pnt p = gp_Pnt(i & 1 ? hi.X() : lo.X(), i & 2 ? hi.Y() : lo.Y(), i & 4 ? hi.Z() : lo.Z()).Transformed(trsf);
      m_annotationCorners.push_back({p.X(), p.Y(), p.Z()});
    }
    if (centre) {
      const gp_Pnt c = gp_Pnt((lo.XYZ() + hi.XYZ()) / 2).Transformed(trsf);
      *centre = {c.X(), c.Y(), c.Z()};
    }
  }
  redrawScene();
  return true;
}

void Viewport::clearAnnotationTarget() {
  m_annotationCorners.clear();
  if (Job* job = std::exchange(m_targetJob, nullptr)) job->cancel();
  if (!m_initialised || m_annotationTarget.IsNull()) return;
  m_ctx->Remove(m_annotationTarget, Standard_False);
  m_annotationTarget.Nullify();
  redrawScene();
}

QRect Viewport::annotationTargetRect() const {
  QRect rect;
  for (const auto& corner : m_annotationCorners) {
    const QRect at(widgetPoint(corner), QSize(1, 1));
    rect = rect.isNull() ? at : rect.united(at);
  }
  return rect;
}

opad::Frame Viewport::annotationCameraPlane(const opad::Vec3& origin) const {
  opad::Frame frame;
  frame.origin = origin;
  if (!m_initialised) return frame;
  const gp_Vec up(m_view->Camera()->OrthogonalizedUp());
  const gp_Vec right = gp_Vec(m_view->Camera()->Direction()).Crossed(up);
  frame.x = {right.X(), right.Y(), right.Z()};
  frame.y = {up.X(), up.Y(), up.Z()};
  return frame;
}

void Viewport::previewAnnotationDrawing(const opad::json& drawing) {
  if(!m_initialised)return;
  if(!m_drawingPreview.IsNull()){m_ctx->Remove(m_drawingPreview,false);m_drawingPreview.Nullify();}
  if(!drawing.is_null()) {
    Handle(NoteGraphic) graphic=new NoteGraphic();graphic->addDrawing(drawing,m_cubeScale);graphic->SetInfiniteState(true);
    graphic->SetZLayer(Graphic3d_ZLayerId_TopOSD);m_ctx->Display(graphic,0,-1,false);m_drawingPreview=graphic;
  }
  redrawScene();
}

// Called from handleViewRedraw: after a camera move the cards need a new place (queued, the frame is being drawn).
void Viewport::noteCameraMoved() {
  const auto state = m_view->Camera()->WorldViewProjState();
  const QSize pixels(qRound(width() * devicePixelRatioF()), qRound(height() * devicePixelRatioF()));
  if (m_noteCamera == state && m_noteSize == pixels) return;
  m_noteCamera = state;
  m_noteSize = pixels;
  resetHoverFade();
  QMetaObject::invokeMethod(this, [this] { emit notesMoved(); }, Qt::QueuedConnection);
}
