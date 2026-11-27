// OPAD_BENCH_CLOUD=<prefix> on a document holding a big point cloud (tools/bench_cases/cloud.py: 20 million points, linked as
// a scan is): every step of working with it timed with the UI watchdog's longest stall since the step began
// (trace::stalls), which must stay under 250 ms (CLAUDE.md "nothing long runs on the UI thread"). The cloud is opened and
// drawn (a sample of BodyPrs::cloudBudget points), picked as a body and as a point (the Vertex filter: an ordinal, no owner
// per point), zoomed into (the visible part drawn at full density, ViewportCloud.cpp), given a colour overlay (a deviation
// map, Viewport::setCloudColours) kept through the zoom back out and cleared, a box of its points selected with
// Select points, sliced (Slice points, through the design's plan and commit) and the slice turned into a sketch.
// "bench: cloud: <step> ... PASS/FAIL".
#include <QApplication>
#include <QElapsedTimer>
#include <QAbstractButton>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QTimer>

#include <memory>

#include "BenchRegistry.hpp"
#include "BodyShape.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/point_cloud.hpp"

namespace {
constexpr qint64 kStallLimitMs = 250;
}

OPAD_BENCH(OPAD_BENCH_CLOUD, cloud) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, x = 0, y = 0;
    bool all = true;
    std::string cloud, slice;
    size_t points = 0, drawn = 0, sketches = 0, drawnBefore = 0;
    std::shared_ptr<const std::vector<uint32_t>> overlay;
    QString step;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  const QString prefix = value;
  auto* timer = new QTimer(&w);
  timer->setInterval(20);
  trace::resetStalls();
  st->step = "open and draw";
  st->clock.start();
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, prefix] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    DesignController* design = w.m_design;
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: cloud: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    if (++st->ticks > 9000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->step));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || view->filterSwitching();
    switch (st->phase) {
      case 0: {  // opened: the window's own start (its first frames) is not the cloud's
        if (st->ticks == 1) begin("open and draw");
        if (busy) return;
        for (const auto& id : doc->scene.all_bodies())
          if (const opad::Node* n = doc->node(id); n && n->representation == "points") st->cloud = id;
        if (st->cloud.empty() || view->benchCloudDrawn(st->cloud) == 0) {
          if (st->clock.elapsed() > 150000) {
            require(false, "a point cloud is drawn");
            return finish();
          }
          return;
        }
        if (++st->settled < 10) return;
        const auto prs = view->cloudPrs(st->cloud);
        st->points = prs ? prs->cloudData->size() : 0;
        st->drawn = view->benchCloudDrawn(st->cloud);
        end(QStringLiteral("%1 points, %2 drawn").arg(st->points).arg(st->drawn));
        require(st->points >= 1000000 && st->drawn == std::min(st->points, BodyPrs::cloudBudget), QStringLiteral("the cloud is drawn: %1 of %2 points").arg(st->drawn).arg(st->points));
        require(prs && prs->cloudSpacing > 0, QStringLiteral("its spacing measured: %1 mm").arg(prs ? prs->cloudSpacing : 0));
        view->fitAll();
        st->phase = 1;
        return;
      }
      case 1: {
        if (busy || view->cameraMoving()) return;
        begin("picked as a body");
        if (!require(view->benchBodyPoint(st->cloud, st->x, st->y), "a point of the view on the cloud")) return finish();
        const std::string found = view->benchPickAt(st->x, st->y);
        end(QStringLiteral("found %1").arg(QString::fromStdString(found)));
        require(found == st->cloud, "the cloud is picked as a body");
        st->phase = 101;  // the next tick: the search for a point above (many picks in a row) is the bench's, not the filter's
        return;
      }
      case 101: {
        begin("the Vertex filter");
        view->setSelectionFilter(Viewport::SelFilter::Vertex);
        st->phase = 2;
        return;
      }
      case 2: {
        if (busy || st->clock.elapsed() < 300) return;
        end();
        begin("a point clicked");
        view->benchClickAt(st->x, st->y);
        const auto picked = view->selection();
        end(QStringLiteral("%1 selected").arg(picked.size()));
        require(picked.size() == 1 && picked.front().kind == opad::Ref::Kind::Vertex && picked.front().body == st->cloud && picked.front().index >= 0 &&
                    size_t(picked.front().index) < st->points,
                QStringLiteral("one point of the cloud selected (%1)").arg(picked.empty() ? QString("none") : QString::fromStdString(picked.front().str())));
        view->setSelectionFilter(Viewport::SelFilter::Body);
        st->phase = 3;
        return;
      }
      case 3: {
        if (busy) return;
        begin("zoomed in: the visible part at full density");
        auto camera = view->cameraJson();
        camera["scale"] = camera.value("scale", 1.0) / 8;
        view->setCameraJson(camera);
        view->requestRefinement();
        st->phase = 4;
        return;
      }
      case 4: {
        const size_t now = view->benchCloudDrawn(st->cloud);
        if ((now == st->drawn || busy) && st->clock.elapsed() < 60000) return;
        end(QStringLiteral("%1 points drawn").arg(now));
        require(now != st->drawn && now > 0 && now < st->points, QStringLiteral("the zoomed-in part is drawn (%1 points)").arg(now));
        // A deviation map's colours (display only), one a point: red above the middle, none (alpha 0) below.
        auto colours = std::make_shared<std::vector<uint32_t>>(st->points);
        for (size_t i = 0; i < st->points; ++i) (*colours)[i] = i * 2 > st->points ? 0xFFFF2020u : 0u;
        st->overlay = colours;
        begin("a colour overlay on the zoomed-in part");
        view->setCloudColours(st->cloud, colours);
        st->phase = 40;
        return;
      }
      case 40: {
        if ((!view->benchCloudOverlaid(st->cloud) || busy) && st->clock.elapsed() < 60000) return;
        end(QStringLiteral("%1 points drawn").arg(view->benchCloudDrawn(st->cloud)));
        require(view->benchCloudOverlaid(st->cloud), "the overlay recolours the zoomed-in part");
        st->drawnBefore = view->benchCloudDrawn(st->cloud);
        begin("zoomed out: the overlay kept on the whole sample");
        view->fitAll();
        view->requestRefinement();
        st->phase = 41;
        return;
      }
      case 41: {
        const size_t now = view->benchCloudDrawn(st->cloud);
        if ((now == st->drawnBefore || busy || view->cameraMoving()) && st->clock.elapsed() < 60000) return;
        if (!view->benchCloudOverlaid(st->cloud) && st->clock.elapsed() < 60000) return;
        end(QStringLiteral("%1 points drawn").arg(now));
        require(view->benchCloudOverlaid(st->cloud) && now == st->drawn, QStringLiteral("the overlay follows the swap back to the sample (%1 points)").arg(now));
        begin("the overlay cleared");
        view->setCloudColours(st->cloud, nullptr);
        view->requestRefinement();
        st->phase = 42;
        return;
      }
      case 42: {
        if ((busy || view->benchCloudOverlaid(st->cloud)) && st->clock.elapsed() < 30000) return;
        if (st->clock.elapsed() < 800) return;  // its pass
        end();
        require(!view->benchCloudOverlaid(st->cloud) && view->benchCloudDrawn(st->cloud) == st->drawn, "the cloud is drawn in its own colours again");
        st->overlay.reset();
        st->phase = 5;
        return;
      }
      case 5: {
        if (busy || view->cameraMoving()) return;
        view->requestRefinement();  // back to the whole sample
        view->selectNodes({st->cloud});
        begin("Select points opened");
        w.action("cloud.region")->trigger();
        st->phase = 6;
        return;
      }
      case 6: {
        if (busy || st->clock.elapsed() < 400) return;
        end();
        QWidget* panel = w.findChild<QWidget*>("cloudRegionPanel");
        if (!require(panel && panel->isVisible(), "the Select points panel is shown")) return finish();
        if (QAbstractButton* box = panel->findChild<QAbstractButton*>("cloudTool1")) box->click();
        // A box over the middle of the view, through the viewport's own mouse events.
        begin("a box of points selected");
        const QPointF a(view->width() * 0.35, view->height() * 0.35), b(view->width() * 0.65, view->height() * 0.65);
        auto send = [view](QEvent::Type type, const QPointF& at, Qt::MouseButtons buttons) {
          QMouseEvent e(type, at, view->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
          QCoreApplication::sendEvent(view, &e);
        };
        send(QEvent::MouseButtonPress, a, Qt::LeftButton);
        for (int k = 1; k <= 10; ++k) send(QEvent::MouseMove, a + (b - a) * (k / 10.0), Qt::LeftButton);
        send(QEvent::MouseButtonRelease, b, Qt::NoButton);
        st->phase = 7;
        return;
      }
      case 7: {
        QLabel* status = w.findChild<QLabel*>("cloudRegionStatus");
        const bool done = status && status->text().contains(QStringLiteral("selected"));
        if ((!done || busy) && st->clock.elapsed() < 60000) return;
        end(status ? status->text().section('\n', 0, 0) : QString());
        require(done, "the box selected points of the cloud");
        if (QWidget* panel = w.findChild<QWidget*>("cloudRegionPanel")) panel->hide();
        st->sketches = doc->scene.sketches.size();
        begin("Slice points planned and committed");
        const opad::json inputs = {{"bodies", opad::json::array({{{"body", st->cloud}}})}, {"plane", {{"base", "xy"}}}, {"offset", "60 mm"}, {"band", "0.5 mm"}, {"count", "1"}, {"spacing", "5 mm"}};
        design->applyOps({opad::design::make_feature_op("cloud_slice", "Slice1", inputs)}, "Slice points", [st, require](bool ok, const QString& error) {
          if (!ok) require(false, "the slice was committed: " + error);
        });
        st->phase = 8;
        return;
      }
      case 8: {
        if ((busy || doc->scene.all_bodies().size() < 2) && st->clock.elapsed() < 90000) return;
        for (const auto& id : doc->scene.all_bodies())
          if (const opad::Node* n = doc->node(id); n && id != st->cloud && n->representation == "points") st->slice = id;
        size_t sliced = 0;
        if (!st->slice.empty()) sliced = opad::cloud::point_count(opad::body_shape(doc->doc, doc->node(st->slice)->body_key));
        end(QStringLiteral("%1 points in the slice").arg(sliced));
        require(!st->slice.empty() && sliced > 100, QStringLiteral("a slice of %1 points").arg(sliced));
        if (st->slice.empty()) return finish();
        view->selectNodes({st->slice});
        st->phase = 9;
        return;
      }
      case 9: {
        // The selection reaches the window from the view's selection job: the command reads it there.
        bool selected = false;
        for (const auto& id : w.currentNodeIds()) selected = selected || id == st->slice;
        if ((busy || !selected) && st->clock.elapsed() < 10000) return;
        begin("Slice to sketch");
        w.action("cloud.sliceSketch")->trigger();
        st->phase = 10;
        return;
      }
      case 10: {
        if ((busy || doc->scene.sketches.size() == st->sketches) && st->clock.elapsed() < 60000) return;
        const bool made = doc->scene.sketches.size() > st->sketches;
        size_t lines = 0;
        if (made)
          for (const auto& e : doc->scene.sketches.back().geometry.value("entities", opad::json::array())) lines += e.value("type", "") == "line";
        end(QStringLiteral("%1 lines").arg(lines));
        require(made && lines >= 8 && lines <= 1000, QStringLiteral("the slice is a sketch of %1 lines (a usable number)").arg(lines));
        trace::log(QStringLiteral("bench: cloud: %1").arg(trace::stallHistogram()));
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
