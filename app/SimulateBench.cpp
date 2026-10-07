// The Simulate workspace as a user drives it (OPAD_BENCH_SIMULATE=<prefix>): the workspace from its command, a joint
// made from a picked circular edge with the panel's Add, the slider moving the mechanism, a dynamic study from the
// ribbon played back and checked against the pendulum it is (it swings to the other side: energy kept), a motion study,
// then a cantilever's fixed support and end force on picked faces, its static study's result map with its legend
// (tip deflection against F L^3 / 3 E I) and its vibration modes (first one against the cantilever formula).
// Screenshots of the view and the panel at <prefix>.*.png.
#include <BRepGProp.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QToolButton>

#include <cmath>
#include <functional>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SimPlot.hpp"
#include "SimulateArea.hpp"
#include "SimulatePanel.hpp"
#include "ToolPanel.hpp"
#include "Tracking.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/sim/fea.hpp"

namespace {
struct Step {
  std::function<bool()> ready;
  std::function<void(bool)> act;
  int ms = 60000;
};

void runSteps(QObject* context, std::shared_ptr<std::vector<Step>> steps, size_t i, std::function<void()> finish) {
  if (i >= steps->size()) return finish();
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [=] {
    const Step& step = (*steps)[i];
    const bool ok = !step.ready || step.ready();
    if (!ok && clock->elapsed() < step.ms) return;
    timer->stop();
    timer->deleteLater();
    step.act(ok);
    runSteps(context, steps, i + 1, finish);
  });
  timer->start(50);
}

// The sub-shape of a body that `match` picks, as the view would give it (its ref).
std::optional<opad::Ref> pick(AppDocument* doc, const std::string& body, opad::Ref::Kind kind, const std::function<bool(const TopoDS_Shape&)>& match) {
  const TopoDS_Shape shape = opad::node_world_shape(doc->doc, doc->scene, body);
  for (int i = 0; i < 10000; ++i) {
    TopoDS_Shape sub;
    try {
      sub = opad::subshape(shape, kind, i);
    } catch (const std::exception&) {
      break;
    }
    if (match(sub)) return opad::Ref{body, kind, i, {0, 0, 0}};
  }
  return std::nullopt;
}

