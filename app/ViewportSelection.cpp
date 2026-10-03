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
#include <algorithm>
#include <set>

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
  struct State {
    std::vector<Handle(SelectMgr_EntityOwner)> candidates,visible;
    std::set<const SelectMgr_EntityOwner*> remaining;
    std::vector<QPoint> seeds;
    std::vector<Probe> probes;
    std::vector<gp_Pnt> points;  // the probe's, untested
    size_t seeded=0,published=0,probed=0;
    bool started=false,loaded=false;
    QElapsedTimer feedback;
    int x=0,y=0,stride=8;
  };
  auto state=std::make_shared<State>();state->x=left;state->y=top;state->feedback.start();
  for(int i=1;i<=selector->NbPicked();++i) {
    auto owner=selector->Picked(i);
    if(!Handle(CircleOwner)::DownCast(owner).IsNull() || !Handle(OccluderOwner)::DownCast(owner).IsNull()) continue;
    const auto body=m_nodeOf.find(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get());
    if(body==m_nodeOf.end()) continue;
    state->candidates.push_back(owner);
    if(const auto sub=Handle(SubShapeOwner)::DownCast(owner);!sub.IsNull() && (sub->kind()==opad::Ref::Kind::Edge || sub->kind()==opad::Ref::Kind::Vertex)) {
      state->probes.push_back({sub,body->second});continue;
    }
    state->remaining.insert(owner.get());
    const auto center=selector->PickedEntity(i)->CenterOfGeometry().Transformed(owner->Selectable()->Transformation());
    int x,y;view->Convert(center.X(),center.Y(),center.Z(),x,y);
    state->seeds.emplace_back(std::clamp(x,left,right),std::clamp(y,top,bottom));
  }
  // An edge or a vertex is in sight when one of its points in the box is (pointVisible, the hover's test): the points
  // 3 px apart along it on screen, middle first. A pixel pick of the box in the Edge mode of a big assembly took 50-100 ms
  // and the scan never got through its tens of thousands of edges.
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
  // Visibility is tested against foreground geometry, in short cancellable slices: edges and vertices by their points,
  // then faces and bodies by the pixels of the box (a coarse pass resolves normal selections quickly; pixel coverage
  // catches narrow exposed portions in the remaining candidates).
  m_boxJob=m_jobs->sliced(tr("Selecting visible objects"),[=,this](Job& job) {
    if(generation!=m_doc->generation || camera!=view->Camera()->WorldViewProjState()) {job.cancel();return false;}
    QElapsedTimer step;step.start();
    while(state->probed<state->probes.size()) {
      if(step.elapsed()>=10) {publish(false);return true;}
      const Probe& probe=state->probes[state->probed];
      if(!state->loaded) {state->points=probePoints(probe.owner);state->loaded=true;}
      bool seen=false;
      while(!state->points.empty() && !seen && step.elapsed()<10) {seen=pointVisible(state->points.back(),probe.body);state->points.pop_back();}
      if(!seen && !state->points.empty()) continue;
      if(seen) state->visible.push_back(probe.owner);
      ++state->probed;state->loaded=false;state->points.clear();
    }
    publish(false);
    if(state->remaining.empty()) return false;
    // Up to 8 pixels, but no longer than 10 ms: one pick of a big assembly alone can take tens of ms.
    for(int count=0;count<8 && (count==0 || step.elapsed()<10);++count) {
      const bool seed=state->seeded<state->seeds.size();
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
      if(seed) continue;
      state->x+=state->stride;
      if(state->x>right) {state->x=left;state->y+=state->stride;}
      if(state->y>bottom) {
        if(state->stride==1) return false;
        state->stride=1;state->x=left;state->y=top;
      }
    }
    return true;
  },finish);
}
