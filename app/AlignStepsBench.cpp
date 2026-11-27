// OPAD_BENCH_ALIGN_STEPS=<prefix> on the align-pairs document (a block "Block" and its noisy scan, moved away; gui case
// align-steps, tools/bench_cases/align.py): Align's plane steps as Ahmad met them on a scan. Align started on the scan
// opens no picker by itself: the From box is marked as the next step, with a line saying which side; a click on the box
// opens the picker, titled for From. Fit to points: its explanation shows, every vertex clicked on the scan counts (the
// countdown goes down, the picks are all vertices), a click on a picked vertex takes it back; Use fits the plane, closes
// the picker and puts the fit and its quality in the panel, From marked done, To marked next and no picker opened again;
// To's box opens the picker for To, XY chosen, previewed, committed. Every step under the stall limit:
// "bench: align steps: ... PASS".
#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QTimer>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "FitPick.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/scan_fit.hpp"

namespace {
constexpr qint64 kStallLimitMs = 250;

gp_Trsf scanMove() {  // as tools/bench_cases/align.py _pairs_move
  gp_Trsf r, t;
  r.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 1, 2)), 25 * M_PI / 180);
  t.SetTranslation(gp_Vec(180, 30, 20));
  return t * r;
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_ALIGN_STEPS, alignSteps) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, click = 0;
    bool all = true, jobDone = false, jobOk = false, retried = false;
    std::string scan;
    std::vector<opad::Vec3> nodes;  // the scan's top-face nodes to click (world)
    opad::json found;
    QString stepName;
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
    PlanePicker* picker = design->planePicker();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: align steps: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    auto begin = [st](const QString& step) {
      st->stepName = step;
      trace::resetStalls();
      st->clock.start();
    };
    auto end = [st, require](const QString& extra = {}) {
      const trace::Stalls s = trace::stalls();
      require(s.longest < kStallLimitMs, QStringLiteral("%1: %2 ms, longest stall %3 ms (%4 stalls)%5")
                                             .arg(st->stepName).arg(st->clock.elapsed()).arg(s.longest).arg(s.count).arg(extra.isEmpty() ? QString() : ", " + extra));
    };
    auto picked = [view] {
      int vertices = 0, other = 0;
      for (const auto& r : view->selection()) (r.kind == opad::Ref::Kind::Vertex ? vertices : other)++;
      return std::make_pair(vertices, other);
    };
    if (++st->ticks > 30000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->stepName));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || view->filterSwitching();
    switch (st->phase) {
      case 0: {
        if (busy || ++st->settled < 25) return;
        for (const auto& id : doc->scene.all_bodies())
          if (const opad::Node* n = doc->node(id); n && n->representation == "mesh") st->scan = id;
        if (!require(!st->scan.empty(), "a scan mesh")) return finish();
        // Top-face nodes of the scan, spread over it, found on a worker.
        auto docCopy = std::make_shared<opad::Document>(doc->doc);
        auto scene = std::make_shared<opad::Scene>(doc->scene);
        auto out = std::make_shared<opad::json>();
        const std::string scan = st->scan;
        w.m_jobs->async(QStringLiteral("Finding nodes"), [docCopy, scene, scan, out](Progress) {
          const auto m = opad::scanfit::fit_mesh(opad::node_world_shape(*docCopy, *scene, scan));
          *out = opad::json::array();
          for (const gp_Pnt& p : {gp_Pnt(20, 16, 40), gp_Pnt(80, 14, 40), gp_Pnt(52, 46, 40), gp_Pnt(28, 40, 40)}) {
            const gp_Pnt moved = p.Transformed(scanMove());
            size_t best = 0;
            for (size_t i = 1; i < m->nodes.size(); ++i)
              if (m->nodes[i].Distance(moved) < m->nodes[best].Distance(moved)) best = i;
            out->push_back({m->nodes[best].X(), m->nodes[best].Y(), m->nodes[best].Z()});
          }
        }, [st, out](bool ok, const QString&) {
          st->jobDone = true;
          st->jobOk = ok;
          st->found = *out;
        });
        st->phase = 1;
        return;
      }
      case 1: {
        if (!st->jobDone) return;
        for (const auto& p : st->found) st->nodes.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
        QApplication::setActiveWindow(&w);
        begin("Align started on the scan");
        design->startFeature("align", {{"bodies", opad::json::array({{{"body", st->scan}, {"kind", "body"}}})}});
        st->phase = 2;
        return;
      }
      case 2: {  // no picker by itself: From is the next step, said in the panel
        if ((busy || !design->featureActive() || st->clock.elapsed() < 800) && st->clock.elapsed() < 20000) return;
        end();
        require(!design->pickingPlane() && !picker->active(), "no plane picker opens by itself");
        require(form->nextPlane() == "from_plane", "the From plane is marked as the next step");
        require(form->planeStepText("from_plane").startsWith("From:") && form->planeStepText("to_plane").startsWith("To:"),
                "the panel says which side each plane is: " + form->planeStepText("from_plane"));
        form->window()->grab().save(prefix + ".from-next.png");
        begin("the From box clicked");
        form->activate("from_plane");
        st->phase = 3;
        return;
      }
      case 3: {  // the picker, titled for From
        if (!picker->active() && st->clock.elapsed() < 20000) return;
        end();
        if (!require(picker->active(), "the box opens the picker")) return finish();
        require(picker->panel()->windowTitle().contains(i18n::t("From face or plane")), "the picker says it chooses From: " + picker->panel()->windowTitle());
        auto* points = picker->panel()->findChild<QPushButton*>("fitPoints");
        if (!require(points != nullptr, "Fit to points is offered")) return finish();
        begin("Fit to points on");
        points->click();
        st->phase = 4;
        return;
      }
      case 4: {
        if (busy || !picker->pointsMode()) return;
        end();
        require(!picker->pointsHint().isEmpty(), "its explanation shows: " + picker->pointsHint());
        require(picker->statusText().contains("3"), "it asks for three vertices: " + picker->statusText());
        st->click = 0;
        st->settled = 0;
        begin("vertices clicked on the scan");
        st->phase = 5;
        return;
      }
      case 5: {  // a click per few ticks; each one counts
        if (busy || ++st->settled < 8) return;
        st->settled = 0;
        const auto [vertices, other] = picked();
        if (st->click > 0 && vertices == st->click - 1 && picker->statusText().contains("No vertex") && !st->retried) {
          // Picking found no vertex there (the depth-first pick of a 2 mm mesh can miss by a pixel): it said so; once more.
          trace::log(QStringLiteral("bench: align steps: click %1 found no vertex and said so; clicking again").arg(st->click));
          view->benchClickAt(QPointF(view->widgetPoint(st->nodes[size_t(st->click - 1)])));
          st->retried = true;
          return;
        }
        if (st->click > 0) {
          require(vertices == st->click && other == 0,
                  QStringLiteral("click %1 counted: %2 vertices picked, %3 other picks; it says \"%4\"").arg(st->click).arg(vertices).arg(other).arg(picker->statusText()));
        }
        if (st->click == 4) {
          end();
          require(picker->statusText().contains("4"), "it counts four points: " + picker->statusText());
          picker->panel()->grab().save(prefix + ".points.png");
          // A click on a picked vertex takes it back.
          begin("a picked vertex clicked again");
          view->benchClickAt(QPointF(view->widgetPoint(st->nodes[3])));
          st->phase = 6;
          return;
        }
        st->retried = false;
        const opad::Vec3 at = st->nodes[size_t(st->click)];
        const QPoint where = view->widgetPoint(at);
        trace::log(QStringLiteral("bench: align steps: clicking vertex %1 at %2,%3 of %4x%5").arg(st->click + 1).arg(where.x()).arg(where.y()).arg(view->width()).arg(view->height()));
        view->benchClickAt(QPointF(where));
        ++st->click;
        return;
      }
      case 6: {
        if (busy || ++st->settled < 8) return;
        end();
        const auto [vertices, other] = picked();
        require(vertices == 3 && other == 0 && picker->statusText().contains("taken back"), QStringLiteral("taken back: %1 vertices; it says \"%2\"").arg(vertices).arg(picker->statusText()));
        begin("a click on empty space");
        view->benchClickAt(QPointF(40, view->height() - 40));
        st->settled = 0;
        st->phase = 60;
        return;
      }
      case 60: {
        if (busy || ++st->settled < 8) return;
        end();
        const auto [vertices, other] = picked();
        require(vertices == 3 && picker->statusText().contains("No vertex"), QStringLiteral("it keeps the picks and says why nothing was added: \"%1\"").arg(picker->statusText()));
        auto* use = picker->panel()->findChild<QPushButton*>("usePoints");
        if (!require(use && use->isEnabled(), "Use is enabled with three points")) return finish();
        begin("Use");
        use->click();
        st->phase = 7;
        return;
      }
      case 7: {  // fitted, back in the panel: From done with its quality, To next, no picker reopened
        const opad::json from = form->picks("from_plane");
        if ((picker->active() || !fitpick::fitted(from) || busy) && st->clock.elapsed() < 30000) return;
        if (st->clock.elapsed() < 800) return;  // a picker that would reopen by itself has had its chance
        end();
        require(fitpick::fitted(from) && from["fit"].contains("points"), "From is the plane fitted through the points");
        require(!picker->active() && !design->pickingPlane(), "no picker opened again for To");
        require(form->nextPlane() == "to_plane", "To is marked as the next step");
        require(form->planeStepText("from_plane").contains("done"), "From is marked done: " + form->planeStepText("from_plane"));
        form->window()->grab().save(prefix + ".from-done.png");
        begin("the To box clicked, XY chosen");
        form->activate("to_plane");
        st->phase = 8;
        return;
      }
      case 8: {
        if (!picker->active() && st->clock.elapsed() < 20000) return;
        require(picker->active() && picker->panel()->windowTitle().contains(i18n::t("To face or plane")), "the picker says it chooses To: " + picker->panel()->windowTitle());
        picker->choose({{"base", "xy"}});
        st->phase = 9;
        return;
      }
      case 9: {
        const bool ready = form->picks("to_plane").contains("base") && !design->previewing() && view->previewMovedCount() > 0;
        if ((!ready || busy) && st->clock.elapsed() < 30000) return;
        end();
        require(ready, "previewed onto XY");
        begin("committed");
        emit form->accepted();
        st->phase = 10;
        return;
      }
      case 10: {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 30000) return;
        end();
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        require(f && f->kind == "align" && fitpick::isFit(f->inputs.value("from_plane", opad::json())), "an Align step on the fitted plane was committed");
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
