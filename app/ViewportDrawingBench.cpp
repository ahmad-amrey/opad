// OPAD_BENCH_BIGDRAWING=<prefix> (UI-42, UI-11): a drawing layer of 100,000 lines (tools/bench_cases/viewer.py writes the
// DXF) opens with no display step over 50 ms of UI-thread CPU: its Edge filter picks the lines in groups built on the mesh
// worker, where one sensitive per line made OCCT build a picking BVH over 100,000 entities on the UI thread (0.6-1 s). The
// picking stays exact: hovering the middle of a line detects that line, a click selects it alone, Ctrl+click takes it out
// again (the same owner), a crossing box takes every line with a part in it and a window box every line wholly in it
// (judged by the lines' own ends; those within 3 px of the box's border are not judged), selectRefs selects a line by its
// ordinal. The Body filter and back, and closing the drawing (a new document), stay within 150 ms of CPU a step.
// <prefix>.png: the view with the window box's lines selected.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLineF>
#include <QTimer>

#include <BRep_Tool.hxx>
#include <TColStd_ListOfInteger.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <functional>
#include <memory>
#include <set>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return done();
}

bool crosses(const QRectF& r, const QPointF& a, const QPointF& b) {
  if (r.contains(a) || r.contains(b)) return true;
  const QLineF line(a, b);
  for (const QLineF& side : {QLineF(r.topLeft(), r.topRight()), QLineF(r.topRight(), r.bottomRight()), QLineF(r.bottomRight(), r.bottomLeft()),
                             QLineF(r.bottomLeft(), r.topLeft())})
    if (line.intersects(side, nullptr) == QLineF::BoundedIntersection) return true;
  return false;
}
}  // namespace

