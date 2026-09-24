#include "CurveSamples.hpp"
#include "opad/design/sketch_edit.hpp"
#include "SketchEditor.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_AspectText3d.hxx>
#include <Graphic3d_Text.hxx>
#include <Poly_Triangulation.hxx>
#include <Prs3d_Presentation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <QKeyEvent>
#include <QSettings>
#include <cmath>

#include "I18n.hpp"
#include "Jobs.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/sketch_geom.hpp"

using namespace opad::design;

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
}  // namespace

// ---------------------------------------------------------------- the overlay object
// Everything the editor shows, as plain arrays it fills before each redisplay: never pickable (the editor
// hit-tests in sketch coordinates itself), drawn in the Topmost layer.
class SketchPrs : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(SketchPrs, AIS_InteractiveObject)
 public:
  struct Seg { opad::Vec3 a, b; QColor c; };
  struct Pt { opad::Vec3 p; QColor c; };
  struct Txt { opad::Vec3 p; QString s; QColor c; };
  std::vector<Seg> solid, dashed, thin;
  std::vector<Pt> points, bigPoints;
  std::vector<Txt> texts;
  std::vector<opad::Vec3> fill;
  QColor fillColor, textBack;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    if (!fill.empty()) {
      Handle(Graphic3d_ArrayOfTriangles) tri = new Graphic3d_ArrayOfTriangles(static_cast<int>(fill.size()));
      for (const auto& p : fill) tri->AddVertex(gp_Pnt(p[0], p[1], p[2]));
      Handle(Graphic3d_AspectFillArea3d) a = new Graphic3d_AspectFillArea3d();
      a->SetInteriorStyle(Aspect_IS_SOLID);
      a->SetInteriorColor(Quantity_ColorRGBA(occ(fillColor), 0.18f));
      a->SetAlphaMode(Graphic3d_AlphaMode_Blend);
      a->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
      a->SetSuppressBackFaces(false);
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(a);
      g->AddPrimitiveArray(tri);
    }
    auto lines = [&](const std::vector<Seg>& segs, Aspect_TypeOfLine type, double width) {
      if (segs.empty()) return;
      Handle(Graphic3d_ArrayOfSegments) arr = new Graphic3d_ArrayOfSegments(static_cast<int>(segs.size()) * 2, 0, Standard_True);
      for (const auto& s : segs) {
        arr->AddVertex(gp_Pnt(s.a[0], s.a[1], s.a[2]), occ(s.c));
        arr->AddVertex(gp_Pnt(s.b[0], s.b[1], s.b[2]), occ(s.c));
      }
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(occ(segs.front().c), type, width));
      g->AddPrimitiveArray(arr);
    };
    lines(thin, Aspect_TOL_SOLID, 1.0);
    lines(dashed, Aspect_TOL_DASH, 1.5);
    lines(solid, Aspect_TOL_SOLID, 2.0);
    auto markers = [&](const std::vector<Pt>& pts, double scale) {
      if (pts.empty()) return;
      Handle(Graphic3d_ArrayOfPoints) arr = new Graphic3d_ArrayOfPoints(static_cast<int>(pts.size()), Standard_True);
      for (const auto& p : pts) arr->AddVertex(gp_Pnt(p.p[0], p.p[1], p.p[2]), occ(p.c));
      Handle(Graphic3d_Group) g = prs->NewGroup();
      g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_O_POINT, occ(pts.front().c), scale));
      g->AddPrimitiveArray(arr);
    };
    markers(points, 1.5);
    markers(bigPoints, 2.5);
    // One group per colour: a text aspect has a single colour.
    std::map<QRgb, Handle(Graphic3d_Group)> groups;
    for (const auto& t : texts) {
      Handle(Graphic3d_Group)& g = groups[t.c.rgb()];
      if (g.IsNull()) {
        g = prs->NewGroup();
        Handle(Graphic3d_AspectText3d) a = new Graphic3d_AspectText3d(occ(t.c), "", 1.0, 0.0);
        a->SetDisplayType(Aspect_TODT_SUBTITLE);
        a->SetColorSubTitle(Quantity_ColorRGBA(occ(textBack)));
        g->SetGroupPrimitivesAspect(a);
      }
      Handle(Graphic3d_Text) text = new Graphic3d_Text(13.0f);
      text->SetText(TCollection_ExtendedString(t.s.toUtf8().constData(), Standard_True));
      text->SetPosition(gp_Pnt(t.p[0], t.p[1], t.p[2]));
      text->SetHorizontalAlignment(Graphic3d_HTA_CENTER);
      text->SetVerticalAlignment(Graphic3d_VTA_CENTER);
      g->AddText(text);
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
};

// ---------------------------------------------------------------- life cycle
SketchEditor::SketchEditor(AppDocument* doc, Viewport* viewport, JobRunner* jobs, QObject* parent) : QObject(parent), m_doc(doc), m_viewport(viewport), m_jobs(jobs) {
  connect(m_viewport,&Viewport::notesMoved,this,[this] {
    if(!m_active) return;
    const double pixels=m_viewport->pixelSize();
    if(pixels<m_samplePixelSize*.75 || pixels>m_samplePixelSize*1.5) rebuild();
  });
  m_fillTimer.setSingleShot(true);
  m_fillTimer.setInterval(150);
  connect(&m_fillTimer, &QTimer::timeout, this, [this] {
    if (!m_active) return;
    if (m_fillJob) m_fillJob->cancel();
    // Finding the closed regions is kernel work: on a worker, from a copy of the sketch.
    auto sk = std::make_shared<Sketch>(m_sk);
    auto out = std::make_shared<std::vector<opad::Vec3>>();
    const opad::Frame frame = m_frame;
    m_fillJob = m_jobs->async(tr("Finding profiles"), [sk, out, frame](Progress) {
      for (const auto& region : sketch_regions(*sk, frame)) {
        BRepMesh_IncrementalMesh(region.face, 0.05, Standard_False, 0.2, Standard_False);
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(region.face, loc);
        if (tri.IsNull()) continue;
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
          int a, b, c;
          tri->Triangle(i).Get(a, b, c);
          for (int n : {a, b, c}) {
            const gp_Pnt p = tri->Node(n).Transformed(loc.Transformation());
            out->push_back({p.X(), p.Y(), p.Z()});
          }
        }
      }
    }, [this, out](bool ok, const QString&) {
      m_fillJob = nullptr;
      if (!ok || !m_active) return;
      m_fill = std::move(*out);
      rebuild();
    });
  });
}

