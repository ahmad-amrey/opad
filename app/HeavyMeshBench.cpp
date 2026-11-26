// OPAD_BENCH_HEAVYMESH=<prefix> on a document holding a big mesh body (a 3D scan): the steps of working with it, each
// timed with the UI watchdog's longest stall since the step began (trace::stalls). The mesh body with the most triangles is
// selected, then Move / copy starts on it with Rotate on and its Axis box activated (the Edge filter with round faces: the
// pick targets of every displayed body built), the axis picked on the scan at the view's centre, a 30 degree turn previewed
// and committed, undone; then the Face, Vertex and Body filters, the section on and off, and a distance and the properties
// measured on a worker. Every step must keep the UI thread under the stall limit (250 ms, CLAUDE.md "nothing long runs on
// the UI thread"): "bench: heavy mesh: <step> ... PASS/FAIL".
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QTimer>

#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

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

OPAD_BENCH(OPAD_BENCH_HEAVYMESH, heavyMesh) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, filter = 0, x = 0, y = 0;
    bool all = true, waitingJob = false, jobDone = false;
    std::string mesh, other;
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
      trace::log(QString("bench: heavy mesh: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    // The step's time and its longest stall; the stall must stay under the limit.
    // The scan found at its point by the current filter, as a hover there finds it; the pick's own time.
    auto pickable = [st, view, require](const QString& filter) {
      QElapsedTimer t;
      t.start();
      const std::string found = view->benchPickAt(st->x, st->y);
      require(found == st->mesh, QStringLiteral("the scan is picked in the %1 filter (%2 ms)").arg(filter).arg(t.elapsed()));
    };
    auto end = [st, require](const QString& extra = {}) {
      const trace::Stalls s = trace::stalls();
      require(s.longest < kStallLimitMs, QStringLiteral("%1: %2 ms, longest stall %3 ms (%4 stalls)%5")
                                             .arg(st->step).arg(st->clock.elapsed()).arg(s.longest).arg(s.count).arg(extra.isEmpty() ? QString() : ", " + extra));
    };
    if (++st->ticks > 30000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->step));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || view->filterSwitching();
    switch (st->phase) {
      case 0: {  // loaded and drawn: the biggest mesh body
        if (busy) return;
        if (++st->settled < 25) return;
        int most = -1;
        for (const auto& id : doc->scene.all_bodies()) {
          const opad::Node* n = doc->node(id);
          if (!n || !doc->scene.effectively_visible(id)) continue;
          TopoDS_Shape shape;
          try {
            shape = opad::body_shape(doc->doc, n->body_key);
          } catch (const std::exception& e) {
            trace::log(QStringLiteral("bench: heavy mesh: %1 has no shape: %2").arg(QString::fromStdString(n->name), e.what()));
            continue;
          }
          if (!opad::is_mesh_shape(shape)) {
            if (st->other.empty()) st->other = id;
            continue;
          }
          if (const int t = meshTriangles(shape); t > most) most = t, st->mesh = id;
        }
        if (!require(!st->mesh.empty(), QStringLiteral("a visible mesh body (%1 triangles)").arg(most))) return finish();
        if (!require(view->benchBodyPoint(st->mesh, st->x, st->y), "a point of the view on it")) return finish();
        QApplication::setActiveWindow(&w);
        st->phase = 100;  // the next tick: the search's own gap is not the step's
        return;
      }
      case 100: {
        begin("select the scan");
        view->selectNodes({st->mesh});
        st->phase = 1;
        return;
      }
      case 1: {
        if (busy) return;
        end();
        begin("Move / copy started on it, Rotate on");
        design->startFeature("move", {{"bodies", opad::json::array({{{"body", st->mesh}, {"kind", "body"}}})}, {"rotate", true}});
        st->phase = 2;
        return;
      }
      case 2: {
        if (busy || !design->featureActive()) return;
        if (st->clock.elapsed() < 300) return;  // the panel's own activation settles
        end(QStringLiteral("active input %1").arg(form->activeInput()));
        begin("its Axis box activated (Edge filter, round faces)");
        form->activate("axis");
        st->phase = 3;
        return;
      }
      case 3: {
        if (busy || st->clock.elapsed() < 300) return;
        end(QStringLiteral("filter %1").arg(int(view->selectionFilter())));
        // A point where this mode finds the scan: the origin's axes are drawn as candidates now, over the scan's middle.
        require(view->benchBodyPoint(st->mesh, st->x, st->y), "a point where the Axis box finds the scan");
        pickable("Edge");
        st->phase = 101;
        return;
      }
      case 101: {
        begin("an edge of the scan clicked for the axis");
        view->benchClickAt(st->x, st->y);
        st->phase = 4;
        return;
      }
      case 4: {
        if (busy || st->clock.elapsed() < 300) return;
        end(QStringLiteral("axis %1").arg(QString::fromStdString(form->picks("axis").dump())));
        {
          QStringList seen;
          for (const auto& r : view->selection()) seen << QString::fromStdString(r.str());
          for (const auto& c : view->selectedCandidates()) seen << QString::fromStdString(c);
          trace::log("bench: heavy mesh: selected after the click: " + seen.join(" | "));
        }
        // A scan's side clicked for an axis is the axis fitted around it (FitPick), fitted on a worker.
        require(form->picks("axis").contains("fit") && form->picks("axis")["fit"].value("body", "") == st->mesh, "the axis is fitted on the scan");
        if (!design->featureActive()) {
          require(false, "the feature is still open after the axis pick");
          return finish();
        }
        form->setPicks("axis", {{"base", "z"}});  // a definite axis for the preview
        begin("a 30 degree turn previewed");
        form->setValue("angle", "30 deg");
        st->phase = 5;
        return;
      }
      case 5: {
        if ((busy || view->previewMovedCount() == 0) && st->clock.elapsed() < 120000) return;
        end(QStringLiteral("%1 drawn moved, %2 meshed").arg(view->previewMovedCount()).arg(view->previewBodyCount()));
        form->window()->grab().save(prefix + ".move-panel.png");
        begin("committed");
        QApplication::setActiveWindow(&w);
        QWidget* to = QApplication::focusWidget();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
        QApplication::sendEvent(to ? to : view, &press);
        st->phase = 6;
        return;
      }
      case 6: {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 120000) return;
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        end(QStringLiteral("feature %1").arg(f ? QString::fromStdString(f->kind) : QStringLiteral("none")));
        require(f && f->kind == "move", "a Move step was committed");
        begin("undone");
        doc->undo();
        st->phase = 7;
        return;
      }
      case 7: {  // the filters one after the other
        if (busy || st->clock.elapsed() < 200) return;
        end();
        static const Viewport::SelFilter filters[] = {Viewport::SelFilter::Face, Viewport::SelFilter::Edge, Viewport::SelFilter::Vertex, Viewport::SelFilter::Body};
        static const char* names[] = {"Face", "Edge", "Vertex", "Body"};
        if (st->filter > 0) pickable(names[st->filter - 1]);
        if (st->filter < 4) {
          begin(QStringLiteral("the %1 filter").arg(names[st->filter]));
          view->setSelectionFilter(filters[st->filter]);
          ++st->filter;
          return;
        }
        view->setSelectionFilter(Viewport::SelFilter::Face);
        st->phase = 8;
        return;
      }
      case 8: {
        if (busy || st->clock.elapsed() < 200) return;
        begin("a triangle of the scan clicked in the Face filter");
        view->benchClickAt(st->x, st->y);
        const auto picked = view->selection();
        end(QStringLiteral("%1 selected").arg(picked.size()));
        require(picked.size() == 1 && picked.front().kind == opad::Ref::Kind::Face && picked.front().body == st->mesh, "one face of the scan selected");
        view->setSelectionFilter(Viewport::SelFilter::Body);
        st->phase = 9;
        return;
      }
      case 9: {
        if (busy) return;
        begin("section on");
        w.action("inspect.section")->setChecked(true);
        st->phase = 10;
        return;
      }
      case 10: {
        if (busy || st->clock.elapsed() < 500) return;
        end();
        begin("section off");
        w.action("inspect.section")->setChecked(false);
        st->phase = 11;
        return;
      }
      case 11: {
        if (busy || st->clock.elapsed() < 300) return;
        end();
        if (st->other.empty()) {
          st->phase = 13;
          return;
        }
        begin("distance from the scan to the other body (worker)");
        st->jobDone = false;
        auto out = std::make_shared<std::string>();
        const opad::Ref a = opad::Ref::parse(st->mesh), b = opad::Ref::parse(st->other);
        w.m_jobs->async(QStringLiteral("Measuring"), [doc, a, b, out](Progress) {
          *out = opad::measure_distance(doc->doc, doc->scene, a, b).dump().substr(0, 160);
        }, [st, out](bool ok, const QString& error) {
          st->jobDone = true;
          trace::log(QStringLiteral("bench: heavy mesh: distance %1: %2").arg(ok ? "ok" : error, QString::fromStdString(*out)));
        });
        st->phase = 12;
        return;
      }
      case 12: {
        if (!st->jobDone) return;
        end();
        st->phase = 13;
        return;
      }
      case 13: {
        trace::log(QStringLiteral("bench: heavy mesh: %1").arg(trace::stallHistogram()));
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