bool Viewport::benchBigDrawing(const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: big drawing: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!require(m_items.size() == 1, QString("one layer displayed (%1)").arg(m_items.size()))) return false;
  const std::string id = m_items.begin()->first;
  const Item& item = m_items.begin()->second;
  const auto body = Handle(BodyShape)::DownCast(item.ais);
  if (!require(!body.IsNull() && body->groupedEdges(), QString("its lines are picked in %1 groups").arg(body.IsNull() || !body->prs() ? 0 : body->prs()->edgeGroups.size())))
    return false;
  require(m_filter == SelFilter::Edge, "the Edge filter is on (a drawing opens in it)");
  // Every line's ends in the view's coordinates, by ordinal (as BodyPrs and the owners count them).
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(item.located, TopAbs_EDGE, edges);
  const gp_Trsf placed = item.ais->Transformation();
  std::vector<std::pair<gp_Pnt, gp_Pnt>> ends(size_t(edges.Extent()));
  for (int i = 1; i <= edges.Extent(); ++i) {
    TopoDS_Vertex a, b;
    TopExp::Vertices(TopoDS::Edge(edges(i)), a, b);
    ends[size_t(i - 1)] = {BRep_Tool::Pnt(a).Transformed(placed), BRep_Tool::Pnt(b).Transformed(placed)};
  }
  require(ends.size() >= 100000, QString("%1 lines in the layer").arg(ends.size()));
  // Close up on the line nearest the drawing's middle: 40 mm across, its neighbours a hundred pixels away.
  Bnd_Box bounds;
  item.ais->BoundingBox(bounds);
  const gp_Pnt middle = bounds.IsVoid() ? gp::Origin() : gp_Pnt((bounds.CornerMin().XYZ() + bounds.CornerMax().XYZ()) / 2);
  int k = 0;
  auto mid = [&](size_t i) { return gp_Pnt((ends[i].first.XYZ() + ends[i].second.XYZ()) / 2); };
  for (size_t i = 1; i < ends.size(); ++i)
    if (mid(i).Distance(middle) < mid(size_t(k)).Distance(middle)) k = int(i);
  m_needFit = false;
  standardView("top");
  const auto camera = m_view->Camera();  // straight down on it (SetCenter alone keeps the eye and tilts the view)
  camera->SetEyeAndCenter(mid(size_t(k)).Translated(gp_Vec(camera->Direction()).Reversed() * camera->Distance()), mid(size_t(k)));
  camera->SetScale(40);
  m_view->Redraw();  // the picker clips to the camera's z range, which a frame sets
  const gp_Pnt target = mid(size_t(k));
  const QPoint at = widgetPoint({target.X(), target.Y(), target.Z()});
  moveTo(devicePos(at));
  const auto detected = m_ctx->HasDetected() ? Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner()) : Handle(SubShapeOwner)();
  require(!detected.IsNull() && detected->index() == k,
          QString("hovering the middle of line %1 detects it (%2)").arg(k).arg(detected.IsNull() ? -1 : detected->index()));
  m_ctx->ClearSelected(Standard_False);
  benchClickAt(at);
  auto refs = selection();
  require(refs.size() == 1 && refs[0].kind == opad::Ref::Kind::Edge && refs[0].index == k && refs[0].body == id,
          QString("a click selects it alone (%1 refs, %2)").arg(refs.size()).arg(refs.empty() ? QString() : QString::fromStdString(refs[0].str())));
  benchClickAt(at, Qt::ControlModifier);
  require(selection().empty(), QString("Ctrl+click on it again takes it out (%1 refs left)").arg(selection().size()));
  // Boxes around it, judged by the lines' ends on screen.
  auto widget = [this](const gp_Pnt& p) {
    Standard_Integer x = 0, y = 0;
    m_view->Convert(p.X(), p.Y(), p.Z(), x, y);
    const QPointF scale = viewScale();
    return QPointF(x / scale.x(), y / scale.y());
  };
  auto box = [&](bool crossing) {
    const QRect r(at - QPoint(150, 150), at + QPoint(150, 150));
    m_ctx->ClearSelected(Standard_False);
    UpdateRubberBand(devicePos(crossing ? r.bottomRight() : r.topLeft()), devicePos(crossing ? r.topLeft() : r.bottomRight()));
    myGL.Selection = myUI.Selection;
    myGL.Selection.Scheme = AIS_SelectionScheme_Replace;
    myGL.Selection.ToApplyTool = true;
    QElapsedTimer t;
    t.start();
    handleSelectionPoly(m_ctx, m_view);
    const qint64 rectangle = t.elapsed();
    waitUntil([this] { return !m_boxJob; }, 30000);
    std::set<int> taken;
    for (const auto& ref : selection())
      if (ref.kind == opad::Ref::Kind::Edge && ref.body == id) taken.insert(ref.index);
    const QRectF inner = QRectF(r).adjusted(3, 3, -3, -3), outer = QRectF(r).adjusted(-3, -3, 3, 3);
    int in = 0, missed = 0, wrong = 0;
    for (size_t i = 0; i < ends.size(); ++i) {
      const QPointF a = widget(ends[i].first), b = widget(ends[i].second);
      const bool surely = crossing ? crosses(inner, a, b) : inner.contains(a) && inner.contains(b);
      const bool maybe = crossing ? crosses(outer, a, b) : outer.contains(a) && outer.contains(b);
      in += surely;
      missed += surely && !taken.count(int(i));
      wrong += !maybe && taken.count(int(i));
    }
    require(in > 0 && missed == 0 && wrong == 0 && rectangle < 1000,
            QString("a %1 box takes the %2 lines %3 (%4 taken, %5 missed, %6 outside it taken; the rectangle pick %7 ms)")
                .arg(crossing ? "crossing" : "window").arg(in).arg(crossing ? "with a part in it" : "wholly in it").arg(taken.size())
                .arg(missed).arg(wrong).arg(rectangle));
  };
  box(true);
  box(false);
  waitUntil([this] { return !m_subJob; }, 20000);
  grabImage().save(prefix + ".png");
  opad::Ref line;
  line.body = id;
  line.kind = opad::Ref::Kind::Edge;
  line.index = k + 1;
  selectRefs({line});
  refs = selection();
  require(refs.size() == 1 && refs[0].index == k + 1, QString("selectRefs selects line %1 by its ordinal (%2 refs)").arg(k + 1).arg(refs.size()));
  clearSelection();
  myUI.Reset();
  return all;
}