SketchEditor::~SketchEditor() = default;

void SketchEditor::begin(const std::string& sketchId, const QString& name, const opad::json& plane, const opad::Frame& frame, const opad::json& geometry) {
  m_trackingPoint = 0; m_inferenceLocked = false;
  m_id = sketchId;
  m_name = name;
  m_plane = plane;
  m_frame = frame;
  m_sk = Sketch::from_json(geometry);
  m_initialGeometry = m_sk.to_json();
  if (m_sk.points.empty()) m_sk.add_point(0, 0, true);  // the origin: something to constrain the first curve to
  Sketch analysis=m_sk; m_solved = solve(analysis,solveOptions()); // opening never drifts stored coordinates
  m_undo.clear();
  m_redo.clear();
  m_sel.clear();
  m_clicks.clear();
  m_chain.clear();
  m_picked.clear();
  m_fill.clear();
  m_placingDim = false;
  m_modified = false;
  m_active = true;
  m_tool = "select";
  m_prs = new SketchPrs();
  m_cameraBefore = m_viewport->cameraJson();
  m_viewport->beginSketchInput(this, frame, sketchId);
  m_viewport->showOverlay(m_prs);
  m_viewport->lookAt(frame,true,false);
  rebuild();
  scheduleFill();
  emit toolChanged(m_tool);
  toolPrompt();
  emit changed();
}

void SketchEditor::end() {
  if (!m_active) return;
  if (m_tool == "project") m_viewport->setEdgeHover(false);
  m_active = false;
  m_fillTimer.stop();
  if (m_fillJob) m_fillJob->cancel();
  if (m_dimEdit) m_dimEdit->hide();
  m_viewport->removeOverlay(m_prs);
  m_prs.Nullify();
  m_viewport->endSketchInput();
  m_viewport->setCameraJson(m_cameraBefore);
  emit changed();
}

bool SketchEditor::empty() const { return m_sk.entities.empty(); }

bool SketchEditor::isFixedPoint(int id) const {
  const SkPoint* p = m_sk.point(id);
  return p && p->fixed;
}

double SketchEditor::tol() const { return 8.0 * m_viewport->pixelSize(); }

void SketchEditor::scheduleFill() { m_fillTimer.start(); }

// ---------------------------------------------------------------- undo
void SketchEditor::begin_change() {
  m_dangling.clear();
  m_sk.id_watermark=m_sk.next_id()-1;
  m_before = m_sk;
  m_inChange = true;
}

void SketchEditor::cancel_change() {
  if (!m_inChange) return;
  m_sk = m_before;
  m_inChange = false;
}

bool SketchEditor::end_change(const QString& what) {
  m_inChange = false;
  SolveResult r;
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    evaluate_dimensions(m_sk,ParamTable(defs));
    r = solve(m_sk,solveOptions());
  } catch (const std::exception& e) {
    m_sk = m_before;
    emit status(i18n::t(QString::fromUtf8(e.what())));
    rebuild();
    return false;
  }
  if (!r.converged) {
    m_sk = m_before;  // never leave the sketch over-constrained: the change is refused
    QStringList ids;for(int id:r.failed)ids<<QString::number(id);
    emit status(tr("%1 conflicts with constraints %2. Edit or remove them, or switch a dimension to reference.").arg(what,ids.join(", ")));
    rebuild();
    return false;
  }
  m_solved = r;
  m_undo.push_back(m_before);
  if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
  m_redo.clear();
  m_modified = true;
  rebuild();
  scheduleFill();
  emit changed();
  return true;
}

void SketchEditor::undo() {
  if (m_undo.empty()) return;
  m_redo.push_back(m_sk);
  m_sk = m_undo.back();
  m_undo.pop_back();
  m_clicks.clear();
  m_chain.clear();
  m_picked.clear();
  m_sel.clear();
  m_solved = solve(m_sk);
  m_modified = true;
  rebuild();
  scheduleFill();
  emit changed();
}

void SketchEditor::redo() {
  if (m_redo.empty()) return;
  m_undo.push_back(m_sk);
  m_sk = m_redo.back();
  m_redo.pop_back();
  m_sel.clear();
  m_solved = solve(m_sk);
  m_modified = true;
  rebuild();
  scheduleFill();
  emit changed();
}

// ---------------------------------------------------------------- geometry queries
std::vector<std::pair<double, double>> SketchEditor::sampled(const SkEntity& e) const {
  std::vector<std::pair<double, double>> out;
  if(e.type==SkEntity::Type::Point) return out;
  const auto edge=entity_edge(m_sk,e,opad::Frame());
  if(!edge.IsNull()) for(const auto& point:curveSamples(edge,std::max(1e-7,m_viewport->pixelSize()*0.25))) out.push_back({point.X(),point.Y()});
  return out;
}

double SketchEditor::distanceTo(const SkEntity& e, double u, double v) const {
  if (e.type == SkEntity::Type::Point) {
    const SkPoint* p = m_sk.point(e.p.empty() ? 0 : e.p[0]);
    return p ? std::hypot(p->x - u, p->y - v) : 1e300;
  }
  if (e.type == SkEntity::Type::Circle) {
    const SkPoint* c = m_sk.point(e.p.empty() ? 0 : e.p[0]);
    return c ? std::fabs(std::hypot(c->x - u, c->y - v) - e.r) : 1e300;
  }
  const auto pts = sampled(e);
  double best = 1e300;
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const double ax = pts[i].first, ay = pts[i].second, bx = pts[i + 1].first, by = pts[i + 1].second;
    const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    const double t = len2 < 1e-18 ? 0 : std::clamp(((u - ax) * dx + (v - ay) * dy) / len2, 0.0, 1.0);
    best = std::min(best, std::hypot(ax + t * dx - u, ay + t * dy - v));
  }
  return best;
}

