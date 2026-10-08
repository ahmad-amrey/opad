// OPAD_BENCH_REFINED_HIGHLIGHT=<prefix> on the cylinder fixture: X-ray highlight off (the selection depth-tested); its shape
// meshed again in place (as an export did), the selected wall still highlighted as drawn; then zoomed in
// on the wall until the zoom refinement draws it finer, its wall face selected: the highlight is the refined face's own
// triangles (more than the base mesh's), in the depth-tested layer, so it lies exactly on what is drawn (it cut in and out
// of the finer surface: stripes over the face, a dashed edge). Its rim selected too: drawn along the refined points. Frame:
// <prefix>.png.
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <BRepAdaptor_Surface.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
bool settleUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  QCoreApplication::processEvents();
  return done();
}
}  // namespace

bool Viewport::benchRefinedHighlight(const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: refined highlight: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!require(m_items.size() == 1, "one body (the cylinder)")) return false;
  const std::string id = m_items.begin()->first;
  const Handle(BodyShape) body = Handle(BodyShape)::DownCast(m_items.begin()->second.ais);
  if (!require(!body.IsNull() && body->prs(), "drawn from worker arrays")) return false;
  setXrayHighlight(false);
  require(!selectionXray(), "X-ray highlight off: the selection is depth-tested");
  // The wall face and its top rim, by ordinal.
  TopTools_IndexedMapOfShape faces, edges;
  TopExp::MapShapes(body->Shape(), TopAbs_FACE, faces);
  TopExp::MapShapes(body->Shape(), TopAbs_EDGE, edges);
  int wall = -1;
  for (int i = 1; i <= faces.Extent() && wall < 0; ++i)
    if (BRepAdaptor_Surface(TopoDS::Face(faces(i))).GetType() == GeomAbs_Cylinder) wall = i - 1;
  if (!require(wall >= 0, "the cylinder's wall")) return false;
  TopLoc_Location loc;
  const int baseTriangles = BRep_Tool::Triangulation(TopoDS::Face(faces(wall + 1)), loc)->NbTriangles();
  auto triangles = [this] {
    int n = 0;
    if (const auto hl = Handle(SubHighlight)::DownCast(m_subHl); !hl.IsNull())
      for (const auto& a : hl->m_triangles) n += a->ItemNumber();
    return n;
  };
  opad::Ref face;
  face.body = id;
  face.kind = opad::Ref::Kind::Face;
  face.index = wall;
  {
    bool applied = false;
    const auto once = connect(this, &Viewport::filterApplied, this, [&applied] { applied = true; });
    setSelectionFilter(SelFilter::Face);  // faces are picked (and selected) in the Face filter
    settleUntil([&] { return applied; }, 30000);
    disconnect(once);
  }
  // The shape meshed again in place at another tolerance (as a render or an export did to the shared shape): a highlight from
  // the face's triangulation now would cut in and out of the face drawn. It is the drawn arrays' face.
  const BodyPrs* drawn = body->drawnIndex();  // the base mesh's, or a first refinement's if the window was refined already
  if (!require(drawn, "the drawn arrays know their faces and edges")) return false;
  const int drawnWall = drawn->faceTriangles[size_t(wall)].second;
  BRepMesh_IncrementalMesh(body->Shape(), 0.002, Standard_False, 0.1, Standard_True);
  const int remeshed = BRep_Tool::Triangulation(TopoDS::Face(faces(wall + 1)), loc)->NbTriangles();
  selectRefs({face});
  settleUntil([&] { return !m_subHl.IsNull() && !m_subJob; }, 10000);
  require(triangles() == drawnWall && remeshed != drawnWall,
          QString("meshed again in place (%1 triangles now), the selected wall is still highlighted as drawn (%2 triangles)").arg(remeshed).arg(triangles()));
  clearSelection();
  // Zoomed in on the wall, still: refined (a pass at a time, as the camera coming to rest asks).
  m_needFit = false;
  m_view->SetProj(V3d_Yneg);
  m_view->FitAll(fitBounds(), 0.1, Standard_False);
  m_view->SetZoom(40);
  m_view->Redraw();
  const bool refined = settleUntil([&] {
    if (!m_refineJob) refineVisible();
    return body->displayPrs() && body->displayPrs()->indexed() && !m_refineJob;
  }, 60000);
  if (!require(refined, QString("zoomed in, the body is drawn refined (%1 triangles, the base mesh's wall %2)")
                            .arg(body->displayPrs() ? int(body->displayPrs()->triangleCount()) : 0).arg(baseTriangles)))
    return false;
  const auto& shown = body->displayPrs();
  const int refinedWall = shown->faceTriangles[size_t(wall)].second;
  // The wall selected: the highlight from the refined triangles, depth-tested.
  selectRefs({face});
  settleUntil([&] { return !m_subHl.IsNull() && !m_subJob; }, 10000);
  require(!m_subHl.IsNull() && triangles() == refinedWall && refinedWall > baseTriangles && m_subHl->ZLayer() == Graphic3d_ZLayerId_Top,
          QString("the selected wall is highlighted with the refined mesh's %1 triangles (not the base mesh's %2), depth-tested").arg(triangles()).arg(baseTriangles));
  m_view->Redraw();
  grabImage().save(prefix + ".png");
  // An edge of it: along the refined points.
  int rim = -1;
  for (int i = 1; i <= edges.Extent() && rim < 0; ++i)
    if (!BRep_Tool::Degenerated(TopoDS::Edge(edges(i))) && shown->edgeLineOf(i - 1) && shown->edgeLineOf(i - 1)->size() > 2) rim = i - 1;
  if (require(rim >= 0, "a curved edge of it")) {
    opad::Ref edge;
    edge.body = id;
    edge.kind = opad::Ref::Kind::Edge;
    edge.index = rim;
    bool edgesApplied = false;
    const auto edgeFilter = connect(this, &Viewport::filterApplied, this, [&edgesApplied] { edgesApplied = true; });
    setSelectionFilter(SelFilter::Edge);
    settleUntil([&] { return edgesApplied; }, 30000);
    disconnect(edgeFilter);
    selectRefs({edge});
    settleUntil([&] { return !m_subHl.IsNull() && !m_subJob; }, 10000);
    int segments = 0;
    if (const auto hl = Handle(SubHighlight)::DownCast(m_subHl); !hl.IsNull())
      for (const auto& a : hl->m_segments) segments += a->VertexNumber() / 2;
    require(segments == int(shown->edgeLineOf(rim)->size()) - 1, QString("the selected edge is drawn along its %1 refined segments").arg(segments));
  }
  clearSelection();
  return all;
}

OPAD_BENCH(OPAD_BENCH_REFINED_HIGHLIGHT, refinedHighlight) {
  Viewport* v = w.m_viewport;
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(&w);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, v, timer, clock, value] {
    const bool ready = !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() > 0 && !w.m_jobs->busy();
    if (!ready && clock->elapsed() < 120000) return;
    timer->stop();
    timer->deleteLater();
    const bool ok = ready && v->benchRefinedHighlight(value);
    QCoreApplication::exit(ok ? 0 : 2);
  });
  timer->start(50);
  return true;
}
