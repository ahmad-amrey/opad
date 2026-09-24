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

#include "Jobs.hpp"
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
    if (!s.visible || s.id == m_hiddenSketch) continue;
    keep.insert(s.id);
    const std::string stamp = s.geometry.dump() + s.frame.to_json().dump();
    auto it = m_sketchWires.find(s.id);
    if (it != m_sketchWires.end() && it->second.stamp == stamp) continue;
    if (it != m_sketchWires.end()) { for(const auto& image:it->second.backdrops)m_ctx->Remove(image,false);m_nodeOf.erase(it->second.ais.get()); m_ctx->Remove(it->second.ais, Standard_False); }
    std::shared_ptr<PreparedSketch> prepared;
    if(s.geometry.value("entities",opad::json::array()).size()>256 || !s.geometry.value("images",opad::json::array()).empty()) {
      auto found=m_preparedSketches.find(s.id);
      if(found==m_preparedSketches.end() || found->second->stamp!=stamp) {
        prepared=std::make_shared<PreparedSketch>();prepared->stamp=stamp;m_preparedSketches[s.id]=prepared;
        const auto geometry=s.geometry;const auto frame=s.frame;const auto id=s.id;const auto generation=m_doc->generation;
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
  for (const auto& c : candidates) {
    if (c.shape.IsNull()) continue;
    // Small planar regions and a few curves: meshing them here is cheaper than a job round trip. (The
    // context's drawers never triangulate by themselves, see initViewer.)
    if (c.shape.ShapeType() <= TopAbs_FACE) BRepMesh_IncrementalMesh(c.shape, 0.05, Standard_False, 0.3, Standard_False);
    Handle(AIS_Shape) ais = new AIS_Shape(c.shape);
    const bool surface = c.shape.ShapeType() <= TopAbs_FACE;
    ais->SetColor(occ(m_tokens.sel));
    if (surface) {
      ais->SetTransparency(c.strong ? 0.6 : 0.82);
      ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
      ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.0));
    } else {
      ais->SetWidth(3.0);
      ais->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O_POINT, occ(m_tokens.sel), 3.0));
    }
    ais->SetZLayer(Graphic3d_ZLayerId_Top);  // over coplanar body faces
    m_ctx->Display(ais, surface ? AIS_Shaded : AIS_WireFrame, -1, Standard_False);
    m_ctx->Load(ais, -1);
    m_ctx->Activate(ais, 0);
    m_candidates.push_back({c.id, ais});
  }
  redrawScene();
}

void Viewport::clearCandidates() {
  if (!m_initialised || m_candidates.empty()) return;
  for (const auto& c : m_candidates) m_ctx->Remove(c.second, Standard_False);
  m_candidates.clear();
  redrawScene();
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

void Viewport::selectRefs(const std::vector<opad::Ref>& refs, const std::vector<std::string>& candidates) {
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
  if (on) return setSelectionFilter(m_filter);  // sliced: re-activates every body in the current mode
  if (m_filterJob) m_filterJob->cancel();
  for (auto& [id, it] : m_items) m_ctx->Deactivate(it.ais);
  for (auto& [id, it] : m_sketchWires) m_ctx->Deactivate(it.ais);
}

// ---------------------------------------------------------------- feature preview
void Viewport::setPreviewBodies(const std::vector<std::pair<std::string, TopoDS_Shape>>& shapes, const std::vector<std::string>& hidden) {
  if (!m_initialised) return;
  clearPreviewBodies();
  auto hide = [this](const std::string& node) {
    auto it = m_items.find(node);
    if (it == m_items.end() || m_previewHidden.count(node)) return;
    m_ctx->Erase(it->second.ais, Standard_False);
    m_previewHidden.insert(node);
  };
  for (const auto& id : hidden) hide(id);
  for (const auto& [node, shape] : shapes) {
    if (shape.IsNull()) continue;
    if (!node.empty()) hide(node);
    Handle(AIS_Shape) ais = new AIS_Shape(shape);
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
    ais->SetTransparency(0.25);
    ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
    ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.0));
    m_ctx->Display(ais, AIS_Shaded, -1, Standard_False);
    m_previewBodies.push_back(ais);
  }
  redrawScene();
}

void Viewport::clearPreviewBodies() {
  if (!m_initialised) return;
  if (m_previewBodies.empty() && m_previewHidden.empty()) return;
  for (const auto& p : m_previewBodies) m_ctx->Remove(p, Standard_False);
  m_previewBodies.clear();
  for (const auto& node : m_previewHidden) {
    auto it = m_items.find(node);
    if (it == m_items.end()) continue;
    m_ctx->Display(it->second.ais, m_style == Style::Wireframe ? AIS_WireFrame : AIS_Shaded, -1, Standard_False);
    activateSelection(it->second.ais);
  }
  m_previewHidden.clear();
  redrawScene();
}

