// Viewport, design side: sketches in the scene, pick candidates for feature inputs (sketch regions, points,
// planes, axes), feature previews, and the mouse/keyboard hand-over to the sketch editor.
#include "Viewport.hpp"
#include "SketchBackdrop.hpp"
#include <BRepBuilderAPI_MakePolygon.hxx>

#include <AIS_AnimationCamera.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <Graphic3d_Camera.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <SelectMgr_Selection.hxx>
#include <SelectMgr_SensitiveEntity.hxx>
#include <Select3D_SensitiveEntity.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS.hxx>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QCursor>
#include <QSettings>

#include "Jobs.hpp"
#include "Motion.hpp"
#include "opad/geometry.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_geom.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
}  // namespace

// ---------------------------------------------------------------- sketches in the scene
void Viewport::syncSketches() {
  if (!m_initialised) return;
  std::set<std::string> keep;
  for (const auto& s : m_doc->scene.sketches) {
    if (s.id == m_hiddenSketch || (!m_isolated.empty()?!m_isolated.count(s.id):!s.visible)) continue;
    keep.insert(s.id);
    // Pictures by their samples (UI-71: dumping a photo backdrop's megabytes on every sync).
    const std::string stamp = opad::design::geometry_stamp(s.geometry) + s.frame.to_json().dump();
    auto it = m_sketchWires.find(s.id);
    if (it != m_sketchWires.end() && it->second.stamp == stamp) continue;
    if (it != m_sketchWires.end()) { for(const auto& image:it->second.backdrops)m_ctx->Remove(image,false);m_nodeOf.erase(it->second.ais.get()); m_ctx->Remove(it->second.ais, Standard_False); }
    std::shared_ptr<PreparedSketch> prepared;
    auto count=[&](const char* key) { const auto f=s.geometry.find(key); return f!=s.geometry.end() && f->is_array() ? f->size() : size_t(0); };
    if(count("entities")>256 || count("images")>0) {
      auto found=m_preparedSketches.find(s.id);
      if(found==m_preparedSketches.end() || found->second->stamp!=stamp) {
        prepared=std::make_shared<PreparedSketch>();prepared->stamp=stamp;m_preparedSketches[s.id]=prepared;
        const auto geometry=s.geometry;const auto frame=s.frame;const auto id=s.id;const auto generation=m_doc->generation;
        m_jobs->backgroundNext();
        m_jobs->async(tr("Preparing sketch curves"),[prepared,geometry,frame](Progress progress) {
          TopoDS_Compound shape;BRep_Builder b;b.MakeCompound(shape);
          auto sk=opad::design::Sketch::from_json(geometry);
          for(const auto& e:opad::design::sketch_edges(sk,frame,true)) {if(progress.cancelled()) return;b.Add(shape,e);}
          for(const auto& e:sk.entities) if(e.type==opad::design::SkEntity::Type::Point)
            if(const auto* p=sk.point(e.p.empty()?0:e.p[0])) {
              const auto w=frame.to_world(p->x,p->y);b.Add(shape,BRepBuilderAPI_MakeVertex(gp_Pnt(w[0],w[1],w[2])).Vertex());
            }
          prepared->backdrops=prepareSketchBackdrops(sk.images,frame,progress);
          for(const auto& image:sk.images) {
            const double x=image.at("position")[0].get<double>(),y=image.at("position")[1].get<double>(),w=image.at("width").get<double>(),h=image.at("height").get<double>(),angle=image.value("angle",0.0);BRepBuilderAPI_MakePolygon polygon;
            for(auto [u,v]:std::vector<std::pair<double,double>>{{0,0},{w,0},{w,h},{0,h}}){const auto point=frame.to_world(x+u*std::cos(angle)-v*std::sin(angle),y+u*std::sin(angle)+v*std::cos(angle));polygon.Add(gp_Pnt(point[0],point[1],point[2]));}polygon.Close();b.Add(shape,polygon.Wire());
          }
          Bnd_Box box;BRepBndLib::Add(shape,box);prepared->shape=shape;prepared->prs=BodyPrs::build(shape,box);
        },[this,prepared,id,generation](bool ok,const QString&) {
          if(generation!=m_doc->generation || !m_preparedSketches.count(id) || m_preparedSketches[id]!=prepared) return;
          prepared->ready=ok; if(ok) requestSync();
        });
        continue;
      }
      prepared=found->second;if(!prepared->ready) continue;
    }
    TopoDS_Compound comp;
    BRep_Builder bb;
    bb.MakeCompound(comp);
    bool any = false;
    if(prepared) {comp=TopoDS::Compound(prepared->shape);any=true;}
    else try {
      const opad::design::Sketch sk = opad::design::Sketch::from_json(s.geometry);
      for (const auto& e : opad::design::sketch_edges(sk, s.frame, true)) { bb.Add(comp, e); any = true; }
      for (const auto& e : sk.entities)
        if (e.type == opad::design::SkEntity::Type::Point)
          if (const auto* p = sk.point(e.p.empty() ? 0 : e.p[0])) {
            const opad::Vec3 w = s.frame.to_world(p->x, p->y);
            bb.Add(comp, BRepBuilderAPI_MakeVertex(gp_Pnt(w[0], w[1], w[2])).Vertex());
            any = true;
          }
    } catch (const std::exception&) {
    }
    if (!any) {
      m_sketchWires.erase(s.id);
      keep.erase(s.id);
      continue;
    }
    Bnd_Box bounds; BRepBndLib::Add(comp,bounds); auto prs=prepared?prepared->prs:BodyPrs::build(comp,bounds);
    Handle(AIS_Shape) ais = new BodyShape(comp,prs);
    ais->Attributes()->SetWireAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.5));
    ais->Attributes()->SetLineAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.5));
    ais->Attributes()->SetFreeBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.5));
    ais->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O_POINT, occ(m_tokens.sel), 2.0));
    m_ctx->Display(ais, AIS_WireFrame, -1, Standard_False);
    activateSelection(ais); m_nodeOf[ais.get()]=s.id;
    m_sketchWires[s.id] = SketchWire{ais, prs, stamp,{}};
    if(prepared){m_sketchWires[s.id].backdrops=prepared->backdrops;for(const auto& image:prepared->backdrops)showBackdrop(image);}
    SketchWire& wire=m_sketchWires[s.id];wire.look.color={m_tokens.sel.redF(),m_tokens.sel.greenF(),m_tokens.sel.blueF()};  // as drawn above
    if(layered())applySketchLook(wire,sketchLook(s.id));
    if(m_side)maskSide(s.id,nullptr);
  }
  std::erase_if(m_preparedSketches,[&](const auto& entry) {return !keep.count(entry.first);});
  for (auto it = m_sketchWires.begin(); it != m_sketchWires.end();) {
    if (keep.count(it->first)) { ++it; continue; }
    for(const auto& image:it->second.backdrops)m_ctx->Remove(image,false);
    m_nodeOf.erase(it->second.ais.get()); m_ctx->Remove(it->second.ais, Standard_False);
    it = m_sketchWires.erase(it);
  }
}