gp_Pnt centre(const TopoDS_Shape& face) {
  GProp_GProps g;
  BRepGProp::SurfaceProperties(face, g);
  return g.CentreOfMass();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_SIMULATE, simulate) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: simulate: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Simulate* area = w.findChild<Simulate*>();
  if (!area) {
    require(false, "the Simulate area is registered");
    QCoreApplication::exit(2);
    return true;
  }
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  const QString prefix = value;
  AreaServices* services = &area->services();
  auto action = [services](const char* id) { return services->action(id); };
  // A pick: through the view as a click selects; a sub-shape the view's selection modes leave out (an edge outside a
  // tool) reaches the window the way the view hands a pick over (selectionMoved), as the area reads it.
  auto pickRefs = [services, &w](const std::vector<opad::Ref>& refs) {
    services->select(refs);
    if (services->selection().refs.size() != refs.size()) {
      trace::log("bench: simulate: the view kept no sub-shape pick: handed to the window as picked");
      w.selectionMoved(refs);
    }
  };
  // Settled: nothing displaying, every body on screen, and so for 400 ms (a change just made has not started its display yet).
  auto since = std::make_shared<QElapsedTimer>();
  auto visible = [doc] {
    int n = 0;
    for (const auto& id : doc->scene.all_bodies()) n += doc->scene.effectively_visible(id);
    return n;
  };
  auto idle = [v, &w, since, visible] {
    const bool now = !v->looksPending() && !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() >= visible();
    if (!now) {
      since->invalidate();
      return false;
    }
    if (!since->isValid()) since->start();
    return since->elapsed() > 400;
  };
  // The view with what floats over it painted in (the legend is a native child the view's grab leaves out), and the panel
  // scrolled to its chart or result rows.
  auto shot = [v, area, prefix](const QString& name) {
    QImage view = v->grabImage();
    if (auto* legend = v->findChild<QWidget*>("simLegend"); legend && legend->isVisible()) {
      QPainter p(&view);
      const qreal dpr = view.devicePixelRatio() > 0 ? view.devicePixelRatio() : 1;
      p.drawPixmap(QPointF(legend->x() * dpr / view.devicePixelRatio(), legend->y()), legend->grab());
    }
    view.save(prefix + "." + name + ".view.png");
    if (area->panel()->isVisible()) {
      if (auto* scroll = area->form()->findChild<QScrollArea*>(); scroll && area->form()->plot()->isVisible()) scroll->ensureWidgetVisible(area->form()->plot());
      area->panel()->grab().save(prefix + "." + name + ".panel.png");
    }
  };
  auto state = std::make_shared<std::map<std::string, std::string>>();
  auto steps = std::make_shared<std::vector<Step>>();
  auto& list = *steps;
  constexpr double kPi = 3.14159265358979323846;

  // ---- the workspace, and a pendulum: a 120 x 12 x 12 arm lying along X with a 6 mm pivot hole across it at x = 0
  list.push_back({nullptr, [=, &w](bool) {
                    QAction* ws = action("workspace.simulate");
                    require(ws != nullptr, "the Simulate workspace has its command");
                    if (ws) ws->trigger();
                    require(services->workspace() == "simulate", "the command switches to the Simulate workspace");
                    const opad::json arm = doc->run("feature", {{"kind", "box"}, {"name", "Pendulum"},
                                                                {"inputs", {{"plane", {{"origin", {50, 0, 0}}, {"normal", {0, 0, 1}}}}, {"length", 120}, {"width", 12}, {"height", 12}}}});
                    (*state)["arm"] = arm["body_ids"][0].get<std::string>();
                    doc->run("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"origin", {0, -10, 6}}, {"normal", {0, 1, 0}}}}, {"diameter", 6}, {"height", 20},
                                                                           {"operation", "cut"}, {"targets", {(*state)["arm"]}}}}});
                    action("simulate.panel")->trigger();
                    require(area->panel()->isVisible(), "Simulation panel opens");
                  }});
  // ---- a revolute joint from the pivot hole's rim, picked, with the panel's Add
  list.push_back({idle, [=](bool) {
                    const auto rim = pick(doc, (*state)["arm"], opad::Ref::Kind::Edge, [](const TopoDS_Shape& e) {
                      BRepAdaptor_Curve c(TopoDS::Edge(e));
                      return c.GetType() == GeomAbs_Circle && std::fabs(c.Circle().Radius() - 3) < 1e-6 && c.Circle().Location().Y() > 5;
                    });
                    require(bool(rim), "the pivot hole's rim is found to pick");
                    if (!rim) return;
                    pickRefs({*rim});
                    const SelectionContext picked = services->selection();
                    require(picked.refs.size() == 1 && picked.refs[0].kind == opad::Ref::Kind::Edge, QString("the rim is the selection (%1 refs)").arg(picked.refs.size()));
                    auto* kind = area->form()->findChild<QComboBox*>("simJointKind");
                    kind->setCurrentIndex(kind->findData("revolute"));
                    area->form()->findChild<QPushButton*>("simAddJoint")->click();
                    const auto& joints = doc->scene.joints;
                    require(joints.size() == 1 && joints[0].kind == "revolute" && joints[0].part == (*state)["arm"] && joints[0].base.empty(),
                            "Add makes a revolute joint of the arm to the world at the picked rim");
                    if (!joints.empty()) {
                      (*state)["joint"] = joints[0].id;
                      const opad::Frame& f = joints[0].at_part;
                      require(std::fabs(std::fabs(f.normal()[1]) - 1) < 1e-9, "its axis is the hole's (along Y)");
                    }
                  }});
  // ---- the slider moves the mechanism: a preview while dragged, written when let go
  list.push_back({idle, [=](bool) {
                    auto* slider = area->form()->findChild<QSlider*>("simJointSlider");
                    require(slider && slider->isEnabled(), "the joint's slider is enabled");
                    if (!slider) return;
                    slider->setSliderDown(true);
                    slider->setValue(slider->minimum() + (slider->maximum() - slider->minimum()) * 9 / 16);  // 45 deg of -360..360
                    require(v->previewMovedCount() > 0, "dragging the slider previews the arm turning");
                    slider->setSliderDown(false);
                    QMetaObject::invokeMethod(slider, "sliderReleased");
                    const opad::Joint* j = doc->scene.joint((*state)["joint"]);
                    require(j && std::fabs(j->values[0] - 45) < 0.5, QString("letting go writes the joint at 45 deg (got %1)").arg(j ? j->values[0] : -1));
                    doc->run("joint_set", {{"values", {{(*state)["joint"], {0}}}}});  // back to horizontal for the drop
                  }});
  // ---- a dynamic study from the ribbon: the arm falls from horizontal and swings to the other side
  list.push_back({idle, [=](bool) { action("simulate.dynamic")->trigger(); }});
  list.push_back({[area] { return area->shownRun() && !area->running() && area->shownRun()->kind == "dynamic"; },
                  [=](bool ok) {
                    require(ok, "the dynamic study runs from the ribbon");
                    if (!ok) return;
                    const auto run = area->shownRun();
                    require(area->playing(), "it plays when it finishes");
                    double swing = 0;
                    for (const auto& s : run->series)
                      if (s.group == "value" && s.name.find("rotation") != std::string::npos)
                        for (double x : s.v) swing = std::max(swing, std::fabs(x));
                    require(std::fabs(swing - 180) < 3, QString("the arm swings to the other side (energy kept): %1 deg of 180").arg(swing));
                    require(area->form()->plot()->curves() == 1, "the chart shows a series");
                  },
                  180000});
  list.push_back({[area] { return area->frame() > 20; }, [=](bool ok) {
                    require(ok, "playback advances the frames");
                    require(v->previewMovedCount() > 0, "the arm is shown where the frame has it");
                    shot("dynamic");
                  }});
  // ---- a motion study: the joint driven a full turn
  list.push_back({nullptr, [=](bool) { action("simulate.motion")->trigger(); }});
  list.push_back({[area] { return area->shownRun() && !area->running() && area->shownRun()->kind == "motion"; },
                  [=](bool ok) {
                    require(ok, "the motion study runs from the ribbon");
                    if (ok) require(area->shownRun()->t.size() == 121, "with its 121 frames");
                  },
                  120000});
  list.push_back({[area] { return area->frame() > 30; }, [=](bool ok) {
                    require(ok, "the motion plays");
                    shot("motion");
                  }});
  // ---- a cantilever 200 x 20 x 20: fixed at x = 100, 1000 N down at x = 300 (picked faces), static and modal studies
  list.push_back({nullptr, [=](bool) {
                    area->panel()->hide();
                    const opad::json beam = doc->run("feature", {{"kind", "box"}, {"name", "Cantilever"},
                                                                 {"inputs", {{"plane", {{"origin", {200, 100, 0}}, {"normal", {0, 0, 1}}}}, {"length", 200}, {"width", 20}, {"height", 20}}}});
                    (*state)["beam"] = beam["body_ids"][0].get<std::string>();
                    doc->run("part_properties", {{"targets", {(*state)["beam"]}}, {"set", {{"material", "steel"}}}});
                    doc->run("appearance", {{"target", (*state)["arm"]}, {"visible", false}});  // the beam alone in the view
                  }});
  list.push_back({idle, [=](bool) {
                    if (QAction* fit = action("view.fit")) fit->trigger();
                  }});
  list.push_back({idle, [=, &w](bool) {
                    auto face_at = [&](double x) {
                      return pick(doc, (*state)["beam"], opad::Ref::Kind::Face, [x](const TopoDS_Shape& f) {
                        return BRepAdaptor_Surface(TopoDS::Face(f)).GetType() == GeomAbs_Plane && std::fabs(centre(f).X() - x) < 1e-6;
                      });
                    };
                    const auto root = face_at(100), tip = face_at(300);
                    require(root && tip, "the beam's end faces are found to pick");
                    if (!root || !tip) return;
                    pickRefs({*root});
                    action("simulate.fixed")->trigger();
                    pickRefs({*tip});
                    // The force's dialog, answered as a user would: the default 0, 0, -1000 N.
                    auto* answer = new QTimer(&w);
                    QObject::connect(answer, &QTimer::timeout, answer, [answer] {
                      if (auto* d = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); d && d->isVisible()) {
                        trace::log("bench: simulate: the force's dialog answered with " + d->textValue());
                        d->accept();
                        answer->deleteLater();
                      }
                    });
                    answer->start(100);
                    action("simulate.force")->trigger();
                    QStringList kinds;
                    for (const auto& l : doc->scene.loads) kinds << QString::fromStdString(l.kind);
                    require(doc->scene.loads.size() == 2 && doc->scene.loads[0].kind == "fixed" && doc->scene.loads[1].kind == "force",
                            "Fixed support and Force are added on the picked faces (" + kinds.join(", ") + ")");
                    action("simulate.static")->trigger();
                  }});
  list.push_back({[area] { return area->shownRun() && !area->running() && area->shownRun()->kind == "static" && area->resultShown(); },
                  [=](bool ok) {
                    require(ok, "the static study runs and shows its result map");
                    if (!ok) return;
                    const opad::json s = area->shownRun()->summary;
                    // Tip deflection: F L^3 / 3 E I plus shear F L / (k G A), k = 5/6.
                    const double E = 210000, I = std::pow(20.0, 4) / 12, G = E / (2 * 1.3), A = 400, L = 200, F = 1000;
                    const double want = F * L * L * L / (3 * E * I) + F * L / (5.0 / 6 * G * A);
                    const double got = s.value("max_displacement_mm", 0.0);
                    require(std::fabs(got - want) / want < 0.05, QString("tip deflection %1 mm against beam theory %2 mm").arg(got).arg(want));
                    auto* legend = v->findChild<QWidget*>("simLegend");
                    require(legend && legend->isVisible(), "the legend shows over the view");
                    shot("static");
                  },
                  600000});
  list.push_back({nullptr, [=](bool) { action("simulate.modal")->trigger(); }});
  list.push_back({[area] { return area->shownRun() && !area->running() && area->shownRun()->kind == "modal" && area->resultShown(); },
                  [=](bool ok) {
                    require(ok, "the modal study runs and shows its first mode");
                    if (!ok) return;
                    const auto& f = area->shownRun()->fea->frequencies;
                    const double E = 210000, I = std::pow(20.0, 4) / 12, rho = 7.85e-9, A = 400, L = 200;
                    const double want = 1.875104 * 1.875104 / (2 * kPi) * std::sqrt(E * I / (rho * A * L * L * L * L));
                    require(!f.empty() && std::fabs(f[0] - want) / want < 0.05, QString("first frequency %1 Hz against the cantilever formula %2 Hz").arg(f.empty() ? 0 : f[0]).arg(want));
                    shot("modal");
                  },
                  600000});
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
