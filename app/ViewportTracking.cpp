#include "Viewport.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <Prs3d_LineAspect.hxx>
#include <TopoDS.hxx>
#include <QApplication>
#include <QLineF>
#include <QSettings>
#include <cmath>
#include <algorithm>

void Viewport::setTracking(bool on) {
  m_trackingEnabled = on; QSettings().setValue("view/tracking", on);
  clearTracking(); m_trackingDirty = true; redrawScene();
}
void Viewport::setExtensionTracking(bool on) {
  m_extensionEnabled = on; QSettings().setValue("view/extensions", on);
  clearTracking(); m_trackingDirty = true; redrawScene();
}
void Viewport::clearTracking() {
  auto old = m_centers.find(m_trackingMarker);
  if (m_initialised && old != m_centers.end() && !m_ctx->IsSelected(old->second.ais)) {
    m_centerObjects.erase(old->second.ais.get());
    m_ctx->Remove(old->second.ais, false); m_centers.erase(old);
  }
  m_haveTrackingAnchor = false; m_trackingLocked = false; m_trackingMarker.clear();
  m_trackingAnchors.clear(); m_trackingCandidates.clear(); m_inferenceChoice = 0;
  if (!m_trackingGuide.IsNull() && m_initialised) m_ctx->Remove(m_trackingGuide, false);
  m_trackingGuide.Nullify();
}

