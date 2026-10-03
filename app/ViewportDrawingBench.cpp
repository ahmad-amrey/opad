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
  m_view->Camera()->SetCenter(mid(size_t(k)));
  m_view->Camera()->SetScale(40);
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
