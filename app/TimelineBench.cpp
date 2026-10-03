#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QToolButton>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SmartSelect.hpp"
#include "Theme.hpp"
#include "TimelineArea.hpp"
#include "Toast.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/provenance.hpp"
#include "opad/geometry.hpp"

// OPAD_BENCH_TIMELINE=<prefix> (TODO 11 UI-99; case timeline in tools/bench_cases/smart.py) on the 40 mm base with a 10 mm
// boss joined on top; the bench rounds the boss's top edges (Round), renames the body and colours it first. The pointer on
// a marker (mouse events into the timeline): Boss's five faces in the candidate amber, Round's four, the rename's body
// tinted whole, nothing once it leaves. A click on Round's marker selects its four faces with its actions on the chip, on
// Base's (it made the body) the body, Right from there Boss's faces; Ctrl+C on the timeline copies the marker's op id (the
// clipboard is given back). Names on the markers make them wider; the design history alone hides the rename and the
// colour. Roll back to here on Boss: the round is not there, the playhead between the two markers, the chip says so; the
// playhead dragged to the end rolls forward, dragged before Boss rolls back past it; a change made then is appended and
// rolls forward, so does a command that edits. The menu on no marker offers the view entries. UI-96 from the timeline:
// Delete on Boss's marker asks about Round with the result previewed and Remove its faces instead, Delete Boss only leaves
// Round failing as one step undone from the toast; Round's marker, used by nothing, deletes at once. Shots:
// <prefix>.hover.png, .names.png, .design.png, .rolledback.png, .menu.png, .question.png.
OPAD_BENCH(OPAD_BENCH_TIMELINE, timeline) {
  struct State {
    int phase = 0, ticks = 0, wait = 0;
    std::string body, base, boss, round, rename, colour;
    std::vector<opad::Ref> bossFaces, roundFaces;
    std::unique_ptr<QMimeData> clipboard;
    size_t ops = 0;
  };
  auto state = std::make_shared<State>();
  SmartSelect* area = nullptr;
  TimelineArea* timelineArea = nullptr;
  for (AreaController* a : w.m_areas) {
    if (auto* smart = dynamic_cast<SmartSelect*>(a)) area = smart;
    if (auto* t = dynamic_cast<TimelineArea*>(a)) timelineArea = t;
  }
  if (!area || !timelineArea) {
    trace::log("bench: timeline FAIL: the smart selection or timeline area is off");
    QCoreApplication::exit(2);
    return true;
  }
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, area, timelineArea, state, timer, prefix = value] {
    TimelineWidget* t = w.m_timeline;
    try {
      if (++state->ticks > 1200) throw opad::Error("timed out in phase " + std::to_string(state->phase));
      if (w.m_doc->loading || w.m_doc->designBusy || w.m_doc->snapshotBusy() || w.m_jobs->busy() || area->busy()) return;
      auto require = [](bool ok, const std::string& why) {
        if (!ok) throw opad::Error(why);
      };
      auto pass = [](const QString& what) { trace::log("bench: timeline: " + what + " PASS"); };
      auto waitFor = [&](bool ok, const std::string& why) {
        if (ok) {
          state->wait = 0;
          return true;
        }
        require(++state->wait < 80, why);
        return false;
      };
      auto mouse = [&](QEvent::Type type, const QPoint& at, Qt::MouseButton button = Qt::NoButton) {
        const Qt::MouseButtons buttons = type == QEvent::MouseButtonPress || (type == QEvent::MouseMove && button != Qt::NoButton) ? Qt::MouseButtons(Qt::LeftButton) : Qt::NoButton;
        QMouseEvent e(type, QPointF(at), QPointF(t->mapToGlobal(at)), type == QEvent::MouseMove ? Qt::NoButton : button, buttons, Qt::NoModifier);
        QApplication::sendEvent(t, &e);
      };
      auto hover = [&](const std::string& op) { mouse(QEvent::MouseMove, t->markerAt(op).center()); };
      auto click = [&](const std::string& op) {
        mouse(QEvent::MouseMove, t->markerAt(op).center());
        mouse(QEvent::MouseButtonPress, t->markerAt(op).center(), Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, t->markerAt(op).center(), Qt::LeftButton);
      };
      auto drag = [&](int x) {  // the playhead, to x
        const QPoint from = t->playhead().center();
        mouse(QEvent::MouseMove, from);
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
        mouse(QEvent::MouseMove, QPoint((from.x() + x) / 2, from.y()), Qt::LeftButton);
        mouse(QEvent::MouseMove, QPoint(x, from.y()), Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, QPoint(x, from.y()), Qt::LeftButton);
      };
      auto selected = [&](const std::vector<opad::Ref>& refs) { return smart::sameRefs(w.m_viewport->selection(), refs); };
      const auto amber = theme::current().candidate;
      auto tinted = [&] {
        const auto c = w.m_viewport->bodyLook(state->body).color;
        return std::abs(c[0] - amber.redF()) < 1e-3 && std::abs(c[1] - amber.greenF()) < 1e-3 && std::abs(c[2] - amber.blueF()) < 1e-3;
      };
      switch (state->phase) {
        case 0: {
          require(w.m_doc->scene.all_bodies().size() == 1, "one body");
          state->body = w.m_doc->scene.all_bodies().front();
          for (const auto& f : w.m_doc->scene.features) {
            if (f.name == "Base") state->base = f.id;
            if (f.name == "Boss") state->boss = f.id;
          }
          require(!state->base.empty() && !state->boss.empty(), "the fixture's Base and Boss");
          TopTools_IndexedMapOfShape edges;
          TopExp::MapShapes(opad::node_world_shape(w.m_doc->doc, w.m_doc->scene, state->body), TopAbs_EDGE, edges);
          opad::json top = opad::json::array();
          for (int i = 1; i <= edges.Extent(); ++i) {
            BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
            if (std::abs(c.Value(c.FirstParameter()).Z() - 20) < 1e-6 && std::abs(c.Value(c.LastParameter()).Z() - 20) < 1e-6) top.push_back(state->body + "/edge/" + std::to_string(i - 1));
          }
          require(top.size() == 4, "four top edges on the boss");
          w.m_design->applyOps({opad::design::make_feature_op("fillet", "Round", {{"edges", top}, {"radius", "2 mm"}})}, "fillet");
          break;
        }
        case 1: {
          for (const auto& f : w.m_doc->scene.features)
            if (f.name == "Round") state->round = f.id;
          require(!state->round.empty() && w.m_doc->scene.feature(state->round)->error.empty(), "Round applied");
          state->rename = w.m_doc->run("rename", opad::json{{"target", state->body}, {"name", "Part"}}).value("id", "");
          state->colour = w.m_doc->run("appearance", opad::json{{"target", state->body}, {"color", {0.2, 0.5, 0.8}}}).value("id", "");
          require(!state->rename.empty() && !state->colour.empty(), "a rename and a colour step");
          opad::design::Provenance provenance(w.m_doc->doc);
          const auto owners = provenance.face_owners(state->body);
          for (size_t i = 0; i < owners.size(); ++i) {
            const opad::Ref r = opad::Ref::parse(state->body + "/face/" + std::to_string(i));
            if (owners[i].op == state->boss) state->bossFaces.push_back(r);
            if (owners[i].op == state->round) state->roundFaces.push_back(r);
          }
          require(state->bossFaces.size() == 5 && state->roundFaces.size() == 4, "the boss owns 5 faces, Round 4");
          require(t->markerCount() == 5 && !t->showNames() && !t->designOnly(), "five markers, icons only, every step");
          w.setWorkspace("design");
          w.m_viewport->standardView("iso");
          hover(state->boss);
          break;
        }
        case 2: {
          if (!waitFor(w.m_viewport->candidateRefsShown() == 5, "the pointer on Boss's marker draws its five faces (" + std::to_string(w.m_viewport->candidateRefsShown()) + ")")) return;
          w.m_viewport->grabImage().save(prefix + ".hover.png");
          pass("the pointer on Boss's marker draws its five faces in amber");
          hover(state->round);
          break;
        }
        case 3: {
          if (!waitFor(w.m_viewport->candidateRefsShown() == 4, "the pointer on Round's marker draws its four faces")) return;
          pass("on Round's marker, its four faces");
          hover(state->rename);
          break;
        }
        case 4: {
          if (!waitFor(tinted() && w.m_viewport->candidateRefsShown() == 0 && !w.m_viewport->looksPending(), "the pointer on the rename's marker tints the body")) return;
          pass("on the rename's marker, the body it renamed tinted whole");
          QEvent leave(QEvent::Leave);
          QApplication::sendEvent(t, &leave);
          break;
        }
        case 5: {
          if (!waitFor(!tinted() && w.m_viewport->candidateRefsShown() == 0, "leaving the timeline clears the amber")) return;
          pass("off the markers, nothing in amber");
          click(state->round);
          break;
        }
        case 6: {
          if (!waitFor(selected(state->roundFaces) && area->found().active >= 0 && area->chip()->isVisible(), "a click on Round's marker selects its four faces")) return;
          QStringList buttons;
          for (QToolButton* b : area->chip()->actionButtons()) buttons << b->defaultAction()->objectName();
          require(buttons.contains("smartDelete") && buttons.contains("smartEdit") && area->chip()->text() == SmartSelect::tr("%1 · %2 faces").arg("Round").arg(4),
                  "the chip names Round with its actions: " + area->chip()->text().toStdString() + " " + buttons.join(",").toStdString());
          pass("a click on Round's marker selects the four faces it made, the chip carries Round's actions");
          click(state->base);
          break;
        }
        case 7: {
          if (!waitFor(w.m_viewport->selection().size() == 1 && w.m_viewport->selection().front().kind == opad::Ref::Kind::Body && w.m_viewport->selection().front().body == state->body,
                       "a click on Base's marker selects the body")) return;
          pass("Base made the body: a click on its marker selects the body");
          QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
          QApplication::sendEvent(t, &right);
          break;
        }
        case 8: {
          if (!waitFor(t->currentOp() == state->boss && selected(state->bossFaces), "Right on the timeline goes to Boss and selects its faces")) return;
          pass("Right on the timeline steps to Boss's marker and selects its five faces");
          // Ctrl+C: the marker's op id (what was on the clipboard is put back).
          state->clipboard = std::make_unique<QMimeData>();
          if (const QMimeData* was = QGuiApplication::clipboard()->mimeData())
            for (const QString& format : was->formats()) state->clipboard->setData(format, was->data(format));
          QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier);
          QApplication::sendEvent(t, &override);
          require(override.isAccepted(), "the timeline takes Ctrl+C before the window's shortcuts");
          QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
          QApplication::sendEvent(t, &copy);
          const QString copied = QGuiApplication::clipboard()->text();
          QGuiApplication::clipboard()->setMimeData(state->clipboard.release());
          require(copied == QString::fromStdString(state->boss), "Ctrl+C copied \"" + copied.toStdString() + "\"");
          pass("Ctrl+C on the timeline copies the marker's op id");
          const int narrow = t->markerAt(state->boss).width();
          w.action("timeline.names")->trigger();
          require(t->showNames() && t->markerAt(state->boss).width() > narrow + 20, "names make the markers wider");
          t->grab().save(prefix + ".names.png");
          pass(QString("names on the markers: Boss's marker %1 px wide, was %2").arg(t->markerAt(state->boss).width()).arg(narrow));
          w.action("timeline.designOnly")->trigger();
          require(t->designOnly() && t->markerCount() == 3 && t->markerAt(state->rename).isNull() && t->markerAt(state->colour).isNull(), "the design history alone: three markers");
          t->grab().save(prefix + ".design.png");
          pass("the design history alone hides the rename and the colour");
          w.action("timeline.designOnly")->trigger();
          w.action("timeline.names")->trigger();
          require(!t->designOnly() && !t->showNames() && t->markerCount() == 5, "back to every step, icons only");
          // Roll back to here, on Boss: the model as it was right after it.
          QMenu menu;
          w.buildTimelineMenu(menu, state->boss);
          QAction* back = menu.findChild<QAction*>("timelineRollBack");
          require(back && back->isEnabled(), "the marker's menu offers Roll back to here");
          back->trigger();
          break;
        }
        case 9: {
          if (!waitFor(w.m_doc->rolledBack(), "Roll back to here rolls back")) return;
          const QRect boss = t->markerAt(state->boss), round = t->markerAt(state->round), head = t->playhead();
          require(w.m_doc->rollback() == state->round && w.m_doc->scene.feature(state->boss) && !w.m_doc->scene.feature(state->round), "rolled back before Round");
          require(head.center().x() > boss.right() && head.center().x() < round.left(), "the playhead between Boss and Round");
          require(timelineArea->chip() && timelineArea->chip()->isVisibleTo(w.m_chips) && w.action("timeline.rollForward")->isEnabled(), "the chip and Roll forward say it is rolled back");
          t->grab().save(prefix + ".rolledback.png");
          pass("Roll back to here on Boss: Round is not there, the playhead between them, the chip \"" + timelineArea->chip()->text() + "\"");
          drag(t->width() - 80);
          break;
        }
        case 10: {
          if (!waitFor(!w.m_doc->rolledBack() && w.m_doc->rollback().empty() && w.m_doc->scene.feature(state->round), "the playhead dragged to the end rolls forward")) return;
          require(!timelineArea->chip()->isVisibleTo(w.m_chips), "the chip goes");
          pass("the playhead dragged to the end rolls forward");
          drag(t->markerAt(state->boss).left() - 4);
          break;
        }
        case 11: {
          if (!waitFor(w.m_doc->rollback() == state->boss && !w.m_doc->scene.feature(state->boss), "the playhead dragged before Boss rolls back past it")) return;
          pass("the playhead dragged before Boss: the model without the boss");
          const size_t ops = w.m_doc->doc.ops.size();
          w.m_doc->run("rename", opad::json{{"target", state->body}, {"name", "Part 2"}});
          require(w.m_doc->doc.ops.size() == ops + 1 && !w.m_doc->rolledBack() && w.m_doc->scene.feature(state->round), "a change made while rolled back is appended and rolls forward");
          pass("a change made while rolled back is appended at the end and rolls forward");
          require(timelineArea->rollTo(state->round) && w.m_doc->rolledBack(), "rolled back again");
          w.action("design.chamfer")->trigger();  // a command that edits
          require(!w.m_doc->rolledBack() && w.m_design->featureActive(), "a command that edits rolls forward first");
          pass("a command that edits rolls forward before it starts");
          w.m_design->escape();
          break;
        }
        case 12: {
          if (!waitFor(!w.m_design->featureActive(), "Esc leaves the chamfer")) return;
          QMenu menu;
          w.buildTimelineMenu(menu, std::string());
          QStringList entries;
          for (QAction* a : menu.actions())
            if (!a->isSeparator()) entries << a->objectName();
          require(entries == QStringList{"timeline.names", "timeline.designOnly"}, "on no marker the menu offers the view entries: " + entries.join(",").toStdString());
          menu.grab().save(prefix + ".menu.png");
          pass("the menu on no marker: names and the design history");
          // UI-96 from the timeline: Delete on Boss's marker asks about Round, as Delete on its faces does.
          state->ops = w.m_doc->doc.ops.size();
          QMenu marker;
          w.buildTimelineMenu(marker, state->boss);
          marker.findChild<QAction*>("timelineDelete")->trigger();
          break;
        }
        case 13: {
          QMenu* question = area->openMenu();
          if (!waitFor(question && question->isVisible() && question->objectName() == "smartDeleteQuestion", "Delete on Boss's marker asks about Round")) return;
          QAction* all = question->findChild<QAction*>("deleteWithDependents");
          QAction* only = question->findChild<QAction*>("deleteOnly");
          require(all && all->text() == SmartSelect::tr("Delete %1 and %2").arg("Boss", "Round") && only, "the question names Round");
          require(question->findChild<QAction*>("deleteFacesInstead"), "it offers removing Boss's faces instead (found from its marker)");
          require(w.m_viewport->previewBodyCount() > 0 && w.m_doc->doc.ops.size() == state->ops, "the result previewed, nothing committed yet");
          question->grab().save(prefix + ".question.png");
          pass("Delete on Boss's marker asks \"" + all->text() + "\" with the result previewed");
          only->trigger();
          question->close();
          break;
        }
        case 14: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Delete Boss only is committed")) return;
          const auto& deleted = w.m_doc->scene.deleted_ops;
          const opad::Feature* round = w.m_doc->scene.feature(state->round);
          require(std::count(deleted.begin(), deleted.end(), state->boss) && !std::count(deleted.begin(), deleted.end(), state->round), "Boss tombstoned, Round kept");
          require(round && !round->error.empty(), "Round fails without the boss and says so");
          require(w.m_doc->undoLabel() == SmartSelect::tr("delete %1").arg("Boss") && w.m_viewport->previewBodyCount() == 0, "one undo step, the preview gone");
          Toast* toast = w.m_toasts->toasts().isEmpty() ? nullptr : w.m_toasts->toasts().back();
          require(toast && toast->text() == SmartSelect::tr("Deleted %1").arg("Boss") && toast->actionButton(), "a toast with Undo");
          pass("Delete Boss only: the boss is tombstoned as one step, Round is marked as failing; the toast offers Undo");
          toast->actionButton()->click();
          break;
        }
        case 15: {
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops && w.m_doc->scene.feature(state->boss), "the toast's Undo brings the boss back")) return;
          require(w.m_doc->scene.feature(state->round)->error.empty(), "Round works again");
          pass("the toast's Undo brought the boss back, Round works again");
          QMenu marker;
          w.buildTimelineMenu(marker, state->round);
          marker.findChild<QAction*>("timelineDelete")->trigger();
          break;
        }
        case 16: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Round, used by nothing, is deleted from its marker")) return;
          require(!area->openMenu() && std::count(w.m_doc->scene.deleted_ops.begin(), w.m_doc->scene.deleted_ops.end(), state->round), "no question, Round tombstoned");
          pass("Round, which nothing uses, is deleted from its marker without a question");
          timer->stop();
          QCoreApplication::exit(0);
          return;
        }
      }
      ++state->phase;
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: timeline FAIL: %1").arg(e.what()));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}

