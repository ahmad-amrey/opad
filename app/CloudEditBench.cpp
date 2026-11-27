// OPAD_BENCH_CLOUDEDIT=<prefix> on a document holding a big point cloud (tools/bench_cases/cloud.py: 20 million points,
// embedded: a linked file's parts are read-only): Edit point cloud entered on it (its tab and panel shown, the editor prepared on a worker), a
// Push/Pull drag, a Delete brush drag, Add points (a click, then a drag), Paint select then Del, a Box select then Delete
// selected, Undo and Redo inside the mode, Finish (one cloud_edit step), then the document's Undo and Redo of that step.
// The drags go through the viewport's own mouse events one move per tick, and each checks the stroke drawn over the cloud
// while it runs, then the cloud drawn as edited once it ends. Every step is timed with the UI watchdog's longest stall
// since it began (trace::stalls), which must stay under 250 ms. "bench: cloud edit: <step> ... PASS/FAIL".
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QTimer>

#include <memory>

#include "BenchRegistry.hpp"
#include "BodyShape.hpp"
#include "CloudEditArea.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Ribbon.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"

namespace {
constexpr qint64 kStallLimitMs = 250;
}

OPAD_BENCH(OPAD_BENCH_CLOUDEDIT, cloudedit) {
  CloudEditArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* c = dynamic_cast<CloudEditArea*>(a)) area = c;
  if (!area) return false;
  struct State {
    int phase = 0, ticks = 0, settled = 0, after = 0, drag = 0;
    bool all = true;
    std::string cloud;
    size_t points = 0, drawn = 0, strokeSeen = 0, moved = 0, deleted = 0, added = 0, step = 0, features = 0;
    QList<QPointF> path;
    QString step_;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  auto* timer = new QTimer(&w);
  timer->setInterval(20);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, area] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: cloud edit: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    auto begin = [st](const QString& step) {
      st->step_ = step;
      trace::resetStalls();
      st->clock.start();
    };
    auto end = [st, require](const QString& extra = {}) {
      const trace::Stalls s = trace::stalls();
      require(s.longest < kStallLimitMs, QStringLiteral("%1: %2 ms, longest stall %3 ms (%4 stalls)%5")
                                             .arg(st->step_).arg(st->clock.elapsed()).arg(s.longest).arg(s.count).arg(extra.isEmpty() ? QString() : ", " + extra));
    };
    auto send = [view](QEvent::Type type, const QPointF& at, Qt::MouseButtons buttons) {
      QMouseEvent e(type, at, view->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
      QCoreApplication::sendEvent(view, &e);
    };
    auto line = [view](double x0, double y0, double x1, double y1, int n) {
      QList<QPointF> out;
      for (int k = 0; k <= n; ++k) out << QPointF(view->width() * (x0 + (x1 - x0) * k / n), view->height() * (y0 + (y1 - y0) * k / n));
      return out;
    };
    // A drag through the view's events, one move a tick; the stroke drawn over the cloud is sampled along the way.
    auto startDrag = [st, send](const QList<QPointF>& path, int after) {
      st->path = path;
      st->drag = 1;
      st->strokeSeen = 0;
      st->after = after;
      send(QEvent::MouseButtonPress, path.front(), Qt::LeftButton);
      st->phase = 100;
    };
    auto count = [area](const char* key) { return area->summary().value(key, size_t(0)); };
    if (++st->ticks > 30000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->step_));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || area->working() || view->cloudDrawing();
    switch (st->phase) {
      case 0: {  // the cloud opened and drawn
        if (busy) return;
        for (const auto& id : doc->scene.all_bodies())
          if (const opad::Node* n = doc->node(id); n && n->representation == "points") st->cloud = id;
        if (st->cloud.empty() || view->benchCloudDrawn(st->cloud) == 0) {
          if (st->ticks > 7500) {
            require(false, "a point cloud is drawn");
            return finish();
          }
          return;
        }
        if (++st->settled < 10) return;
        const auto prs = view->cloudPrs(st->cloud);
        st->points = prs ? prs->cloudData->size() : 0;
        require(st->points >= 1000000, QStringLiteral("the cloud has %1 points").arg(st->points));
        view->fitAll();
        st->phase = 1;
        return;
      }
      case 1: {
        if (busy || view->cameraMoving()) return;
        view->benchRedraw();
        view->selectNodes({st->cloud});  // as a click on it would (its own highlight work is the selection's, not the mode's)
        st->settled = 0;
        st->phase = 16;
        return;
      }
      case 16: {
        if (busy || ++st->settled < 30) return;
        begin("entered");
        w.action("cloud.edit")->trigger();
        st->phase = 2;
        return;
      }
      case 2: {
        if ((!area->prepared() || busy) && st->clock.elapsed() < 120000) return;
        end(QStringLiteral("editor prepared for %1 points").arg(st->points));
        QWidget* panel = w.findChild<QWidget*>("cloudEditPanel");
        require(area->active() && area->prepared(), "the mode is entered and its editor prepared");
        require(w.m_ribbon->contextualTabShown("design.cloudEdit") && w.workspaceId() == "design", "its Edit points tab is shown in Design");
        require(panel && panel->isVisible(), "its panel is shown");
        require(w.action("cloud.edit.finish")->isEnabled() && !w.action("cloud.edit")->isEnabled(), "Finish is offered, Edit point cloud is not");
        if (!area->prepared()) return finish();
        st->drawn = view->benchCloudDrawn(st->cloud);
        area->setTool(CloudEditArea::Tool::Pull);
        area->setRadiusPixels(30);
        begin("Push/Pull drag");
        startDrag(line(0.4, 0.5, 0.6, 0.5, 30), 3);
        return;
      }
      case 100: {  // dragging
        if (st->drag < st->path.size()) {
          send(QEvent::MouseMove, st->path[st->drag++], Qt::LeftButton);
          st->strokeSeen = std::max(st->strokeSeen, view->benchCloudStroke(st->cloud));
          return;
        }
        send(QEvent::MouseButtonRelease, st->path.back(), Qt::NoButton);
        st->phase = st->after;
        return;
      }
      case 3: {
        if ((busy || !view->benchCloudEdited(st->cloud) || view->benchCloudStroke(st->cloud)) && st->clock.elapsed() < 60000) return;
        st->moved = count("moved");
        end(QStringLiteral("%1 points moved").arg(st->moved));
        require(st->strokeSeen > 0, QStringLiteral("the stroke is drawn over the cloud while dragging (%1 points)").arg(st->strokeSeen));
        require(st->moved > 100 && view->benchCloudEdited(st->cloud) && !view->benchCloudStroke(st->cloud), "the cloud is drawn as edited once the stroke ends");
        require(area->step() == 1 && w.action("edit.undo")->isEnabled(), "one stroke kept, Undo offered");
        area->setTool(CloudEditArea::Tool::Erase);
        st->drawn = view->benchCloudDrawn(st->cloud);
        begin("Delete brush drag");
        startDrag(line(0.45, 0.35, 0.55, 0.35, 12), 4);
        return;
      }
      case 4: {
        if ((busy || area->step() < 2 || !view->benchCloudEdited(st->cloud)) && st->clock.elapsed() < 60000) return;
        st->deleted = count("deleted");
        const size_t drawn = view->benchCloudDrawn(st->cloud);
        end(QStringLiteral("%1 points deleted, %2 drawn").arg(st->deleted).arg(drawn));
        require(st->strokeSeen > 0, "the points being deleted are shown while dragging");
        require(st->deleted > 100 && drawn < st->drawn, "the deleted points are no longer drawn");
        area->setTool(CloudEditArea::Tool::Add);
        begin("Add point (a click)");
        const QPointF at(view->width() * 0.5, view->height() * 0.65);
        send(QEvent::MouseButtonPress, at, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, at, Qt::NoButton);
        st->phase = 5;
        return;
      }
      case 5: {
        if ((busy || area->step() < 3) && st->clock.elapsed() < 60000) return;
        st->added = count("added");
        end(QStringLiteral("%1 added").arg(st->added));
        require(st->added == 1, "a click adds one point on the surface");
        begin("Add points (a drag)");
        startDrag(line(0.42, 0.65, 0.58, 0.65, 16), 6);
        return;
      }
      case 6: {
        if ((busy || area->step() < 4) && st->clock.elapsed() < 60000) return;
        const size_t added = count("added");
        end(QStringLiteral("%1 added").arg(added));
        require(added > st->added + 2, QStringLiteral("a drag adds a row of points (%1)").arg(added - st->added));
        st->added = added;
        area->setTool(CloudEditArea::Tool::Paint);
        begin("Paint select");
        startDrag(line(0.42, 0.75, 0.58, 0.75, 16), 7);
        return;
      }
      case 7: {
        if ((busy || area->selected() == 0) && st->clock.elapsed() < 60000) return;
        end(QStringLiteral("%1 selected").arg(area->selected()));
        require(st->strokeSeen > 0, "the painted points are shown while painting");
        require(area->selected() > 100 && area->step() == 4, "painting selects points and changes nothing yet");
        st->deleted = count("deleted");
        begin("Del deletes the selection");
        w.action("edit.delete")->trigger();
        st->phase = 8;
        return;
      }
      case 8: {
        if ((busy || area->step() < 5) && st->clock.elapsed() < 60000) return;
        const size_t deleted = count("deleted");
        end(QStringLiteral("%1 deleted").arg(deleted));
        require(deleted > st->deleted && area->selected() == 0, "Del deleted the selected points");
        st->deleted = deleted;
        area->setTool(CloudEditArea::Tool::Box);
        begin("Box select, Delete selected");
        startDrag(line(0.56, 0.56, 0.62, 0.62, 6), 9);
        return;
      }
      case 9: {
        if ((busy || area->selected() == 0) && st->clock.elapsed() < 60000) return;
        require(area->selected() > 0, QStringLiteral("a box selects points (%1)").arg(area->selected()));
        w.action("cloud.edit.deleteSelected")->trigger();
        st->phase = 10;
        return;
      }
      case 10: {
        if ((busy || area->step() < 6) && st->clock.elapsed() < 60000) return;
        const size_t deleted = count("deleted");
        end(QStringLiteral("%1 deleted").arg(deleted));
        require(deleted > st->deleted, "Delete selected deleted them");
        st->deleted = deleted;
        st->step = area->step();
        begin("Undo in the mode");
        w.action("edit.undo")->trigger();
        st->phase = 11;
        return;
      }
      case 11: {
        if ((busy || !view->benchCloudEdited(st->cloud)) && st->clock.elapsed() < 60000) return;
        end();
        require(area->step() == st->step - 1 && count("deleted") < st->deleted && area->active(), "Undo takes the last stroke back, the mode stays");
        begin("Redo in the mode");
        w.action("edit.redo")->trigger();
        st->phase = 12;
        return;
      }
      case 12: {
        if ((busy || !view->benchCloudEdited(st->cloud)) && st->clock.elapsed() < 60000) return;
        end();
        require(area->step() == st->step && count("deleted") == st->deleted, "Redo brings it back");
        st->features = doc->scene.features.size();
        st->added = count("added");
        begin("Finish");
        w.action("cloud.edit.finish")->trigger();
        st->phase = 13;
        return;
      }
      case 13: {
        if ((busy || area->active()) && st->clock.elapsed() < 180000) return;
        end();
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        const bool made = f && f->kind == "cloud_edit" && doc->scene.features.size() == st->features + 1;
        require(made && f->error.empty(), "Finish wrote one cloud_edit step" + (f && !f->error.empty() ? ": " + QString::fromStdString(f->error) : QString()));
        size_t after = 0;
        if (made)
          for (const auto& c : f->result.value("points", opad::json::array())) after = c.value("after", size_t(0));
        require(after == st->points - st->deleted + st->added, QStringLiteral("the edited cloud has %1 points (%2 - %3 + %4)").arg(after).arg(st->points).arg(st->deleted).arg(st->added));
        require(!w.m_ribbon->contextualTabShown("design.cloudEdit"), "the Edit points tab is gone");
        begin("the document's Undo of the step");
        doc->undo();
        st->phase = 14;
        return;
      }
      case 14: {
        if (busy && st->clock.elapsed() < 60000) return;
        end();
        require(doc->scene.features.size() == st->features, "Undo takes the whole edit back in one step");
        begin("the document's Redo");
        doc->redo();
        st->phase = 15;
        return;
      }
      case 15: {
        if (busy && st->clock.elapsed() < 60000) return;
        end();
        require(doc->scene.features.size() == st->features + 1 && doc->scene.features.back().kind == "cloud_edit", "Redo puts it back");
        trace::log(QStringLiteral("bench: cloud edit: %1").arg(trace::stallHistogram()));
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
