// OPAD_BENCH_CROSSLOCK=<prefix> (UI-32): the cross lock in the Distance tool, on two blocks in an iso view (the crosslock
// case) and on a drawing in 2D mode (crosslock-drawing, OPAD_BENCH_CROSSLOCK_2D=1 requires the mode). Cases in
// tools/bench_cases/viewer.py; Qt events stay within the hidden window. The anchors, the guides and the pairs are found
// on the model itself: A and B vertices in sight whose crossing P (B's coordinate along the axis drawn longest, on A's
// line) lies in sight in empty space, far enough from both on screen.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QTimer>

#include <BRep_Tool.hxx>
#include <Prs3d_PointAspect.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <memory>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Units.hpp"
#include "Viewport.hpp"

namespace {
void waitFor(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}
}  // namespace

// (1) Resting on A acquires it; its guide along the axis is locked by a tap (a double tap when several are offered) until
// a click. (2) Resting on B while locked acquires B, and the locked point snaps to the crossing P (exact, two guides, an X
// marker, the status names it with the distance along the line only). (3) Along the line away from P: the plain
// projection; 5 px from P, or anywhere on B's guide: P. (4) A click far from P takes P as the tool's pick and ends the
// lock. (5) Esc on a lock unlocks it at the shortcut-override stage (the tool keeps its pick); without a lock Esc is the
// window's. (6) A held Shift lock acquires a third anchor C, snaps to its crossing and ends on release. (7) An anchor's own
// line (an edge direction) met in space and a coordinate plane both in reach: a tap shows the other one, a tap with none
// in reach unlocks. (8) Several guides at the pointer: taps cycle, a double tap locks the one shown before it. <prefix>.cross.png (the crossing with both guides), <prefix>.held.png.
bool Viewport::benchCrossLock(const QString& prefix) {
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: crosslock: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!m_initialised || m_items.empty() || !m_pickAccumulate) return require(false, "a model is displayed and a tool takes points");
  QString status;
  const auto watch = connect(this, &Viewport::hoverChanged, this, [&status](const QString& s) { status = s; });
  std::vector<double> frames;  // the tracker's own time per hover (once more on the same state: a redraw waits for the GPU)
  auto hover = [&](const QPointF& at) {
    QMouseEvent e(QEvent::MouseMove, at, mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &e);
    paintEvent(nullptr);
    QElapsedTimer clock;
    clock.start();
    m_trackingDirty = true;
    updateTracking();
    frames.push_back(clock.nsecsElapsed() / 1e6);
  };
  auto rest = [&](const QPointF& at) {  // the dwell timer's paint, as on screen
    hover(at);
    waitFor(kTrackingDwellMs + 150);
    paintEvent(nullptr);
  };
  auto key = [&](QEvent::Type type, int k) {  // as the window delivers it; true when accepted
    QKeyEvent e(type, k, k == Qt::Key_Shift && type == QEvent::KeyPress ? Qt::ShiftModifier : Qt::NoModifier);
    e.setAccepted(type != QEvent::ShortcutOverride);  // Qt asks for an override with the event ignored
    QCoreApplication::sendEvent(this, &e);
    return e.isAccepted();
  };
  auto tap = [&] {
    key(QEvent::KeyPress, Qt::Key_Shift);
    key(QEvent::KeyRelease, Qt::Key_Shift);
    paintEvent(nullptr);
  };
  auto click = [&](const QPointF& at) {
    QMouseEvent press(QEvent::MouseButtonPress, at, mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, at, mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &press);
    QCoreApplication::sendEvent(this, &release);
    paintEvent(nullptr);
  };
  auto widget = [&](const gp_Pnt& p) { return QPointF(widgetPoint({p.X(), p.Y(), p.Z()})); };
  auto px = [&](const gp_Pnt& a, const gp_Pnt& b) { return QLineF(widget(a), widget(b)).length(); };
  auto shownPoint = [&](gp_Pnt& at) {
    const auto m = m_centers.find(m_trackingMarker);
    if (m == m_centers.end()) return false;
    at = m->second.point;
    return true;
  };
  auto anchorAt = [&](const gp_Pnt& p) {
    return std::any_of(m_trackingAnchors.begin(), m_trackingAnchors.end(), [&](const auto& a) { return a.point.Distance(p) < 1e-7; });
  };
  auto guideFrom = [&](const gp_Pnt& p) {  // a guide segment drawn from p (in sight or behind a face)
    for (const auto* object : {&m_trackingGuide, &m_trackingGuideBehind}) {
      if (object->IsNull()) continue;
      TopTools_IndexedMapOfShape map;
      TopExp::MapShapes((*object)->Shape(), TopAbs_VERTEX, map);
      for (int i = 1; i <= map.Extent(); ++i)
        if (BRep_Tool::Pnt(TopoDS::Vertex(map(i))).Distance(p) < 1e-7) return true;
    }
    return false;
  };
  auto xMarker = [&] {
    const auto m = m_centers.find(m_trackingMarker);
    return m != m_centers.end() && m->second.ais->Attributes()->PointAspect()->Aspect()->Type() == Aspect_TOM_X;
  };

  QElapsedTimer settling;  // the vertex mode's switch is a job
  settling.start();
  while ((m_filterJob || m_displayJob) && settling.elapsed() < 30000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  const bool flat = m_twoDimensional;
  if (qEnvironmentVariableIsSet("OPAD_BENCH_CROSSLOCK_2D")) require(flat, "a drawing opens in 2D mode");
  clearTracking();
  if (!flat) m_view->SetProj(V3d_XposYnegZpos);
  m_view->FitAll(fitBounds(), 0.12, Standard_False);
  m_view->Redraw();  // the picker's z range
  handleViewRedraw(m_ctx, m_view);
  paintEvent(nullptr);
  const QRect inside = rect().adjusted(40, 40, -40, -40);
  auto inView = [&](const gp_Pnt& p) { return inside.contains(widget(p).toPoint()) && (flat || !(widget(p).x() > width() - 220 && widget(p).y() < 220)); };
  // The locked line's axis: the one drawn longest on screen (X first), and the other in-plane axis for synthetic guides.
  std::vector<std::pair<double, gp_Vec>> axes;
  for (const gp_Vec& d : {gp_Vec(1, 0, 0), gp_Vec(0, 1, 0), gp_Vec(0, 0, 1)}) axes.push_back({px(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 0).Translated(d * (pixelSize() * 100))), d});
  std::stable_sort(axes.begin(), axes.end(), [](const auto& a, const auto& b) { return std::round(a.first) > std::round(b.first); });
  const gp_Vec u = axes[0].second, v = axes[1].second;
  struct Vertex { gp_Pnt p; std::string body; };
  std::vector<Vertex> vertices;
  for (const auto& [id, item] : m_items) {
    if (!m_ctx->IsDisplayed(item.ais)) continue;
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(item.ais->Shape(), TopAbs_VERTEX, map);
    for (int i = 1; i <= map.Extent(); ++i) {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(map(i))).Transformed(item.ais->Transformation());
      if (inView(p) && pointVisible(p, id) && std::none_of(vertices.begin(), vertices.end(), [&](const Vertex& w) { return w.p.Distance(p) < 1e-7; }))
        vertices.push_back({p, id});
    }
  }
  std::vector<bool> hoverable;  // pointing at it takes that vertex (one on a silhouette may lose to its neighbour)
  for (const auto& w : vertices) {
    hover(QPointF(6, height() - 6));
    hover(widget(w.p));
    gp_Pnt hit;
    const auto owner = Handle(SubShapeOwner)::DownCast(m_ctx->HasDetected() ? m_ctx->DetectedOwner() : nullptr);
    hoverable.push_back(!owner.IsNull() && owner->kind() == opad::Ref::Kind::Vertex && detectedPoint(hit) && hit.Distance(w.p) < 1e-6);
  }
  clearTracking();
  auto crossingOf = [&](const gp_Pnt& a, const gp_Pnt& b) { return a.Translated(u * gp_Vec(a, b).Dot(u)); };
  auto clearOf = [&](const gp_Pnt& p, double margin) {
    return std::all_of(vertices.begin(), vertices.end(), [&](const Vertex& w) { return px(w.p, p) >= margin; });
  };
  auto across = [&](const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& p) {  // the screen angle of b's guide to the line, degrees
    const QLineF line(widget(a), widget(a.Translated(u * (pixelSize() * 100)))), guide(widget(b), widget(p));
    const double angle = std::abs(std::fmod(line.angleTo(guide), 180.0));
    return std::min(angle, 180 - angle);
  };
  // A and B: the pair whose crossing lies furthest from both on screen.
  const Vertex* A = nullptr;
  const Vertex* B = nullptr;
  double best = 0;
  for (const auto& a : vertices)
    for (const auto& b : vertices) {
      if (!hoverable[&a - vertices.data()] || !hoverable[&b - vertices.data()]) continue;
      const gp_Pnt p = crossingOf(a.p, b.p);
      const double score = std::min(px(a.p, p), px(b.p, p));
      if (&a == &b || px(a.p, p) < 90 || px(b.p, p) < 60 || score <= best || !inView(p) || !clearOf(p, 25) || across(a.p, b.p, p) < 30 || !pointVisible(p, a.body)) continue;
      best = score, A = &a, B = &b;
    }
  if (!require(A && B, QString("two vertices in sight that line up in empty space (%1 vertices in sight, %2 hoverable, axis %3 %4 %5)")
                           .arg(vertices.size()).arg(std::count(hoverable.begin(), hoverable.end(), true)).arg(u.X()).arg(u.Y()).arg(u.Z()))) {
    disconnect(watch);
    return false;
  }
  const gp_Pnt a = A->p, b = B->p, P = crossingOf(a, b);
  trace::log(QString("bench: crosslock: A (%1, %2, %3), B (%4, %5, %6), P %7 px from A, %8 px from B")
                 .arg(a.X()).arg(a.Y()).arg(a.Z()).arg(b.X()).arg(b.Y()).arg(b.Z()).arg(px(a, P), 0, 'f', 0).arg(px(b, P), 0, 'f', 0));
  auto onLine = [&](double t) { return a.Translated(gp_Vec(a, P) * t); };  // a point of A's line, t = 1 at P
  auto along = [&](const gp_Pnt& p, double pixels) { return p.Translated(u * (gp_Vec(a, P).Dot(u) >= 0 ? 1 : -1) * pixels * pixelSize()); };
  gp_Pnt Q = onLine(0.4);
  for (const double t : {0.4, 0.3, 0.5, 0.6})
    if (clearOf(onLine(t), 20)) { Q = onLine(t); break; }
  auto isLine = [&](const TrackingCandidate& c) { return !c.intersection && c.anchor.Distance(a) < 1e-9 && c.direction.IsParallel(u, 1e-6); };
  // Shows A's guide along u at `at` (taps cycle to it, 450 ms apart: never a double tap), then locks it: held, or until a
  // click (a tap when it is the only guide offered, else a double tap). Logged as `what` with how it locked.
  auto lockOn = [&](const gp_Pnt& at, bool sticky, const QString& what) {
    hover(widget(at));
    const int count = int(m_trackingCandidates.size());
    for (int i = 0; count > 1 && i < count && !isLine(m_trackingCandidates[m_inferenceChoice]); ++i) {
      tap();
      waitFor(450);
      hover(widget(at));
    }
    if (m_trackingCandidates.empty() || !isLine(m_trackingCandidates[m_inferenceChoice]))
      return require(false, QString("%1 (A's guide offered among %2)").arg(what).arg(m_trackingCandidates.size()));
    const QString how = QString("%1 of %2 guides, %3").arg(m_inferenceChoice + 1).arg(m_trackingCandidates.size())
                            .arg(!sticky ? "held" : m_trackingCandidates.size() == 1 ? "a tap" : "a double tap");
    if (!sticky) {
      key(QEvent::KeyPress, Qt::Key_Shift);
      paintEvent(nullptr);
      return require(m_shift.locked() && m_shift.held() && isLine(m_lockedTracking), QString("%1 (%2)").arg(what, how));
    }
    if (m_trackingCandidates.size() == 1) tap();
    else tap(), tap();
    return require(m_shift.sticky() && isLine(m_lockedTracking), QString("%1 (%2)").arg(what, how));
  };

  // (1) A, then its guide locked
  hover(QPointF(6, height() - 6));
  rest(widget(a));
  require(anchorAt(a), "resting on A acquires it");
  lockOn(Q, true, "A's guide locked until a click");
  gp_Pnt at;
  require(shownPoint(at) && gp_Vec(a, at).Crossed(u).Magnitude() < 1e-6 && !m_trackingCross, "the locked point lies on A's line");

  // (2) B acquired while locked: the crossing from B itself
  rest(widget(b));
  require(anchorAt(b) && anchorAt(a) && m_shift.locked() && m_shift.sticky(), "resting on B while locked acquires it and keeps the lock");
  const bool onB = shownPoint(at);
  require(onB && at.Distance(P) < 1e-7 && m_trackingCross && guideFrom(a) && guideFrom(b) && xMarker(),
          QString("pointing at B: the locked point is the crossing (%1 px off), both guides drawn, an X marker").arg(onB ? px(at, P) : -1.0, 0, 'f', 1));
  require(status.contains(QStringLiteral("∩")) && status.contains(tr("%1 from the anchor").arg(units::format(units::Kind::Length, a.Distance(P)))),
          QString("the status names the crossing and the distance along the line: \"%1\"").arg(status));

  // (3) along the line
  hover(widget(along(P, 40)));
  const bool past = shownPoint(at);
  require(past && px(at, P) > 20 && gp_Vec(a, at).Crossed(u).Magnitude() < 1e-6 && !m_trackingCross && guideFrom(a) && !guideFrom(b),
          QString("40 px past the crossing: the projection on the line, one guide (%1 px from P)").arg(px(at, P), 0, 'f', 1));
  hover(widget(along(P, 5)));
  require(shownPoint(at) && at.Distance(P) < 1e-7 && m_trackingCross, "5 px from the crossing along the line: the crossing, exact");
  grabImage().save(prefix + ".cross.png");
  const gp_Pnt middle = b.Translated(gp_Vec(b, P) * 0.5);
  hover(widget(middle));
  require(shownPoint(at) && at.Distance(P) < 1e-7, QString("on B's guide %1 px from the crossing: the crossing").arg(px(middle, P), 0, 'f', 0));

  // (4) the pick
  const size_t picks = selection().size();
  click(widget(middle));
  const auto refs = selection();
  require(refs.size() == picks + 1 && refs.back().kind == opad::Ref::Kind::Point && gp_Pnt(refs.back().point[0], refs.back().point[1], refs.back().point[2]).Distance(P) < 1e-7,
          "a click far from the crossing takes it as the tool's point");
  require(!m_shift.locked(), "the pick ends the lock");

  // (5) Esc
  lockOn(Q, true, "locked again");
  const bool overridden = key(QEvent::ShortcutOverride, Qt::Key_Escape);
  const bool eaten = key(QEvent::KeyPress, Qt::Key_Escape);
  paintEvent(nullptr);
  require(overridden && eaten && !m_shift.locked() && selection().size() == picks + 1, "Esc unlocks before the window's Esc (the tool keeps its pick)");
  require(!key(QEvent::ShortcutOverride, Qt::Key_Escape), "without a lock Esc is the window's");

  // (6) a held lock acquires a third anchor and ends on release
  const Vertex* C = nullptr;
  for (const auto& c : vertices) {
    const gp_Pnt p = crossingOf(a, c.p);
    if (!hoverable[&c - vertices.data()] || c.p.Distance(a) < 1e-7 || c.p.Distance(b) < 1e-7 || px(a, p) < 60 || px(c.p, p) < 40 || px(p, P) < 30 || !inView(p) || !clearOf(p, 20) ||
        across(a, c.p, p) < 30 || !pointVisible(p, A->body) || anchorAt(c.p))
      continue;
    C = &c;
    break;
  }
  if (require(C != nullptr, "a third vertex in sight lining up elsewhere")) {
    const gp_Pnt c = C->p, PC = crossingOf(a, c);
    lockOn(Q, false, "Shift held on A's guide");
    rest(widget(c));
    require(anchorAt(c) && m_shift.held() && m_shift.locked() && shownPoint(at) && at.Distance(PC) < 1e-7 && m_trackingCross,
            "resting on C with Shift held acquires it, the locked point is C's crossing");
    grabImage().save(prefix + ".held.png");
    key(QEvent::KeyRelease, Qt::Key_Shift);
    paintEvent(nullptr);
    require(!m_shift.locked() && !m_shift.sticky(), "releasing the held Shift unlocks");
  }

  // (7) an edge's line met in space and a coordinate plane, both in reach of the pointer: taps show each
  lockOn(Q, true, "locked for the crossings in reach");
  waitFor(450);  // the next taps are no double tap
  const double r = 1 / std::sqrt(2.0), unit = pixelSize();
  const gp_Pnt b1 = P.Translated((u + v) * r * (60 * unit)), p2 = along(P, 6), b2 = p2.Translated(v * (50 * unit));
  m_trackingAnchors = {{a, u, true, A->body}, {b1, (u + v) * r, true, {}}, {b2, {}, false, {}}};
  showTrackingAnchors();
  hover(widget(along(P, 3)));
  gp_Pnt first, second, third;
  const bool both = shownPoint(first) && m_crossings == 2;
  tap();
  shownPoint(second);
  tap();
  shownPoint(third);
  require(both && pointVisible(p2) && m_shift.sticky() && ((first.Distance(P) < 1e-7 && second.Distance(p2) < 1e-7) || (first.Distance(p2) < 1e-7 && second.Distance(P) < 1e-7)) &&
              third.Distance(first) < 1e-7,
          QString("two crossings in reach (%1): an edge direction's line and a coordinate plane, a tap shows the other, another the first").arg(m_crossings));
  hover(widget(along(P, -40)));
  require(m_crossings == 0, "40 px back along the line: no crossing in reach");
  tap();
  require(!m_shift.locked(), "a tap with no crossing in reach unlocks");

  // (8) several guides at the pointer (A's line, another anchor's line crossing it there, their intersection): taps cycle to
  // A's line, a double tap locks it (the guide shown before the first tap), Esc unlocks.
  waitFor(450);
  m_trackingAnchors = {{a, u, true, A->body}, {Q.Translated(v * (40 * unit)), {}, false, {}}};
  showTrackingAnchors();
  hover(widget(Q));
  const size_t offered = m_trackingCandidates.size();
  require(offered > 1, QString("%1 guides at the pointer").arg(offered));
  lockOn(Q, true, "A's line among them locked");
  require(shownPoint(at) && gp_Vec(a, at).Crossed(u).Magnitude() < 1e-6, "the locked point lies on A's line");
  key(QEvent::ShortcutOverride, Qt::Key_Escape);
  key(QEvent::KeyPress, Qt::Key_Escape);
  require(!m_shift.locked(), "Esc unlocks it");

  disconnect(watch);
  clearTracking();
  std::sort(frames.begin(), frames.end());
  require(frames.size() > 10 && frames[frames.size() / 2] < 5 && frames.back() < 50, QString("the tracker over %1 hovers: median %2 ms, worst %3 ms")
                                                                .arg(frames.size()).arg(frames[frames.size() / 2], 0, 'f', 1).arg(frames.back(), 0, 'f', 1));
  return all;
}

// The model once every body is displayed: the Distance tool in vertex mode, then Viewport::benchCrossLock.
OPAD_BENCH(OPAD_BENCH_CROSSLOCK, crosslock) {
  Viewport* v = w.m_viewport;
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(&w);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, v, timer, clock, value] {
    int expected = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) expected += w.m_doc->scene.effectively_visible(id) && !w.m_doc->scene.node(id)->body_missing;
    const bool ready = !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0;
    if (!ready && clock->elapsed() < 60000) return;
    timer->stop();
    timer->deleteLater();
    bool ok = ready;
    if (!ready) trace::log("bench: crosslock: the model is displayed FAIL");
    else {
      w.startTool("distance");
      w.action("select.vertices")->trigger();
      ok = v->benchCrossLock(value);
      w.cancelTool();
    }
    QCoreApplication::exit(ok ? 0 : 2);
  });
  timer->start(50);
  return true;
}