// OPAD_BENCH_TIMELINEPERF=1 (UI-99 on a big model; case timeline-engine, the Engine beside the repository): its hidden
// root shown first (an appearance step), then the pointer rests on every marker (an import's bodies tinted, what a feature
// made found on a worker), the model is rolled back before the last step and forward again by the playhead, names and the
// design history toggled; no event-loop gap over 250 ms meanwhile.
OPAD_BENCH(OPAD_BENCH_TIMELINEPERF, timelineperf) {
  struct State {
    int phase = 0, ticks = 0;
    size_t marker = 0;
    QElapsedTimer clock, tick;
    qint64 gap = 0;
    std::vector<std::string> markers;
  };
  auto state = std::make_shared<State>();
  SmartSelect* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* smart = dynamic_cast<SmartSelect*>(a)) area = smart;
  auto* timer = new QTimer(&w);
  timer->setInterval(20);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, area, state, timer] {
    TimelineWidget* t = w.m_timeline;
    try {
      if (state->tick.isValid() && state->phase >= 3) state->gap = std::max(state->gap, state->tick.restart());
      if (++state->ticks > 30000) throw opad::Error("timed out in phase " + std::to_string(state->phase));
      if (!area) throw opad::Error("the smart selection area is off");
      if (w.m_doc->loading || w.m_jobs->busy() || area->busy() || w.m_viewport->looksPending()) return;
      auto move = [&](const QPoint& at, Qt::MouseButton button, QEvent::Type type) {
        QMouseEvent e(type, QPointF(at), QPointF(t->mapToGlobal(at)), type == QEvent::MouseMove ? Qt::NoButton : button,
                      type == QEvent::MouseButtonRelease || button == Qt::NoButton ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(t, &e);
      };
      switch (state->phase) {
        case 0: {
          std::vector<std::string> hidden;
          for (const auto& [id, n] : w.m_doc->scene.nodes)
            if (!n.visible) hidden.push_back(id);
          if (!hidden.empty()) w.m_doc->run("appearance", opad::json{{"targets", hidden}, {"visible", true}});
          break;
        }
        case 1:
          if (w.m_viewport->displayedCount() < int(w.m_doc->scene.all_bodies().size()) / 2) return;
          for (const auto& op : w.m_doc->doc.ops)
            if (!t->markerAt(op.id).isNull()) state->markers.push_back(op.id);
          if (state->markers.size() < 2) throw opad::Error("two markers at least");
          trace::log(QString("bench: timelineperf: %1 bodies, %2 markers PASS").arg(w.m_doc->scene.all_bodies().size()).arg(state->markers.size()));
          break;
        case 2:
          state->tick.start();
          state->clock.start();
          move(t->markerAt(state->markers[0]).center(), Qt::NoButton, QEvent::MouseMove);
          break;
        case 3: {  // one marker after another, each once the last one's highlight is applied
          trace::log(QString("bench: timelineperf: marker %1 of %2 (%3) shown in %4 ms PASS").arg(state->marker + 1).arg(state->markers.size())
                         .arg(t->label(*w.m_doc->doc.find_op(state->markers[state->marker]))).arg(state->clock.elapsed()));
          if (++state->marker < state->markers.size()) {
            state->clock.start();
            move(t->markerAt(state->markers[state->marker]).center(), Qt::NoButton, QEvent::MouseMove);
            return;
          }
          QEvent leave(QEvent::Leave);
          QApplication::sendEvent(t, &leave);
          state->clock.start();
          const QPoint head = t->playhead().center();  // before the last marker
          move(head, Qt::LeftButton, QEvent::MouseButtonPress);
          move(QPoint(t->markerAt(state->markers.back()).left() - 4, head.y()), Qt::LeftButton, QEvent::MouseMove);
          move(QPoint(t->markerAt(state->markers.back()).left() - 4, head.y()), Qt::LeftButton, QEvent::MouseButtonRelease);
          break;
        }
        case 4: {
          if (!w.m_doc->rolledBack()) throw opad::Error("the playhead rolled the model back");
          trace::log(QString("bench: timelineperf: rolled back before the last step in %1 ms PASS").arg(state->clock.elapsed()));
          state->clock.start();
          w.action("timeline.rollForward")->trigger();
          break;
        }
        case 5: {
          if (w.m_doc->rolledBack()) throw opad::Error("rolled forward");
          trace::log(QString("bench: timelineperf: rolled forward in %1 ms PASS").arg(state->clock.elapsed()));
          state->clock.start();
          for (const char* id : {"timeline.names", "timeline.designOnly", "timeline.designOnly", "timeline.names"}) w.action(id)->trigger();
          trace::log(QString("bench: timelineperf: names and the design history toggled in %1 ms PASS").arg(state->clock.elapsed()));
          if (state->gap > 250) throw opad::Error("the event loop waited " + std::to_string(state->gap) + " ms");
          trace::log(QString("bench: timelineperf: longest event-loop gap %1 ms PASS").arg(state->gap));
          timer->stop();
          QCoreApplication::exit(0);
          return;
        }
      }
      ++state->phase;
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: timelineperf FAIL: %1").arg(e.what()));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}

// OPAD_BENCH_ROLLBACK=<prefix> (UI-99; case timeline-rollback in tools/bench_cases/smart.py) on the 40 mm base with a 10 mm
// boss joined on top, rounded by the bench (Round). Rolled back before Round, Shift+Left on the timeline (the window's own
// Shift+Left, a view, does not get it) draws the playhead before Boss at once and rolls back there once the keys rest;
// Shift+Right twice rolls forward to the end, Shift+Home before everything, Shift+End to the end again. Rolled back before
// Round, editing Boss takes over the roll-back and Esc gives it back; editing it again to 15 mm high and OK commits the
// edit and leaves the model rolled back before Round with the taller boss; rolled forward, Round sits on it. Shots:
// <prefix>.keys.png (the playhead moved by keys, the model not yet), .edited.png (the timeline after the edit).
OPAD_BENCH(OPAD_BENCH_ROLLBACK, rollback) {
  struct State {
    int phase = 0, ticks = 0, wait = 0;
    std::string body, base, boss, round;
  };
  auto state = std::make_shared<State>();
  TimelineArea* timelineArea = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* t = dynamic_cast<TimelineArea*>(a)) timelineArea = t;
  if (!timelineArea) {
    trace::log("bench: rollback FAIL: the timeline area is off");
    QCoreApplication::exit(2);
    return true;
  }
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, timelineArea, state, timer, prefix = value] {
    TimelineWidget* t = w.m_timeline;
    try {
      if (++state->ticks > 1200) throw opad::Error("timed out in phase " + std::to_string(state->phase));
      if (w.m_doc->loading || w.m_doc->designBusy || w.m_doc->snapshotBusy() || w.m_jobs->busy()) return;
      auto require = [](bool ok, const std::string& why) {
        if (!ok) throw opad::Error(why);
      };
      auto pass = [](const QString& what) { trace::log("bench: rollback: " + what + " PASS"); };
      auto waitFor = [&](bool ok, const std::string& why) {
        if (ok) {
          state->wait = 0;
          return true;
        }
        require(++state->wait < 80, why);
        return false;
      };
      auto key = [&](int k) {  // Shift+k on the timeline: claimed before the window's shortcuts, then pressed
        QKeyEvent override(QEvent::ShortcutOverride, k, Qt::ShiftModifier);
        QApplication::sendEvent(t, &override);
        require(override.isAccepted(), "the timeline claims Shift+" + QKeySequence(k).toString().toStdString());
        QKeyEvent press(QEvent::KeyPress, k, Qt::ShiftModifier);
        QApplication::sendEvent(t, &press);
      };
      auto between = [&](const std::string& left, const std::string& right) {
        const int x = t->playhead().center().x();
        return (left.empty() || x > t->markerAt(left).right()) && (right.empty() || x < t->markerAt(right).left());
      };
      auto height = [&] {
        const opad::Feature* f = w.m_doc->scene.feature(state->boss);
        return f ? QString::fromStdString(f->inputs.value("height", "")) : QString();
      };
      switch (state->phase) {
        case 0: {
          require(w.m_doc->scene.all_bodies().size() == 1, "one body");
          state->body = w.m_doc->scene.all_bodies().front();
          for (const auto& f : w.m_doc->scene.features) {
            if (f.name == "Base") state->base = f.id;
            if (f.name == "Boss") state->boss = f.id;
          }
          require(!state->base.empty() && !state->boss.empty(), "the fixture's Base and Boss");
          TopTools_IndexedMapOfShape edges;
          TopExp::MapShapes(opad::node_world_shape(w.m_doc->doc, w.m_doc->scene, state->body), TopAbs_EDGE, edges);
          opad::json top = opad::json::array();
          for (int i = 1; i <= edges.Extent(); ++i) {
            BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
            if (std::abs(c.Value(c.FirstParameter()).Z() - 20) < 1e-6 && std::abs(c.Value(c.LastParameter()).Z() - 20) < 1e-6) top.push_back(state->body + "/edge/" + std::to_string(i - 1));
          }
          require(top.size() == 4, "four top edges on the boss");
          w.m_design->applyOps({opad::design::make_feature_op("fillet", "Round", {{"edges", top}, {"radius", "2 mm"}})}, "fillet");
          break;
        }
        case 1: {
          for (const auto& f : w.m_doc->scene.features)
            if (f.name == "Round") state->round = f.id;
          require(!state->round.empty() && t->markerCount() == 3, "Round applied, three markers");
          w.setWorkspace("design");
          require(timelineArea->rollTo(state->round) && w.m_doc->rolledBack() && between(state->boss, state->round), "rolled back before Round");
          t->setFocus();
          key(Qt::Key_Left);
          require(t->playheadMoving() && between(state->base, state->boss) && w.m_doc->rollback() == state->round, "Shift+Left draws the playhead before Boss at once, the model waits");
          t->grab().save(prefix + ".keys.png");
          pass("Shift+Left on the timeline: the playhead before Boss at once, the model still before Round");
          break;
        }
        case 2: {
          if (!waitFor(!t->playheadMoving() && w.m_doc->rollback() == state->boss, "the model follows once the keys rest")) return;
          require(w.m_doc->rolledBack() && !w.m_doc->scene.feature(state->boss) && between(state->base, state->boss), "rolled back before Boss");
          pass("the keys at rest: the model rolled back before Boss");
          key(Qt::Key_Right);
          key(Qt::Key_Right);
          require(between(state->round, {}), "Shift+Right twice: the playhead at the end");
          break;
        }
        case 3: {
          if (!waitFor(!t->playheadMoving() && !w.m_doc->rolledBack() && w.m_doc->scene.feature(state->round), "Shift+Right twice rolls forward")) return;
          pass("Shift+Right twice rolls forward to the end");
          key(Qt::Key_Home);
          break;
        }
        case 4: {
          if (!waitFor(!t->playheadMoving() && w.m_doc->rollback() == state->base && w.m_doc->scene.all_bodies().empty(), "Shift+Home rolls back before everything")) return;
          pass("Shift+Home: before every step, no body");
          key(Qt::Key_End);
          break;
        }
        case 5: {
          if (!waitFor(!t->playheadMoving() && !w.m_doc->rolledBack() && w.m_doc->scene.all_bodies().size() == 1, "Shift+End rolls forward")) return;
          pass("Shift+End rolls forward to the end");
          require(timelineArea->rollTo(state->round), "rolled back before Round again");
          w.m_design->editOp(state->boss);
          require(w.m_design->featureActive() && !w.m_doc->rolledBack() && w.m_doc->rollback() == state->boss, "editing Boss takes the roll-back over");
          require(!timelineArea->rollTo({}), "the playhead stays while the editor is open");
          w.m_design->escape();
          require(!w.m_design->featureActive() && w.m_doc->rolledBack() && w.m_doc->rollback() == state->round && timelineArea->chip()->isVisibleTo(w.m_chips),
                  "Esc gives the roll-back before Round back");
          pass("editing Boss while rolled back before Round, then Esc: rolled back before Round again, the chip says so");
          w.m_design->editOp(state->boss);
          w.m_design->featurePanel()->setValue("height", "15 mm");
          emit w.m_design->featurePanel()->accepted();
          break;
        }
        case 6: {
          if (!waitFor(!w.m_design->featureActive() && height() == "15 mm", "OK commits the edit of Boss")) return;
          require(w.m_doc->rolledBack() && w.m_doc->rollback() == state->round && !w.m_doc->scene.feature(state->round), "the model stays rolled back before Round");
          Bnd_Box box;
          BRepBndLib::Add(opad::node_world_shape(w.m_doc->doc, w.m_doc->scene, state->body), box);
          require(std::abs(box.CornerMax().Z() - 25) < 0.1, "the boss is 15 mm high there (top at " + std::to_string(box.CornerMax().Z()) + ")");
          require(timelineArea->chip()->isVisibleTo(w.m_chips) && between(state->boss, state->round), "the chip and the playhead stay");
          t->grab().save(prefix + ".edited.png");
          pass("Boss edited to 15 mm: the model stays rolled back before Round, with the taller boss");
          w.action("timeline.rollForward")->trigger();
          break;
        }
        case 7: {
          if (!waitFor(!w.m_doc->rolledBack(), "Roll forward")) return;
          const opad::Feature* round = w.m_doc->scene.feature(state->round);
          require(round && round->error.empty(), "Round follows the taller boss");
          pass("rolled forward: Round sits on the taller boss");
          timer->stop();
          QCoreApplication::exit(0);
          return;
        }
      }
      ++state->phase;
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: rollback FAIL: %1").arg(e.what()));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
