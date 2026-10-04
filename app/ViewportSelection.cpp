#include "Viewport.hpp"
#include "Jobs.hpp"
#include "CurveSamples.hpp"
#include <AIS_RubberBand.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <QLineF>
#include <SelectMgr_ViewerSelector.hxx>
#include <Graphic3d_Camera.hxx>
#include <QElapsedTimer>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <Poly_Triangulation.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <Graphic3d_ZLayerSettings.hxx>
#include <Image_PixMap.hxx>
#include <V3d_ImageDumpOptions.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>

// The view as drawn now (UI-43): where a point is drawn and, when the frame's depth could be read back, whether something
// is drawn in front of it there. A box tests every candidate's points against it in microseconds; a pick of the Engine took
// a millisecond and a box of its faces never finished.
struct Viewport::DepthImage {
  Graphic3d_Mat4d world;     // world -> clip
  int width = 0, height = 0;  // device pixels
  bool zeroToOne = false, ortho = true;
  gp_Pnt eye;
  gp_Dir direction;
  double distance = 1, pixel = 1;  // world units a widget pixel at the focus (pointVisible's slack: 3 of them)
  std::vector<float> depth;  // rows from the top; empty when it could not be read
  QRect unknown;             // the view cube's corner: drawn in a projection of its own
  bool readable() const { return !depth.empty(); }
  bool project(const gp_Pnt& p, double& x, double& y, double& d) const {  // device pixels and depth in [0, 1]
    const Graphic3d_Vec4d c = world * Graphic3d_Vec4d(p.X(), p.Y(), p.Z(), 1);
    if (!(c.w() > 1e-12)) return false;
    x = (c.x() / c.w() + 1) / 2 * width;
    y = (1 - c.y() / c.w()) / 2 * height;
    d = zeroToOne ? c.z() / c.w() : (c.z() / c.w() + 1) / 2;
    return true;
  }
  // 1: in sight, 0: something is drawn in front of it there (by more than the slack), -1: the image cannot tell.
  int verdict(const gp_Pnt& p) const {
    double x = 0, y = 0, d = 0, x2 = 0, y2 = 0, d2 = 0;
    if (!readable() || !project(p, x, y, d)) return -1;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    if (ix < 0 || iy < 0 || ix >= width || iy >= height || unknown.contains(ix, iy)) return -1;
    const double scale = ortho ? 1.0 : std::max(1e-6, gp_Vec(eye, p).Dot(gp_Vec(direction)) / distance);
    const gp_Vec back = ortho ? gp_Vec(direction).Reversed() : gp_Vec(p, eye).Normalized();
    if (!project(p.Translated(back * 3 * pixel * scale), x2, y2, d2)) return -1;
    return d <= double(depth[size_t(iy) * size_t(width) + size_t(ix)]) + (d - d2) ? 1 : 0;
  }
};

// One frame drawn into an image for its depth. For that frame the layers that clear the depth (Topmost: the selection, the
// view cube; TopOSD) keep it, so it holds the front of every body drawn.
std::shared_ptr<const Viewport::DepthImage> Viewport::captureDepth() {
  auto image = std::make_shared<DepthImage>();
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  const auto camera = m_view->Camera();
  image->width = w;
  image->height = h;
  image->world = camera->ProjectionMatrix() * camera->OrientationMatrix();
  image->zeroToOne = camera->IsZeroToOneDepth();
  image->ortho = camera->IsOrthographic();
  image->eye = camera->Eye();
  image->direction = camera->Direction();
  image->distance = camera->Distance();
  image->pixel = pixelSize();
  if (w <= 0 || h <= 0) return image;
  std::vector<std::pair<Graphic3d_ZLayerId, Graphic3d_ZLayerSettings>> kept;
  for (const Graphic3d_ZLayerId id : {Graphic3d_ZLayerId_Topmost, Graphic3d_ZLayerId_TopOSD}) {
    Graphic3d_ZLayerSettings settings = m_viewer->ZLayerSettings(id);
    kept.emplace_back(id, settings);
    settings.SetClearDepth(Standard_False);
    m_viewer->SetZLayerSettings(id, settings);
  }
  Image_PixMap pixels;
  V3d_ImageDumpOptions options;
  options.Width = w;
  options.Height = h;
  options.BufferType = Graphic3d_BT_Depth;
  const bool ok = m_view->ToPixMap(pixels, options);
  for (const auto& [id, settings] : kept) m_viewer->SetZLayerSettings(id, settings);
  if (!ok || pixels.Format() != Image_Format_GrayF || int(pixels.SizeX()) != w || int(pixels.SizeY()) != h) return image;
  // ToPixMap fitted the camera's depth range to the frame: the projection is the one the depth was drawn with.
  image->world = camera->ProjectionMatrix() * camera->OrientationMatrix();
  image->depth.resize(size_t(w) * size_t(h));
  for (int y = 0; y < h; ++y) std::memcpy(&image->depth[size_t(y) * size_t(w)], pixels.Row(size_t(y)), size_t(w) * sizeof(float));
  const QPointF cube = cubeCentre() * viewScale().x();
  const int r = int(80 * viewScale().x());
  image->unknown = QRect(int(cube.x()) - r, int(cube.y()) - r, 2 * r, 2 * r);
  return image;
}