// ---------------------------------------------------------------- candidates
void Viewport::showCandidates(const std::vector<Candidate>& candidates) {
  if (!m_initialised) return;
  for (const auto& c : m_candidates) m_ctx->Remove(c.second, Standard_False);
  m_candidates.clear();
  markPickedPoints();
  for (const auto& c : candidates) {
    if (c.shape.IsNull()) continue;
    // Small planar regions and a few curves: meshing them here is cheaper than a job round trip. (The
    // context's drawers never triangulate by themselves, see initViewer.)
    if (!c.presentation && c.shape.ShapeType() <= TopAbs_FACE) BRepMesh_IncrementalMesh(c.shape, 0.05, Standard_False, 0.3, Standard_False);
    Handle(AIS_Shape) ais = c.presentation?new BodyShape(c.shape,c.presentation):new AIS_Shape(c.shape);
    const bool surface = c.presentation?!c.presentation->triangles.IsNull():c.shape.ShapeType() <= TopAbs_FACE;
    // A plain material: the default physical one ignores colours, so profiles were drawn as opaque grey sheets and
    // the hover and pick tints below never showed on them.
    ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
    ais->SetColor(occ(m_tokens.sel));
    if (surface) {
      ais->SetTransparency(c.strong ? 0.6 : 0.82);
      ais->Attributes()->ShadingAspect()->Aspect()->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // a flat tint
      ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
      ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.0));
    } else {
      ais->SetWidth(3.0);
      ais->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O_POINT, occ(m_tokens.sel), 4.0 * displayScale()));
    }
    // Over coplanar body faces. A sketch point lies *on* its face, which hid half of its marker in Top (that layer
    // shares the depth buffer), leaving a speck to aim at: points go to Topmost.
    const bool point = !surface && c.shape.ShapeType() == TopAbs_VERTEX;
    const Graphic3d_ZLayerId layer = point ? Graphic3d_ZLayerId_Topmost : Graphic3d_ZLayerId_Top;
    ais->SetZLayer(layer);
    // A quiet tint for hovering and picking these, not the bodies' white hover and grey X-ray selection: on a large
    // sketch region those flooded the view, and the X-ray layer showed the picked profile through the preview.
    auto style = [&](Prs3d_TypeOfHighlight kind, const QColor& colour, float transparency) {
      Handle(Prs3d_Drawer) d = new Prs3d_Drawer();
      d->SetLink(m_ctx->HighlightStyle(kind));
      d->SetDisplayMode(surface ? AIS_Shaded : AIS_WireFrame);
      d->SetColor(occ(colour));
      d->SetTransparency(surface ? transparency : 0.0f);
      d->SetZLayer(layer);
      return d;
    };
    ais->SetDynamicHilightAttributes(style(Prs3d_TypeOfHighlight_Dynamic, m_tokens.hov, 0.72f));
    ais->SetHilightAttributes(style(Prs3d_TypeOfHighlight_Selected, m_tokens.sel, 0.55f));
    m_ctx->Display(ais, surface ? AIS_Shaded : AIS_WireFrame, -1, Standard_False);
    m_ctx->Load(ais, -1);
    m_ctx->Activate(ais, 0);
    // A sketch line or point is a hair to aim at: the context's 4 px missed a path clicked a few pixels off. 12 px each
    // side is the 24 px target of UI-124.
    if (!surface) m_ctx->SetSelectionSensitivity(ais, 0, static_cast<int>(std::lround(12 * displayScale())));
    m_candidates.push_back({c.id, ais});
  }
  redrawScene();
}

