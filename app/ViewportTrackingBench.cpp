// OPAD_BENCH_TRACKING=<prefix> (UI-31): tracking and extension never act on what lies behind a face. Case in
// tools/bench_cases/viewer.py on the as1 sample; Qt events stay within the hidden window. What is "behind" is judged here
// by the box selection's test (the front surface at the point's pixel), not by the tracker's own ray (pointVisible).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QTimer>

#include <AIS_RubberBand.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <memory>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
void waitFor(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}
std::vector<gp_Pnt> spread(const std::vector<gp_Pnt>& all, size_t n) {  // n of them, evenly through the list
  std::vector<gp_Pnt> out;
  const size_t step = std::max<size_t>(1, all.size() / std::max<size_t>(1, n));
  for (size_t i = 0; i < all.size() && out.size() < n; i += step) out.push_back(all[i]);
  return out;
}
}  // namespace

// In the Distance tool (it takes points), iso view: (1) vertex mode: no vertex behind a face is hovered or, resting on it,
// acquired; the vertices in sight are hovered; resting on one acquires it only after the dwell, resting on it again
// releases it. (2) The guide points of an anchor are offered where they can be seen, never behind a face. (3) Seen from
// the opposite side the camera move drops the anchors it hides, keeps the others, and a vertex hidden before is acquired.
// (4) Edge mode: no edge behind a face is hovered, the edges in sight are. (5) 2D mode with no tool taking points tracks
// nothing; with the tool it does. <prefix>.anchor.png, <prefix>.guide.png, <prefix>.hidden.png.
bool Viewport::benchTracking(const QString& prefix) {
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: tracking: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!m_initialised || m_items.empty() || !m_pickAccumulate) return require(false, "a model is displayed and a tool takes points");
  auto settle = [&] {
    QElapsedTimer t;
    t.start();
    while ((m_filterJob || m_displayJob) && t.elapsed() < 30000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  };
  std::vector<double> frames;  // each hover's frame: the pick, the occlusion tests, the tracker, the redraw
  int hovers = 0, faceTaken = 0;
  auto hover = [&](const QPointF& at) {
    QElapsedTimer clock;
    clock.start();
    QMouseEvent e(QEvent::MouseMove, at, mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &e);
    paintEvent(nullptr);
    frames.push_back(clock.nsecsElapsed() / 1e6);
    ++hovers;
    faceTaken += m_ctx->HasDetected() && !Handle(OccluderOwner)::DownCast(m_ctx->DetectedOwner()).IsNull();
  };
  const QPointF away(6, height() - 6);
  auto rest = [&](const QPointF& at) {  // the dwell timer's paint, as on screen
    hover(at);
    waitFor(kTrackingDwellMs + 150);
    paintEvent(nullptr);
  };
  auto widget = [&](const gp_Pnt& p) { return QPointF(widgetPoint({p.X(), p.Y(), p.Z()})); };
  auto view = [&](V3d_TypeOfOrientation o) {
    m_view->SetProj(o);
    m_view->FitAll(fitBounds(), 0.08, Standard_False);
    m_view->Redraw();  // the picker's z range
    handleViewRedraw(m_ctx, m_view);
    paintEvent(nullptr);
  };
  auto depth = [&](const gp_Pnt& p) { return gp_Vec(m_view->Camera()->Eye(), p).Dot(gp_Vec(m_view->Camera()->Direction())); };
  // At the point's pixel and 2 px around it: -1 a surface in front of it by more than 8 px at all five, 1 none in front
  // by more than 2 px at any, 0 otherwise (a silhouette, a face seen edge-on).
  auto seen = [&](const gp_Pnt& p) {
    Standard_Integer x = 0, y = 0;
    m_view->Convert(p.X(), p.Y(), p.Z(), x, y);
    const double d = depth(p), px = pixelSize();
    int behind = 0, clear = 0;
    for (const auto& [dx, dy] : {std::pair{0, 0}, {2, 0}, {-2, 0}, {0, 2}, {0, -2}}) {
      m_navSelector->Pick(x + dx, y + dy, m_view);
      double front = RealLast();
      for (int i = 1; i <= m_navSelector->NbPicked(); ++i) {
        const auto node = m_navNodes.find(m_navSelector->Picked(i)->Selectable().get());
        if (node == m_navNodes.end()) continue;
        const auto item = m_items.find(node->second);
        if (item != m_items.end() && m_ctx->IsDisplayed(item->second.ais)) front = std::min(front, depth(m_navSelector->PickedPoint(i)));
      }
      behind += front < d - 8 * px;
      clear += front >= d - 2 * px;
    }
    return behind == 5 ? -1 : clear == 5 ? 1 : 0;
  };
  auto points = [&](TopAbs_ShapeEnum type) {
    const QRect inside = rect().adjusted(24, 24, -24, -24);
    std::vector<gp_Pnt> out;
    for (const auto& [id, item] : m_items) {
      if (!m_ctx->IsDisplayed(item.ais)) continue;
      TopTools_IndexedMapOfShape map;
      TopExp::MapShapes(item.ais->Shape(), type, map);
      const gp_Trsf tr = item.ais->Transformation();
      for (int i = 1; i <= map.Extent() && out.size() < 4000; ++i) {
        gp_Pnt p;
        if (type == TopAbs_VERTEX) p = BRep_Tool::Pnt(TopoDS::Vertex(map(i)));
        else {
          const TopoDS_Edge& edge = TopoDS::Edge(map(i));
          Standard_Real first = 0, last = 0;
          if (BRep_Tool::Degenerated(edge) || BRep_Tool::Curve(edge, first, last).IsNull()) continue;
          p = BRepAdaptor_Curve(edge).Value((first + last) / 2);
        }
        p.Transform(tr);
        const QPointF w = widget(p);
        if (inside.contains(w.toPoint()) && !(w.x() > width() - 220 && w.y() < 220)) out.push_back(p);  // not under the cube
      }
    }
    return out;
  };
  // What the pointer took: the kind and point of the detected owner.
  auto taken = [&](opad::Ref::Kind& kind, gp_Pnt& at) {
    if (!m_ctx->HasDetected() || !m_nodeOf.count(m_ctx->DetectedInteractive().get()) || !detectedPoint(at)) return false;
    const auto owner = Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner());
    kind = owner.IsNull() ? opad::Ref::Kind::Body : owner->kind();
    return true;
  };
  auto anchorAt = [&](const gp_Pnt& p) {
    return std::any_of(m_trackingAnchors.begin(), m_trackingAnchors.end(), [&](const auto& a) { return a.point.Distance(p) < pixelSize(); });
  };
  auto classify = [&](TopAbs_ShapeEnum type, std::vector<gp_Pnt>& hidden, std::vector<gp_Pnt>& visible) {
    hidden.clear();
    visible.clear();
    for (const auto& p : points(type)) {
      const int s = seen(p);
      if (s < 0) hidden.push_back(p);
      else if (s > 0) visible.push_back(p);
    }
  };
  // Hovers each point; counts what was taken of the given kind behind a face (`behind`) and in sight (`inSight`).
  auto sweep = [&](const std::vector<gp_Pnt>& at, opad::Ref::Kind want, int& behind, int& inSight) {
    behind = inSight = 0;
    for (const auto& p : at) {
      hover(away);
      hover(widget(p));
      opad::Ref::Kind kind;
      gp_Pnt hit;
      if (!taken(kind, hit) || kind != want) continue;
      (seen(hit) < 0 ? behind : inSight) += 1;
    }
  };

  // The hover's pick (the context's MoveTo and the occlusion test after it) over a grid of the view, the second pass
  // timed (the first builds OCCT's lazy selection trees): median and worst.
  auto pickTimes = [&](const QString& mode) {
    std::vector<double> times;
    double cold = 0;
    for (int pass = 0; pass < 2; ++pass)
      for (int j = 1; j < 8; ++j)
        for (int i = 1; i < 10; ++i) {
          QElapsedTimer clock;
          clock.start();
          moveTo(devicePos(QPointF(width() * i / 10.0, height() * j / 8.0)));
          const double ms = clock.nsecsElapsed() / 1e6;
          if (pass) times.push_back(ms);
          else cold = std::max(cold, ms);
        }
    m_ctx->ClearDetected(Standard_False);
    std::sort(times.begin(), times.end());
    trace::log(QString("bench: tracking: %1 mode picks: median %2 ms, worst %3 ms over %4 (first pass worst %5 ms)")
                   .arg(mode).arg(times[times.size() / 2], 0, 'f', 1).arg(times.back(), 0, 'f', 1).arg(times.size()).arg(cold, 0, 'f', 1));
  };
  clearTracking();
  setSelectionFilter(SelFilter::Vertex);
  settle();
  view(V3d_XposYnegZpos);
  pickTimes("vertex");
  // (1) vertices
  std::vector<gp_Pnt> hidden, visible;
  classify(TopAbs_VERTEX, hidden, visible);
  require(hidden.size() >= 4 && visible.size() >= 4, QString("iso view: %1 vertices behind a face, %2 in sight").arg(hidden.size()).arg(visible.size()));
  int behind = 0, inSight = 0;
  const auto hiddenSample = spread(hidden, 16), visibleSample = spread(visible, 16);
  sweep(hiddenSample, opad::Ref::Kind::Vertex, behind, inSight);
  require(behind == 0, QString("vertex mode: pointing at %1 vertices behind a face hovers none of them (%2)").arg(hiddenSample.size()).arg(behind));
  int acquired = 0;
  for (const auto& p : spread(hidden, 4)) {
    hover(away);
    rest(widget(p));
    for (const auto& a : m_trackingAnchors) acquired += seen(a.point) < 0;
  }
  if (!hidden.empty()) {
    hover(widget(hidden.front()));
    grabImage().save(prefix + ".hidden.png");
  }
  require(acquired == 0, QString("resting %1 ms on 4 of them acquires no anchor behind a face (%2)").arg(kTrackingDwellMs).arg(acquired));
  sweep(visibleSample, opad::Ref::Kind::Vertex, behind, inSight);
  require(behind == 0 && inSight * 10 >= int(visibleSample.size()) * 8,
          QString("vertex mode: %1 of %2 vertices in sight hovered, none behind").arg(inSight).arg(visibleSample.size()));
  clearTracking();
  bool dwelt = false;
  for (const auto& p : visible) {
    hover(away);
    hover(widget(p));
    opad::Ref::Kind kind;
    gp_Pnt at;
    if (!taken(kind, at) || kind != opad::Ref::Kind::Vertex || seen(at) <= 0) continue;
    const bool early = m_trackingAnchors.empty();
    rest(widget(p));
    const bool got = anchorAt(at) && !m_anchorMarks.IsNull() && m_ctx->IsDisplayed(m_anchorMarks);
    grabImage().save(prefix + ".anchor.png");
    hover(away);
    rest(widget(p));
    const bool released = !anchorAt(at) && m_anchorMarks.IsNull();
    hover(away);
    rest(widget(p));
    require(early && got && released && anchorAt(at),
            QString("a vertex in sight: no anchor before %1 ms (%2), acquired after it with its cross (%3), released resting on it again (%4), acquired again")
                .arg(kTrackingDwellMs).arg(early).arg(got).arg(released));
    dwelt = true;
    break;
  }
  if (!dwelt) require(false, "a vertex in sight to rest on");

  // (2) guide points: anchors set on vertices in sight, the pointer along their axes
  int offered = 0, guideSeen = 0, guideBehind = 0, leaked = 0;
  for (const auto& a : spread(visible, 10)) {
    m_trackingAnchors = {{a, {}, false}};
    for (const gp_Vec d : {gp_Vec(1, 0, 0), gp_Vec(0, 1, 0), gp_Vec(0, 0, 1)})
      for (int k = -60; k <= 60; ++k) {
        if (std::abs(k) < 3) continue;
        const gp_Pnt p = a.Translated(d * (k * 6 * pixelSize()));
        const QPointF w = widget(p);
        if (!rect().adjusted(24, 24, -24, -24).contains(w.toPoint())) continue;
        const int s = seen(p);
        if (s == 0) continue;
        hover(w);
        const bool there = std::any_of(m_trackingCandidates.begin(), m_trackingCandidates.end(), [&](const TrackingCandidate& c) {
          return !c.intersection && c.anchor.Distance(a) < 1e-9 && c.direction.IsParallel(d, 1e-6) && c.point.Distance(p) < 3 * pixelSize();
        });
        (s < 0 ? guideBehind : guideSeen) += 1;
        (s < 0 ? leaked : offered) += there;
        if (s < 0 && there) {
          const auto c = std::find_if(m_trackingCandidates.begin(), m_trackingCandidates.end(), [&](const TrackingCandidate& c) { return !c.intersection && c.point.Distance(p) < 3 * pixelSize(); });
          Standard_Integer x = 0, y = 0;
          m_view->Convert(p.X(), p.Y(), p.Z(), x, y);
          m_navSelector->Pick(x, y, m_view);
          QString hits;
          for (int i = 1; i <= m_navSelector->NbPicked(); ++i) {
            const auto node = m_navNodes.find(m_navSelector->Picked(i)->Selectable().get());
            hits += QString(" %1@%2px").arg(node == m_navNodes.end() ? QString("?") : QString::fromStdString(node->second).left(8)).arg((depth(p) - depth(m_navSelector->PickedPoint(i))) / pixelSize(), 0, 'f', 1);
          }
          trace::log(QString("bench: tracking: guide point behind a face offered: %1 px from the cursor's, visible %2/%3, pixel hits%4")
                         .arg(c == m_trackingCandidates.end() ? -1.0 : c->point.Distance(p) / pixelSize(), 0, 'f', 2).arg(pointVisible(p)).arg(c == m_trackingCandidates.end() ? -1 : int(pointVisible(c->point))).arg(hits));
        }
      }
    if (guideBehind >= 20 && guideSeen >= 20) break;
  }
  require(guideBehind > 0 && leaked == 0, QString("guide points behind a face: %1, offered %2").arg(guideBehind).arg(leaked));
  require(guideSeen > 0 && offered * 10 >= guideSeen * 9, QString("guide points in sight: %1 of %2 offered").arg(offered).arg(guideSeen));
  for (int k = 10; k <= 60 && !visible.empty(); ++k) {  // a guide on screen for the picture
    m_trackingAnchors = {{visible.front(), {}, false}};
    hover(widget(visible.front().Translated(gp_Vec(1, 0, 0) * (k * 6 * pixelSize()))));
    if (!m_trackingGuide.IsNull()) break;
  }
  grabImage().save(prefix + ".guide.png");

  // (3) the other side: anchors it hides are dropped
  clearTracking();
  for (const auto& p : spread(visible, 6)) m_trackingAnchors.push_back({p, {}, false});
  showTrackingAnchors();
  const auto before = m_trackingAnchors;
  view(V3d_XnegYposZneg);
  int dropped = 0, wrong = 0;
  for (const auto& a : before) {
    const bool kept = anchorAt(a.point);
    dropped += !kept;
    wrong += kept ? seen(a.point) < 0 : seen(a.point) > 0;
  }
  require(dropped > 0 && wrong == 0, QString("seen from the other side: %1 of %2 anchors dropped, %3 wrong").arg(dropped).arg(before.size()).arg(wrong));
  clearTracking();
  bool reacquired = false;
  for (const auto& p : hidden) {
    if (seen(p) <= 0) continue;
    hover(away);
    hover(widget(p));
    opad::Ref::Kind kind;
    gp_Pnt at;
    if (!taken(kind, at) || kind != opad::Ref::Kind::Vertex || seen(at) <= 0) continue;
    rest(widget(p));
    reacquired = anchorAt(at);
    break;
  }
  require(reacquired, "a vertex hidden in the first view is acquired from the other side");

  // (4) edges
  clearTracking();
  setSelectionFilter(SelFilter::Edge);
  settle();
  view(V3d_XposYnegZpos);
  pickTimes("edge");
  classify(TopAbs_EDGE, hidden, visible);
  const auto hiddenEdges = spread(hidden, 16), visibleEdges = spread(visible, 16);
  sweep(hiddenEdges, opad::Ref::Kind::Edge, behind, inSight);
  require(!hiddenEdges.empty() && behind == 0, QString("edge mode: pointing at %1 edges behind a face hovers none of them (%2)").arg(hiddenEdges.size()).arg(behind));
  sweep(visibleEdges, opad::Ref::Kind::Edge, behind, inSight);
  require(behind == 0 && !visibleEdges.empty() && inSight * 10 >= int(visibleEdges.size()) * 8,
          QString("edge mode: %1 of %2 edges in sight hovered, none behind").arg(inSight).arg(visibleEdges.size()));
  // A box (window, then crossing) around a part in edge mode: edges only, never the faces standing in for occlusion.
  if (!visibleEdges.empty()) {
    QSignalBlocker quiet(this);  // the Distance tool would take the boxed edges as its picks
    const QPoint c = widget(visibleEdges.front()).toPoint();
    for (const bool crossing : {false, true}) {
      m_ctx->ClearSelected(false);
      const QPoint a = c - QPoint(70, 70), b = c + QPoint(70, 70);
      UpdateRubberBand(devicePos(crossing ? b : a), devicePos(crossing ? a : b));
      myGL.Selection = myUI.Selection;
      myGL.Selection.Scheme = AIS_SelectionScheme_Replace;
      myGL.Selection.ToApplyTool = true;
      handleSelectionPoly(m_ctx, m_view);
      QElapsedTimer t;
      t.start();
      while (m_boxJob && t.elapsed() < 20000) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      const bool finished = !m_boxJob;  // a big model's visibility scan may take longer: what it published so far counts
      if (m_boxJob) m_boxJob->cancel();
      int faces = 0, edges = 0, other = 0;
      for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) faces += !Handle(OccluderOwner)::DownCast(m_ctx->SelectedOwner()).IsNull();
      for (const auto& ref : selection()) (ref.kind == opad::Ref::Kind::Edge ? edges : other) += 1;
      require(faces == 0 && other == 0 && edges > 0, QString("edge mode %1 box (%2 in %3 ms): %4 edges, %5 other picks, %6 occluding faces selected")
                                                         .arg(crossing ? "crossing" : "window", finished ? "finished" : "stopped").arg(t.elapsed()).arg(edges).arg(other).arg(faces));
    }
    myUI.Reset();
    myGL.Reset();
    myUI.Selection.Points.Clear();
    myGL.Selection.Points.Clear();
    m_ctx->Remove(myRubberBand, false);
    myRubberBand->ClearPoints();
    m_ctx->ClearSelected(false);
    OnSelectionChanged(m_ctx, m_view);
  }

  // (5) 2D mode: only a tool that takes points tracks
  clearTracking();
  setSelectionFilter(SelFilter::Vertex);
  settle();
  setPickAccumulate(false);
  setTwoDimensional(true);
  m_view->FitAll(fitBounds(), 0.08, Standard_False);
  m_view->Redraw();
  paintEvent(nullptr);
  classify(TopAbs_VERTEX, hidden, visible);
  bool idle = false, tracked = false;
  for (const auto& p : visible) {
    hover(away);
    hover(widget(p));
    opad::Ref::Kind kind;
    gp_Pnt at;
    if (!taken(kind, at) || kind != opad::Ref::Kind::Vertex || seen(at) <= 0) continue;
    rest(widget(p));
    idle = m_trackingAnchors.empty() && m_trackingGuide.IsNull() && m_anchorMarks.IsNull();
    setPickAccumulate(true, true);
    hover(away);
    rest(widget(p));
    tracked = anchorAt(at);
    break;
  }
  require(idle && tracked, QString("2D mode: resting on a vertex with no tool tracks nothing (%1), with the Distance tool it acquires it (%2)").arg(idle).arg(tracked));
  setPickAccumulate(true, true);
  setTwoDimensional(false);
  clearTracking();
  require(faceTaken == 0, QString("no face stayed taken as the hovered thing in %1 hovers (%2)").arg(hovers).arg(faceTaken));
  // The median: a hidden window's redraw waits for the GPU, which other processes share (the worst is reported).
  std::sort(frames.begin(), frames.end());
  require(frames.size() > 10 && frames[frames.size() / 2] < 50, QString("%1 hover frames with their redraw: median %2 ms, 95th percentile %3 ms, worst %4 ms")
                                                              .arg(hovers).arg(frames[frames.size() / 2], 0, 'f', 1).arg(frames[frames.size() * 95 / 100], 0, 'f', 1).arg(frames.back(), 0, 'f', 1));
  {  // the test itself, as the hover, the anchors and every guide point run it
    const auto sample = spread(points(TopAbs_VERTEX), 200);
    QElapsedTimer clock;
    clock.start();
    for (const auto& p : sample) pointVisible(p);
    const double each = sample.empty() ? 0 : clock.nsecsElapsed() / 1e6 / double(sample.size());
    require(!sample.empty() && each < 2, QString("the occlusion test: %1 ms each over %2 points").arg(each, 0, 'f', 3).arg(sample.size()));
  }
  return all;
}

// The model once every body is displayed: the Distance tool in vertex mode, then Viewport::benchTracking.
OPAD_BENCH(OPAD_BENCH_TRACKING, tracking) {
  Viewport* v = w.m_viewport;
  for (const auto& root : w.m_doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
    if (const auto* n = w.m_doc->scene.node(root); n && !n->visible) w.m_doc->run("appearance", {{"target", root}, {"visible", true}});
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(&w);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, v, timer, clock, value] {
    int expected = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) expected += w.m_doc->scene.effectively_visible(id) && !w.m_doc->scene.node(id)->body_missing;
    const bool ready = !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0;
    if (!ready && clock->elapsed() < 120000) return;
    timer->stop();
    timer->deleteLater();
    bool ok = ready;
    if (!ready) trace::log("bench: tracking: the model is displayed FAIL");
    else {
      w.startTool("distance");
      w.action("select.vertices")->trigger();
      ok = v->benchTracking(value);
      w.cancelTool();
    }
    QCoreApplication::exit(ok ? 0 : 2);
  });
  timer->start(50);
  return true;
}