SketchEditor::Hit SketchEditor::hitTest(double u, double v) const {
  const double t = tol();
  Hit hit;
  double best = t;
  for (const auto& p : m_sk.points) {
    if (!selectable(p.id)) continue;
    const double d = std::hypot(p.x - u, p.y - v);
    if (d < best) { best = d; hit = {Hit::Point, p.id}; }
  }
  if (hit.kind != Hit::None) return hit;
  for (const auto& c : m_sk.constraints) {
    if (!c.is_dimension() || !selectable(c.id)) continue;
    double lu, lv;
    labelPosition(c, lu, lv);
    if (std::fabs(lu - u) < 3.5 * t && std::fabs(lv - v) < 1.4 * t) return {Hit::Dimension, c.id};
  }
  best = t;
  for(const auto& [id,x,y]:m_glyphHits)if(selectable(id) && std::fabs(x-u)<t && std::fabs(y-v)<t)return {Hit::Dimension,id};
  for (const auto& e : m_sk.entities) {
    if (!selectable(e.id)) continue;
    const double d = distanceTo(e, u, v);
    if (d < best) { best = d; hit = {Hit::Entity, e.id}; }
  }
  return hit;
}

SketchEditor::Snap SketchEditor::snap(double u, double v, bool infer) const {
  Snap s;
  s.u = u;
  s.v = v;
  if(!infer)return s; // Alt suppresses both inference and automatic coincidence
  const double t = tol();
  auto enabled=[](const char* name){return QSettings().value(QString("sketch/snap/")+name,true).toBool();};
  const bool automatic=enabled("inference");
  const bool extensions=QSettings().value("view/extensions",true).toBool(), tracking=QSettings().value("view/tracking",true).toBool();
  if (m_inferenceLocked && infer) {
    const double along=(u-m_lockX)*m_lockDx+(v-m_lockY)*m_lockDy;
    s.u=m_lockX+along*m_lockDx; s.v=m_lockY+along*m_lockDy; s.tracking=true; return s;
  }

  double best = t;
  std::set<int> centers;
  for(const auto& e:m_sk.entities)if(e.type==SkEntity::Type::Circle || e.type==SkEntity::Type::Arc || e.type==SkEntity::Type::Ellipse)centers.insert(e.p[0]);
  for (const auto& p : m_sk.points) {
    if(!(centers.count(p.id)?enabled("center"):enabled("endpoint")))continue;
    if (!m_chain.empty() && p.id == m_chain.back()) continue;  // not onto the point the segment starts at
    const double d = std::hypot(p.x - u, p.y - v);
    if (d < best) { best = d; s.point = p.id; s.u = p.x; s.v = p.y; }
  }
  if (s.point) return s;
  auto candidate=[&](double x,double y){double d=std::hypot(x-u,y-v);if(d<best){best=d;s.u=x;s.v=y;s.tracking=true;}};
  std::vector<const SkEntity*> nearby;
  for(const auto& e:m_sk.entities) {
    if(e.type==SkEntity::Type::Line) {
      const auto *a=m_sk.point(e.p[0]),*b=m_sk.point(e.p[1]);
      if(enabled("midpoint"))candidate((a->x+b->x)/2,(a->y+b->y)/2);
      if(enabled("intersection") && distanceTo(e,u,v)<t)nearby.push_back(&e);
    } else if(e.type==SkEntity::Type::Circle || e.type==SkEntity::Type::Arc) {
      const auto* c=m_sk.point(e.p[0]);
      const double r=e.type==SkEntity::Type::Circle?e.r:std::hypot(m_sk.point(e.p[1])->x-c->x,m_sk.point(e.p[1])->y-c->y);
      if(enabled("quadrant"))for(int q=0;q<4;++q) {
        const double x=c->x+r*std::cos(q*M_PI/2),y=c->y+r*std::sin(q*M_PI/2);
        if(distanceTo(e,x,y)<t)candidate(x,y);
      }
      if(enabled("intersection") && distanceTo(e,u,v)<t)nearby.push_back(&e);
    }
  }
  for(size_t i=0;i<nearby.size();++i)for(size_t j=i+1;j<nearby.size();++j) {
    // Local display polylines use the same quarter-pixel tolerance as picking; intersections snap to it.
    const auto a=sampled(*nearby[i]),b=sampled(*nearby[j]);
    for(size_t k=1;k<a.size();++k)for(size_t l=1;l<b.size();++l) {
      const double ax=a[k-1].first,ay=a[k-1].second,dx=a[k].first-ax,dy=a[k].second-ay;
      const double bx=b[l-1].first,by=b[l-1].second,ex=b[l].first-bx,ey=b[l].second-by,den=dx*ey-dy*ex;
      if(std::fabs(den)<1e-15)continue;
      const double ta=((bx-ax)*ey-(by-ay)*ex)/den,tb=((bx-ax)*dy-(by-ay)*dx)/den;
      if(ta>=0 && ta<=1 && tb>=0 && tb<=1)candidate(ax+ta*dx,ay+ta*dy);
    }
  }
  if(s.tracking)return s;
  best = t;
  for (const auto& e : m_sk.entities) {
    if(!enabled("nearest"))break;
    if (e.type != SkEntity::Type::Line && e.type != SkEntity::Type::Circle && e.type != SkEntity::Type::Arc) continue;
    double d = distanceTo(e, u, v);
    if (extensions && e.type==SkEntity::Type::Line && e.p.size()==2 && (e.p[0]==m_trackingPoint || e.p[1]==m_trackingPoint)) {
      const auto *a=m_sk.point(e.p[0]), *b=m_sk.point(e.p[1]);
      if(a && b) { const double dx=b->x-a->x,dy=b->y-a->y,len=std::hypot(dx,dy); if(len>1e-9) d=std::abs((u-a->x)*dy-(v-a->y)*dx)/len; }
    }
    if (d >= best) continue;
    best = d;
    s.entity = automatic ? e.id : 0;
    s.tracking = !automatic;
    // Foot of the perpendicular, so the new point starts on the curve.
    if (e.type == SkEntity::Type::Line) {
      const SkPoint *a = m_sk.point(e.p[0]), *b = m_sk.point(e.p[1]);
      const double dx = b->x - a->x, dy = b->y - a->y, len2 = dx * dx + dy * dy;
      const double k = len2 < 1e-18 ? 0 : ((u - a->x) * dx + (v - a->y) * dy) / len2;
      s.u = a->x + k * dx;
      s.v = a->y + k * dy;
    } else {
      const SkPoint* c = m_sk.point(e.p[0]);
      const double r = e.type == SkEntity::Type::Circle ? e.r : std::hypot(m_sk.point(e.p[1])->x - c->x, m_sk.point(e.p[1])->y - c->y);
      const double d0 = std::hypot(u - c->x, v - c->y);
      if (d0 > 1e-12) { s.u = c->x + (u - c->x) * r / d0; s.v = c->y + (v - c->y) * r / d0; }
    }
  }
  if (s.entity || s.tracking) return s;
  if (const auto* reference=m_sk.point(m_trackingPoint); tracking && reference) {
    if(std::abs(u-reference->x)<t) { s.u=reference->x; s.tracking=true; }
    if(std::abs(v-reference->y)<t) { s.v=reference->y; s.tracking=true; }
    if(s.tracking) return s;
  }
  // Horizontal / vertical inference against the previous click of a line-like tool.
  const bool lineLike = m_tool == "line" && !m_chain.empty();
  if (lineLike && tracking && automatic) {
    const SkPoint* from = m_sk.point(m_chain.back());
    if (from) {
      const double dx = u - from->x, dy = v - from->y;
      if (std::fabs(dy) < t && std::fabs(dx) > 3 * t) { s.v = from->y; s.horizontal = true; }
      else if (std::fabs(dx) < t && std::fabs(dy) > 3 * t) { s.u = from->x; s.vertical = true; }
    }
  }
  if(lineLike && enabled("angle") && !s.horizontal && !s.vertical) {
    const auto* p=m_sk.point(m_chain.back());const double dx=u-p->x,dy=v-p->y,len=std::hypot(dx,dy),step=QSettings().value("sketch/angleStep",15).toDouble()*M_PI/180;
    const double angle=std::round(std::atan2(dy,dx)/step)*step;
    if(len>t && std::fabs(std::sin(angle-std::atan2(dy,dx))*len)<t){s.u=p->x+len*std::cos(angle);s.v=p->y+len*std::sin(angle);s.tracking=true;}
  }
  if(enabled("grid") && m_viewport->gridSnap() && !s.tracking && !s.horizontal && !s.vertical) {
    const double step=m_viewport->gridStep(); s.u=std::round(s.u/step)*step; s.v=std::round(s.v/step)*step;
  }
  return s;
}