void Viewport::clearCandidates() {
  if (!m_initialised || m_candidates.empty()) return;
  for (const auto& c : m_candidates) m_ctx->Remove(c.second, Standard_False);
  m_candidates.clear();
  markPickedPoints();
  redrawScene();
}

// Highlighting only recolours a marker, so in the candidates' own blue a picked point's ring looked like the others:
// a filled dot (not pickable) sits in each picked one.
void Viewport::markPickedPoints() {
  if (!m_initialised) return;
  for (const auto& mark : m_pointMarks) m_ctx->Remove(mark, Standard_False);
  m_pointMarks.clear();
  for (const auto& [id, ais] : m_candidates) {
    if (ais->Shape().IsNull() || ais->Shape().ShapeType() != TopAbs_VERTEX || !m_ctx->IsSelected(ais)) continue;
    Handle(AIS_Shape) mark = new AIS_Shape(ais->Shape());
    mark->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_BALL, occ(m_tokens.sel), 3.0 * displayScale()));
    mark->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(mark, AIS_WireFrame, -1, Standard_False);  // -1: never picked
    m_pointMarks.push_back(mark);
  }
}

std::string Viewport::hoveredCandidate() const {
  if(m_initialised && m_ctx->HasDetected())for(const auto& candidate:m_candidates)if(candidate.second==m_ctx->DetectedInteractive())return candidate.first;
  return {};
}
std::vector<std::string> Viewport::selectedCandidates() const {
  std::vector<std::string> out;
  if (!m_initialised) return out;
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) {
    const Handle(AIS_InteractiveObject) obj = m_ctx->SelectedInteractive();
    for (const auto& c : m_candidates)
      if (c.second == obj) out.push_back(c.first);
  }
  return out;
}

bool Viewport::benchPickPoint(const std::function<bool(const std::string&, const opad::Ref&)>& want, int& x, int& y, bool inside) {
  if (!m_initialised) return false;
  m_view->Redraw();  // the picker clips to the z range of the last frame
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  auto detects = [&](int px, int py) {
    m_ctx->MoveTo(px, py, m_view, Standard_False);
    dropOccluded();  // as the pointer does
    bool ok = false;
    if (m_ctx->HasDetected()) {
      const auto detected = m_ctx->DetectedInteractive();
      std::string candidate;
      opad::Ref entity;
      for (const auto& c : m_candidates)
        if (c.second == detected) candidate = c.first;
      if (const auto node = m_nodeOf.find(detected.get()); node != m_nodeOf.end()) entity.body = node->second;
      if (const auto sub = Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner()); !sub.IsNull() && !entity.body.empty()) entity.kind = sub->kind(), entity.index = sub->index();
      ok = (!candidate.empty() || !entity.body.empty()) && want(candidate, entity);
    }
    m_ctx->ClearDetected(Standard_False);
    return ok;
  };
  constexpr int n = 64;
  std::vector<char> hit(n * n, 0);
  double cx = 0, cy = 0;
  int count = 0;
  for (int j = 1; j < n; ++j)
    for (int i = 1; i < n; ++i)
      if (detects(w * i / n, h * j / n)) hit[j * n + i] = 1, cx += i, cy += j, ++count;
  if (!count) return false;
  cx /= count, cy /= count;
  double best = 1e9;
  for (int j = 2; j < n - 1; ++j)
    for (int i = 2; i < n - 1; ++i) {
      bool in = hit[j * n + i];
      for (int dj = -1; dj <= 1 && in && inside; ++dj)
        for (int di = -1; di <= 1 && in; ++di) in = hit[(j + dj) * n + i + di];
      if (in && std::hypot(i - cx, j - cy) < best) best = std::hypot(i - cx, j - cy), x = w * i / n, y = h * j / n;
    }
  return best < 1e9;
}

