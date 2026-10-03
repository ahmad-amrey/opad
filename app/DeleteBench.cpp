#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SmartSelect.hpp"
#include "opad/geometry.hpp"
#include <QMenu>
#include <QToolButton>

// OPAD_BENCH_DELETE=<prefix> (TODO 11 UI-04, Del routing; cases delete-design and delete-import in
// tools/bench_cases/smart.py). Del takes out what the selection covers and nothing more, as one step with an Undo toast
// instead of a question.
//   A designed box: one picked face does not tombstone the box, it opens smart selection's menu; all six faces are the
//   box feature: Del deletes it (nothing uses it), Undo from the toast; the body picked whole goes to a Remove feature,
//   the box feature stays, Undo from the toast.
//   An import of two bodies, no history: one body goes to a Remove feature (the other stays, the import is not
//   tombstoned); both are the whole import: it is tombstoned; Undo from the toast each time.
OPAD_BENCH(OPAD_BENCH_DELETE, deleterouting) {
  struct State {
    int phase = 0, ticks = 0, wait = 0;
    size_t ops = 0;
    std::vector<std::string> bodies;
    std::string source;
    QString name;  // the first body's, read while it is there
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
      if (++state->ticks > 1200) throw opad::Error("timed out in phase " + std::to_string(state->phase));
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
