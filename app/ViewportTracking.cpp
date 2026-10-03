#include "Viewport.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
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
  m_trackingLocked = false; m_trackingMarker.clear();
  m_trackingAnchors.clear(); m_trackingCandidates.clear(); m_inferenceChoice = 0;
  m_dwelling = false; m_dwellTimer.stop();
  bool removed = false;
  for (auto* object : {&m_trackingGuide, &m_trackingGuideBehind, &m_anchorMarks}) {
    if (!object->IsNull() && m_initialised) { m_ctx->Remove(*object, false); removed = true; }
    object->Nullify();
  }
  if (m_trackingShown) { m_trackingShown = false; emit hoverChanged(m_hover); }
  if (removed) redrawScene();
}

void Viewport::showTrackingAnchors() {
  if (!m_initialised) return;
  if (!m_anchorMarks.IsNull()) { m_ctx->Remove(m_anchorMarks, false); m_anchorMarks.Nullify(); }
  if (!m_trackingAnchors.empty()) {
    BRep_Builder builder; TopoDS_Compound points; builder.MakeCompound(points);
    for (const auto& anchor : m_trackingAnchors) builder.Add(points, BRepBuilderAPI_MakeVertex(anchor.point).Vertex());
    m_anchorMarks = new AIS_Shape(points);
    const QColor c = m_tokens.sel;
    m_anchorMarks->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_PLUS, Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB), 2.0));
    m_anchorMarks->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(m_anchorMarks, 0, -1, false);  // never pickable
  }
  redrawScene();
}

void Viewport::dwellAnchor() {
  TrackingAnchor hovered{};
  bool have = false;
  gp_Pnt at;
  if (m_ctx->HasDetected() && m_nodeOf.count(m_ctx->DetectedInteractive().get()) && detectedPoint(at) && pointVisible(at)) {
    auto owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
    if (!owner.IsNull()) {
      if (auto mine = Handle(SubShapeOwner)::DownCast(owner); !mine.IsNull()) mine->prepare();  // a mesh owner's shape on demand
      if (owner->HasShape()) {
        const auto& shape = owner->Shape(); const gp_Trsf tr = m_ctx->DetectedInteractive()->Transformation();
        if (shape.ShapeType() == TopAbs_VERTEX) {
          hovered = {BRep_Tool::Pnt(TopoDS::Vertex(shape)).Transformed(tr), {}, false}; have = true;
        } else if (shape.ShapeType() == TopAbs_EDGE) {
          BRepAdaptor_Curve curve(TopoDS::Edge(shape));
          if (curve.GetType() == GeomAbs_Line) {
            const gp_Pnt a = curve.Value(curve.FirstParameter()).Transformed(tr), b = curve.Value(curve.LastParameter()).Transformed(tr);
            const double da = QLineF(widgetPoint({a.X(),a.Y(),a.Z()}),m_trackingCursor).length();
            const double db = QLineF(widgetPoint({b.X(),b.Y(),b.Z()}),m_trackingCursor).length();
            if (std::min(da,db) < 14) {
              hovered = {da < db ? a : b, gp_Vec(a,b), false}; hovered.hasDirection = hovered.direction.SquareMagnitude() > 1e-18; have = true;
            }
          }
        }
        have = have && pointVisible(hovered.point);  // the end of a line seen in its middle can be behind a face
      }
    }
  }
  if (!have) { m_dwelling = false; m_dwellTimer.stop(); return; }
  if (!m_dwelling || m_dwellAnchor.point.Distance(hovered.point) > 1e-7) {
    m_dwellAnchor = hovered; m_dwelling = true; m_dwellDone = false;
    m_dwellClock.start(); m_dwellTimer.start(kTrackingDwellMs);
    return;
  }
  if (hovered.hasDirection) m_dwellAnchor = hovered;
  if (m_dwellDone || m_dwellClock.elapsed() < kTrackingDwellMs) return;
  m_dwellDone = true;
  auto found = std::find_if(m_trackingAnchors.begin(), m_trackingAnchors.end(), [&](const auto& a) { return a.point.Distance(m_dwellAnchor.point) < 1e-7; });
  if (found == m_trackingAnchors.end()) {
    if (m_trackingAnchors.size() == 6) m_trackingAnchors.erase(m_trackingAnchors.begin());
    m_trackingAnchors.push_back(m_dwellAnchor);
  } else if (m_dwellAnchor.hasDirection && !found->hasDirection) *found = m_dwellAnchor;  // a point anchor gains its line
  else m_trackingAnchors.erase(found);
  m_trackingCandidates.clear();
  showTrackingAnchors();
}

void Viewport::pruneTracking() {
  const auto state = m_view->Camera()->WorldViewProjState();
  if (state == m_trackingCamera) return;
  m_trackingCamera = state;
  const size_t before = m_trackingAnchors.size();
  m_trackingAnchors.erase(std::remove_if(m_trackingAnchors.begin(), m_trackingAnchors.end(), [&](const auto& a) { return !pointVisible(a.point); }), m_trackingAnchors.end());
  if (m_trackingAnchors.size() == before) return;
  m_trackingCandidates.clear(); m_trackingDirty = true;
  showTrackingAnchors();
}