void Viewport::selectRefs(const std::vector<opad::Ref>& refs, const std::vector<std::string>& candidates) {
  resetHoverFade();
  if (!m_initialised) return;
  if (m_selJob) m_selJob->cancel();
  clearShade();
  m_ctx->ClearSelected(Standard_False);
  m_selApplied.clear();
  for (const auto& r : refs) {
    auto it = m_items.find(r.body);
    auto sk=m_sketchWires.find(r.body);
    if (it == m_items.end() && sk==m_sketchWires.end()) continue;
    const Handle(AIS_Shape)& ais = it!=m_items.end()?it->second.ais:sk->second.ais;
    if (r.kind == opad::Ref::Kind::Center) {
      std::shared_ptr<BodyPrs> prs;
      if(sk!=m_sketchWires.end()) prs=sk->second.prs;
      else { std::lock_guard<std::mutex> lock(m_meshMu); auto p = m_prs.find(it->second.key); if (p != m_prs.end()) prs = p->second; }
      if (prs && prs->circles.count(r.index)) {
        gp_Pnt point = prs->circles.at(r.index).center.Transformed(ais->Transformation());
        Handle(AIS_Shape) marker = centerMarker(r, point);
        if (!m_ctx->IsSelected(marker)) m_ctx->AddOrRemoveSelected(marker, false);
      }
      continue;
    }
    if (r.kind == opad::Ref::Kind::Body) {
      m_ctx->AddOrRemoveSelected(ais, Standard_False);
      m_selApplied.push_back(ais);
      continue;
    }
    const TopAbs_ShapeEnum t = r.kind == opad::Ref::Kind::Face ? TopAbs_FACE : r.kind == opad::Ref::Kind::Edge ? TopAbs_EDGE : TopAbs_VERTEX;
    const Handle(SelectMgr_Selection)& sel = ais->Selection(AIS_Shape::SelectionMode(t));
    if (sel.IsNull()) continue;
    for (NCollection_Vector<Handle(SelectMgr_SensitiveEntity)>::Iterator e(sel->Entities()); e.More(); e.Next()) {
      Handle(SubShapeOwner) owner = Handle(SubShapeOwner)::DownCast(e.Value()->BaseSensitive()->OwnerId());
      if (owner.IsNull() || owner->index() != r.index) continue;
      if (!m_ctx->IsSelected(owner)) m_ctx->AddOrRemoveSelected(owner, Standard_False);
      break;
    }
  }
  for (const auto& id : candidates)
    for (const auto& c : m_candidates)
      if (c.first == id && !m_ctx->IsSelected(c.second)) m_ctx->AddOrRemoveSelected(c.second, Standard_False);
  applySelectionLayers();
  refreshSubHighlight();
  redrawScene();
}

void Viewport::setBodiesPickable(bool on) {
  if (m_bodiesPickable == on) return;
  m_bodiesPickable = on;
  if (!on) clearCenters();
  if (!m_initialised) return;
  if (on) return applySelectionFilter(m_filter);  // sliced: re-activates every body in the current mode
  if (m_filterJob) m_filterJob->cancel();
  for (auto& [id, it] : m_items) m_ctx->Deactivate(it.ais);
  for (auto& [id, it] : m_sketchWires) m_ctx->Deactivate(it.ais);
}

// ---------------------------------------------------------------- feature preview
void Viewport::setPreviewBodies(const std::vector<std::pair<std::string, TopoDS_Shape>>& shapes, const std::vector<std::string>& hidden) {
  std::vector<PreviewPart> parts;
  for (const auto& [node, shape] : shapes) parts.push_back({node, shape, nullptr});
  setPreviewBodies(parts, hidden);
}

void Viewport::setPreviewBodies(const std::vector<PreviewPart>& parts, const std::vector<std::string>& hidden) {
  if (!m_initialised) return;
  clearPreviewBodies();
  auto hide = [this](const std::string& node) {
    auto it = m_items.find(node);
    if (it == m_items.end() || m_previewHidden.count(node)) return;
    m_ctx->Erase(it->second.ais, Standard_False);
    // Its selection glow goes too: left behind, a moved body looked copied. clearPreviewBodies brings it back.
    if (auto glow = m_bodyGlows.find(it->second.ais.get()); glow != m_bodyGlows.end()) {
      m_ctx->Remove(glow->second, Standard_False);
      m_bodyGlows.erase(glow);
    }
    m_previewHidden.insert(node);
  };
  for (const auto& id : hidden) hide(id);
  for (const auto& [node, shape, prs] : parts) {
    if (shape.IsNull()) continue;
    if (!node.empty()) hide(node);
    Handle(AIS_Shape) ais = prs ? Handle(AIS_Shape)(new BodyShape(shape, prs)) : new AIS_Shape(shape);
    ais->Attributes()->SetAutoTriangulation(Standard_False);  // the worker meshed it
    ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
    QColor tint = m_tokens.sel;
    if (!node.empty()) {  // a changed body keeps its colour, leaning towards the preview tint
      auto it = m_items.find(node);
      if (it != m_items.end()) {
        const auto& c = it->second.color;
        tint = QColor::fromRgbF(static_cast<float>(c[0] * 0.6 + tint.redF() * 0.4), static_cast<float>(c[1] * 0.6 + tint.greenF() * 0.4), static_cast<float>(c[2] * 0.6 + tint.blueF() * 0.4));
      }
    }
    ais->SetColor(occ(tint));
    // A lone face is a construction plane's sheet: see-through, it must not hide the model it cuts through.
    ais->SetTransparency(shape.ShapeType() == TopAbs_FACE ? 0.78 : 0.25);
    if (shape.ShapeType() == TopAbs_EDGE) ais->SetWidth(2.5);  // a construction axis
    ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
    ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.0));
    m_ctx->Display(ais, AIS_Shaded, -1, Standard_False);
    m_previewBodies.push_back(ais);
  }
  redrawScene();
}

