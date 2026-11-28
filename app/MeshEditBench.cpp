// OPAD_BENCH_MESHEDIT=<prefix> on a document of mesh bodies (tools/bench_cases/mesh_edit.py: a 1.2M-triangle tube embedded,
// a box mesh through its wall, a small box mesh with its top missing, floating above the bed): the print-prep tools on
// meshes through their panels, each previewed and committed, with the UI watchdog's longest stall checked per step
// (250 ms, CLAUDE.md "nothing long runs on the UI thread"): Scale per axis about the bed, Combine (a mesh cut), Split by a
// plane (two capped halves), Repair mesh (its report in the panel), Simplify mesh, Drop to bed and Lay flat (Align with
// the face picker open). "bench: mesh edit: <step> ... PASS/FAIL".
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QTimer>

#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh_edit.hpp"

namespace {
constexpr qint64 kStallLimitMs = 250;
}  // namespace

OPAD_BENCH(OPAD_BENCH_MESHEDIT, meshEdit) {
  struct State {
    int phase = 0, ticks = 0, settled = 0;
    bool all = true;
    std::string tube, cutter, broken, half;
    size_t tubeTriangles = 0;
    QString step;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  const QString prefix = value;
  auto* timer = new QTimer(&w);
  timer->setInterval(20);
  auto tick = std::make_shared<std::function<void()>>();
  QObject::connect(timer, &QTimer::timeout, &w, [st, timer, tick] {
    try {
      (*tick)();
    } catch (const std::exception& e) {  // a bench step that throws fails the case, never the app
      trace::log(QString("bench: mesh edit: step %1 threw: %2 FAIL").arg(st->phase).arg(e.what()));
      timer->stop();
      QCoreApplication::exit(2);
    }
  });
  *tick = [&w, st, timer, prefix] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    DesignController* design = w.m_design;
    FeaturePanel* form = design->featurePanel();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: mesh edit: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    auto begin = [st](const QString& step) {
      st->step = step;
      trace::resetStalls();
      st->clock.start();
    };
    auto end = [st, require](const QString& extra = {}) {
      const trace::Stalls s = trace::stalls();
      require(s.longest < kStallLimitMs, QStringLiteral("%1: %2 ms, longest stall %3 ms (%4 stalls)%5")
                                             .arg(st->step).arg(st->clock.elapsed()).arg(s.longest).arg(s.count).arg(extra.isEmpty() ? QString() : ", " + extra));
    };
    auto world = [doc](const std::string& id) { return opad::meshops::from_shape(opad::node_world_shape(doc->doc, doc->scene, id)); };
    auto box = [&](const std::string& id) {
      double e[6];
      opad::meshops::bounds(world(id)).Get(e[0], e[1], e[2], e[3], e[4], e[5]);
      return std::array<double, 6>{e[0], e[1], e[2], e[3], e[4], e[5]};
    };
    auto commit = [&w, view] {
      QApplication::setActiveWindow(&w);
      QWidget* to = QApplication::focusWidget();
      QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
      QApplication::sendEvent(to ? to : view, &press);
    };
    auto refs = [](const std::vector<std::string>& ids) {
      opad::json a = opad::json::array();
      for (const auto& id : ids) a.push_back({{"body", id}, {"kind", "body"}});
      return a;
    };
    if (++st->ticks > 9000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->step));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || design->previewing();
    // A feature started, previewed (its bodies drawn), committed: phases p (start), p+1 (preview), p+2 (committed).
    auto feature = [&](int p, const QString& name, const QString& kind, const std::vector<std::pair<QString, opad::json>>& given,
                       const std::function<void()>& check, const std::function<void()>& previewed = {}) {
      if (st->phase == p) {
        if (busy) return;
        begin(name + " started");
        design->startFeature(kind, given);
        st->phase = p + 1;
      } else if (st->phase == p + 1) {
        // Bodies drawn, or (Drop to bed: placed as they are) drawn moved.
        const bool shown = view->previewShown() || view->previewMovedCount() > 0;
        if ((busy || !design->featureActive() || !shown) && st->clock.elapsed() < 120000) return;
        end(QStringLiteral("previewed, %1 drawn, %2 moved").arg(view->previewBodyCount()).arg(view->previewMovedCount()));
        if (!require(shown, name + " shows its preview")) return finish();
        if (previewed) previewed();
        form->window()->grab().save(prefix + "." + kind + ".png");
        begin(name + " committed");
        commit();
        st->phase = p + 2;
      } else if (st->phase == p + 2) {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 120000) return;
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        end(QStringLiteral("step %1").arg(f ? QString::fromStdString(f->kind) : QStringLiteral("none")));
        if (!require(f && f->kind == kind.toStdString() && f->error.empty(), name + " is a step that worked" + (f ? " " + QString::fromStdString(f->error) : QString())))
          return finish();
        st->phase = p + 3;
        check();  // which moves on to the next tool's phase
      }
    };

    switch (st->phase) {
      case 0: {  // loaded and drawn: the tube is the mesh with the most triangles, the cutter the box through it
        if (busy) return;
        if (++st->settled < 25) return;
        for (const auto& id : doc->scene.all_bodies()) {
          const opad::Node* n = doc->node(id);
          if (!n || n->representation != "mesh") continue;
          const size_t t = world(id).triangles();
          const opad::Node* file = doc->node(n->parent);  // an STL's body is "Mesh", in a component named after its file
          const std::string name = file ? file->name : n->name;
          if (name.find("tube") != std::string::npos) st->tube = id, st->tubeTriangles = t;
          else if (name.find("cutter") != std::string::npos) st->cutter = id;
          else if (name.find("broken") != std::string::npos) st->broken = id;
        }
        if (!require(!st->tube.empty() && !st->cutter.empty() && !st->broken.empty() && st->tubeTriangles > 1000000,
                     QStringLiteral("the tube (%1 triangles), the cutter and the broken box are mesh bodies").arg(st->tubeTriangles)))
          return finish();
        st->phase = 10;
        return;
      }
      default:
        break;
    }
    if (st->phase >= 10 && st->phase < 20)
      return feature(10, "Scale per axis about the bed", "scale",
                     {{"bodies", refs({st->tube})}, {"mode", "axes"}, {"factor_x", "1.1"}, {"factor_y", "1"}, {"factor_z", "0.9"}, {"about", "bed"}}, [&] {
                       const auto e = box(st->tube);
                       require(std::fabs(e[5] - 108) < 0.05 && std::fabs(e[2]) < 1e-6, QStringLiteral("the tube is 108 mm high, still on the bed (z %1 .. %2)").arg(e[2]).arg(e[5]));
                       require(doc->node(st->tube)->representation == "mesh", "the tube is still a mesh");
                       st->phase = 20;
                     });
    if (st->phase >= 20 && st->phase < 30)
      return feature(20, "Combine: the cutter cut out of the tube", "combine",
                     {{"target", refs({st->tube})}, {"tools", refs({st->cutter})}, {"operation", "cut"}}, [&] {
                       require(!doc->node(st->cutter), "the cutter is used up");
                       require(opad::meshops::check(world(st->tube), {false, 0}).closed(), "the cut tube is closed");
                       st->phase = 30;
                     });
    if (st->phase >= 30 && st->phase < 40)
      return feature(30, "Split by the XZ plane", "split", {{"bodies", refs({st->tube})}, {"plane", {{"base", "xz"}}}}, [&] {
                       for (const auto& id : doc->scene.all_bodies())
                         if (id != st->tube && id != st->broken) st->half = id;
                       require(!st->half.empty(), "the tube became two bodies");
                       require(!st->half.empty() && opad::meshops::check(world(st->half), {false, 0}).closed() && opad::meshops::check(world(st->tube), {false, 0}).closed(),
                               "both halves are capped (closed)");
                       st->phase = 40;
                     });
    if (st->phase >= 40 && st->phase < 50)
      return feature(40, "Repair mesh on the open box", "mesh_repair", {{"bodies", refs({st->broken})}}, [&] {
                       require(opad::meshops::check(world(st->broken)).printable(), "the box is printable after it");
                       st->phase = 50;
                     }, [&] {
                       const QString note = form->resultNote();
                       require(note.contains("Found:") && note.contains("Printable"), "the panel says what it found and that it is printable: " + QString(note).replace(QChar('\n'), " / ").left(200));
                     });
    if (st->phase >= 50 && st->phase < 60) {
      if (st->phase == 50) st->tubeTriangles = world(st->half).triangles();
      return feature(50, "Simplify mesh to 30%", "mesh_simplify", {{"bodies", refs({st->half})}, {"mode", "ratio"}, {"ratio", "30"}}, [&] {
                       const size_t now = world(st->half).triangles();
                       require(now <= st->tubeTriangles * 31 / 100 && now > st->tubeTriangles / 5, QStringLiteral("%1 triangles to %2").arg(st->tubeTriangles).arg(now));
                       st->phase = 60;
                     }, [&] { require(form->resultNote().contains("triangles to"), "the panel says how many triangles are left: " + form->resultNote().left(120)); });
    }
    if (st->phase >= 60 && st->phase < 70)
      return feature(60, "Drop to bed", "drop_to_bed", {{"bodies", refs({st->broken})}}, [&] {
                       require(std::fabs(box(st->broken)[2]) < 1e-6, QStringLiteral("the box stands on Z = 0 (%1)").arg(box(st->broken)[2]));
                       st->phase = 70;
                     });
    switch (st->phase) {
      case 70: {
        if (busy) return;
        begin("Lay flat on the box");
        view->selectNodes({st->broken});
        st->phase = 71;
        return;
      }
      case 71: {
        if (busy) return;
        if (QAction* a = w.action("design.layFlat")) a->trigger();
        st->phase = 72;
        return;
      }
      case 72: {
        if ((!design->featureActive() || !design->planePicker()->active()) && st->clock.elapsed() < 20000) return;
        end();
        require(design->featureActive() && form->spec() && form->spec()->kind == "align", "Lay flat is Align");
        require(design->planePicker()->active(), "the face picker is open for the face that goes down");
        require(form->picks("to_plane").dump().find("xy") != std::string::npos, "onto the XY plane (the bed)");
        design->escape();
        design->escape();
        st->phase = 73;
        return;
      }
      case 73:
        if (busy) return;
        require(true, "done");
        return finish();
      default:
        return;
    }
  };
  timer->start();
  return true;
}
