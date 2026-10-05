// OPAD_BENCH_CAVITYZOOM=<prefix> or 1 (case cavity-zoom in tools/bench_cases/viewer.py): the report "camera zoom as I am
// inside the part (imagine trying to select the upper face of a slot in a box with its upper face removed)". The document
// is an open box (60 x 40 x 30, walls 5) with a pocket in its floor and a slot through its back wall. The wheel zooms at a
// face notch by notch, the pointer kept on it, and after every notch:
//   - it got closer (perspective: the eye moved towards the face by at least 5 % of the distance; orthographic: the scale),
//   - the face stays under the pointer (perspective: the very point, to the pixel), inside the camera's depth range (not
//     clipped by the near plane), and in the Faces filter it is what a click there picks.
// Perspective from above the box at the pocket's floor ends inside the box, close to the floor, and as many notches out
// come back out; the same in the Edges and Vertices filters and with bodies unpickable (a sketch), where the selection's picker finds
// nothing on the face; from inside the box at the slot's upper face (facing down: seen from inside only) it ends close to
// it; from inside, at the slot's opening (nothing under the pointer), every notch flies on and the eye leaves the box
// through the slot; orthographic at the pocket's floor zooms on as steadily. <prefix>.<run>.png: each run's last frame.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointingDevice>
#include <QTimer>
#include <QWheelEvent>

#include <BRepAdaptor_Surface.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <functional>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"