void Viewport::setPreviewDisplay(const std::vector<std::shared_ptr<const BodyPrs>>& arrays) {
  if (!m_initialised) return;
  bool any = false;
  for (size_t i = 0; i < m_previewBodies.size() && i < arrays.size(); ++i) {
    Handle(BodyShape) body = Handle(BodyShape)::DownCast(m_previewBodies[i]);
    if (body.IsNull() || !body->setDisplayPrs(arrays[i])) continue;
    m_ctx->Redisplay(body, Standard_False);
    any = true;
  }
  if (any) redrawScene();
}

void Viewport::clearPreviewBodies() {
  if (!m_initialised) return;
  if (m_previewBodies.empty() && m_previewHidden.empty()) return;
  for (const auto& p : m_previewBodies) m_ctx->Remove(p, Standard_False);
  m_previewBodies.clear();
  for (const auto& node : m_previewHidden) {
    auto it = m_items.find(node);
    if (it == m_items.end() || !it->second.look.visible) continue;  // hidden by its look meanwhile (UI-121)
    m_ctx->Display(it->second.ais, m_style == Style::Wireframe ? AIS_WireFrame : AIS_Shaded, -1, Standard_False);
    activateSelection(it->second.ais);
  }
  const bool hadHidden = !m_previewHidden.empty();
  m_previewHidden.clear();
  if (hadHidden) applySelectionLayers();  // glows of the ones still selected
  redrawScene();
}

void Viewport::setEdgeHover(bool on) {
  if (!m_initialised) return;
  const bool references = ghostsPickable();
  m_edgeHover = on;
  referencesChanged(references);  // Project takes edges of other components too (UI-33)
  if (on) {
    m_bodiesPickable = true;
    setSelectionFilter(SelFilter::Edge);
  } else {
    m_bodiesPickable = true;  // so the call below is not a no-op
    setBodiesPickable(false);
  }
}

bool Viewport::hoveredEdge(TopoDS_Shape& edge) const {
  if (!m_initialised || !m_ctx->HasDetected()) return false;
  Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
  if (owner.IsNull() || !owner->HasShape() || owner->Shape().ShapeType() != TopAbs_EDGE) return false;
  edge = owner->Shape();
  Handle(AIS_InteractiveObject) obj = m_ctx->DetectedInteractive();
  if (!obj.IsNull() && obj->HasTransformation()) edge = edge.Moved(TopLoc_Location(obj->LocalTransformation()));
  return true;
}

void Viewport::setPreparedPreview(const TopoDS_Shape& shape,std::shared_ptr<const BodyPrs> prs,const std::vector<std::string>& hidden) {
  if(!m_initialised)return;clearPreviewBodies();
  for(const auto& id:hidden)if(auto it=m_items.find(id);it!=m_items.end()){m_ctx->Erase(it->second.ais,false);m_previewHidden.insert(id);}
  Handle(AIS_Shape) ais=new BodyShape(shape,std::move(prs));ais->SetColor(occ(m_tokens.sel));ais->SetTransparency(0.25);
  ais->Attributes()->SetFaceBoundaryDraw(true);m_ctx->Display(ais,AIS_Shaded,-1,false);m_previewBodies.push_back(ais);redrawScene();
}

bool Viewport::referenceAt(const QPointF& point,opad::Ref& ref) {
  if(!m_initialised)return false;
  moveTo(devicePos(point));
  return hoveredReference(ref);
}
bool Viewport::surfaceAt(const QPointF& point, std::string& candidate, TopoDS_Face& face, opad::Vec3& at) {
  candidate.clear();
  face.Nullify();
  if (!m_initialised) return false;
  moveTo(devicePos(point));
  if (!m_ctx->HasDetected()) return false;
  gp_Pnt hit;
  if (!detectedPoint(hit)) return false;
  at = {hit.X(), hit.Y(), hit.Z()};
  const Handle(AIS_InteractiveObject) object = m_ctx->DetectedInteractive();
  for (const auto& c : m_candidates)
    if (c.second == object) {
      candidate = c.first;
      return true;
    }
  const Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
  if (owner.IsNull() || !owner->HasShape() || owner->Shape().ShapeType() != TopAbs_FACE || !m_nodeOf.count(object.get())) return false;
  face = TopoDS::Face(owner->Shape());
  if (!object.IsNull() && object->HasTransformation()) face = TopoDS::Face(face.Moved(TopLoc_Location(object->LocalTransformation())));
  return true;
}