OPAD_BENCH(OPAD_BENCH_BIGDRAWING, bigdrawing) {
  Viewport* v = w.m_viewport;
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(&w);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, v, timer, clock, value] {
    const bool ready = !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() > 0;
    if (!ready && clock->elapsed() < 120000) return;
    timer->stop();
    timer->deleteLater();
    bool all = ready;
    auto require = [&all](bool ok, const QString& what) {
      trace::log(QString("bench: big drawing: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      all = all && ok;
    };
    require(ready, "the drawing is displayed");
    if (ready) {
      require(v->longestDisplayCpu() < 50 && v->longestDisplay() < 200,
              QString("no display step over 50 ms (the longest: %1 ms of CPU, %2 ms in all)").arg(v->longestDisplayCpu()).arg(v->longestDisplay()));
      all = v->benchBigDrawing(value) && all;
      // The filter there and back, and the drawing closed: each step within the budget (UI-thread CPU; 4x in wall time).
      auto step = [&](const QString& what, const std::function<void()>& fn, const std::function<bool()>& done) {
        waitUntil([&w] { return !w.m_jobs->busy(); }, 30000);
        trace::resetStalls();
        const qint64 cpu = trace::threadCpuMs();
        QElapsedTimer t;
        t.start();
        fn();
        const qint64 call = t.elapsed(), callCpu = trace::threadCpuMs() - cpu;
        const bool settled = waitUntil([&] { return done() && !w.m_jobs->busy(); }, 30000);
        const trace::Stalls s = trace::stalls();
        require(settled && std::max(callCpu, s.longestCpu) < 150 && std::max(call, s.longest) < 600,
                QString("%1: %2 ms in the command (%3 ms CPU), longest stall %4 ms (%5 ms CPU)").arg(what).arg(call).arg(callCpu).arg(s.longest).arg(s.longestCpu));
      };
      bool filtered = false;
      const auto watch = QObject::connect(v, &Viewport::filterApplied, &w, [&filtered] { filtered = true; });
      step("Body filter", [&] { w.action("select.bodies")->trigger(); }, [&] { return filtered; });
      filtered = false;
      step("Edge filter again", [&] { w.action("select.edges")->trigger(); }, [&] { return filtered; });
      QObject::disconnect(watch);
      step("closing it (new document)", [&] { w.m_doc->newDocument(); }, [v] { return v->displayedCount() == 0; });
    }
    trace::log("bench: big drawing: " + trace::stallHistogram());
    QCoreApplication::exit(all ? 0 : 2);
  });
  timer->start(50);
  return true;
}

