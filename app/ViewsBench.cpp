// OPAD_BENCH_VIEWS (TODO 11 wave 3, help audit P7): the view commands do what their guides show. On a 3D document: each of
// the seven standard views turns the camera in an animation (a frame partway differs from where it was and from where it
// ends) and ends where the instant move goes, along its axis; in 2D mode Isometric is off and a standard view takes the
// grid to the plane it looks at. On a drawing (opened in 2D mode): Turn 90° left twists it a quarter in an animation, the view
// still looking at its plane and the grid lying in it. <prefix>.top-partway.png, <prefix>.roll.png.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QTimer>

#include <AIS_AnimationCamera.hxx>
#include <Bnd_Box.hxx>

#include <cmath>
#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
// Runs `run` once every body of the document is displayed (or after a minute, reported), then quits with its outcome.
void whenShown(QObject* context, Viewport* v, std::function<bool()> run) {
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(context);
  QObject::connect(timer, &QTimer::timeout, context, [v, timer, clock, run] {
    const bool ready = !v->pumpJob() && v->remainingBodies() == 0 && v->displayedCount() > 0;
    if (!ready && clock->elapsed() < 60000) return;
    timer->stop();
    timer->deleteLater();
    if (!ready) trace::log("bench: views: the model is displayed FAIL");
    QCoreApplication::exit(ready && run() ? 0 : 2);
  });
  timer->start(50);
}
}  // namespace