void Viewport::updateTracking() {
  if (!m_trackingDirty || !m_snapClick.empty()) return;
  m_trackingDirty = false;
  if (!m_initialised) return;
  if ((!m_trackingEnabled && !m_extensionEnabled) || m_sketchInput || m_blocked || (!m_twoDimensional && !m_pickAccumulate)) {
    clearTracking(); return;
  }
  if (QApplication::mouseButtons() != Qt::NoButton) return;
  if (!m_shiftHeld && m_ctx->HasDetected() && m_nodeOf.count(m_ctx->DetectedInteractive().get())) {
    auto owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
    if (!owner.IsNull() && owner->HasShape()) {
      const auto& shape = owner->Shape(); const gp_Trsf tr = m_ctx->DetectedInteractive()->Transformation();
      bool acquired = false;
      if (shape.ShapeType() == TopAbs_VERTEX) {
        m_trackingAnchor = BRep_Tool::Pnt(TopoDS::Vertex(shape)).Transformed(tr);
        m_trackingHasDirection = false; acquired = true;
      } else if (shape.ShapeType() == TopAbs_EDGE) {
        BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        if (curve.GetType() == GeomAbs_Line) {
          const gp_Pnt a = curve.Value(curve.FirstParameter()).Transformed(tr), b = curve.Value(curve.LastParameter()).Transformed(tr);
          const double da = QLineF(widgetPoint({a.X(),a.Y(),a.Z()}),m_trackingCursor).length();
          const double db = QLineF(widgetPoint({b.X(),b.Y(),b.Z()}),m_trackingCursor).length();
          if (std::min(da,db) < 14) {
            m_trackingAnchor = da < db ? a : b; m_trackingDirection = gp_Vec(a,b);
            m_trackingHasDirection = m_trackingDirection.SquareMagnitude() > 1e-18; acquired = true;
          }
        }
      }
      if (acquired) {
        m_haveTrackingAnchor = true;
        auto found = std::find_if(m_trackingAnchors.begin(), m_trackingAnchors.end(), [&](const auto& a) {
          return a.point.Distance(m_trackingAnchor) < 1e-7;
        });
        const TrackingAnchor anchor{m_trackingAnchor,m_trackingDirection,m_trackingHasDirection};
        if (found == m_trackingAnchors.end()) {
          if (m_trackingAnchors.size() == 6) m_trackingAnchors.erase(m_trackingAnchors.begin());
          m_trackingAnchors.push_back(anchor);
        } else if (anchor.hasDirection) *found = anchor;
      }
    }
  }
  // Also accepts an explicitly supplied anchor (the deterministic regression).
  if (m_trackingAnchors.empty() && m_haveTrackingAnchor)
    m_trackingAnchors.push_back({m_trackingAnchor,m_trackingDirection,m_trackingHasDirection});
  const auto at = devicePos(m_trackingCursor);
  double x,y,z,dx,dy,dz; m_view->ConvertWithProj(at.x(),at.y(),x,y,z,dx,dy,dz);
  const gp_Pnt origin(x,y,z); const gp_Vec ray(dx,dy,dz);
  auto distance = [&](const gp_Pnt& p) { return QLineF(widgetPoint({p.X(),p.Y(),p.Z()}),m_trackingCursor).length(); };
  std::vector<TrackingCandidate> lines;
  for (const auto& anchor : m_trackingAnchors) {
    std::vector<gp_Vec> directions;
    if (m_trackingEnabled) directions = {gp_Vec(1,0,0),gp_Vec(0,1,0),gp_Vec(0,0,1)};
    if (m_extensionEnabled && anchor.hasDirection) directions.insert(directions.begin(),anchor.direction.Normalized());
    for (const auto& d : directions) {
      bool duplicate = false;
      for (const auto& line : lines)
        if (line.anchor.Distance(anchor.point)<1e-7 && line.direction.Crossed(d).SquareMagnitude()<1e-12) duplicate = true;
      if (!duplicate) lines.push_back({anchor.point,{},d,false,{}});
    }
  }
  auto project = [&](TrackingCandidate& candidate) {
    const gp_Vec w(origin,candidate.anchor), d = candidate.direction;
    const double b = d.Dot(ray), denominator = d.SquareMagnitude()*ray.SquareMagnitude()-b*b;
    if (std::abs(denominator)<1e-12) return false;
    const double t = (b*ray.Dot(w)-ray.SquareMagnitude()*d.Dot(w))/denominator;
    candidate.point = candidate.anchor.Translated(d*t); return true;
  };
  if (!m_trackingLocked) {
    m_trackingCandidates.clear();
    // A composite snap is a real world-space intersection, not just two lines
    // crossing in screen projection. Skew lines in 3D must never create a false pick.
    for (size_t i=0;i<lines.size();++i) for (size_t j=i+1;j<lines.size();++j) {
      const auto& a=lines[i]; const auto& b=lines[j];
      if (a.anchor.Distance(b.anchor)<1e-7) continue;
      const double dot=a.direction.Dot(b.direction), det=1-dot*dot;
      if (det<1e-10) continue;
      const gp_Vec delta(a.anchor,b.anchor);
      const double t=(delta.Dot(a.direction)-dot*delta.Dot(b.direction))/det;
      const double u=(dot*delta.Dot(a.direction)-delta.Dot(b.direction))/det;
      const gp_Pnt p=a.anchor.Translated(a.direction*t), q=b.anchor.Translated(b.direction*u);
      if(p.Distance(q)>1e-7 || distance(p)>10) continue;
      bool duplicate=false;
      for(const auto& c:m_trackingCandidates) if(c.point.Distance(p)<1e-7) duplicate=true;
      if(!duplicate) m_trackingCandidates.push_back({a.anchor,p,a.direction,true,b.anchor});
    }
    for (auto line : lines) if(project(line) && distance(line.point)<10 && line.point.Distance(line.anchor)>pixelSize()*3)
      m_trackingCandidates.push_back(line);
  }
  const bool centerAvailable = !m_activeCenter.empty();
  const int count = int(m_trackingCandidates.size()) + int(centerAvailable);
  if (count) m_inferenceChoice = std::clamp(m_inferenceChoice,0,count-1);
  const int chosen = m_inferenceChoice-int(centerAvailable);
  TrackingCandidate candidate;
  bool found = false;
  if (m_trackingLocked) {
    candidate=m_lockedTracking; found=candidate.intersection || project(candidate);
  } else if (!m_centerLocked && !m_trackingCandidates.empty()) {
    // Keep a nearby guide visible even while the circle is the Shift candidate.
    candidate=m_trackingCandidates[std::max(0,chosen)]; found=true;
  }
  if (!m_trackingGuide.IsNull()) { m_ctx->Remove(m_trackingGuide,false); m_trackingGuide.Nullify(); }
  auto old=m_centers.find(m_trackingMarker);
  if(old!=m_centers.end() && !m_ctx->IsSelected(old->second.ais)) {
    m_centerObjects.erase(old->second.ais.get()); m_ctx->Remove(old->second.ais,false); m_centers.erase(old);
  }
  m_trackingMarker.clear();
  if (!found) { refreshCenterStyles(); return; }
  BRep_Builder builder; TopoDS_Compound guides; builder.MakeCompound(guides);
  auto guide=[&](const gp_Pnt& from) {
    if(from.Distance(candidate.point)>1e-9) builder.Add(guides,BRepBuilderAPI_MakeEdge(from,candidate.point).Edge());
  };
  guide(candidate.anchor); if(candidate.intersection) guide(candidate.secondAnchor);
  m_trackingGuide=new AIS_Shape(guides);
  const QColor c=m_tokens.sel; const Quantity_Color color(c.redF(),c.greenF(),c.blueF(),Quantity_TOC_sRGB);
  m_trackingGuide->Attributes()->SetWireAspect(new Prs3d_LineAspect(color,Aspect_TOL_DASH,m_trackingLocked?3.0:1.5));
  m_trackingGuide->SetZLayer(Graphic3d_ZLayerId_Topmost); m_ctx->Display(m_trackingGuide,0,-1,false);
  opad::Ref ref; ref.kind=opad::Ref::Kind::Point; ref.point={candidate.point.X(),candidate.point.Y(),candidate.point.Z()};
  centerMarker(ref,candidate.point); m_trackingMarker=ref.str();
  refreshCenterStyles();
  emit hoverChanged(m_trackingLocked ? tr("Tracking locked - release Shift to unlock")
                    : candidate.intersection ? tr("Extension intersection - tap Shift to cycle, hold to lock")
                    : tr("Extension / alignment - tap Shift to cycle, hold to lock"));
  emit hoverPoint(true,ref.point);
}