void Viewport::setEdgeHover(bool on) {
  if (!m_initialised) return;
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

bool Viewport::referenceAt(const QPointF& point,opad::Ref& ref) {
  if(!m_initialised)return false;
  const auto pos=devicePos(point);m_ctx->MoveTo(pos.x(),pos.y(),m_view,false);
  return hoveredReference(ref);
}

// ---------------------------------------------------------------- overlays
void Viewport::showOverlay(const Handle(AIS_InteractiveObject)& obj) {
  if (!m_initialised || obj.IsNull()) return;
  obj->SetZLayer(Graphic3d_ZLayerId_Topmost);
  m_ctx->Display(obj, 0, -1, Standard_False);
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
  redrawScene();
}

// ---------------------------------------------------------------- sketch input
void Viewport::beginSketchInput(SketchInput* input, const opad::Frame& frame, const std::string& hiddenSketch) {
  m_sketchInput = input;
  m_sketchFrame = frame;
  m_sketchDrag = false;
  m_hiddenSketch = hiddenSketch;
  clearSelection();
  setBodiesPickable(false);  // the left button draws; bodies are not picked while sketching
  syncSketches();
  setFocus();
  redrawScene();
}

void Viewport::endSketchInput() {
  m_sketchInput = nullptr;
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
  if(!animate) {
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

QPoint Viewport::widgetPoint(const opad::Vec3& world) const {
  if (!m_initialised) return {};
  Standard_Integer px = 0, py = 0;
  m_view->Convert(world[0], world[1], world[2], px, py);
  const QPointF scale = viewScale();
  return QPoint(qRound(px / scale.x()), qRound(py / scale.y()));
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
  if (m_blocked) return;
  double u, v;
  if (m_sketchInput && e->button() == Qt::LeftButton && planePoint(e->position(), m_sketchFrame, u, v)) return m_sketchInput->sketchDoubleClick(u, v);
  mousePressEvent(e);
}

bool Viewport::event(QEvent* e) {
  if (e->type() == QEvent::NativeGesture && handleNativeGesture(static_cast<QNativeGestureEvent*>(e))) return true;
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
  // Sketch editing keeps Esc/Enter/Delete. Tool shortcuts are configurable QActions;
  // their outside-sketch counterparts are disabled while the editor is active.
  if (e->type() == QEvent::ShortcutOverride && m_sketchInput) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (!(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && m_sketchInput->sketchKey(k)) {
      e->accept();
      return true;
    }
  }
  return QWidget::event(e);
}

void Viewport::keyPressEvent(QKeyEvent* e) {
  if (m_sketchInput) return e->accept();  // already handled (or refused) at the shortcut-override stage
  if (inferenceKey(e)) return e->accept();
  QWidget::keyPressEvent(e);
}

void Viewport::benchDesignShot(const QString& path) {
  if (!m_initialised) return;
  m_view->FitAll(0.1, Standard_False);
  m_view->Redraw();
  grabImage().save(path);
}

bool Viewport::hoveredReference(opad::Ref& ref) const {
  if(!m_initialised||!m_ctx->HasDetected())return false;
  const auto object=m_ctx->DetectedInteractive();const auto found=m_nodeOf.find(object.get());if(found==m_nodeOf.end())return false;
  ref.body=found->second;ref.kind=opad::Ref::Kind::Body;
  const auto owner=Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner());
  if(m_filter!=SelFilter::Body){if(owner.IsNull())return false;ref.kind=owner->kind();ref.index=owner->index();}return true;
}
void Viewport::showBackdrop(const Handle(AIS_InteractiveObject)& obj) {
  if(!m_initialised||obj.IsNull())return;obj->SetZLayer(Graphic3d_ZLayerId_Default);m_ctx->Display(obj,3,-1,false);redrawScene();
}
opad::json Viewport::sectionState() const {return {{"enabled",m_sectionEnabled},{"origin",m_sectionOrigin},{"normal",m_sectionNormal},{"caps",m_sectionCaps}};}
void Viewport::restoreSection(const opad::json& state) {setSection(state.at("enabled").get<bool>(),state.at("origin").get<opad::Vec3>(),state.at("normal").get<opad::Vec3>(),state.at("caps").get<bool>());}
