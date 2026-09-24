#include "Viewport.hpp"
#include "Jobs.hpp"
#include <AIS_RubberBand.hxx>
#include <SelectMgr_ViewerSelector.hxx>
#include <Graphic3d_Camera.hxx>
#include <QSettings>
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
  struct State {
    std::vector<Handle(SelectMgr_EntityOwner)> candidates,visible;
    std::set<const SelectMgr_EntityOwner*> remaining;
    int x=0,y=0,stride=8;
  };
  auto state=std::make_shared<State>();state->x=left;state->y=top;
  for(int i=1;i<=selector->NbPicked();++i) {
    auto owner=selector->Picked(i);
    if(Handle(CircleOwner)::DownCast(owner).IsNull() && m_nodeOf.count(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get())) {state->candidates.push_back(owner);state->remaining.insert(owner.get());}
  }
  const auto scheme=myGL.Selection.Scheme;
  auto finish=[this,state,scheme](bool ok) {
    m_boxJob=nullptr;if(!ok) return;
    AIS_NArray1OfEntityOwner owners;
    if(!state->visible.empty()) {owners.Resize(1,int(state->visible.size()),false);for(size_t i=0;i<state->visible.size();++i)owners.SetValue(int(i)+1,state->visible[i]);}
    m_ctx->Select(owners,scheme);OnSelectionChanged(m_ctx,m_view);redrawScene();
  };
  if(trace::enabled()) trace::log(QString("box: crossing=%1 through=%2 candidates=%3").arg(m_boxCrossing).arg(m_selectThrough).arg(state->candidates.size()));
  if(m_selectThrough || state->candidates.empty()) {state->visible=state->candidates;finish(true);return;}
  const auto generation=m_doc->generation;const auto camera=view->Camera()->WorldViewProjState();
  // Visibility is tested against foreground geometry, in short cancellable slices.
  // A coarse pass resolves normal selections quickly; pixel coverage catches thin
  // edges and narrow exposed portions in the remaining candidates.
  m_boxJob=m_jobs->sliced(tr("Selecting visible objects"),[=,this](Job& job) {
    if(generation!=m_doc->generation || camera!=view->Camera()->WorldViewProjState()) {job.cancel();return false;}
    if(state->remaining.empty()) return false;
    for(int count=0;count<32;++count) {
      selector->Pick(state->x,state->y,view);
      if(selector->NbPicked()) {
        m_navSelector->Pick(state->x,state->y,view);
        const bool haveFront=m_navSelector->NbPicked()>0;
        const gp_Pnt front=haveFront?m_navSelector->PickedPoint(1):selector->PickedPoint(1);
        for(int rank=1;rank<=selector->NbPicked();++rank) {
          const auto owner=selector->Picked(rank);
          if(gp_Vec(front,selector->PickedPoint(rank)).Dot(gp_Vec(view->Camera()->Direction()))>pixelSize()*3) continue;
          if(state->remaining.erase(owner.get())) state->visible.push_back(owner);
          break;
        }
      }
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
