#include "Viewport.hpp"
#include "Units.hpp"
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
  m_shift.reset(); m_trackingMarker.clear(); m_trackingCross = false;
  m_trackingAnchors.clear(); m_trackingCandidates.clear(); m_inferenceChoice = m_crossChoice = m_crossings = 0;
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
  const auto body = m_ctx->HasDetected() ? m_nodeOf.find(m_ctx->DetectedInteractive().get()) : m_nodeOf.end();
  if (body != m_nodeOf.end() && detectedPoint(at) && pointVisible(at, body->second)) {
    auto owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
    if (!owner.IsNull()) {
      if (auto mine = Handle(SubShapeOwner)::DownCast(owner); !mine.IsNull()) mine->prepare();  // a mesh owner's shape on demand
      if (owner->HasShape()) {
        const auto& shape = owner->Shape(); const gp_Trsf tr = m_ctx->DetectedInteractive()->Transformation();
        if (shape.ShapeType() == TopAbs_VERTEX) {
          hovered = {BRep_Tool::Pnt(TopoDS::Vertex(shape)).Transformed(tr), {}, false, body->second}; have = true;
        } else if (shape.ShapeType() == TopAbs_EDGE) {
          BRepAdaptor_Curve curve(TopoDS::Edge(shape));
          if (curve.GetType() == GeomAbs_Line) {
            const gp_Pnt a = curve.Value(curve.FirstParameter()).Transformed(tr), b = curve.Value(curve.LastParameter()).Transformed(tr);
            const double da = QLineF(widgetPoint({a.X(),a.Y(),a.Z()}),m_trackingCursor).length();
            const double db = QLineF(widgetPoint({b.X(),b.Y(),b.Z()}),m_trackingCursor).length();
            if (std::min(da,db) < 14) {
              hovered = {da < db ? a : b, gp_Vec(a,b), false, body->second}; hovered.hasDirection = hovered.direction.SquareMagnitude() > 1e-18; have = true;
            }
          }
        }
        have = have && pointVisible(hovered.point, hovered.body);  // the end of a line seen in its middle can be behind a face
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
  m_trackingAnchors.erase(std::remove_if(m_trackingAnchors.begin(), m_trackingAnchors.end(), [&](const auto& a) { return !pointVisible(a.point, a.body); }), m_trackingAnchors.end());
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
  dwellAnchor();  // also while locked: the second anchor of a cross lock
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
      if (!duplicate) lines.push_back({anchor.point,{},d,false,{},anchor.body});
    }
  }
  auto project = [&](TrackingCandidate& candidate) {
    const gp_Vec w(origin,candidate.anchor), d = candidate.direction;
    const double b = d.Dot(ray), denominator = d.SquareMagnitude()*ray.SquareMagnitude()-b*b;
    if (std::abs(denominator)<1e-12) return false;
    const double t = (b*ray.Dot(w)-ray.SquareMagnitude()*d.Dot(w))/denominator;
    candidate.point = candidate.anchor.Translated(d*t); return true;
  };
  if (!m_shift.locked()) {
    m_trackingCandidates.clear();
    // A composite snap is a real world-space intersection, not just two lines
    // crossing in screen projection. Skew lines in 3D must never create a false pick.
    for (size_t i=0;i<lines.size();++i) for (size_t j=i+1;j<lines.size();++j) {
      const auto& a=lines[i]; const auto& b=lines[j];
      gp_Pnt p;
      if (a.anchor.Distance(b.anchor)<1e-7 || !tracking::meet({a.anchor,a.direction},{b.anchor,b.direction},p) || distance(p)>10) continue;
      bool duplicate=false;
      for(const auto& c:m_trackingCandidates) if(c.point.Distance(p)<1e-7) duplicate=true;
      if(!duplicate && pointVisible(p,a.body)) m_trackingCandidates.push_back({a.anchor,p,a.direction,true,b.anchor,a.body});
    }
    for (auto line : lines) if(project(line) && distance(line.point)<10 && line.point.Distance(line.anchor)>pixelSize()*3 && pointVisible(line.point,line.body))
      m_trackingCandidates.push_back(line);
  }
  const int count = int(m_trackingCandidates.size());
  if (count) m_inferenceChoice = std::clamp(m_inferenceChoice,0,count-1);
  const int chosen = m_inferenceChoice;
  TrackingCandidate candidate;
  bool found = false, crossing = false;
  m_crossings = 0;
  if (m_shift.locked()) {
    candidate=m_lockedTracking; found=candidate.intersection || project(candidate);
    if (found && !candidate.intersection) {
      // Cross lock (UI-32): the locked line snaps to where it lines up with another anchor (its x, y or z, or one of its
      // lines met in space) once the pointer is within 10 px of that anchor's guide to it; a crossing behind a face is not offered.
      std::vector<gp_Pnt> anchors; std::vector<tracking::Line> others;
      for (const auto& a : m_trackingAnchors) anchors.push_back(a.point);
      for (const auto& l : lines) others.push_back({l.anchor,l.direction});
      auto xy=[&](const gp_Pnt& p) { const QPoint w=widgetPoint({p.X(),p.Y(),p.Z()}); return gp_XY(w.x(),w.y()); };
      const gp_XY cursor(m_trackingCursor.x(),m_trackingCursor.y()), locked=xy(candidate.point);
      const gp_XY along=xy(candidate.anchor.Translated(candidate.direction*(pixelSize()*100)))-xy(candidate.anchor);
      std::vector<std::pair<double,tracking::Crossing>> inReach;
      for (const auto& c : tracking::crossings({candidate.anchor,candidate.direction},anchors,others))
        if (const double r=tracking::reach(cursor,locked,along,xy(c.from),xy(c.point)); r<10 && pointVisible(c.point,candidate.body)) inReach.push_back({r,c});
      std::stable_sort(inReach.begin(),inReach.end(),[](const auto& a,const auto& b) { return a.first<b.first; });
      m_crossings=int(inReach.size());
      if (inReach.empty()) m_crossChoice=0;
      else {
        const auto& c=inReach[m_crossChoice%inReach.size()].second;
        candidate.point=c.point; candidate.secondAnchor=c.from; candidate.intersection=crossing=true;
      }
    }
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
    m_trackingCross=false; refreshCenterStyles();
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
    int start=0; bool hidden=!pointVisible(from.Translated(step*0.5),candidate.body);
    for(int i=1;i<=pieces;++i) {
      const bool next=i<pieces && !pointVisible(from.Translated(step*(i+0.5)),candidate.body);
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
  show(m_trackingGuide,seen,c,Aspect_TOL_DASH,m_shift.locked()?3.0:1.5);
  if(anyBehind) show(m_trackingGuideBehind,behind,faint,Aspect_TOL_DOT,1.0);
  opad::Ref ref; ref.kind=opad::Ref::Kind::Point; ref.point={candidate.point.X(),candidate.point.Y(),candidate.point.Z()};
  centerMarker(ref,candidate.point); m_trackingMarker=ref.str();
  m_trackingCross=candidate.intersection;  // two guides cross there: an X
  refreshCenterStyles();
  m_trackingShown = true;
  const bool locked=m_shift.locked();
  const QString what=crossing ? tr("Locked line ∩ alignment from another anchor") : locked ? tr("Tracking locked")
                    : candidate.intersection ? tr("Extension intersection") : tr("Extension / alignment");
  const QString keys=!locked ? (count>1 ? tr("tap Shift to cycle, double-tap to lock, hold to lock while held") : tr("tap Shift to lock, hold to lock while held"))
                    : !m_shift.sticky() ? tr("release Shift to unlock")
                    : m_crossings>1 ? tr("click to pick, tap Shift for the next crossing, Esc to unlock") : tr("click to pick, Esc or tap Shift to unlock");
  QString text=what+QStringLiteral(" - ")+keys;
  if (locked && !m_lockedTracking.intersection)  // the distance along the locked line, nothing to other edges
    text+=QStringLiteral(" · ")+tr("%1 from the anchor").arg(units::format(units::Kind::Length,candidate.anchor.Distance(candidate.point)));
  emit hoverChanged(text);
  emit hoverPoint(true,ref.point);
}
