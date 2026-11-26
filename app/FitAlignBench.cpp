// OPAD_BENCH_FITALIGN=<prefix> on a document holding a big mesh body with no exact face (a 3D scan; gui case fit-align,
// tools/bench_cases/fit_align.py): Align is started on it, its From plane picked by a click on the scan (the flat around
// the triangle clicked, fitted on a worker: FitPick, design/fit_refs.cpp), To set to XY, the preview checked (the scan
// drawn moved), committed, the plane fitted again on the moved scan on a worker (it must now face -Z: it lies on XY);
// undone; then Align scan to axes on it, its candidate shown with its quality, Next X (a quarter turn, previewed again),
// committed, and the scan's box measured on a worker (flat on XY, its sides along X and Y, centred). Every step keeps the
// UI thread under the stall limit (250 ms, CLAUDE.md "nothing long runs on the UI thread"): "bench: fit align: ... PASS".
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPushButton>
#include <QTimer>

#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "FitPick.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/scan_fit.hpp"

namespace {
constexpr qint64 kStallLimitMs = 250;

int meshTriangles(const TopoDS_Shape& shape) {
  int n = 0;
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    TopLoc_Location loc;
    if (const auto t = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc); !t.IsNull()) n += t->NbTriangles();
  }
  return n;
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_FITALIGN, fitAlign) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, x = 0, y = 0, triangles = 0;
    bool all = true, jobDone = false, jobOk = false;
    std::string mesh;
    opad::json firstX, measured;
    QString step;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  const QString prefix = value;
  auto* timer = new QTimer(&w);
  timer->setInterval(20);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, prefix] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    DesignController* design = w.m_design;
    FeaturePanel* form = design->featurePanel();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: fit align: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    auto enter = [&w, view] {
      QApplication::setActiveWindow(&w);
      QWidget* to = QApplication::focusWidget();
      QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
      QApplication::sendEvent(to ? to : view, &press);
    };
    // The scan's nodes in world coordinates now, measured on a worker: its box.
    auto measure = [&w, doc, st](std::function<void(const opad::json&)> then = {}) {
      st->jobDone = st->jobOk = false;
      auto out = std::make_shared<opad::json>();
      auto docCopy = std::make_shared<opad::Document>(doc->doc);
      auto scene = std::make_shared<opad::Scene>(doc->scene);
      const std::string mesh = st->mesh;
      w.m_jobs->async(QStringLiteral("Measuring the scan"), [docCopy, scene, mesh, out](Progress) {
        const auto m = opad::scanfit::fit_mesh(opad::node_world_shape(*docCopy, *scene, mesh));
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        for (const auto& p : m->nodes)
          for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p.Coord(k + 1)), hi[k] = std::max(hi[k], p.Coord(k + 1));
        *out = {{"lo", {lo[0], lo[1], lo[2]}}, {"hi", {hi[0], hi[1], hi[2]}}};
      }, [st, out](bool ok, const QString& error) {
        st->jobDone = true;
        st->jobOk = ok;
        st->measured = *out;
        if (!ok) trace::log("bench: fit align: measuring failed: " + error);
      });
    };
    if (++st->ticks > 30000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->step));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || view->filterSwitching();
    switch (st->phase) {
      case 0: {  // loaded and drawn: the biggest mesh body
        if (busy || ++st->settled < 25) return;
        for (const auto& id : doc->scene.all_bodies()) {
          const opad::Node* n = doc->node(id);
          if (!n || n->representation != "mesh") continue;
          try {
            if (const int t = meshTriangles(opad::body_shape(doc->doc, n->body_key)); t > st->triangles) st->triangles = t, st->mesh = id;
          } catch (const std::exception&) {
          }
        }
        if (!require(!st->mesh.empty() && st->triangles >= 1000000, QStringLiteral("a mesh body of a million triangles or more (%1)").arg(st->triangles))) return finish();
        QApplication::setActiveWindow(&w);
        st->phase = 100;
        return;
      }
      case 100: {
        begin("Align started on the scan");
        design->startFeature("align", {{"bodies", opad::json::array({{{"body", st->mesh}, {"kind", "body"}}})}});
        st->phase = 1;
        return;
      }
      case 1: {  // its From plane box clicked: the plane picker opens
        if (busy || !design->featureActive() || st->clock.elapsed() < 400) return;
        if (!design->pickingPlane()) {
          if (form->activeInput() != "from_plane") form->activate("from_plane");
          if (st->clock.elapsed() < 20000) return;
        }
        if (!require(design->pickingPlane(), "the From plane box opens the plane picker")) return finish();
        end(QStringLiteral("active input %1").arg(form->activeInput()));
        if (!require(view->benchBodyPoint(st->mesh, st->x, st->y), "a point of the view on the scan")) return finish();
        begin("a triangle of the scan clicked for From plane");
        view->benchClickAt(st->x, st->y);
        st->phase = 2;
        return;
      }
      case 2: {  // fitted on a worker, put into the input with its result
        const opad::json from = form->picks("from_plane");
        if (!fitpick::fitted(from) && st->clock.elapsed() < 120000) return;
        end(QStringLiteral("From: %1 %2").arg(fitpick::title(from), fitpick::quality(from)));
        const opad::json q = from.value("result", opad::json::object()).value("quality", opad::json::object());
        require(fitpick::fitted(from) && from["fit"].value("body", "") == st->mesh, "From plane is a plane fitted on the scan");
        require(q.value("grade", "") == "good" && q.value("total", 0) > 1000,
                QStringLiteral("the fit is good over the flat (%1 triangles, rms %2 mm)").arg(q.value("total", 0)).arg(q.value("rms_mm", 0.0)));
        begin("To plane: XY");
        st->phase = 20;
        return;
      }
      case 20: {  // the To plane box clicked, XY chosen in its picker
        if (busy) return;
        if (!design->pickingPlane()) {
          if (form->activeInput() != "to_plane") form->activate("to_plane");
          if (st->clock.elapsed() < 20000) return;
        }
        if (design->pickingPlane()) design->planePicker()->choose({{"base", "xy"}});
        else form->setPicks("to_plane", {{"base", "xy"}});
        st->phase = 3;
        return;
      }
      case 3: {  // the preview draws the scan where it goes
        const bool ready = form->picks("to_plane").contains("base") && !design->previewing() && view->previewMovedCount() > 0;
        if ((!ready || busy) && st->clock.elapsed() < 120000) return;
        end(QStringLiteral("%1 drawn moved").arg(view->previewMovedCount()));
        require(view->previewMovedCount() > 0, "the preview moves the scan onto XY");
        form->window()->grab().save(prefix + ".align-panel.png");
        begin("committed");
        enter();
        st->phase = 4;
        return;
      }
      case 4: {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 120000) return;
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        end(QStringLiteral("feature %1").arg(f ? QString::fromStdString(f->kind) : QStringLiteral("none")));
        if (!require(f && f->kind == "align", "an Align step was committed")) return finish();
        const opad::json stored = f->inputs.value("from_plane", opad::json());
        require(fitpick::fitted(stored), "the step keeps the fit's result (replay never fits again)");
        // The same fit asked again of the moved scan (no result: fitted now, on a worker) faces -Z: the flat lies on XY.
        begin("the plane fitted again on the moved scan (worker)");
        st->jobDone = false;
        auto out = std::make_shared<opad::json>();
        auto docCopy = std::make_shared<opad::Document>(doc->doc);
        auto scene = std::make_shared<opad::Scene>(doc->scene);
        const opad::json ask = {{"fit", stored.value("fit", opad::json())}};
        w.m_jobs->async(QStringLiteral("Fitting"), [docCopy, scene, ask, out](Progress) { *out = opad::design::fit_reference(*docCopy, *scene, ask, "plane"); },
                        [st, out](bool ok, const QString& error) {
                          st->jobDone = true;
                          st->jobOk = ok;
                          st->measured = *out;
                          if (!ok) trace::log("bench: fit align: refit failed: " + error);
                        });
        st->phase = 5;
        return;
      }
      case 5: {
        if (!st->jobDone) return;
        end();
        const opad::json n = st->measured.value("world", opad::json::object()).value("normal", opad::json::array({0, 0, 0}));
        require(st->jobOk && n.size() == 3 && n[2].get<double>() < -0.9999, QStringLiteral("the fitted flat now faces -Z (%1)").arg(QString::fromStdString(n.dump())));
        begin("undone");
        doc->undo();
        st->phase = 6;
        return;
      }
      case 6: {
        if (busy || st->clock.elapsed() < 300) return;
        end();
        view->selectNodes({st->mesh});
        st->phase = 106;
        return;
      }
      case 106: {
        if (busy) return;
        begin("Align scan to axes");
        w.action("design.alignScan")->trigger();
        st->phase = 7;
        return;
      }
      case 7: {  // its candidate fitted and previewed, with its quality
        const opad::json from = form->picks("from_plane");
        const bool ready = design->featureActive() && fitpick::fitted(from) && !design->previewing() && view->previewMovedCount() > 0;
        if ((!ready || busy) && st->clock.elapsed() < 120000) return;
        end(QStringLiteral("%1: %2").arg(fitpick::title(from), fitpick::quality(from)));
        const opad::json r = from.value("result", opad::json::object());
        require(r.value("of", "") == "scan_axes" && r.value("method", "") == "flats", "a scan_axes candidate from the flats");
        require(r.value("bases", 0) >= 2 && r.value("turns", 0) >= 4, QStringLiteral("candidates to cycle: %1 bases, %2 X turns").arg(r.value("bases", 0)).arg(r.value("turns", 0)));
        require(view->previewMovedCount() > 0, "the preview stands the scan on XY");
        auto* next = form->findChild<QPushButton*>("fitNextTurn");
        require(next && next->isVisible() && next->isEnabled(), "Next X is offered");
        form->window()->grab().save(prefix + ".scan-axes-panel.png");
        st->firstX = r.value("x", opad::json());
        begin("Next X");
        if (next) next->click();
        st->phase = 8;
        return;
      }
      case 8: {
        const opad::json from = form->picks("from_plane");
        const bool ready = fitpick::fitted(from) && from["fit"].value("turn", 0) == 1 && !design->previewing() && view->previewMovedCount() > 0;
        if ((!ready || busy) && st->clock.elapsed() < 120000) return;
        end(fitpick::quality(from));
        const opad::json x = from.value("result", opad::json::object()).value("x", opad::json());
        double dot = 1;
        if (x.size() == 3 && st->firstX.size() == 3) dot = x[0].get<double>() * st->firstX[0].get<double>() + x[1].get<double>() * st->firstX[1].get<double>() + x[2].get<double>() * st->firstX[2].get<double>();
        require(std::fabs(dot) < 0.05, QStringLiteral("Next X is a quarter turn (x . x0 = %1)").arg(dot));
        begin("committed");
        enter();
        st->phase = 9;
        return;
      }
      case 9: {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 120000) return;
        end();
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        require(f && f->kind == "align" && f->inputs.value("frame", false), "an Align step matching the frames was committed");
        begin("the scan's box measured (worker)");
        measure();
        st->phase = 10;
        return;
      }
      case 10: {
        if (!st->jobDone) return;
        end();
        const auto& lo = st->measured.value("lo", opad::json::array({0, 0, 0})), &hi = st->measured.value("hi", opad::json::array({0, 0, 0}));
        const double dx = hi[0].get<double>() - lo[0].get<double>(), dy = hi[1].get<double>() - lo[1].get<double>(), dz = hi[2].get<double>() - lo[2].get<double>();
        const QString box = QStringLiteral("%1 x %2 x %3 from z %4").arg(dx, 0, 'f', 2).arg(dy, 0, 'f', 2).arg(dz, 0, 'f', 2).arg(lo[2].get<double>(), 0, 'f', 3);
        require(st->jobOk && std::fabs(dz - 6) < 0.4 && std::fabs(lo[2].get<double>()) < 0.3, "it stands flat on XY: " + box);
        require((std::fabs(dx - 120) < 0.6 && std::fabs(dy - 80) < 0.6) || (std::fabs(dx - 80) < 0.6 && std::fabs(dy - 120) < 0.6), "its sides run along X and Y: " + box);
        require(std::fabs(lo[0].get<double>() + hi[0].get<double>()) < 0.6 && std::fabs(lo[1].get<double>() + hi[1].get<double>()) < 0.6, "it is centred on the origin");
        trace::log(QStringLiteral("bench: fit align: %1").arg(trace::stallHistogram()));
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