bool Viewport::benchViews(const QString& prefix, const std::function<void(const QString&)>& trigger, const std::function<bool(const QString&)>& enabled) {
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: views: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!require(m_initialised && !m_items.empty(), "a model is displayed")) return false;
  myViewAnimation->Stop();
  auto direction = [this] { return m_view->Camera()->Direction(); };
  auto gridNormal = [this] { return m_viewer->PrivilegedPlane().Direction(); };
  // The grid OCCT draws (its structure's box) is flat across `normal`: a patch in that plane.
  auto gridFlat = [this](const gp_Dir& normal) {
    grabImage();  // a hidden view lays its 2D grid out for the frame
    const Bnd_Box box = benchGridBox();
    if (box.IsVoid()) return false;
    const gp_Vec extent(box.CornerMin(), box.CornerMax());
    return std::abs(extent.Dot(gp_Vec(normal))) < 0.01 * extent.Magnitude();
  };
  // A command run as on screen (Animate view changes on): whether it started an animation from where the camera was, the
  // camera partway (0.12 s in) and where it ends.
  struct Run {
    bool running = false, startsHere = false;
    ViewState partway, end;
  };
  auto animated = [&](const QString& command, const QString& shot = QString()) {
    Run r;
    const ViewState origin = viewState();
    m_forceAnimate = true;
    trigger(command);
    r.running = !myViewAnimation->IsStopped();
    r.startsHere = viewState().same(origin);
    myViewAnimation->Update(0.12);
    r.partway = viewState();
    if (!shot.isEmpty()) grabImage().save(shot);
    myViewAnimation->Update(10.0);
    myViewAnimation->Stop();
    m_forceAnimate = false;
    r.end = viewState();
    return r;
  };

  if (!m_twoDimensional) {
    // (1) The seven standard views turn the camera, each from another view, and end looking along their axes.
    const std::pair<const char*, gp_Dir> views[] = {{"top", gp_Dir(0, 0, -1)},  {"front", gp_Dir(0, 1, 0)}, {"right", gp_Dir(-1, 0, 0)},
                                                    {"iso", gp_Dir(-1, 1, -1)}, {"bottom", gp_Dir(0, 0, 1)}, {"back", gp_Dir(0, -1, 0)},
                                                    {"left", gp_Dir(1, 0, 0)}};
    for (const auto& [name, looking] : views) {
      const QString view = name;
      standardView(view == "iso" ? "top" : "iso");
      const Handle(Graphic3d_Camera) from = new Graphic3d_Camera(*m_view->Camera());
      const ViewState origin = viewState();
      standardView(view);
      const ViewState instant = viewState();
      m_view->Camera()->Copy(from);
      const Run r = animated("view." + view, view == "top" ? prefix + ".top-partway.png" : QString());
      require(r.running && r.startsHere && !r.partway.same(origin) && !r.partway.same(instant) && r.end.same(instant) && direction().IsEqual(looking, 1e-6),
              QString("view.%1 turns the camera from where it was (partway at 0.12 s) to the instant move's view, along its axis").arg(view));
    }
    // (2) 2D mode: no corner view; a standard view looks at another principal plane, and the grid goes there with it.
    standardView("top");
    trigger("view.2d");
    require(m_twoDimensional && !enabled("view.iso") && enabled("view.top") && enabled("view.rollleft"), "2D mode: Isometric is off, the other views and the turns are on");
    const gp_Dir plan = direction();
    standardView("iso", true);  // asked anyway (an agent, a stale menu)
    require(direction().IsEqual(plan, 1e-9) && myViewAnimation->IsStopped(), "a corner view asked for in 2D mode changes nothing");
    require(gridNormal().IsParallel(plan, 1e-9) && gridFlat(plan), "the plan's grid lies in XY");
    for (const auto& [name, looking] : {std::pair<const char*, gp_Dir>{"front", gp_Dir(0, 1, 0)}, {"right", gp_Dir(-1, 0, 0)}, {"top", gp_Dir(0, 0, -1)}}) {
      const Run r = animated(QString("view.") + name);
      require(r.running && direction().IsEqual(looking, 1e-6) && gridNormal().IsParallel(looking, 1e-9) && gridFlat(looking),
              QString("in 2D mode view.%1 turns to its plane and the grid lies in that plane").arg(name));
    }
    trigger("view.2d");
    require(!m_twoDimensional && enabled("view.iso"), "out of 2D mode Isometric is on again");
    standardView("iso");
    fitAll();
    return all;
  }

  // (3) A drawing in 2D mode: Turn 90° left twists it about the view axis in an animation, the plane kept.
  require(!enabled("view.iso") && enabled("view.rollleft") && enabled("view.rollright"), "a drawing opens in 2D mode: Isometric off, the turns on");
  const gp_Dir flat = direction(), up = m_view->Camera()->Up();
  require(gridNormal().IsParallel(flat, 1e-9) && gridFlat(flat), "the drawing's grid lies in its plane");
  const gp_Dir turned = up.Rotated(gp_Ax1(gp::Origin(), flat), M_PI / 2);  // counter-clockwise on screen
  const Run left = animated("view.rollleft", prefix + ".roll-partway.png");
  const gp_Dir upPartway(left.partway.up[0], left.partway.up[1], left.partway.up[2]), upEnd(left.end.up[0], left.end.up[1], left.end.up[2]);
  require(left.running && left.startsHere && !upPartway.IsEqual(up, 1e-3) && !upPartway.IsEqual(turned, 1e-3) && upEnd.IsEqual(turned, 1e-6) && direction().IsEqual(flat, 1e-9),
          QString("Turn 90° left twists the drawing a quarter counter-clockwise in an animation, the view still on its plane (twist %1°)").arg(twistAngle()));
  grabImage().save(prefix + ".roll.png");
  require(gridNormal().IsParallel(flat, 1e-9) && gridFlat(flat), "the grid stays in the drawing's plane");
  const Run right = animated("view.rollright");
  require(right.running && std::abs(twistAngle()) < 1e-6 && direction().IsEqual(flat, 1e-9), "Turn 90° right turns it back");
  return all;
}

// The viewport's part through the window's commands.
OPAD_BENCH(OPAD_BENCH_VIEWS, views) {
  Viewport* v = w.m_viewport;
  whenShown(&w, v, [&w, v, value] {
    return v->benchViews(value, [&w](const QString& id) { w.action(id)->trigger(); }, [&w](const QString& id) { return w.action(id)->isEnabled(); });
  });
  return true;
}
