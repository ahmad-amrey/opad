#include "Viewport.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Tool.hxx>
#include <Prs3d_LineAspect.hxx>
#include <TopoDS.hxx>
#include <QApplication>
#include <QLineF>
#include <cmath>

void Viewport::clearTracking() {
  auto old = m_centers.find(m_trackingMarker);
  if (m_initialised && old != m_centers.end() && !m_ctx->IsSelected(old->second.ais)) {
    m_centerObjects.erase(old->second.ais.get());
    m_ctx->Remove(old->second.ais, false);
    m_centers.erase(old);
  }
  m_haveTrackingAnchor = false; m_trackingLocked = false; m_trackingMarker.clear();
  if (!m_trackingGuide.IsNull() && m_initialised) m_ctx->Remove(m_trackingGuide, false);
  m_trackingGuide.Nullify();
}

void Viewport::updateTracking() {
  if (!m_trackingDirty) return;
  m_trackingDirty = false;
  if (!m_initialised) return;
  if (!m_trackingEnabled || m_sketchInput || m_blocked || (!m_twoDimensional && !m_pickAccumulate)) {
    clearTracking();
    return;
  }
  if (QApplication::mouseButtons() != Qt::NoButton) return;
  const bool shift = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
  if (!shift) m_trackingLocked = false;
  if (!m_trackingLocked && m_ctx->HasDetected() && m_nodeOf.count(m_ctx->DetectedInteractive().get())) {
    auto owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
    if (!owner.IsNull() && owner->HasShape()) {
      const auto& shape=owner->Shape(); const gp_Trsf tr=m_ctx->DetectedInteractive()->Transformation();
      if (shape.ShapeType()==TopAbs_VERTEX) {
        m_trackingAnchor=BRep_Tool::Pnt(TopoDS::Vertex(shape)).Transformed(tr);
        m_haveTrackingAnchor=true; m_trackingHasDirection=false;
      } else if (shape.ShapeType()==TopAbs_EDGE) {
        BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        if(curve.GetType()==GeomAbs_Line) {
          gp_Pnt a=curve.Value(curve.FirstParameter()).Transformed(tr), b=curve.Value(curve.LastParameter()).Transformed(tr);
          const QPoint pa=widgetPoint({a.X(),a.Y(),a.Z()}), pb=widgetPoint({b.X(),b.Y(),b.Z()});
          const double da=QLineF(pa,m_trackingCursor).length(), db=QLineF(pb,m_trackingCursor).length();
          if(std::min(da,db)<14) {
            m_trackingAnchor=da<db?a:b; m_trackingDirection=gp_Vec(a,b);
            m_trackingHasDirection=m_trackingDirection.SquareMagnitude()>1e-18; m_haveTrackingAnchor=true;
          }
        }
      }
    }
  }
  if (!m_haveTrackingAnchor) return;
  const auto at=devicePos(m_trackingCursor);
  double x,y,z,dx,dy,dz; m_view->ConvertWithProj(at.x(),at.y(),x,y,z,dx,dy,dz);
  const gp_Vec ray(dx,dy,dz), w(gp_Pnt(x,y,z),m_trackingAnchor);
  std::vector<gp_Vec> directions={gp_Vec(1,0,0),gp_Vec(0,1,0),gp_Vec(0,0,1)};
  if(m_trackingHasDirection) directions.insert(directions.begin(),m_trackingDirection.Normalized());
  if(m_trackingLocked) directions={m_trackingLockDirection};
  double best=m_trackingLocked?1e9:10; gp_Pnt point; gp_Vec selected; bool found=false;
  for(const auto& d:directions) {
    const double b=d.Dot(ray), denominator=d.SquareMagnitude()*ray.SquareMagnitude()-b*b;
    if(std::abs(denominator)<1e-12) continue;
    const double t=(b*ray.Dot(w)-ray.SquareMagnitude()*d.Dot(w))/denominator;
    const gp_Pnt p=m_trackingAnchor.Translated(d*t);
    const double distance=QLineF(widgetPoint({p.X(),p.Y(),p.Z()}),m_trackingCursor).length();
    if(distance<best && std::abs(t)>pixelSize()*3) { best=distance; point=p; selected=d; found=true; }
  }
  if (found && !m_trackingMarker.empty()) {
    auto old=m_centers.find(m_trackingMarker);
    if(old!=m_centers.end() && old->second.point.Distance(point)<pixelSize()*0.25) return;
  }
  if(!m_trackingGuide.IsNull()) { m_ctx->Remove(m_trackingGuide,false); m_trackingGuide.Nullify(); }
  if(!m_trackingMarker.empty()) {
    auto old=m_centers.find(m_trackingMarker);
    if(old!=m_centers.end() && !m_ctx->IsSelected(old->second.ais)) {
      m_centerObjects.erase(old->second.ais.get()); m_ctx->Remove(old->second.ais,false); m_centers.erase(old);
    }
    m_trackingMarker.clear();
  }
  if(!found) return;
  if(shift) { m_trackingLocked=true; m_trackingLockDirection=selected; }
  m_trackingGuide=new AIS_Shape(BRepBuilderAPI_MakeEdge(m_trackingAnchor,point).Edge());
  const QColor c=m_tokens.sel; const Quantity_Color color(c.redF(),c.greenF(),c.blueF(),Quantity_TOC_sRGB);
  m_trackingGuide->Attributes()->SetWireAspect(new Prs3d_LineAspect(color,Aspect_TOL_DASH,m_trackingLocked?2.5:1.5));
  m_trackingGuide->SetZLayer(Graphic3d_ZLayerId_Topmost); m_ctx->Display(m_trackingGuide,0,-1,false);
  opad::Ref ref; ref.kind=opad::Ref::Kind::Point; ref.point={point.X(),point.Y(),point.Z()};
  centerMarker(ref,point); m_trackingMarker=ref.str();
  emit hoverChanged(m_trackingLocked?tr("Tracking locked - click point, release Shift to unlock"):tr("Extension / alignment - hold Shift to lock"));
  emit hoverPoint(true,ref.point);
}