void Viewport::updateTracking() {
  if (!m_trackingDirty || !m_snapClick.empty()) return;
  m_trackingDirty = false;
  if (!m_initialised) return;
  // Only a tool that takes points uses a guide point (a click on it picks it): browsing, also in 2D, tracks nothing.
  if ((!m_trackingEnabled && !m_extensionEnabled) || m_sketchInput || m_blocked || !m_pickAccumulate) {
    clearTracking(); return;
  }
  if (QApplication::mouseButtons() != Qt::NoButton) return;
  if (!m_shiftHeld) dwellAnchor();
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
      if(!duplicate && pointVisible(p)) m_trackingCandidates.push_back({a.anchor,p,a.direction,true,b.anchor});
    }
    for (auto line : lines) if(project(line) && distance(line.point)<10 && line.point.Distance(line.anchor)>pixelSize()*3 && pointVisible(line.point))
      m_trackingCandidates.push_back(line);
  }
  const int count = int(m_trackingCandidates.size());
  if (count) m_inferenceChoice = std::clamp(m_inferenceChoice,0,count-1);
  const int chosen = m_inferenceChoice;
  TrackingCandidate candidate;
  bool found = false;
  if (m_trackingLocked) {
    candidate=m_lockedTracking; found=candidate.intersection || project(candidate);
  } else if (!m_centerLocked && !m_trackingCandidates.empty()) {
    // Keep a nearby guide visible even while the circle is the Shift candidate.
    candidate=m_trackingCandidates[std::max(0,chosen)]; found=true;
  }
  const bool hadGuide = !m_trackingGuide.IsNull();
  for (auto* object : {&m_trackingGuide, &m_trackingGuideBehind}) {
    if (!object->IsNull()) m_ctx->Remove(*object,false);
    object->Nullify();
  }
  auto old=m_centers.find(m_trackingMarker);
  if(old!=m_centers.end() && !m_ctx->IsSelected(old->second.ais)) {
    m_centerObjects.erase(old->second.ais.get()); m_ctx->Remove(old->second.ais,false); m_centers.erase(old);
  }
  m_trackingMarker.clear();
  if (!found) {
    refreshCenterStyles();
    if (hadGuide) redrawScene();
    if (m_trackingShown) { m_trackingShown = false; emit hoverChanged(m_hover); }  // the status speaks of guides only while one shows
    return;
  }
  // Depth cue: the stretches of a guide behind a face are drawn faint and dotted, judged at the middle of every 8 px of it.
  BRep_Builder builder; TopoDS_Compound seen, behind; builder.MakeCompound(seen); builder.MakeCompound(behind);
  bool anyBehind = false;
  auto guide=[&](const gp_Pnt& from) {
    if(from.Distance(candidate.point)<=1e-9) return;
    const int pieces=std::clamp(int(QLineF(widgetPoint({from.X(),from.Y(),from.Z()}),widgetPoint({candidate.point.X(),candidate.point.Y(),candidate.point.Z()})).length()/8),1,32);
    const gp_Vec step=gp_Vec(from,candidate.point)/pieces;
    int start=0; bool hidden=!pointVisible(from.Translated(step*0.5));
    for(int i=1;i<=pieces;++i) {
      const bool next=i<pieces && !pointVisible(from.Translated(step*(i+0.5)));
      if(i<pieces && next==hidden) continue;
      builder.Add(hidden?behind:seen,BRepBuilderAPI_MakeEdge(from.Translated(step*start),from.Translated(step*i)).Edge());
      anyBehind=anyBehind||hidden; start=i; hidden=next;
    }
  };
  guide(candidate.anchor); if(candidate.intersection) guide(candidate.secondAnchor);
  const QColor c=m_tokens.sel, faint=QColor::fromRgbF(c.redF()*.45+m_tokens.vp.redF()*.55,c.greenF()*.45+m_tokens.vp.greenF()*.55,c.blueF()*.45+m_tokens.vp.blueF()*.55);
  auto show=[&](Handle(AIS_Shape)& object,const TopoDS_Compound& shape,const QColor& tone,Aspect_TypeOfLine line,double width) {
    object=new AIS_Shape(shape);
    object->Attributes()->SetWireAspect(new Prs3d_LineAspect(Quantity_Color(tone.redF(),tone.greenF(),tone.blueF(),Quantity_TOC_sRGB),line,width));
    object->SetZLayer(Graphic3d_ZLayerId_Topmost); m_ctx->Display(object,0,-1,false);
  };
  show(m_trackingGuide,seen,c,Aspect_TOL_DASH,m_trackingLocked?3.0:1.5);
  if(anyBehind) show(m_trackingGuideBehind,behind,faint,Aspect_TOL_DOT,1.0);
  opad::Ref ref; ref.kind=opad::Ref::Kind::Point; ref.point={candidate.point.X(),candidate.point.Y(),candidate.point.Z()};
  centerMarker(ref,candidate.point); m_trackingMarker=ref.str();
  refreshCenterStyles();
  m_trackingShown = true;
  emit hoverChanged(m_trackingLocked ? tr("Tracking locked - release Shift to unlock")
                    : candidate.intersection ? tr("Extension intersection - tap Shift to cycle, hold to lock")
                    : tr("Extension / alignment - tap Shift to cycle, hold to lock"));
  emit hoverPoint(true,ref.point);
}