// OPAD_BENCH_DRAWINGFILTERS=<prefix> (UI-42): a drawing of many layers (text, walls, grids; tools/bench_cases/viewer.py
// writes the DXF, or the architectural DWG where it is) opens with no display step over 50 ms of UI-thread CPU, and every
// filter keeps each step within 150 ms of CPU: the Face filter picks every drawing layer whole (mode 0; OCCT built a
// sensitive per glyph and hatch face on the UI thread, 2.3-2.7 s on the DWG) and a click on a text layer selects the layer;
// the Vertex filter picks a big layer's ends in groups built on the mesh worker (OCCT's per-vertex sensitives: 0.9 s a
// layer, 3 s in all): hovering beside an end detects it, a click selects it alone, a box takes exactly the ends in it,
// selectRefs selects one by its ordinal. <prefix>.vertices.png: the view with the box's ends selected.
bool Viewport::benchDrawingFilter(const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: drawing filters: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  // The layer to try: in the Face filter the one with the most faces (text), in the Vertex filter the biggest one of lines
  // whose ends are grouped.
  std::string id;
  int best = -1;
  for (const auto& [node, item] : m_items) {
    const auto body = Handle(BodyShape)::DownCast(item.ais);
    if (body.IsNull() || !body->prs()) continue;
    const int score = m_filter == SelFilter::Face ? int(body->prs()->triangleCount())
                      : body->groupedVertices() && body->prs()->triangles.IsNull() ? int(body->prs()->vertexShapes.size()) : -1;  // lines: ends apart
    if (score > best) { best = score; id = node; }
  }
  if (!require(best > 0, QString("a layer to pick on (%1)").arg(m_doc->nodeName(id)))) return false;
  const Item& item = m_items.at(id);
  TopTools_IndexedMapOfShape vertices;
  TopExp::MapShapes(item.located, TopAbs_VERTEX, vertices);
  const gp_Trsf placed = item.ais->Transformation();
  // The vertex nearest the layer's middle that no other layer's box holds (a drawing's layers overlap).
  Bnd_Box own;
  item.ais->BoundingBox(own);
  std::vector<Bnd_Box> others;
  for (const auto& [node, it] : m_items)
    if (node != id) it.ais->BoundingBox(others.emplace_back());
  const gp_Pnt middle((own.CornerMin().XYZ() + own.CornerMax().XYZ()) / 2);
  int chosen = 1;
  double nearest = RealLast();
  for (int i = 1; i <= vertices.Extent(); ++i) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))).Transformed(placed);
    if (p.Distance(middle) >= nearest || std::any_of(others.begin(), others.end(), [&p](const Bnd_Box& b) { return !b.IsOut(p); })) continue;
    nearest = p.Distance(middle);
    chosen = i;
  }
  const gp_Pnt target = BRep_Tool::Pnt(TopoDS::Vertex(vertices(chosen))).Transformed(placed);
  m_needFit = false;
  standardView("top");
  const auto camera = m_view->Camera();  // straight down on it (SetCenter alone keeps the eye and tilts the view)
  camera->SetEyeAndCenter(target.Translated(gp_Vec(camera->Direction()).Reversed() * camera->Distance()), target);
  camera->SetScale(40);  // ~20 px a millimetre: the grid's nearest ends are 2 mm apart
  m_view->Redraw();
  const QPoint at = widgetPoint({target.X(), target.Y(), target.Z()});
  m_ctx->ClearSelected(Standard_False);
  if (m_filter == SelFilter::Face) {
    size_t whole = 0, faces = 0, layers = 0;
    for (const auto& [node, it] : m_items) {
      if (!drawingLayer(it.ais)) continue;
      ++layers;
      TColStd_ListOfInteger modes;
      m_ctx->ActivatedModes(it.ais, modes);
      for (const int mode : modes) {
        whole += mode == 0;
        faces += mode == AIS_Shape::SelectionMode(TopAbs_FACE);
      }
    }
    require(layers > 0 && whole == layers && faces == 0,
            QString("every drawing layer is picked whole (%1 of %2 layers; %3 in OCCT's Face mode)").arg(whole).arg(layers).arg(faces));
    moveTo(devicePos(at));
    const auto node = m_ctx->HasDetected() ? m_nodeOf.find(m_ctx->DetectedInteractive().get()) : m_nodeOf.end();
    const bool detected = node != m_nodeOf.end() && node->second == id && Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner()).IsNull();
    require(detected, QString("hovering the text layer %1 detects the layer (%2)").arg(m_doc->nodeName(id), node == m_nodeOf.end() ? QString("nothing") : m_doc->nodeName(node->second)));
    benchClickAt(at);
    const auto refs = selection();
    require(refs.size() == 1 && refs[0].body == id && refs[0].kind == opad::Ref::Kind::Body,
            QString("a click selects the layer (%1 refs, %2)").arg(refs.size()).arg(refs.empty() ? QString() : m_doc->nodeName(refs[0].body) + " " + QString::fromStdString(refs[0].str())));
  } else {
    moveTo(devicePos(at + QPoint(2, 1)));
    const auto detected = m_ctx->HasDetected() ? Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner()) : Handle(SubShapeOwner)();
    require(!detected.IsNull() && detected->kind() == opad::Ref::Kind::Vertex && detected->index() == chosen - 1,
            QString("hovering beside end %1 of %2 detects it (%3)").arg(chosen - 1).arg(m_doc->nodeName(id)).arg(detected.IsNull() ? -1 : detected->index()));
    benchClickAt(at);
    auto refs = selection();
    require(refs.size() == 1 && refs[0].body == id && refs[0].kind == opad::Ref::Kind::Vertex && refs[0].index == chosen - 1,
            QString("a click selects it alone (%1 refs, %2)").arg(refs.size()).arg(refs.empty() ? QString() : QString::fromStdString(refs[0].str())));
    // A box around it, judged by the ends on screen (those within 3 px of its border are not judged).
    const QRect r(at - QPoint(100, 100), at + QPoint(100, 100));
    m_ctx->ClearSelected(Standard_False);
    UpdateRubberBand(devicePos(r.topLeft()), devicePos(r.bottomRight()));
    myGL.Selection = myUI.Selection;
    myGL.Selection.Scheme = AIS_SelectionScheme_Replace;
    myGL.Selection.ToApplyTool = true;
    handleSelectionPoly(m_ctx, m_view);
    waitUntil([this] { return !m_boxJob; }, 30000);
    std::set<int> taken;
    for (const auto& ref : selection())
      if (ref.kind == opad::Ref::Kind::Vertex && ref.body == id) taken.insert(ref.index);
    const QRectF inner = QRectF(r).adjusted(3, 3, -3, -3), outer = QRectF(r).adjusted(-3, -3, 3, 3);
    int in = 0, missed = 0, wrong = 0;
    for (int i = 1; i <= vertices.Extent(); ++i) {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))).Transformed(placed);
      Standard_Integer x = 0, y = 0;
      m_view->Convert(p.X(), p.Y(), p.Z(), x, y);
      const QPointF point(x / viewScale().x(), y / viewScale().y());
      in += inner.contains(point);
      missed += inner.contains(point) && !taken.count(i - 1);
      wrong += !outer.contains(point) && taken.count(i - 1);
    }
    require(in > 0 && missed == 0 && wrong == 0,
            QString("a box takes the %1 ends in it (%2 taken, %3 missed, %4 outside it taken)").arg(in).arg(taken.size()).arg(missed).arg(wrong));
    waitUntil([this] { return !m_subJob; }, 20000);
    grabImage().save(prefix + ".vertices.png");
    opad::Ref end;
    end.body = id;
    end.kind = opad::Ref::Kind::Vertex;
    end.index = chosen % vertices.Extent();  // the next one
    selectRefs({end});
    refs = selection();
    require(refs.size() == 1 && refs[0].index == end.index && refs[0].kind == opad::Ref::Kind::Vertex,
            QString("selectRefs selects end %1 by its ordinal (%2 refs)").arg(end.index).arg(refs.size()));
  }
  clearSelection();
  myUI.Reset();
  return all;
}