int SketchEditor::pointFor(const Snap& s) {
  if (s.point) return s.point;
  const int id = m_sk.add_point(s.u, s.v);
  if (s.entity) m_sk.add_constraint(SkConstraint::Type::Coincident, {id, s.entity});
  return id;
}

// ---------------------------------------------------------------- input
void SketchEditor::sketchPress(double u, double v, Qt::KeyboardModifiers mods) {
  if (!m_active) return;
  if ((m_tool=="select" || (m_tool=="spline" && m_chain.empty())) && mods.testFlag(Qt::AltModifier)) return insertSplineNode(u,v);
  if (m_dimEdit && m_dimEdit->isVisible()) commitDimensionEdit();
  if (m_tool == "select") {
    const Hit h = hitTest(u, v);
    const bool add = mods & (Qt::ShiftModifier | Qt::ControlModifier);
    if (h.kind == Hit::None) {
      if (!add) m_sel.clear();
    } else {
      auto it = std::find(m_sel.begin(), m_sel.end(), h.id);
      if (it != m_sel.end() && add) m_sel.erase(it);
      else if (it == m_sel.end()) {
        if (!add) m_sel.clear();
        m_sel.push_back(h.id);
      }
    }
    // A press on geometry may become a drag.
    m_dragging = h.kind != Hit::None;
    m_dragMoved = false;
    m_dragHit = h;
    m_dragU = u;
    m_dragV = v;
    m_dragStart.clear();
    m_boxSelecting=h.kind==Hit::None;
    m_boxU=u;m_boxV=v;
    if (h.kind == Hit::Point) {
      if (const SkPoint* p = m_sk.point(h.id)) m_dragStart.push_back({p->id, {p->x, p->y}});
    } else if (h.kind == Hit::Entity) {
      if (const SkEntity* e = m_sk.entity(h.id))
        for (int pid : e->p)
          if (const SkPoint* p = m_sk.point(pid)) m_dragStart.push_back({p->id, {p->x, p->y}});
    }
    rebuild();
    emit changed();
    return;
  }
  click(snap(u, v, !mods.testFlag(Qt::AltModifier)), mods);
}