bool Viewport::originReferenceAt(const QPointF& point,opad::Ref& ref) {
  if(!m_initialised)return false;
  // Center candidates already share the vertex selector; enable their cheap
  // sensitive objects while placing an origin, without a separate picking mode.
  setCenterPicking(true,point);
  return referenceAt(point,ref);
}
void Viewport::setPreviewCurves(std::shared_ptr<const BodyPrs> curves,std::shared_ptr<const BodyPrs> construction,const std::vector<std::string>& hidden) {
  if(!m_initialised)return;
  clearPreviewBodies();
  for(const auto& id:hidden)if(auto it=m_items.find(id);it!=m_items.end()) {
    m_ctx->Erase(it->second.ais,false);m_previewHidden.insert(id);
  }
  // Line-only arrays: BodyShape draws them as they are (no shape behind them to walk here).
  TopoDS_Compound none;BRep_Builder().MakeCompound(none);
  for(auto* prs:{&curves,&construction}) {
    if(!*prs || ((*prs)->boundaries.IsNull() && (*prs)->loosePoints.IsNull())) continue;
    Handle(AIS_Shape) ais=new BodyShape(none,std::move(*prs));
    ais->SetColor(occ(m_tokens.sel));ais->SetWidth(prs==&curves?2:1.5);
    if(prs==&construction) ais->Attributes()->WireAspect()->SetTypeOfLine(Aspect_TOL_DASH);
    m_ctx->Display(ais,AIS_WireFrame,-1,false);m_previewBodies.push_back(ais);
  }
  redrawScene();
}
size_t Viewport::previewSegments() const {
  size_t n=0;
  for(const auto& ais:m_previewBodies)
    if(Handle(BodyShape) body=Handle(BodyShape)::DownCast(ais);!body.IsNull() && body->prs() && !body->prs()->boundaries.IsNull()) n+=size_t(body->prs()->boundaries->VertexNumber()/2);
  return n;
}

// ---------------------------------------------------------------- overlays
void Viewport::showOverlay(const Handle(AIS_InteractiveObject)& obj) {
  if (!m_initialised || obj.IsNull()) return;
  obj->SetZLayer(Graphic3d_ZLayerId_Topmost);
  m_ctx->Display(obj, 0, -1, Standard_False);
  m_overlays.erase(std::remove_if(m_overlays.begin(), m_overlays.end(), [&](const Handle(AIS_InteractiveObject)& o) { return o == obj || !m_ctx->IsDisplayed(o); }), m_overlays.end());
  m_overlays.push_back(obj);
  redrawScene();
}

void Viewport::updateOverlay(const Handle(AIS_InteractiveObject)& obj) {
  if (!m_initialised || obj.IsNull()) return;
  m_ctx->Redisplay(obj, Standard_False);
  redrawScene();
}

void Viewport::removeOverlay(const Handle(AIS_InteractiveObject)& obj) {
  if (!m_initialised || obj.IsNull()) return;
  m_ctx->Remove(obj, Standard_False);
  m_overlays.erase(std::remove(m_overlays.begin(), m_overlays.end(), obj), m_overlays.end());
  redrawScene();
}

// ---------------------------------------------------------------- sketch input
void Viewport::beginSketchInput(SketchInput* input, const opad::Frame& frame, const std::string& hiddenSketch) {
  resetHoverFade();
  m_sketchInput = input;
  m_sketchFrame = frame;
  const auto normal=frame.normal();
  m_viewer->SetPrivilegedPlane(gp_Ax3(gp_Pnt(frame.origin[0],frame.origin[1],frame.origin[2]),gp_Dir(normal[0],normal[1],normal[2]),gp_Dir(frame.x[0],frame.x[1],frame.x[2])));
  // No grid echo while sketching: AIS_ViewController drew OCCT's grey star on the grid node nearest the pointer after
  // every move, a node the sketch's snapping had not chosen (an object snap won), so the click went elsewhere. The
  // editor marks the node it uses itself. Outside a sketch the echo is OCCT's as before.
  m_viewer->SetGridEcho(Standard_False);
  m_sketchGrid = QSettings().value("sketch/grid", true).toBool();
  showGrid();  // on the sketch plane, following the zoom
  m_sketchDrag = false;
  m_hiddenSketch = hiddenSketch;
  clearSelection();
  setBodiesPickable(false);  // the left button draws; bodies are not picked while sketching
  syncSketches();
  setFocus();
  redrawScene();
}

void Viewport::endSketchInput() {
  resetHoverFade();
  m_sketchInput = nullptr;
  m_ownCursorWanted = m_ownCursorAside = false;
  applyOwnCursor();
  m_viewer->SetGridEcho(Standard_True);
  m_viewer->SetPrivilegedPlane(gp_Ax3(gp::Origin(),gp::DZ(),gp::DX()));
  showGrid();
  m_hiddenSketch.clear();
  setBodiesPickable(true);
  syncSketches();
  redrawScene();
}