OPAD_BENCH(OPAD_BENCH_DRAWINGFILTERS, drawingfilters) {
  Viewport* v = w.m_viewport;
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(&w);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, v, timer, clock, value] {
    const bool ready = !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() > 0;
    if (!ready && clock->elapsed() < 180000) return;
    timer->stop();
    timer->deleteLater();
    bool all = ready;
    auto require = [&all](bool ok, const QString& what) {
      trace::log(QString("bench: drawing filters: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      all = all && ok;
    };
    require(ready, QString("the drawing is displayed (%1 layers)").arg(v->displayedCount()));
    if (ready) {
      require(v->longestDisplayCpu() < 50 && v->longestDisplay() < 200,
              QString("no display step over 50 ms (the longest: %1 ms of CPU, %2 ms in all)").arg(v->longestDisplayCpu()).arg(v->longestDisplay()));
      bool filtered = false;
      const auto watch = QObject::connect(v, &Viewport::filterApplied, &w, [&filtered] { filtered = true; });
      auto step = [&](const QString& what, const QString& command) {
        waitUntil([&w] { return !w.m_jobs->busy(); }, 30000);
        filtered = false;
        trace::resetStalls();
        const qint64 cpu = trace::threadCpuMs();
        QElapsedTimer t;
        t.start();
        w.action(command)->trigger();
        const qint64 call = t.elapsed(), callCpu = trace::threadCpuMs() - cpu;
        const bool settled = waitUntil([&] { return filtered && !w.m_jobs->busy(); }, 30000);
        const trace::Stalls s = trace::stalls();
        require(settled && std::max(callCpu, s.longestCpu) < 150 && std::max(call, s.longest) < 600,
                QString("%1: %2 ms in the command (%3 ms CPU), longest stall %4 ms (%5 ms CPU), applied after %6 ms")
                    .arg(what).arg(call).arg(callCpu).arg(s.longest).arg(s.longestCpu).arg(t.elapsed()));
      };
      step("Face filter", "select.faces");
      all = v->benchDrawingFilter(value) && all;
      step("Vertex filter", "select.vertices");
      all = v->benchDrawingFilter(value) && all;
      step("Edge filter", "select.edges");
      step("Body filter", "select.bodies");
      QObject::disconnect(watch);
    }
    trace::log("bench: drawing filters: " + trace::stallHistogram());
    QCoreApplication::exit(all ? 0 : 2);
  });
  timer->start(50);
  return true;
}