void SketchEditor::sketchMove(double u, double v, Qt::KeyboardModifiers mods, bool dragging) {
  if (!m_active) return;
  if(m_boxSelecting && dragging) {m_boxU=u;m_boxV=v;rebuild();return;}
  if (m_tool == "select" && dragging && m_dragging) {
    if (!m_dragMoved && std::hypot(u - m_dragU, v - m_dragV) < 0.5 * tol()) return;
    if (!m_dragMoved) {
      m_dragMoved = true;
      begin_change();
    }
    const double du = u - m_dragU, dv = v - m_dragV;
    if (m_dragHit.kind == Hit::Dimension) {
      if (SkConstraint* c = m_sk.constraint(m_dragHit.id)) {
        double lu, lv;
        labelPosition(m_before.constraints[static_cast<size_t>(c - &m_sk.constraints[0])], lu, lv);
        c->pos[0] = lu + du;
        c->pos[1] = lv + dv;
      }
      rebuild();
      return;
    }
    Sketch attempt = m_sk; // carry forward the last constrained solution during a drag
    SolveOptions opt=solveOptions();
    const SkEntity* e = m_dragHit.kind == Hit::Entity ? attempt.entity(m_dragHit.id) : nullptr;
    if (e && e->type == SkEntity::Type::Circle && !e->fixed) {
      // Dragging the rim changes the radius (unless a dimension holds it: the solver pulls it back).
      if (const SkPoint* c = attempt.point(e->p[0])) attempt.entity(m_dragHit.id)->r = std::max(1e-3, std::hypot(u - c->x, v - c->y));
    } else {
      for (const auto& [pid, at] : m_dragStart) opt.drags.push_back({pid, at.first + du, at.second + dv});
    }
    try {
      const SolveResult r = solve(attempt, opt);
      if (r.converged) {
        m_sk = attempt;
        m_solved = r;
      }
    } catch (const std::exception&) {
    }
    rebuild();
    return;
  }
  const Hit h = hitTest(u, v);
  const bool shift=mods.testFlag(Qt::ShiftModifier);
  if(!shift) m_inferenceLocked=false;
  if(h.kind==Hit::Point && !shift) m_trackingPoint=h.id;
  if(shift && !m_inferenceLocked && m_haveCursor && m_tool!="select") {
    const auto* base=m_sk.point(!m_chain.empty()?m_chain.back():m_trackingPoint);
    if(base) {
      const double dx=m_cursor.u-base->x,dy=m_cursor.v-base->y,len=std::hypot(dx,dy);
      if(len>1e-9) { m_lockX=base->x;m_lockY=base->y;m_lockDx=dx/len;m_lockDy=dy/len;m_inferenceLocked=true; }
    }
  }
  const Snap s = snap(u, v, !mods.testFlag(Qt::AltModifier));
  const bool redraw = h.kind != m_hover.kind || h.id != m_hover.id || m_tool != "select" || m_placingDim;
  m_hover = h;
  m_cursor = s;
  m_haveCursor = true;
  if (redraw) rebuild();
}

void SketchEditor::sketchRelease(double u, double v, Qt::KeyboardModifiers) {
  if(m_active && m_boxSelecting) {
    m_boxSelecting=false;
    if(std::hypot(u-m_dragU,v-m_dragV)>tol()) {
      const double x0=std::min(u,m_dragU),x1=std::max(u,m_dragU),y0=std::min(v,m_dragV),y1=std::max(v,m_dragV);
      const bool crossing=u<m_dragU;
      auto inside=[&](double x,double y){return x>=x0 && x<=x1 && y>=y0 && y<=y1;};
      auto crosses=[&](double ax,double ay,double bx,double by) {
        double low=0,high=1;
        auto clip=[&](double p,double q){if(std::fabs(p)<1e-15)return q>=0;double r=q/p;if(p<0)low=std::max(low,r);else high=std::min(high,r);return low<=high;};
        return clip(ax-bx,ax-x0)&&clip(bx-ax,x1-ax)&&clip(ay-by,ay-y0)&&clip(by-ay,y1-ay);
      };
      for(const auto& e:m_sk.entities)if(selectable(e.id)) {
        const auto pts=sampled(e);bool all=!pts.empty(),any=false;
        for(size_t i=0;i<pts.size();++i) {bool in=inside(pts[i].first,pts[i].second);all&=in;any|=in;if(i)any|=crosses(pts[i-1].first,pts[i-1].second,pts[i].first,pts[i].second);}
        if(crossing?any:all)if(std::find(m_sel.begin(),m_sel.end(),e.id)==m_sel.end())m_sel.push_back(e.id);
      }
      for(const auto& p:m_sk.points)if(selectable(p.id)&&inside(p.x,p.y))if(std::find(m_sel.begin(),m_sel.end(),p.id)==m_sel.end())m_sel.push_back(p.id);
      for(const auto& c:m_sk.constraints)if(selectable(c.id)) {double x,y;labelPosition(c,x,y);if(inside(x,y))m_sel.push_back(c.id);}
    }
    rebuild();emit changed();return;
  }
  if (!m_active || !m_dragging) return;
  m_dragging = false;
  if (!m_dragMoved) return;
  m_dragMoved = false;
  // The drag already solved every step; record it as one undo step.
  m_inChange = false;
  m_undo.push_back(m_before);
  m_redo.clear();
  m_modified = true;
  scheduleFill();
  rebuild();
  emit changed();
}

void SketchEditor::sketchDoubleClick(double u, double v) {
  if (!m_active) return;
  if (m_tool == "line" || (m_tool == "spline" && !m_chain.empty())) return finishChain();
  const Hit h = hitTest(u, v);
  if((m_tool=="select" || m_tool=="spline") && h.kind==Hit::Point) {m_sel={h.id};editSplineNode();return;}
  if (h.kind == Hit::Dimension) editDimension(h.id, false);
}

bool SketchEditor::sketchKey(QKeyEvent* e) {
  if (!m_active) return false;
  switch (e->key()) {
    case Qt::Key_Escape:
      if(m_boxSelecting){m_boxSelecting=false;rebuild();return true;}
      if (m_placingDim || !m_clicks.empty() || !m_chain.empty() || !m_picked.empty()) {
        if (m_chain.size() > 1) finishChain();
        else {
          cancel_change();
          m_clicks.clear();
          m_chain.clear();
          m_picked.clear();
          m_placingDim = false;
        }
        toolPrompt();
      } else if (m_tool != "select") {
        setTool("select");
      } else {
        m_sel.clear();
      }
      rebuild();
      emit changed();
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      if (!m_chain.empty()) { finishChain(); return true; }
      return false;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      deleteSelection();
      return true;
    case Qt::Key_L: setTool("line"); return true;
    case Qt::Key_R: setTool("rect"); return true;
    case Qt::Key_C: setTool("circle"); return true;
    case Qt::Key_A: setTool("arc3"); return true;
    case Qt::Key_D: setTool("dimension"); return true;
    case Qt::Key_T: setTool("trim"); return true;
    case Qt::Key_X: toggleConstruction(); return true;
    default:
      return false;
  }
}