opad::Frame Viewport::cameraPlane() const {
  if (!m_initialised) return {};
  // Orthographic cameras can have an extremely short eye-to-center distance.
  // Use the camera's normalized direction instead of subtracting its points.
  const auto camera = m_view->Camera();
  const gp_Dir normal = camera->Direction().Reversed();
  gp_Vec right = gp_Vec(camera->Up()).Crossed(gp_Vec(normal));
  if (right.SquareMagnitude() < 1e-12)
    right = (std::abs(normal.Z()) < .9 ? gp_Vec(0,0,1) : gp_Vec(0,1,0)).Crossed(gp_Vec(normal));
  return opad::design::frame_from_ax3(gp_Ax3(gp_Pnt(0,0,0), normal, gp_Dir(right)));
}

void Viewport::lookAt(const opad::Frame& frame, bool fit, bool animate) {
  if (!m_initialised) return;
  m_needFit = false;
  Handle(Graphic3d_Camera) cam = m_view->Camera();
  Handle(Graphic3d_Camera) start = new Graphic3d_Camera(*cam), end = new Graphic3d_Camera(*cam);
  const opad::Vec3 n = frame.normal();
  const double dist = std::max(cam->Distance(), 1.0);
  const gp_Pnt centre(frame.origin[0], frame.origin[1], frame.origin[2]);
  end->SetUp(gp_Dir(frame.y[0], frame.y[1], frame.y[2]));
  end->SetEyeAndCenter(centre.Translated(gp_Vec(n[0], n[1], n[2]) * dist), centre);
  if (fit) {
    Bnd_Box box = m_view->View()->MinMaxValues();
    if (!box.IsVoid() && box.SquareExtent() > 1e-6) {
      box.Add(centre);
      end->FitMinMax(box, 1e-6, false);
      end->SetScale(end->Scale() * 1.25);
    } else {
      end->SetScale(120.0);  // an empty design: a sheet of paper's worth of plane, not whatever the view was left at
    }
  }
  if(!animate || motion::reduced()) {  // reduced motion (UI-124): the camera jumps
    myViewAnimation->Stop();m_view->SetCamera(end);m_view->Invalidate();requestRedraw();return;
  }
  myViewAnimation->SetView(m_view);
  myViewAnimation->SetCameraStart(start);
  myViewAnimation->SetCameraEnd(end);
  myViewAnimation->SetOwnDuration(0.35);
  myViewAnimation->StartTimer(0.0, 1.0, Standard_True);
  requestRedraw();
}

bool Viewport::planePoint(const QPointF& widgetPos, const opad::Frame& frame, double& u, double& v) const {
  if (!m_initialised) return false;
  const Graphic3d_Vec2i pos = devicePos(widgetPos);
  Standard_Real x, y, z, dx, dy, dz;
  m_view->ConvertWithProj(pos.x(), pos.y(), x, y, z, dx, dy, dz);
  const opad::Vec3 n = frame.normal();
  const double denom = dx * n[0] + dy * n[1] + dz * n[2];
  if (std::fabs(denom) < 1e-9) return false;  // looking along the plane
  const double t = ((frame.origin[0] - x) * n[0] + (frame.origin[1] - y) * n[1] + (frame.origin[2] - z) * n[2]) / denom;
  frame.to_local({x + dx * t, y + dy * t, z + dz * t}, u, v);
  return true;
}

double Viewport::pixelSize() const {
  if (!m_initialised) return 1.0;
  return m_view->Convert(1) * viewScale().x();  // Convert() takes view pixels; callers think in widget points
}

opad::Vec3 Viewport::viewDirection() const {
  if (!m_initialised) return {0, 0, -1};
  const gp_Dir d = m_view->Camera()->Direction();
  return {d.X(), d.Y(), d.Z()};
}

QPoint Viewport::widgetPoint(const opad::Vec3& world) const {
  if (!m_initialised) return {};
  Standard_Integer px = 0, py = 0;
  m_view->Convert(world[0], world[1], world[2], px, py);
  const QPointF scale = viewScale();
  return QPoint(qRound(px / scale.x()), qRound(py / scale.y()));
}

std::function<QPointF(const opad::Vec3&)> Viewport::projector() const {
  if (!m_initialised) return {};
  const Handle(Graphic3d_Camera) camera = new Graphic3d_Camera(m_view->Camera());
  const double w = width(), h = height();
  return [camera, w, h](const opad::Vec3& p) {
    const gp_Pnt n = camera->Project(gp_Pnt(p[0], p[1], p[2]));  // normalised device coordinates, y up
    return QPointF((n.X() + 1) / 2 * w, (1 - n.Y()) / 2 * h);
  };
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
  if (m_blocked) return;
  double u, v;
  if (m_sketchInput && e->button() == Qt::LeftButton && planePoint(e->position(), m_sketchFrame, u, v)) return m_sketchInput->sketchDoubleClick(u, v);
  mousePressEvent(e);
}