void Viewport::UpdateRubberBand(const Graphic3d_Vec2i& from,const Graphic3d_Vec2i& to) {
  m_boxStart=from;m_boxEnd=to;m_boxCrossing=to.x()<from.x();
  AIS_ViewController::UpdateRubberBand(from,to);
}

void Viewport::handleSelectionPoly(const Handle(AIS_InteractiveContext)& ctx,const Handle(V3d_View)& view) {
  if(myGL.Selection.Tool!=AIS_ViewSelectionTool_RubberBand) {AIS_ViewController::handleSelectionPoly(ctx,view);return;}
  const bool apply=myGL.Selection.ToApplyTool;
  myGL.Selection.ToApplyTool=false;
  const Quantity_Color color=m_boxCrossing?Quantity_Color(0.25,0.85,0.48,Quantity_TOC_sRGB):Quantity_Color(0.3,0.6,1.0,Quantity_TOC_sRGB);
  myRubberBand->SetLineColor(color);myRubberBand->SetFilling(color,0.88);
  myRubberBand->SetLineType(m_boxCrossing?Aspect_TOL_DASH:Aspect_TOL_SOLID);
  AIS_ViewController::handleSelectionPoly(ctx,view);
  if(!apply) return;
  ctx->Remove(myRubberBand,false);myRubberBand->ClearPoints();
  if(m_boxJob) m_boxJob->cancel();
  const int left=std::min(m_boxStart.x(),m_boxEnd.x()),right=std::max(m_boxStart.x(),m_boxEnd.x());
  const int top=std::min(m_boxStart.y(),m_boxEnd.y()),bottom=std::max(m_boxStart.y(),m_boxEnd.y());
  auto selector=ctx->MainSelector();selector->AllowOverlapDetection(m_boxCrossing);
  selector->Pick(left,top,right,bottom,view);selector->AllowOverlapDetection(false);
  struct Probe { Handle(SubShapeOwner) owner; std::string body; };
  struct Surface { Handle(SelectMgr_EntityOwner) owner; std::string body; QRect footprint; std::vector<gp_Pnt> points; bool loaded=false,done=false; };
  struct State {
    std::vector<Handle(SelectMgr_EntityOwner)> candidates,visible;
    std::set<const SelectMgr_EntityOwner*> remaining;
    std::vector<QPoint> seeds;
    std::vector<Probe> probes;
    std::vector<Surface> surfaces;  // the faces and bodies: by points on their triangles (then, with no depth, by pixels)
    std::vector<gp_Pnt> points;  // the probe's, untested
    std::shared_ptr<const DepthImage> depth;  // read in the first step
    size_t seeded=0,published=0,probed=0,turn=0,sampling=0;
    bool started=false,loaded=false,scanning=false;
    QElapsedTimer feedback;
    int x=0,y=0,stride=8,picks=0,tests=0;
    QRect scan;  // the pixel pass's: the remaining footprints' bounds at this stride
  };
  auto state=std::make_shared<State>();state->feedback.start();
  const QRect area(QPoint(left,top),QPoint(right,bottom));
  for(int i=1;i<=selector->NbPicked();++i) {
    auto owner=selector->Picked(i);
    if(!Handle(CircleOwner)::DownCast(owner).IsNull() || !Handle(OccluderOwner)::DownCast(owner).IsNull()) continue;
    const auto body=m_nodeOf.find(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get());
    if(body==m_nodeOf.end()) continue;
    if(const auto group=Handle(GroupSensitive)::DownCast(selector->PickedEntity(i));!group.IsNull()) {  // every edge or vertex it took (UI-42)
      for(const int index:group->hits())
        if(const auto sub=group->owner(index);!sub.IsNull()) {state->candidates.push_back(sub);state->probes.push_back({sub,body->second});}
      continue;
    }
    state->candidates.push_back(owner);
    if(const auto sub=Handle(SubShapeOwner)::DownCast(owner);!sub.IsNull() && (sub->kind()==opad::Ref::Kind::Edge || sub->kind()==opad::Ref::Kind::Vertex)) {
      state->probes.push_back({sub,body->second});continue;
    }
    state->remaining.insert(owner.get());
    const auto& entity=selector->PickedEntity(i);
    const gp_Trsf placed=owner->Selectable()->Transformation();
    const auto center=entity->CenterOfGeometry().Transformed(placed);
    int x,y;view->Convert(center.X(),center.Y(),center.Z(),x,y);
    state->seeds.emplace_back(std::clamp(x,left,right),std::clamp(y,top,bottom));
    // Where it can be drawn: its picked entity's box on screen, within the selection box.
    const Select3D_BndBox3d bounds=entity->BoundingBox();
    QRect footprint;
    for(int c=0;bounds.IsValid() && c<8;++c) {
      const gp_Pnt p=gp_Pnt(c&1?bounds.CornerMax().x():bounds.CornerMin().x(),c&2?bounds.CornerMax().y():bounds.CornerMin().y(),c&4?bounds.CornerMax().z():bounds.CornerMin().z()).Transformed(placed);
      view->Convert(p.X(),p.Y(),p.Z(),x,y);
      footprint|=QRect(x,y,1,1);
    }
    state->surfaces.push_back({owner,body->second,footprint.isNull()?area:(footprint.adjusted(-1,-1,1,1)&area),{}});
  }
  state->sampling=state->surfaces.size();
  // An edge or a vertex is in sight when one of its points in the box is: the points 3 px apart along it on screen, middle
  // first. A pixel pick of the box in the Edge mode of a big assembly took 50-100 ms and the scan never got through its tens
  // of thousands of edges.
  auto probePoints=[left,right,top,bottom,view](const Handle(SubShapeOwner)& owner) {
    std::vector<gp_Pnt> out;
    owner->prepare();
    if(!owner->HasShape()) return out;
    const gp_Trsf tr=owner->Selectable()->Transformation();
    const TopoDS_Shape& sub=owner->Shape();
    if(sub.ShapeType()==TopAbs_VERTEX) {out.push_back(BRep_Tool::Pnt(TopoDS::Vertex(sub)).Transformed(tr));return out;}
    if(sub.ShapeType()!=TopAbs_EDGE) return out;
    std::vector<gp_Pnt> line=owner->curve?*owner->curve:edgePolyline(TopoDS::Edge(sub));
    auto screen=[&](const gp_Pnt& p) {Standard_Integer x=0,y=0;view->Convert(p.X(),p.Y(),p.Z(),x,y);return QPointF(x,y);};
    std::vector<QPointF> drawn;double length=0;
    for(auto& p:line) {p.Transform(tr);drawn.push_back(screen(p));length+=drawn.size()>1?QLineF(drawn[drawn.size()-2],drawn.back()).length():0;}
    // Not within 4 px of its ends: the test's slack takes a point beside a corner in sight for seen, and a hidden edge
    // ending there would be selected. An edge that short is judged by its middle.
    const double from=length<8?length/2:4,to=length<8?length/2:length-4;
    double at=0;  // along the line on screen
    for(size_t i=1;i<line.size() && out.size()<1024;++i) {
      const gp_Pnt &a=line[i-1],&b=line[i];
      const QPointF A=drawn[i-1],B=drawn[i];
      const double span=QLineF(A,B).length();
      double t0=0,t1=1;  // the stretch on screen inside the box (Liang-Barsky) and away from the ends
      const double p[4]={A.x()-B.x(),B.x()-A.x(),A.y()-B.y(),B.y()-A.y()},q[4]={A.x()-left,right-A.x(),A.y()-top,bottom-A.y()};
      bool inside=true;
      for(int k=0;k<4 && inside;++k) {
        if(std::abs(p[k])<1e-12) {inside=q[k]>=0;continue;}
        if(p[k]<0) t0=std::max(t0,q[k]/p[k]); else t1=std::min(t1,q[k]/p[k]);
      }
      if(span>1e-9) {t0=std::max(t0,(from-at)/span);t1=std::min(t1,(to-at)/span);}
      else if(at<from || at>to) inside=false;
      at+=span;
      if(!inside || t0>t1) continue;
      const int n=std::max(1,int(std::ceil(span*(t1-t0)/3)));
      for(int k=0;k<n;++k) out.push_back(a.Translated(gp_Vec(a,b)*(t0+(t1-t0)*(k+0.5)/n)));
    }
    std::vector<gp_Pnt> ordered;ordered.reserve(out.size());
    std::vector<bool> taken(out.size(),false);
    for(size_t step=out.size();step>=1;step/=2)
      for(size_t i=step/2;i<out.size();i+=step) if(!taken[i]) {taken[i]=true;ordered.push_back(out[i]);}
    std::reverse(ordered.begin(),ordered.end());  // tested from the back
    return ordered;
  };
  // A face or body is in sight when one of its points drawn in the box is: points on its triangles (of up to 4,096 of them,
  // spread over it), as many as a triangle covers pixels on screen in fours (a big face of two triangles had two middles,
  // both behind something while most of it was in sight), up to 4,096 of them tested against the frame's depth, middle
  // first (64 picked without it). A section's cut-away side is not drawn, nor the back of a closed body (culled; a point near
  // its rim was taken for seen when its slack reached past the rim), nor a triangle seen edge on (on an outline, its points
  // fall on pixels the face in front of it does not cover). Triangles are wound outwards (a reversed face's turned).
  auto surfacePoints=[this,area](const Handle(SelectMgr_EntityOwner)& owner,const DepthImage& screen,size_t most) {
    std::vector<std::array<gp_Pnt,3>> triangles;  // world
    const auto object=Handle(AIS_InteractiveObject)::DownCast(owner->Selectable());
    const gp_Trsf placed=object->Transformation();
    auto addFace=[&](const TopoDS_Face& face,int bound) {
      TopLoc_Location at;
      const auto mesh=BRep_Tool::Triangulation(face,at);
      const gp_Trsf tr=placed*at.Transformation();
      const bool turned=(face.Orientation()==TopAbs_REVERSED)!=(tr.VectorialPart().Determinant()<0);
      if(mesh.IsNull() || mesh->NbTriangles()==0) {  // a facet of a mesh body: its first three corners
        std::vector<gp_Pnt> corners;
        for(TopExp_Explorer v(face,TopAbs_VERTEX);v.More() && corners.size()<3;v.Next()) corners.push_back(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Transformed(placed));
        if(corners.size()==3) triangles.push_back({corners[0],corners[1],corners[2]});
        return;
      }
      for(int k=1;k<=mesh->NbTriangles();k+=std::max(1,mesh->NbTriangles()/bound)) {
        int a,b,c;mesh->Triangle(k).Get(a,b,c);
        if(turned) std::swap(b,c);
        triangles.push_back({mesh->Node(a).Transformed(tr),mesh->Node(b).Transformed(tr),mesh->Node(c).Transformed(tr)});
      }
    };
    if(const auto sub=Handle(SubShapeOwner)::DownCast(owner);!sub.IsNull()) sub->prepare();
    const auto brep=Handle(StdSelect_BRepOwner)::DownCast(owner);
    const auto body=Handle(BodyShape)::DownCast(object);
    if(!brep.IsNull() && brep->ComesFromDecomposition() && brep->HasShape()) {
      if(brep->Shape().ShapeType()==TopAbs_FACE) addFace(TopoDS::Face(brep->Shape()),4096);
    } else if(!body.IsNull() && body->prs() && !body->prs()->triangles.IsNull()) {  // the worker's arrays: no walk of the shape
      const auto& array=body->prs()->triangles;
      const bool indexed=array->EdgeNumber()>0;
      const int count=(indexed?array->EdgeNumber():array->VertexNumber())/3;
      auto vertex=[&](int k) { return array->Vertice(indexed?array->Edge(k):k).Transformed(placed); };
      for(int t=0;t<count;t+=std::max(1,count/4096)) triangles.push_back({vertex(3*t+1),vertex(3*t+2),vertex(3*t+3)});
    } else if(!brep.IsNull() && brep->HasShape()) {
      int faces=0;
      for(TopExp_Explorer f(brep->Shape(),TopAbs_FACE);f.More() && faces<256;f.Next(),++faces) addFace(TopoDS::Face(f.Current()),16);
    }
    const bool closed=!body.IsNull() && body->prs() && body->prs()->closed;  // its back faces are not drawn
    triangles.erase(std::remove_if(triangles.begin(),triangles.end(),[&screen,closed](const std::array<gp_Pnt,3>& t) {
      const gp_Vec normal=gp_Vec(t[0],t[1]).Crossed(gp_Vec(t[0],t[2])),along=screen.ortho?gp_Vec(screen.direction):gp_Vec(screen.eye,t[0]);
      const double facing=normal.Dot(along),size=normal.Magnitude()*along.Magnitude();
      return (closed && facing>0) || std::abs(facing)<=0.02*size;  // seen edge on, it covers no pixel: its points lie on outlines
    }),triangles.end());
    auto pixel=[&screen](const gp_Pnt& p) { double x=0,y=0,d=0;screen.project(p,x,y,d);return QPointF(x,y); };
    std::vector<double> pixels;
    double total=0;
    for(const auto& t:triangles) {
      const QPointF a=pixel(t[0]),b=pixel(t[1]),c=pixel(t[2]);
      total+=pixels.emplace_back(std::abs((b.x()-a.x())*(c.y()-a.y())-(c.x()-a.x())*(b.y()-a.y()))/2);
    }
    const double density=std::min(screen.readable()?0.25:1.0/64,8192/std::max(1.0,total));  // points a pixel, 8,192 in all
    std::vector<gp_Pnt> in;
    for(size_t n=0;n<triangles.size();++n) {
      const auto& t=triangles[n];
      const int k=std::clamp(int(std::ceil((std::sqrt(8*pixels[n]*density+1)-1)/2)),1,11);  // k(k+1)/2 points, up to 66
      for(int i=0;i<k;++i)
        for(int j=0;i+j<k;++j) {
          const double u=(i+1.0/3)/k,v=(j+1.0/3)/k;
          const gp_Pnt p(t[0].XYZ()*(1-u-v)+t[1].XYZ()*u+t[2].XYZ()*v);
          if(m_sectionEnabled && !m_sectionPlane.IsNull() && m_sectionPlane->ProbePoint(Graphic3d_Vec4d(p.X(),p.Y(),p.Z(),1))==Graphic3d_ClipState_Out) continue;
          if(area.contains(pixel(p).toPoint())) in.push_back(p);
        }
    }
    std::vector<gp_Pnt> ordered;
    std::vector<bool> taken(in.size(),false);
    for(size_t step=in.size();step>=1 && ordered.size()<most;step/=2)
      for(size_t i=step/2;i<in.size() && ordered.size()<most;i+=step) if(!taken[i]) {taken[i]=true;ordered.push_back(in[i]);}
    std::reverse(ordered.begin(),ordered.end());  // tested from the back
    return ordered;
  };
  // With no depth read back, after a pick at the middle of each candidate and kPointBudget points tested by pointVisible, a
  // pixel pass goes over the footprints of what is left, every 8th pixel, then every 4th, 2nd and each one (those not picked
  // yet: the grid stays on the box's corner), at most kPixelBudget picks; a candidate no point and no pixel showed is taken
  // for hidden. On as1 the pass over every pixel of a box ran 23-50 s for two parts inside others.
  constexpr int kPointBudget=8192,kPixelBudget=6000;
  auto startLevel=[state,area]() {  // false: no footprint left
    state->scan=QRect();
    for(const auto& c:state->surfaces) if(state->remaining.count(c.owner.get())) state->scan|=c.footprint;
    if(state->scan.isEmpty()) return false;
    const int s=state->stride;
    state->x=area.left()+(state->scan.left()-area.left()+s-1)/s*s-s;  // one before the first: nextPixel steps onto it
    state->y=area.top()+(state->scan.top()-area.top()+s-1)/s*s;
    return true;
  };
  auto nextPixel=[state,area,startLevel]() {  // the next pixel to pick, false when there is none
    for(;;) {
      state->x+=state->stride;
      if(state->x>state->scan.right()) {
        state->x=area.left()+(state->scan.left()-area.left()+state->stride-1)/state->stride*state->stride;
        state->y+=state->stride;
        if(state->y>state->scan.bottom()) {
          if(state->stride==1) return false;
          state->stride/=2;
          if(!startLevel()) return false;
          continue;
        }
      }
      const int s=state->stride;
      if(s<8 && (state->x-area.left())%(2*s)==0 && (state->y-area.top())%(2*s)==0) continue;  // picked at the coarser stride
      for(const auto& c:state->surfaces)
        if(c.footprint.contains(state->x,state->y) && state->remaining.count(c.owner.get())) return true;
    }
  };
  const auto scheme=myGL.Selection.Scheme;
  const auto generation=m_doc->generation;const auto camera=view->Camera()->WorldViewProjState();
  // Publish each owner once: repeated XOR batches would otherwise undo earlier hits.
  auto publish=[this,state,scheme](bool force) {
    if(!force && (state->published==state->visible.size() || (state->started && state->feedback.elapsed()<16))) return;
    AIS_NArray1OfEntityOwner owners;
    const size_t count=state->visible.size()-state->published;
    if(count) {owners.Resize(1,int(count),false);for(size_t i=0;i<count;++i) owners.SetValue(int(i)+1,state->visible[state->published+i]);}
    auto next=scheme;
    if(state->started && (scheme==AIS_SelectionScheme_Replace || scheme==AIS_SelectionScheme_ReplaceExtra)) next=AIS_SelectionScheme_Add;
    m_ctx->Select(owners,next);
    if(!state->started && trace::enabled()) trace::log(QString("box: first feedback %1 ms").arg(state->feedback.elapsed()));
    state->started=true;state->published=state->visible.size();state->feedback.restart();
    OnSelectionChanged(m_ctx,m_view);redrawScene();
  };
  auto finish=[this,state,publish,generation](bool ok) {
    m_boxJob=nullptr;
    if(generation!=m_doc->generation) return;
    if(ok) publish(true);
  };
  if(trace::enabled()) trace::log(QString("box: crossing=%1 through=%2 candidates=%3").arg(m_boxCrossing).arg(m_selectThrough).arg(state->candidates.size()));
  if(m_selectThrough || state->candidates.empty()) {state->visible=state->candidates;finish(true);return;}
  // Visibility is tested against foreground geometry, in short cancellable slices: edges and vertices by their points, faces
  // and bodies by points on their triangles (one of each in turn), against the frame's depth, read once; where it cannot
  // tell, and with none read back, by pointVisible, then by a pick at their middles and the pixels of their footprints.
  m_boxJob=m_jobs->sliced(tr("Selecting visible objects"),[=,this](Job& job) {
    if(generation!=m_doc->generation || camera!=view->Camera()->WorldViewProjState()) {job.cancel();return false;}
    if(!state->depth) {  // one frame, on its own step
      QElapsedTimer clock;clock.start();
      state->depth=captureDepth();
      if(trace::enabled()) trace::log(QString("box: depth %1 in %2 ms").arg(state->depth->readable()?"read":"not read").arg(clock.elapsed()));
      return true;
    }
    const DepthImage& depth=*state->depth;
    auto seen=[&](const gp_Pnt& p,const std::string& body) {
      if(m_sectionEnabled && !m_sectionPlane.IsNull() && m_sectionPlane->ProbePoint(Graphic3d_Vec4d(p.X(),p.Y(),p.Z(),1))==Graphic3d_ClipState_Out) return false;
      if(const int verdict=depth.verdict(p);verdict>=0) return verdict>0;
      ++state->tests;
      return pointVisible(p,body);
    };
    QElapsedTimer step;step.start();
    while(state->probed<state->probes.size()) {
      if(step.elapsed()>=10) {publish(false);return true;}
      const Probe& probe=state->probes[state->probed];
      if(!state->loaded) {state->points=probePoints(probe.owner);state->loaded=true;}
      bool found=false;
      while(!state->points.empty() && !found && step.elapsed()<10) {found=seen(state->points.back(),probe.body);state->points.pop_back();}
      if(!found && !state->points.empty()) continue;
      if(found) state->visible.push_back(probe.owner);
      ++state->probed;state->loaded=false;state->points.clear();
    }
    publish(false);
    if(state->remaining.empty()) return false;
    const bool picks=!depth.readable();
    while(state->sampling>0 && (!picks || state->seeded>=state->seeds.size()) && state->tests<kPointBudget) {  // points, in turn
      if(step.elapsed()>=10) {publish(false);return true;}
      Surface& s=state->surfaces[state->turn++%state->surfaces.size()];
      if(s.done) continue;
      if(!state->remaining.count(s.owner.get())) {s.done=true;--state->sampling;continue;}  // a seed found it
      if(!s.loaded) {s.points=surfacePoints(s.owner,depth,picks?64:4096);s.loaded=true;}
      bool found=false;
      for(int n=0;n<(picks?1:64) && !s.points.empty() && !found;++n) {found=seen(s.points.back(),s.body);s.points.pop_back();}
      if(found) {state->remaining.erase(s.owner.get());state->visible.push_back(s.owner);}
      if(found || s.points.empty()) {s.done=true;--state->sampling;}
    }
    publish(false);
    if(state->remaining.empty() || (!picks && state->sampling==0)) return false;  // the depth showed none of the rest: hidden
    // Up to 8 pixels, but no longer than 10 ms: one pick of a big assembly alone can take tens of ms.
    for(int count=0;count<8 && (count==0 || step.elapsed()<10);++count) {
      const bool seed=state->seeded<state->seeds.size();
      if(!seed && !std::exchange(state->scanning,true) && !(startLevel() && nextPixel())) return false;
      if(!seed && state->picks++>=kPixelBudget) {
        if(trace::enabled()) trace::log(QString("box: %1 left unseen after %2 pixels").arg(state->remaining.size()).arg(kPixelBudget));
        return false;
      }
      const QPoint pixel=seed?state->seeds[state->seeded++]:QPoint(state->x,state->y);
      selector->Pick(pixel.x(),pixel.y(),view);
      if(selector->NbPicked()) {
        m_navSelector->Pick(pixel.x(),pixel.y(),view);
        const bool haveFront=m_navSelector->NbPicked()>0;
        const gp_Pnt front=haveFront?m_navSelector->PickedPoint(1):selector->PickedPoint(1);
        for(int rank=1;rank<=selector->NbPicked();++rank) {
          const auto owner=selector->Picked(rank);
          if(!Handle(OccluderOwner)::DownCast(owner).IsNull()) continue;  // the front test below is the occlusion here
          if(gp_Vec(front,selector->PickedPoint(rank)).Dot(gp_Vec(view->Camera()->Direction()))>pixelSize()*3) continue;
          if(state->remaining.erase(owner.get())) state->visible.push_back(owner);
          break;
        }
      }
      publish(false);
      if(state->remaining.empty()) return false;
      if(seed) {
        if(state->seeded==state->seeds.size()) return true;  // the points on the triangles next
        continue;
      }
      if(!nextPixel()) return false;
    }
    return true;
  },finish);
}