void SketchEditor::deleteSelection() {
  if (m_sel.empty()) return;
  begin_change();
  for (int id : m_sel) {
    if (isFixedPoint(id)) continue;
    // A point that curves hang on goes with its curves only when it was picked alone and on purpose: keep it.
    m_sk.remove(id);
  }
  m_sel.clear();
  end_change(tr("Delete"));
}

void SketchEditor::toggleConstruction() {
  bool any = false;
  begin_change();
  for (int id : m_sel)
    if (SkEntity* e = m_sk.entity(id)) {
      e->construction = !e->construction;
      any = true;
    }
  if (!any) {
    cancel_change();
    emit status(tr("Select curves first, then X turns them into construction geometry (and back)."));
    return;
  }
  end_change(tr("Construction"));
}

// ---------------------------------------------------------------- drawing
void SketchEditor::rebuild() {
  if (m_prs.IsNull()) return;
  const Tokens& t = m_viewport->tokens();
  SketchPrs& d = *static_cast<SketchPrs*>(m_prs.get());
  d.solid.clear();
  d.dashed.clear();
  d.thin.clear();
  d.points.clear();
  d.bigPoints.clear();
  d.texts.clear();
  d.fill = m_fill;
  d.fillColor = t.sel;
  m_glyphHits.clear();
  d.textBack = t.bg2;
  auto W = [&](double u, double v) { return m_frame.to_world(u, v); };
  const double px = m_viewport->pixelSize();
  if(m_boxSelecting) {
    const QColor color=m_boxU<m_dragU?t.green:t.sel;
    auto& lines=m_boxU<m_dragU?d.dashed:d.thin;
    lines.push_back({W(m_dragU,m_dragV),W(m_boxU,m_dragV),color});
    lines.push_back({W(m_boxU,m_dragV),W(m_boxU,m_boxV),color});
    lines.push_back({W(m_boxU,m_boxV),W(m_dragU,m_boxV),color});
    lines.push_back({W(m_dragU,m_boxV),W(m_dragU,m_dragV),color});
  }
  m_samplePixelSize=px;
  const std::set<int> freePts(m_solved.free_points.begin(), m_solved.free_points.end());
  const std::set<int> selected(m_sel.begin(), m_sel.end());
  const std::set<int> picked(m_picked.begin(), m_picked.end());
  auto entityColor = [&](const SkEntity& e) {
    if (selected.count(e.id) || picked.count(e.id)) return t.hov;
    if (m_hover.kind == Hit::Entity && m_hover.id == e.id) return t.hov.lighter(115);
    if (e.fixed) return t.green;
    bool free = false;
    for (int pid : e.p) free = free || freePts.count(pid) > 0;
    if (e.type == SkEntity::Type::Circle || e.type == SkEntity::Type::Ellipse) {
      // A radius nothing holds is a freedom too; the solver reports only points, so look for what pins it.
      bool held = false;
      for (const auto& c : m_sk.constraints)
        if (std::find(c.refs.begin(), c.refs.end(), e.id) != c.refs.end() &&
            (c.type == SkConstraint::Type::Radius || c.type == SkConstraint::Type::Diameter || c.type == SkConstraint::Type::Equal || c.type == SkConstraint::Type::Tangent || c.type == SkConstraint::Type::Coincident || c.type == SkConstraint::Type::Fix))
          held = true;
      free = free || !held;
    }
    return free ? t.sel : t.fg;  // blue while it can still move, neutral once fully defined
  };

  // The plane's axes through the origin, faint.
  const double reach = 4000 * px;
  d.thin.push_back({W(-reach, 0), W(reach, 0), QColor(200, 70, 70, 255)});
  d.thin.push_back({W(0, -reach), W(0, reach), QColor(70, 170, 70, 255)});

  for (const auto& e : m_sk.entities) {
    const QColor c = entityColor(e);
    if (e.type == SkEntity::Type::Point) continue;
    const auto pts = sampled(e);
    auto& into = e.construction ? d.dashed : d.solid;
    for (size_t i = 0; i + 1 < pts.size(); ++i) into.push_back({W(pts[i].first, pts[i].second), W(pts[i + 1].first, pts[i + 1].second), c});
  }
  for (const auto& p : m_sk.points) {
    const bool hot = selected.count(p.id) || picked.count(p.id) || (m_hover.kind == Hit::Point && m_hover.id == p.id);
    const QColor c = hot ? t.hov : p.fixed ? t.green : freePts.count(p.id) ? t.sel : t.fg;
    (hot ? d.bigPoints : d.points).push_back({W(p.x, p.y), c});
    if(m_dangling.count(p.id)) d.bigPoints.push_back({W(p.x,p.y),t.red});
  }

  // Constraint glyphs next to what they hold.
  std::map<int, int> stacked;  // several glyphs on one entity sit side by side
  for (const auto& c : m_sk.constraints) {
    if (c.is_dimension() || c.refs.empty()) continue;
    const char* glyph = nullptr;
    switch (c.type) {
      case SkConstraint::Type::Horizontal: glyph = "H"; break;
      case SkConstraint::Type::Vertical: glyph = "V"; break;
      case SkConstraint::Type::Parallel: glyph = "//"; break;
      case SkConstraint::Type::Perpendicular: glyph = "_|_"; break;
      case SkConstraint::Type::Tangent: glyph = "T"; break;
      case SkConstraint::Type::Equal: glyph = "="; break;
      case SkConstraint::Type::Concentric: glyph = "(o)"; break;
      case SkConstraint::Type::Midpoint: glyph = "M"; break;
      case SkConstraint::Type::Symmetric: glyph = "S"; break;
      case SkConstraint::Type::Collinear: glyph = "C"; break;
      case SkConstraint::Type::Fix: glyph = "F"; break;
      default: break;  // coincident shows as the points meeting
    }
    if (!glyph) continue;
    for (int ref : c.refs) {
      double gu = 0, gv = 0;
      if (const SkEntity* e = m_sk.entity(ref)) {
        const auto pts = sampled(*e);
        if (pts.empty()) continue;
        gu = pts[pts.size() / 2].first;
        gv = pts[pts.size() / 2].second;
        if (e->type == SkEntity::Type::Line) { gu = (pts[0].first + pts[1].first) / 2; gv = (pts[0].second + pts[1].second) / 2; }
      } else if (const SkPoint* p = m_sk.point(ref)) {
        gu = p->x;
        gv = p->y;
      } else {
        continue;
      }
      const int k = stacked[ref]++;
      d.texts.push_back({W(gu + (14 + 16 * k) * px, gv + 12 * px), QString::fromLatin1(glyph), selected.count(c.id) ? t.hov : t.green});
      m_glyphHits.push_back({c.id,gu+(14+16*k)*px,gv+12*px});
      if (c.type == SkConstraint::Type::Midpoint || c.type == SkConstraint::Type::Symmetric || c.type == SkConstraint::Type::Fix) break;  // one glyph is enough
    }
  }

  // Dimensions.
  auto dimension = [&](const SkConstraint& c, bool pending) {
    const QColor col = selected.count(c.id) || (m_hover.kind == Hit::Dimension && m_hover.id == c.id) ? t.hov : pending ? t.hov : t.fg2;
    double lu, lv;
    labelPosition(c, lu, lv);
    auto P = [&](int id, double& x, double& y) {
      if (const SkPoint* p = m_sk.point(id)) { x = p->x; y = p->y; return true; }
      return false;
    };
    auto ends = [&](double& ax, double& ay, double& bx, double& by) {
      if (c.refs.size() == 1) {
        const SkEntity* e = m_sk.entity(c.refs[0]);
        return e && e->p.size() >= 2 && P(e->p[0], ax, ay) && P(e->p[1], bx, by);
      }
      return c.refs.size() == 2 && P(c.refs[0], ax, ay) && P(c.refs[1], bx, by);
    };
    double ax, ay, bx, by;
    using T = SkConstraint::Type;
    if ((c.type == T::Distance || c.type == T::HDistance || c.type == T::VDistance) && ends(ax, ay, bx, by)) {
      // Extension lines to a dimension line through the label.
      double dx = bx - ax, dy = by - ay;
      if (c.type == T::HDistance) dy = 0;
      if (c.type == T::VDistance) dx = 0;
      const double len = std::hypot(dx, dy);
      if (len > 1e-9) {
        const double tx = dx / len, ty = dy / len, nx = -ty, ny = tx;
        const double off = (lu - ax) * nx + (lv - ay) * ny;
        const double a2x = ax + nx * off, a2y = ay + ny * off;
        const double offb = (lu - bx) * nx + (lv - by) * ny;
        const double b2x = bx + nx * offb, b2y = by + ny * offb;
        d.thin.push_back({W(ax, ay), W(a2x, a2y), col});
        d.thin.push_back({W(bx, by), W(b2x, b2y), col});
        d.thin.push_back({W(a2x, a2y), W(b2x, b2y), col});
        lu += nx * 9 * px * (off < 0 ? -1 : 1);  // the value sits beside its dimension line, not on it
        lv += ny * 9 * px * (off < 0 ? -1 : 1);
        for (const auto& [ex, ey, s] : {std::tuple{a2x, a2y, 1.0}, std::tuple{b2x, b2y, -1.0}}) {  // arrow heads
          d.thin.push_back({W(ex, ey), W(ex + s * (tx * 9 - nx * 3) * px, ey + s * (ty * 9 - ny * 3) * px), col});
          d.thin.push_back({W(ex, ey), W(ex + s * (tx * 9 + nx * 3) * px, ey + s * (ty * 9 + ny * 3) * px), col});
        }
      }
    } else if (c.type == T::Distance && c.refs.size() == 2) {  // point-line or line-line: from the point to its foot
      const SkEntity* line = m_sk.entity(c.refs[1]);
      const SkEntity* first = m_sk.entity(c.refs[0]);
      double qx = 0, qy = 0;
      const bool havePoint = first ? (first->p.size() >= 1 && P(first->p[0], qx, qy)) : P(c.refs[0], qx, qy);
      if (line && line->p.size() >= 2 && havePoint && P(line->p[0], ax, ay) && P(line->p[1], bx, by)) {
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        const double k = len2 < 1e-18 ? 0 : ((qx - ax) * dx + (qy - ay) * dy) / len2;
        d.thin.push_back({W(qx, qy), W(ax + k * dx, ay + k * dy), col});
      }
    } else if ((c.type == T::Radius || c.type == T::Diameter) && !c.refs.empty()) {
      const SkEntity* e = m_sk.entity(c.refs[0]);
      if (e && !e->p.empty() && P(e->p[0], ax, ay)) {
        const double r = e->type == SkEntity::Type::Arc && e->p.size() > 1 && P(e->p[1], bx, by) ? std::hypot(bx - ax, by - ay) : e->r;
        const double dl = std::hypot(lu - ax, lv - ay);
        const double ux = dl > 1e-9 ? (lu - ax) / dl : 1, uy = dl > 1e-9 ? (lv - ay) / dl : 0;
        const double from = c.type == T::Diameter ? -r : 0;
        d.thin.push_back({W(ax + ux * from, ay + uy * from), W(ax + ux * std::max(r, dl), ay + uy * std::max(r, dl)), col});
      }
    } else if (c.type == T::Angle && c.refs.size() == 2) {
      const SkEntity *l1 = m_sk.entity(c.refs[0]), *l2 = m_sk.entity(c.refs[1]);
      double cx, cy, ex, ey;
      if (l1 && l2 && l1->p.size() >= 2 && l2->p.size() >= 2 && P(l1->p[0], ax, ay) && P(l1->p[1], bx, by) && P(l2->p[0], cx, cy) && P(l2->p[1], ex, ey)) {
        // An arc about the lines' intersection, through the label.
        const double d1x = bx - ax, d1y = by - ay, d2x = ex - cx, d2y = ey - cy, den = d1x * d2y - d1y * d2x;
        double ox = ax, oy = ay;
        if (std::fabs(den) > 1e-12) {
          const double k = ((cx - ax) * d2y - (cy - ay) * d2x) / den;
          ox = ax + k * d1x;
          oy = ay + k * d1y;
        }
        const double r = std::max(12 * px, std::hypot(lu - ox, lv - oy));
        double a0 = std::atan2(d1y, d1x), a1 = std::atan2(d2y, d2x);
        double sweep = a1 - a0;
        while (sweep > M_PI) sweep -= 2 * M_PI;
        while (sweep < -M_PI) sweep += 2 * M_PI;
        const int n = 24;
        for (int i = 0; i < n; ++i)
          d.thin.push_back({W(ox + r * std::cos(a0 + sweep * i / n), oy + r * std::sin(a0 + sweep * i / n)), W(ox + r * std::cos(a0 + sweep * (i + 1) / n), oy + r * std::sin(a0 + sweep * (i + 1) / n)), col});
      }
    }
    d.texts.push_back({W(lu, lv), dimensionText(c), col});
  };
  for (const auto& c : m_sk.constraints)
    if (c.is_dimension() && !(m_dimEdit && m_dimEdit->isVisible() && m_dimEditing == c.id)) dimension(c, false);
  if (m_placingDim && m_haveCursor) {
    SkConstraint c = m_pendingDim;
    c.pos[0] = m_cursor.u;
    c.pos[1] = m_cursor.v;
    dimension(c, true);
  }

  for(const auto& e:m_sk.entities) if(e.type==SkEntity::Type::Spline && e.degree) {
    for(size_t i=1;i<e.p.size();++i) {
      const auto* a=m_sk.point(e.p[i-1]);const auto* b=m_sk.point(e.p[i]);
      if(a && b) d.thin.push_back({W(a->x,a->y),W(b->x,b->y),t.fg3});
    }
  }
  // Rubber band of the running tool.
  if (m_haveCursor && m_tool != "select") {
    const QColor rb = t.hov;
    const double cu = m_cursor.u, cv = m_cursor.v;
    auto seg = [&](double x0, double y0, double x1, double y1) { d.solid.push_back({W(x0, y0), W(x1, y1), rb}); };
    auto circle = [&](double x, double y, double r) {
      for (int i = 0; i < 72; ++i) seg(x + r * std::cos(i * M_PI / 36), y + r * std::sin(i * M_PI / 36), x + r * std::cos((i + 1) * M_PI / 36), y + r * std::sin((i + 1) * M_PI / 36));
    };
    if (m_tool=="spline" && !m_chain.empty()) {
      Sketch preview=m_sk;auto nodes=m_chain;
      if(const auto* last=preview.point(nodes.back());last && std::hypot(last->x-cu,last->y-cv)>1e-7) nodes.push_back(preview.add_point(cu,cv));
      if(nodes.size()>=2) {
        const int id=add_cubic_spline(preview,nodes);auto edge=entity_edge(preview,*preview.entity(id),opad::Frame{});
        if(!edge.IsNull()) {BRepAdaptor_Curve c(edge);auto previous=c.Value(c.FirstParameter());const int samples=int(nodes.size())*24;
          for(int i=1;i<=samples;++i) {auto at=c.Value(c.FirstParameter()+(c.LastParameter()-c.FirstParameter())*i/samples);seg(previous.X(),previous.Y(),at.X(),at.Y());previous=at;}}
      }
    } else if (m_tool == "line" && !m_chain.empty()) {
      if (const SkPoint* p = m_sk.point(m_chain.back())) seg(p->x, p->y, cu, cv);
    } else if (!m_clicks.empty()) {
      const Snap& a = m_clicks[0];
      if (m_tool == "rect") { seg(a.u, a.v, cu, a.v); seg(cu, a.v, cu, cv); seg(cu, cv, a.u, cv); seg(a.u, cv, a.u, a.v); }
      else if (m_tool == "crect") {
        const double w = std::fabs(cu - a.u), h = std::fabs(cv - a.v);
        seg(a.u - w, a.v - h, a.u + w, a.v - h); seg(a.u + w, a.v - h, a.u + w, a.v + h); seg(a.u + w, a.v + h, a.u - w, a.v + h); seg(a.u - w, a.v + h, a.u - w, a.v - h);
      } else if (m_tool == "circle" || m_tool == "polygon") circle(a.u, a.v, std::hypot(cu - a.u, cv - a.v));
      else if (m_tool == "ellipse" && m_clicks.size() == 1) seg(a.u, a.v, cu, cv);
      else if (m_tool == "slot" || m_tool == "arc3" || m_tool == "arcc" || m_tool == "circle3" || m_tool == "ellipse") {
        seg(m_clicks.back().u, m_clicks.back().v, cu, cv);
        if (m_clicks.size() == 2) seg(a.u, a.v, m_clicks[1].u, m_clicks[1].v);
      }
    }
    d.bigPoints.push_back({W(cu, cv), m_cursor.point || m_cursor.entity ? t.green : rb});
    if (const auto* reference=m_sk.point(m_trackingPoint); QSettings().value("view/tracking",true).toBool() && reference) {
      if(m_cursor.tracking || m_cursor.entity) {
        d.dashed.push_back({W(reference->x,reference->y),W(cu,cv),t.green});
        d.texts.push_back({W(cu+14*px,cv+12*px),m_inferenceLocked?tr("Locked"):tr("Tracking"),t.green});
      }
    }
    if (m_cursor.horizontal) d.texts.push_back({W(cu + 14 * px, cv + 12 * px), "H", t.green});
    if (m_cursor.vertical) d.texts.push_back({W(cu + 14 * px, cv + 12 * px), "V", t.green});
  }
  m_prs->SetToUpdate();
  m_viewport->updateOverlay(m_prs);
}