bool Viewport::event(QEvent* e) {
  if (e->type() == QEvent::NativeGesture && handleNativeGesture(static_cast<QNativeGestureEvent*>(e))) return true;
  // An overlay made while the pointer is blank over the view (a toast, a value box) keeps the arrow, as the others do.
  if (e->type() == QEvent::ChildPolished && m_ownCursorShown)
    if (auto* child = qobject_cast<QWidget*>(static_cast<QChildEvent*>(e)->child()); child && !child->isWindow() && !child->testAttribute(Qt::WA_SetCursor))
      child->setCursor(Qt::ArrowCursor);
  if (e->type() == QEvent::Gesture) {
    auto* gestures = static_cast<QGestureEvent*>(e);
    if (auto* pinch = static_cast<QPinchGesture*>(gestures->gesture(Qt::PinchGesture))) {
      if (m_initialised && !m_blocked && (pinch->changeFlags() & QPinchGesture::ScaleFactorChanged)) {
        finishTrackpadScroll();
        const QPointF position = pinch->hasHotSpot() ? mapFromGlobal(pinch->hotSpot()) : mapFromGlobal(QCursor::pos());
        zoomAt(position, pinch->scaleFactor());
      }
      gestures->accept(pinch);
      return true;
    }
  }
  // Sketch editing keeps Esc/Enter/Delete and the keys that type values. Tool shortcuts are configurable QActions;
  // their outside-sketch counterparts are disabled while the editor is active.
  if (e->type() == QEvent::ShortcutOverride && m_sketchInput) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (!(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && m_sketchInput->sketchKey(k)) {
      e->accept();
      return true;
    }
  }
  // Tab goes round the tool's value boxes; QWidget::event would move the keyboard off the view before keyPressEvent.
  if (e->type() == QEvent::KeyPress && m_sketchInput) {
    auto* k = static_cast<QKeyEvent*>(e);
    if ((k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
      m_sketchInput->sketchType(k);
      e->accept();
      return true;
    }
  }
  return QWidget::event(e);
}

void Viewport::keyPressEvent(QKeyEvent* e) {
  if (m_sketchInput) {  // the editing keys were handled (or refused) at the shortcut override; values are typed now
    m_sketchInput->sketchType(e);
    return e->accept();
  }
  if (inferenceKey(e) || trackingEscape(e)) return e->accept();
  QWidget::keyPressEvent(e);
}

void Viewport::benchDesignShot(const QString& path) {
  if (!m_initialised) return;
  m_view->FitAll(fitBounds(), 0.1, Standard_False);
  m_view->Redraw();
  grabImage().save(path);
}

bool Viewport::hoveredReference(opad::Ref& ref) const {
  if(!m_initialised||!m_ctx->HasDetected())return false;
  const auto object=m_ctx->DetectedInteractive();const auto found=m_nodeOf.find(object.get());if(found==m_nodeOf.end())return false;
  ref.body=found->second;ref.kind=opad::Ref::Kind::Body;
  if(m_filter==SelFilter::Body)return true;
  if(const auto owner=Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner());!owner.IsNull()){ref.kind=owner->kind();ref.index=owner->index();return true;}
  // A body drawn through the stock AIS_Shape (a reopened document) has owners without an ordinal: work it out the way
  // selection() does, or a projection could not take its edges.
  const auto stock=Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());const auto ais=Handle(AIS_Shape)::DownCast(object);
  if(stock.IsNull()||!stock->HasShape()||ais.IsNull())return false;
  const TopoDS_Shape& sub=stock->Shape();
  ref.kind=sub.ShapeType()==TopAbs_FACE?opad::Ref::Kind::Face:sub.ShapeType()==TopAbs_EDGE?opad::Ref::Kind::Edge:sub.ShapeType()==TopAbs_VERTEX?opad::Ref::Kind::Vertex:opad::Ref::Kind::Body;
  if(ref.kind==opad::Ref::Kind::Body)return false;
  ref.index=opad::subshape_index(ais->Shape(),sub);return true;
}
void Viewport::showBackdrop(const Handle(AIS_InteractiveObject)& obj) {
  if(!m_initialised||obj.IsNull())return;obj->SetZLayer(Graphic3d_ZLayerId_Default);m_ctx->Display(obj,3,-1,false);redrawScene();
}
opad::json Viewport::sectionState() const {return {{"enabled",m_sectionEnabled},{"origin",m_sectionOrigin},{"normal",m_sectionNormal},{"caps",m_sectionCaps}};}
void Viewport::restoreSection(const opad::json& state) {setSection(state.at("enabled").get<bool>(),state.at("origin").get<opad::Vec3>(),state.at("normal").get<opad::Vec3>(),state.at("caps").get<bool>());}