bool Viewport::benchCavityZoom(const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: cavity-zoom: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!m_initialised || m_items.empty()) return require(false, "a body is displayed");
  const std::string body = m_items.begin()->first;
  const TopoDS_Shape shape = m_items.begin()->second.ais->Shape().Moved(TopLoc_Location(m_items.begin()->second.ais->LocalTransformation()));
  // The faces by where they are: the pocket's floor (z = 2, facing up) and the slot's upper face (z = 18, facing down).
  auto faceAt = [&shape](double z, double nz) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const TopoDS_Face& f = TopoDS::Face(faces(i));
      BRepAdaptor_Surface s(f);
      if (s.GetType() != GeomAbs_Plane || std::abs(s.Plane().Location().Z() - z) > 1e-6) continue;
      const double n = s.Plane().Axis().Direction().Z() * (f.Orientation() == TopAbs_REVERSED ? -1 : 1);
      if (std::abs(n - nz) < 1e-6) return i - 1;
    }
    return -1;
  };
  const int pocket = faceAt(2, 1), slot = faceAt(18, -1);
  if (!require(pocket >= 0 && slot >= 0, QString("the pocket's floor (%1) and the slot's upper face (%2) are found").arg(pocket).arg(slot))) return false;

  auto wait = [](const std::function<bool()>& done) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < 10000) {
      QEventLoop loop;
      QTimer::singleShot(10, &loop, &QEventLoop::quit);
      loop.exec();
    }
  };
  auto useFilter = [&](SelFilter f) {  // applied in a sliced job
    setSelectionFilter(f);
    wait([this] { return !m_filterJob; });
  };
  const SelFilter filter = m_filter;
  QPointingDevice mouse("bench mouse", 23, QInputDevice::DeviceType::Mouse, QPointingDevice::PointerType::Generic,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Scroll, 1, 3);
  auto notch = [&](const QPointF& at, int notches) {
    QWheelEvent event(at, mapToGlobal(at), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false,
                      Qt::MouseEventNotSynthesized, &mouse);
    QCoreApplication::sendEvent(this, &event);
    paintEvent(nullptr);  // a hidden window never paints: the frame's flush applies the queued zoom
    m_view->Redraw();     // and its z range (AutoZFit), which the picker clips to
  };
  auto faceUnder = [&](const QPointF& at) {  // what a click there picks
    opad::Ref r;
    if (!referenceAt(at, r) || r.body != body || r.kind != opad::Ref::Kind::Face) return -1;
    return r.index;
  };
  auto drawnUnder = [&](const QPointF& at, gp_Pnt& p) { return navigationPoint(devicePos(at), p); };  // whatever picks
  auto depthOf = [&](const gp_Pnt& p) { return gp_Vec(m_view->Camera()->Eye(), p).Dot(gp_Vec(m_view->Camera()->Direction())); };
  auto unclipped = [&](const gp_Pnt& p) {
    const auto c = m_view->Camera();
    const double d = depthOf(p);
    return d > c->ZNear() && d < c->ZFar();
  };
  auto str = [](const gp_Pnt& p) { return QString("%1, %2, %3").arg(p.X(), 0, 'f', 2).arg(p.Y(), 0, 'f', 2).arg(p.Z(), 0, 'f', 2); };

  struct Run {
    QString name;
    bool perspective;
    SelFilter filter;
    bool pickable;  // false: bodies unpickable, as in a sketch
    gp_Pnt eye, look, target;
    int face;       // -1: an opening (nothing under the pointer)
    double faceZ;
    int notches;
    double reach;   // perspective: how close to the face the eye gets (mm)
  };
  const gp_Pnt above(0, -50, 150), middle(0, 0, 15), floorPoint(3, 1, 2), inside(0, -8, 7), slotPoint(2, 17, 18);
  const std::vector<Run> runs = {
      // From above the front wall, steeply down through the open top at the pocket's floor.
      {"perspective into the box", true, SelFilter::Face, true, above, middle, floorPoint, pocket, 2, 60, 1.0},
      {"perspective into the box, Edges filter", true, SelFilter::Edge, true, above, middle, floorPoint, pocket, 2, 60, 1.0},
      {"perspective into the box, Vertices filter", true, SelFilter::Vertex, true, above, middle, floorPoint, pocket, 2, 60, 1.0},
      {"perspective into the box, bodies unpickable", true, SelFilter::Face, false, above, middle, floorPoint, pocket, 2, 60, 1.0},
      // From inside the box, low, up at the slot's upper face in the back wall.
      {"perspective inside the box", true, SelFilter::Face, true, inside, gp_Pnt(0, 10, 15), slotPoint, slot, 18, 60, 0.5},
      // From inside, level, at the middle of the slot's opening: nothing behind it.
      {"perspective through the slot", true, SelFilter::Face, true, gp_Pnt(0, -8, 14), gp_Pnt(0, 10, 14), gp_Pnt(0, 15, 14), -1, 0, 80, 0},
      {"orthographic", false, SelFilter::Face, true, above, middle, floorPoint, pocket, 2, 40, 0},
  };
  const Handle(Graphic3d_Camera) saved = new Graphic3d_Camera(*m_view->Camera());
  m_needFit = false;
  for (const Run& run : runs) {
    useFilter(run.filter);
    setBodiesPickable(run.pickable);
    wait([this] { return !m_filterJob; });  // made pickable again: re-activated in a sliced job
    const auto camera = m_view->Camera();
    camera->SetProjectionType(run.perspective ? Graphic3d_Camera::Projection_Perspective : Graphic3d_Camera::Projection_Orthographic);
    camera->SetEyeAndCenter(run.eye, run.look);
    camera->SetUp(gp::DZ());
    camera->OrthogonalizeUp();
    if (!run.perspective) camera->SetScale(120);
    m_view->Redraw();
    const QPointF at = widgetPoint({run.target.X(), run.target.Y(), run.target.Z()});
    const bool picks = run.filter == SelFilter::Face && run.pickable;  // a click there picks the face
    // The point drawn under the pointer's pixel: the one a zoom at the pointer keeps there.
    gp_Pnt target = run.target;
    const bool drawn = drawnUnder(at, target);
    auto onFace = [&](const QPointF& point) {
      gp_Pnt p;
      return drawnUnder(point, p) && std::abs(p.Z() - run.faceZ) < 1e-3 && (!picks || faceUnder(point) == run.face);
    };
    if (run.face < 0) {
      if (!require(!drawn, run.name + ": nothing is drawn under the pointer before zooming")) continue;
    } else if (!require(drawn && target.Distance(run.target) < 2 && onFace(at) && unclipped(target), run.name + ": the face is under the pointer before zooming")) {
      continue;
    }
    const gp_Ax1 ray(m_view->Camera()->Eye(), gp_Dir(gp_Vec(m_view->Camera()->Eye(), target)));
    QString failure;
    int steps = 0;
    double least = 1e300;
    for (; steps < run.notches; ++steps) {
      const auto c = m_view->Camera();
      const gp_Pnt eye = c->Eye();
      const double distance = eye.Distance(target), scale = c->Scale();
      if (run.perspective && run.face >= 0 && distance <= run.reach) break;
      if (run.face < 0 && eye.Y() > 22) break;  // out through the slot
      notch(at, 1);
      const gp_Pnt now = c->Eye();
      // Progress: perspective towards a face, a share of the distance; through the opening, the eye's advance along the
      // pointer's ray (a floor: never a crawl); orthographic, the scale.
      const double progress = !run.perspective ? 1 - c->Scale() / scale : run.face >= 0 ? 1 - now.Distance(target) / distance
                                                                                            : gp_Vec(eye, now).Dot(gp_Vec(ray.Direction()));
      least = std::min(least, progress);
      const QPoint shown = widgetPoint({target.X(), target.Y(), target.Z()});
      const QString state = QString("notch %1: eye %2 -> %3, %4 -> %5 mm from the target, scale %6 -> %7, near %8 far %9 depth %10, target at %11,%12 px")
                                .arg(steps + 1).arg(str(eye), str(now)).arg(distance, 0, 'g', 4).arg(now.Distance(target), 0, 'g', 4)
                                .arg(scale, 0, 'g', 4).arg(c->Scale(), 0, 'g', 4).arg(c->ZNear(), 0, 'g', 4).arg(c->ZFar(), 0, 'g', 4)
                                .arg(depthOf(target), 0, 'g', 4).arg(shown.x() - at.x()).arg(shown.y() - at.y());
      if (run.face < 0) {
        if (progress < 0.3) failure = "stalled in the opening: " + state;
        else if (gp_Lin(ray).Distance(now) > 1e-4) failure = "the eye left the pointer's ray: " + state;
      } else if (progress < 0.05) {
        failure = "stalled: " + state;
      } else if (!unclipped(target)) {
        failure = "the face is clipped: " + state;
      } else if (!onFace(at)) {
        failure = "the face is not under the pointer: " + state;
      } else if (run.perspective && (std::abs(shown.x() - at.x()) > 1 || std::abs(shown.y() - at.y()) > 1)) {
        failure = "the face slid from under the pointer: " + state;
      }
      if (!failure.isEmpty()) break;
    }
    if (!prefix.isEmpty() && prefix != "1") grabImage().save(prefix + "." + QString(run.name).remove(',').replace(' ', '-') + ".png");
    const gp_Pnt e = m_view->Camera()->Eye();
    if (run.face < 0) {
      require(failure.isEmpty() && e.Y() > 22 && std::abs(e.X()) < 10 && e.Z() > 10 && e.Z() < 18,
              run.name + QString(": %1 notches, each at least %2 mm along the pointer's ray, out through the slot at %3").arg(steps).arg(least, 0, 'f', 2).arg(str(e)) +
                  (failure.isEmpty() ? QString() : " - " + failure));
      continue;
    }
    const QString share = run.perspective ? QString("at least %1% closer").arg(int(std::floor(least * 100))) : QString("the scale at least %1% smaller").arg(int(std::floor(least * 100)));
    require(failure.isEmpty(), run.name + QString(": %1 notches, each %2, the face kept under the pointer, unclipped").arg(steps).arg(share) +
                                   (picks ? QString(" and picked there") : QString()) + (failure.isEmpty() ? QString() : " - " + failure));
    if (!run.perspective || !failure.isEmpty()) continue;
    require(e.Distance(target) <= run.reach, run.name + QString(": the eye reached %1 mm of the face (wanted %2)").arg(e.Distance(target), 0, 'g', 3).arg(run.reach));
    if (run.face == slot) {
      // On at the face reached: the steps keep their floor and the eye goes through it (into the wall) within a few notches.
      int more = 0;
      double step = 1e300;
      for (; more < 20 && m_view->Camera()->Eye().Z() <= 18; ++more) {
        const gp_Pnt eye = m_view->Camera()->Eye();
        notch(at, 1);
        step = std::min(step, eye.Distance(m_view->Camera()->Eye()));
      }
      require(m_view->Camera()->Eye().Z() > 18 && step > 0.01,
              run.name + QString(": on at the face, the eye goes through it in %1 notches of at least %2 mm").arg(more).arg(step, 0, 'f', 3));
    }
    if (run.face == pocket) {
      require(e.Z() < 30 && std::abs(e.X()) < 25 && std::abs(e.Y()) < 15, run.name + ": the eye went in through the open top: " + str(e));
      // Out again: as many notches the other way end above the box once more, the face still under the pointer.
      for (int i = 0; i < steps; ++i) notch(at, -1);
      const gp_Pnt back = m_view->Camera()->Eye();
      require(back.Z() > 30 && onFace(at), run.name + ": zooming out comes back out of the box: " + str(back));
    }
  }
  setBodiesPickable(true);
  m_view->SetCamera(saved);
  m_view->Redraw();
  useFilter(filter);
  return all;
}

namespace {
bool settle(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_CAVITYZOOM, cavityzoom) {
  Viewport* v = w.m_viewport;
  const bool shown = settle([&w, v] { return !w.m_loadJob && v->displayedCount() > 0 && v->remainingBodies() == 0; }, 120000);
  if (!shown) trace::log("bench: cavity-zoom: the box is displayed FAIL");
  const bool ok = shown && v->benchCavityZoom(value);
  trace::log(QString("bench: cavity-zoom: %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
