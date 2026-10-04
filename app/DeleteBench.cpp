#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SmartSelect.hpp"
#include "opad/design/sketch.hpp"
#include "opad/geometry.hpp"
#include "opad/util.hpp"
#include <QElapsedTimer>
#include <QMenu>
#include <QToolButton>

// OPAD_BENCH_DELETE=<prefix> (TODO 11 UI-04, Del routing; cases delete-design and delete-import in
// tools/bench_cases/smart.py). Del takes out what the selection covers and nothing more, as one step with an Undo toast
// instead of a question.
//   A designed box: one picked face does not tombstone the box, it opens smart selection's menu; all six faces are the
//   box feature: Del deletes it (nothing uses it), Undo from the toast; the body picked whole goes to a Remove feature,
//   the box feature stays, Undo from the toast. A sketch with a peg extruded from it: the sketch picked in the browser and
//   deleted asks about the peg with the result previewed (UI-96, as from its marker); cancelled, nothing changes.
//   An import of two bodies, no history: one body goes to a Remove feature (the other stays, the import is not
//   tombstoned); both are the whole import: it is tombstoned; Undo from the toast each time.
//   Restore (edit.restore) on the step that took things out (TODO 11 help audit P9.2), as its guide shows: on the box's
//   Remove step and on the import's delete step it brings them back as one new step, and Undo takes that back.
//   A big model (the Engine, case delete-engine): one body goes to a Remove feature and comes back with Undo, with no
//   event-loop gap over 250 ms.
OPAD_BENCH(OPAD_BENCH_DELETE, deleterouting) {
  struct State {
    int phase = 0, ticks = 0, wait = 0;
    size_t ops = 0;
    std::vector<std::string> bodies;
    std::string source, sketch;
    QString name;  // the first body's, read while it is there
    QElapsedTimer clock, tick;
    qint64 gap = 0;
    size_t tombstones = 0;
    std::string step;  // the Remove or delete step Restore is tried on
  };
  auto state = std::make_shared<State>();
  SmartSelect* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* smart = dynamic_cast<SmartSelect*>(a)) area = smart;
  if (!area) {
    trace::log("bench: delete FAIL: the smart selection area is off");
    QCoreApplication::exit(2);
    return true;
  }
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, area, state, timer, prefix = value] {
    try {
      if (state->tick.isValid()) state->gap = std::max(state->gap, state->tick.restart());
      if (++state->ticks > 3000) throw opad::Error("timed out in phase " + std::to_string(state->phase));
      if (w.m_doc->loading || w.m_doc->designBusy || w.m_doc->snapshotBusy() || w.m_jobs->busy() || area->busy()) return;
      auto require = [](bool ok, const std::string& why) {
        if (!ok) throw opad::Error(why);
      };
      auto pass = [](const QString& what) { trace::log("bench: delete: " + what + " PASS"); };
      auto waitFor = [&](bool ok, const std::string& why) {
        if (ok) {
          state->wait = 0;
          return true;
        }
        require(++state->wait < 80, why);
        return false;
      };
      auto pickBodies = [&](const std::vector<std::string>& ids) {  // as from the browser
        w.m_browser->setSelectedIds(ids);
        w.onBrowserSelection(ids);
      };
      auto lastToast = [&]() -> Toast* { return w.m_toasts->toasts().isEmpty() ? nullptr : w.m_toasts->toasts().back(); };
      auto undoFromToast = [&](const QString& text) {
        Toast* toast = lastToast();
        require(toast && toast->text() == text && toast->actionButton(), "the toast says \"" + text.toStdString() + "\" with Undo (" + (toast ? toast->text().toStdString() : "none") + ")");
        toast->actionButton()->click();
      };
      const bool designed = !w.m_doc->scene.features.empty();
      switch (state->phase) {
        case 0:
          state->bodies = w.m_doc->scene.all_bodies();
          require(!state->bodies.empty(), "bodies");
          state->source = w.m_doc->node(state->bodies.front())->source_op;
          state->name = w.m_doc->nodeName(state->bodies.front());
          state->ops = w.m_doc->doc.ops.size();
          state->tombstones = w.m_doc->scene.deleted_ops.size();
          if (state->bodies.size() > 2) {  // a big import (with a sketch or not): one body out and back, timed
            state->phase = 30;
            return;
          }
          if (!designed) {
            require(state->bodies.size() == 2 && w.m_doc->node(state->bodies.back())->source_op == state->source, "two bodies of one import");
            state->phase = 20;
            return;
          }
          w.setWorkspace("design");
          w.action("select.faces")->trigger();
          break;
        // ---- a designed box
        case 1: {
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Face) return;
          opad::Ref face = opad::Ref::parse(state->bodies.front() + "/face/0");
          w.m_viewport->selectRefs({face});
          w.onViewportSelection();
          w.action("edit.delete")->trigger();
          break;
        }
        case 2: {
          QMenu* menu = area->openMenu();
          if (!waitFor(menu && menu->isVisible(), "Del on a face opens smart selection's menu")) return;
          require(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.deleted_ops.empty(), "Del on a face tombstones nothing");
          menu->grab().save(prefix + ".face.png");
          pass("Del on one face of the box opened smart selection's menu and tombstoned nothing");
          menu->close();
          // All six: the box feature's whole face set.
          std::vector<opad::Ref> all;
          for (int i = 0; i < 6; ++i) all.push_back(opad::Ref::parse(state->bodies.front() + "/face/" + std::to_string(i)));
          w.m_viewport->selectRefs(all);
          w.onViewportSelection();
          w.action("edit.delete")->trigger();
          break;
        }
        case 3: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Del on the box's six faces deletes the box feature")) return;
          require(w.m_doc->scene.all_bodies().empty() && w.m_doc->scene.deleted_ops.size() == 1, "the box feature is tombstoned");
          undoFromToast(SmartSelect::tr("Deleted %1").arg(QString::fromStdString(w.m_doc->doc.find_op(w.m_doc->scene.deleted_ops.front())->data.value("name", ""))));
          break;
        }
        case 4:
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.all_bodies() == state->bodies, "the toast's Undo brings the box back")) return;
          pass("Del on all six faces deleted the box feature; the toast's Undo brought it back");
          w.action("select.bodies")->trigger();
          break;
        case 5:
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Body) return;
          pickBodies(state->bodies);
          w.action("edit.delete")->trigger();
          break;
        case 6: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Del on the body appends a Remove feature")) return;
          const auto& features = w.m_doc->scene.features;
          require(features.back().kind == "remove" && w.m_doc->scene.all_bodies().empty(), "a Remove feature took the body out");
          require(w.m_doc->scene.deleted_ops.empty() && w.m_doc->scene.feature(state->source), "the box feature stays");
          undoFromToast(QObject::tr("Removed %1: a Remove step on the timeline keeps its history").arg(state->name));
          break;
        }
        case 7:
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.all_bodies() == state->bodies, "Undo brings the body back")) return;
          pass("Del on the body appended a Remove feature (the box feature stays); the toast's Undo brought it back");
          pickBodies(state->bodies);
          w.action("edit.delete")->trigger();
          break;
        case 8:
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops && w.m_doc->scene.all_bodies().empty(), "Del on the body again")) return;
          require(w.m_doc->scene.features.back().kind == "remove", "a Remove step again");
          state->step = w.m_doc->scene.features.back().id;
          w.m_timeline->setCurrentOp(state->step);
          w.action("edit.restore")->trigger();
          break;
        case 9:
          if (!waitFor(w.m_doc->doc.is_deleted(state->step) && w.m_doc->scene.all_bodies() == state->bodies, "Restore on the Remove step brings the body back")) return;
          require(w.m_doc->doc.ops.size() == state->ops + 2, "the restore is one new step");
          pass("Restore on the Remove step (its marker selected) brought the body back as one new step");
          w.action("edit.undo")->trigger();
          break;
        case 10:
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops + 1 && w.m_doc->scene.all_bodies().empty(), "Undo takes the restore back")) return;
          w.action("edit.undo")->trigger();
          break;
        case 11: {
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.all_bodies() == state->bodies, "Undo takes the Remove step back")) return;
          pass("Undo took the restore back, then the Remove step");
          opad::design::Sketch square;
          const int a = square.add_point(40, 0), b = square.add_point(46, 0), c = square.add_point(46, 6), d = square.add_point(40, 6);
          square.add_line(a, b);
          square.add_line(b, c);
          square.add_line(c, d);
          square.add_line(d, a);
          opad::json sketch = opad::design::make_sketch_op("Profile", {{"base", "xy"}}, square.to_json());
          sketch["id"] = state->sketch = opad::new_uuid();
          const opad::json profiles = opad::json::array({{{"sketch", state->sketch}, {"all", true}}});
          w.m_design->applyOps({sketch, opad::design::make_feature_op("extrude", "Peg", {{"profiles", profiles}, {"distance", "5 mm"}, {"operation", "new"}})}, "bench peg");
          break;
        }
        case 12: {
          const opad::Feature* peg = nullptr;
          for (const auto& f : w.m_doc->scene.features)
            if (f.name == "Peg") peg = &f;
          if (!waitFor(peg && w.m_doc->scene.sketch(state->sketch), "a sketch and a peg extruded from it")) return;
          require(peg->error.empty(), "the peg is made");
          state->ops = w.m_doc->doc.ops.size();
          pickBodies({state->sketch});
          w.action("edit.delete")->trigger();
          break;
        }
        case 13: {
          QMenu* question = area->openMenu();
          if (!waitFor(question && question->isVisible() && question->objectName() == "smartDeleteQuestion", "Del on the sketch in the browser asks about the peg")) return;
          QAction* all = question->findChild<QAction*>("deleteWithDependents");
          require(all && all->text() == SmartSelect::tr("Delete %1 and %2").arg("Profile", "Peg") && question->findChild<QAction*>("deleteOnly"), "the question names the peg");
          require(w.m_doc->doc.ops.size() == state->ops, "nothing committed while it asks");
          question->grab().save(prefix + ".sketch.png");
          pass("Del on the sketch picked in the browser asks \"" + all->text() + "\" as from its marker");
          question->close();
          break;
        }
        case 14:
          if (!waitFor(!area->openMenu() || !area->openMenu()->isVisible(), "the question closes")) return;
          require(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.sketch(state->sketch), "cancelled: the sketch stays");
          pass("the question closed without a choice: nothing deleted");
          timer->stop();
          QCoreApplication::exit(0);
          return;
        // ---- an import of two bodies
        case 20:
          pickBodies({state->bodies.front()});
          w.action("edit.delete")->trigger();
          break;
        case 21: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Del on one imported body appends a Remove feature")) return;
          require(w.m_doc->scene.all_bodies() == std::vector<std::string>{state->bodies.back()}, "the other body stays");
          require(w.m_doc->scene.deleted_ops.empty() && w.m_doc->scene.features.back().kind == "remove", "the import is not tombstoned, a Remove feature is");
          undoFromToast(QObject::tr("Removed %1: a Remove step on the timeline keeps its history").arg(state->name));
          break;
        }
        case 22:
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.all_bodies().size() == 2, "Undo brings the body back")) return;
          pass("Del on one body of a two-body import removed it alone (Remove feature), the import stays; Undo from the toast");
          pickBodies(state->bodies);
          w.action("edit.delete")->trigger();
          break;
        case 23: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Del on both bodies tombstones the import")) return;
          const auto& deleted = w.m_doc->scene.deleted_ops;
          require(deleted == std::vector<std::string>{state->source} && w.m_doc->scene.features.empty() && w.m_doc->scene.all_bodies().empty(), "the import is tombstoned, nothing else");
          undoFromToast(QObject::tr("Deleted %1").arg(QObject::tr("%1 objects").arg(2)));
          break;
        }
        case 24:
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.all_bodies().size() == 2, "Undo brings the import back")) return;
          pass("Del on every body of the import tombstoned the import; Undo from the toast");
          pickBodies(state->bodies);
          w.action("edit.delete")->trigger();
          break;
        case 25: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops && w.m_doc->scene.all_bodies().empty(), "Del on both bodies again")) return;
          const opad::Op& last = w.m_doc->doc.ops.back();
          require(last.type == "delete" && last.data.value("target", "") == state->source, "a delete step on the import");
          state->step = last.id;
          w.m_timeline->setCurrentOp(state->step);
          w.action("edit.restore")->trigger();
          break;
        }
        case 26:
          if (!waitFor(w.m_doc->doc.is_deleted(state->step) && w.m_doc->scene.all_bodies().size() == 2, "Restore on the delete step brings the import back")) return;
          require(w.m_doc->doc.ops.size() == state->ops + 2 && !w.m_doc->doc.is_deleted(state->source), "the restore is one new step, the import live again");
          pass("Restore on the delete step (its marker selected) brought the import back as one new step");
          timer->stop();
          QCoreApplication::exit(0);
          return;
        // ---- a big import
        case 30:
          state->tick.start();
          state->clock.start();
          pickBodies({state->bodies.front()});
          w.action("edit.delete")->trigger();
          break;
        case 31: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Del on one body of the big import appends a Remove feature")) return;
          require(w.m_doc->scene.deleted_ops.size() == state->tombstones && !w.m_doc->scene.features.empty() && w.m_doc->scene.features.back().kind == "remove",
                  "a Remove feature, no tombstone");
          require(w.m_doc->scene.all_bodies().size() == state->bodies.size() - 1 && !w.m_doc->node(state->bodies.front()), "that body alone is removed");
          pass(QString("Del on one of %1 imported bodies: Remove feature committed in %2 ms").arg(state->bodies.size()).arg(state->clock.elapsed()));
          state->clock.start();
          undoFromToast(QObject::tr("Removed %1: a Remove step on the timeline keeps its history").arg(state->name));
          break;
        }
        case 32:
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.all_bodies().size() == state->bodies.size(), "Undo brings the body back")) return;
          pass(QString("Undo from the toast in %1 ms; longest event-loop gap %2 ms").arg(state->clock.elapsed()).arg(state->gap));
          require(state->gap <= 250, "the event loop waited " + std::to_string(state->gap) + " ms");
          timer->stop();
          QCoreApplication::exit(0);
          return;
      }
      ++state->phase;
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: delete FAIL: %1").arg(e.what()));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
