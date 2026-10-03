#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SmartSelect.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/provenance.hpp"
#include "opad/geometry.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QStatusBar>
#include <QToolButton>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

// OPAD_BENCH_SMARTSELECT=<prefix> (TODO 11 UI-95, SmartSelect; case smartselect in tools/bench_cases/smart.py) on a 40 mm
// base with a 10 mm boss joined on top; the bench rounds the boss's top edges (Round) first. Two boss faces picked: the
// chip names the boss with its five faces and Ctrl+Up as the way there, beside the picks and clear of the view cube;
// hovering it draws the five in amber and pulses the boss's timeline marker. Ctrl+Up selects them (the chip turns into
// the boss's actions), again the body; Ctrl+Down twice climbs back to the two faces. Shift+Space lists the candidates. A
// double-click on the boss's top (real mouse events) selects the boss, another one opens it for editing. An edge
// double-clicked: the loop on the face turned to the viewer; Alt on a straight edge says nothing continues it. The chip's
// Delete on the boss: the question names Round, which uses it, with the result previewed; deleting both leaves the base
// alone, the toast's Undo brings them back. Deleting Round, which nothing uses, asks nothing. The chip's Find in timeline,
// Isolate and Suppress (undone from its toast) on the boss. Shots: <prefix>.chip.png,
// .actions.png, .menu.png, .question.png, .hover.png. Texts are compared in the language shown (case smartselect-rtl).
OPAD_BENCH(OPAD_BENCH_SMARTSELECT, smartselect) {
  struct State {
    int phase = 0, ticks = 0, wait = 0;
    std::string body, boss, round;
    std::vector<opad::Ref> bossFaces, roundFaces, two;
    opad::Ref top;  // the boss's top face
    size_t ops = 0;
    int faces = 0;
  };
  auto state = std::make_shared<State>();
  SmartSelect* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* smart = dynamic_cast<SmartSelect*>(a)) area = smart;
  if (!area) {
    trace::log("bench: smartselect FAIL: the smart selection area is off");
    QCoreApplication::exit(2);
    return true;
  }
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, area, state, timer, prefix = value] {
    try {
      if (++state->ticks > 1500) throw opad::Error("timed out in phase " + std::to_string(state->phase));
      if (w.m_doc->loading || w.m_doc->designBusy || w.m_doc->snapshotBusy() || w.m_jobs->busy() || area->busy()) return;
      auto require = [](bool ok, const std::string& why) {
        if (!ok) throw opad::Error(why);
      };
      auto pass = [](const QString& what) { trace::log("bench: smartselect: " + what + " PASS"); };
      auto pick = [&](const std::vector<opad::Ref>& refs) {  // as a click would
        w.m_viewport->selectRefs(refs);
        w.onViewportSelection();
      };
      auto selected = [&](const std::vector<opad::Ref>& refs) { return smart::sameRefs(w.m_viewport->selection(), refs); };
      auto faces = [](const char* name, int n) { return SmartSelect::tr("%1 · %2 faces").arg(name).arg(n); };  // in the language shown
      auto shape = [&] { return opad::node_world_shape(w.m_doc->doc, w.m_doc->scene, state->body); };
      auto faceCount = [&] { return opad::subshape_count(shape(), opad::Ref::Kind::Face); };
      auto waitFor = [&](bool ok, const std::string& why) {
        if (ok) {
          state->wait = 0;
          return true;
        }
        require(++state->wait < 80, why);
        return false;
      };
      // A double-click as the view reports it: the first click's pick, the double-click itself through the view's event
      // path (the view's own handling held back: a hidden window never paints, so OCCT would keep the clicks queued), the
      // second click's pick.
      auto doubleClick = [&](const opad::Ref& face) {
        pick({face});
        const QPoint at = w.m_viewport->rect().center();
        w.m_viewport->setBlocked(true);
        QMouseEvent press(QEvent::MouseButtonDblClick, QPointF(at), QPointF(w.m_viewport->mapToGlobal(at)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(w.m_viewport, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), QPointF(w.m_viewport->mapToGlobal(at)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(w.m_viewport, &release);
        w.m_viewport->setBlocked(false);
        pick({face});
      };
      switch (state->phase) {
        case 0: {
          require(w.m_doc->scene.all_bodies().size() == 1, "one body");
          state->body = w.m_doc->scene.all_bodies().front();
          for (const auto& f : w.m_doc->scene.features)
            if (f.name == "Boss") state->boss = f.id;
          require(!state->boss.empty(), "the fixture's Boss");
          // Round on the boss's four top edges (z = 20).
          TopTools_IndexedMapOfShape edges;
          TopExp::MapShapes(shape(), TopAbs_EDGE, edges);
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
          opad::design::Provenance provenance(w.m_doc->doc);
          const auto owners = provenance.face_owners(state->body);
          for (size_t i = 0; i < owners.size(); ++i) {
            const opad::Ref r = opad::Ref::parse(state->body + "/face/" + std::to_string(i));
            if (owners[i].op == state->boss) state->bossFaces.push_back(r);
            if (owners[i].op == state->round) state->roundFaces.push_back(r);
          }
          require(state->bossFaces.size() == 5 && state->roundFaces.size() == 4, "the boss owns 5 faces, Round 4");
          {
            TopTools_IndexedMapOfShape faces;
            TopExp::MapShapes(shape(), TopAbs_FACE, faces);
            for (const auto& r : state->bossFaces) {
              GProp_GProps g;
              BRepGProp::SurfaceProperties(faces(r.index + 1), g);
              if (std::abs(g.CentreOfMass().Z() - 20) < 1e-6) state->top = r;
            }
            require(state->top.index >= 0, "the boss's top face");
          }
          state->two = {state->bossFaces[1], state->bossFaces[3]};
          state->faces = faceCount();
          state->ops = w.m_doc->doc.ops.size();
          w.setWorkspace("design");
          w.m_viewport->standardView("iso");
          w.action("select.faces")->trigger();
          break;
        }
        case 2:
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Face) return;
          pick(state->two);
          break;
        case 3: {
          SmartChip* chip = area->chip();
          if (!waitFor(chip->isVisible() && area->found().ready, "the chip shows for two boss faces")) return;
          const auto& f = area->found();
          require(f.best >= 0 && f.candidates[size_t(f.best)].op == state->boss && f.active < 0, "the chip offers the boss");
          require(chip->text() == faces("Boss", 5), "the chip says \"Boss · 5 faces\", not \"" + chip->text().toStdString() + "\"");
          require(chip->hint() == w.action("edit.selectparent")->shortcut().toString(QKeySequence::NativeText) && !chip->hint().isEmpty(), "the chip names Ctrl+Up");
          const QRect r = chip->geometry();
          require(w.m_viewport->rect().contains(r) && !r.intersects(QRect(w.m_viewport->width() - 214, 0, 214, 196)), "the chip is inside the view, clear of the cube");
          chip->grab().save(prefix + ".chip.png");
          pass(QString("two boss faces: the chip offers \"%1\" (%2) at %3,%4").arg(chip->text(), chip->hint()).arg(r.x()).arg(r.y()));
          // Hovered: the five in amber, the boss's marker pulses.
          QEnterEvent enter(QPointF(4, 4), QPointF(4, 4), QPointF(chip->mapToGlobal(QPoint(4, 4))));
          QApplication::sendEvent(chip, &enter);
          require(w.m_viewport->candidateRefsShown() == 5, "hovering the chip draws the boss's five faces (" + std::to_string(w.m_viewport->candidateRefsShown()) + ")");
          require(w.m_timeline->pulsing() == state->boss, "hovering the chip pulses the boss's timeline marker");
          w.m_viewport->grabImage().save(prefix + ".hover.png");
          QEvent leave(QEvent::Leave);
          QApplication::sendEvent(chip, &leave);
          require(w.m_viewport->candidateRefsShown() == 0, "leaving the chip clears the amber");
          pass("hovering the chip shows the boss's faces in amber and pulses its timeline marker");
          w.action("edit.selectparent")->trigger();  // Ctrl+Up
          break;
        }
        case 4: {
          if (!waitFor(selected(state->bossFaces) && area->found().ready && area->found().active >= 0, "Ctrl+Up selects the boss's five faces")) return;
          QStringList buttons;
          for (QToolButton* b : area->chip()->actionButtons()) buttons << b->defaultAction()->objectName();
          require(buttons.join(",") == "smartDelete,smartEdit,smartSuppress,smartFind,smartIsolate", "the chip carries the boss's actions: " + buttons.join(",").toStdString());
          require(area->chip()->text() == faces("Boss", 5) && area->ladder() == 1, "the chip names what is selected");
          area->chip()->grab().save(prefix + ".actions.png");
          pass("Ctrl+Up selected the boss's five faces; the chip carries Delete, Edit, Suppress, Find in timeline, Isolate");
          w.action("edit.selectparent")->trigger();  // again: the body
          break;
        }
        case 5: {
          // Up from the boss: the boss detail the geometry shows (with its rounds) when there is one, then the body.
          const auto picks = w.m_viewport->selection();
          const auto& f = area->found();
          if (picks.size() == 1 && picks.front().kind == opad::Ref::Kind::Body && picks.front().body == state->body) {
            require(!area->chip()->isVisible() && area->ladder() >= 2, "no chip on a body, the rungs below it kept");
            pass(QString("Ctrl+Up climbed to the body (%1 rungs below it)").arg(area->ladder()));
            w.action("select.shrink")->trigger();
            break;
          }
          if (f.ready && f.active >= 0 && f.candidates[size_t(f.active)].kind == "boss" && area->ladder() == 2) {
            pass(QString("Ctrl+Up again selected the boss detail: \"%1\"").arg(area->chip()->text()));
            w.action("edit.selectparent")->trigger();
            return;
          }
          waitFor(false, "Ctrl+Up again selects the body");
          return;
        }
        case 6:
        case 7: {
          if (area->ladder() > 0) {  // one rung per tick, each once the last has settled
            require(++state->wait < 10, "Ctrl+Down climbs back in a few steps");
            w.action("select.shrink")->trigger();
            return;
          }
          state->wait = 0;
          require(selected(state->two) && area->found().ready && area->chip()->isVisible(), "Ctrl+Down climbed back to the two faces picked first");
          pass("Ctrl+Down climbed back step by step to the two faces picked first");
          state->phase = 7;
          w.action("select.related")->trigger();  // Shift+Space
          break;
        }
        case 8: {
          QMenu* menu = area->openMenu();
          if (!waitFor(menu && menu->isVisible(), "Shift+Space opens the menu")) return;
          QStringList entries;
          for (QAction* a : menu->actions())
            if (a->property("smartCandidate").isValid()) entries << a->text();
          require(!entries.isEmpty() && entries.front() == "&1  " + faces("Boss", 5), "the menu lists the boss first: " + entries.join(" | ").toStdString());
          bool del = false;
          for (QAction* a : menu->actions()) del = del || a->objectName() == "smartDelete";
          require(del, "the menu offers deleting the boss");
          menu->grab().save(prefix + ".menu.png");
          pass("Shift+Space lists " + entries.join(" | "));
          // Hovering the body's entry tints the whole body through the look compositor's candidate layer.
          for (QAction* a : menu->actions())
            if (a->property("smartCandidate").isValid() && area->found().candidates[size_t(a->property("smartCandidate").toInt())].kind == "body") emit menu->hovered(a);
          const QColor amber = w.m_viewport->tokens().candidate;
          const auto look = w.m_viewport->bodyLook(state->body).color;
          require(std::abs(look[0] - amber.redF()) < 1e-6 && std::abs(look[1] - amber.greenF()) < 1e-6 && std::abs(look[2] - amber.blueF()) < 1e-6, "hovering the body's entry tints it amber");
          menu->close();
          require(w.m_viewport->bodyLook(state->body).color != look, "closing the menu takes the tint away");
          pass("hovering the body's entry in the menu tints the body in the candidate amber (look layer), closing it clears it");
          w.m_viewport->clearSelection();
          break;
        }
        case 9:
          if (!waitFor(w.m_viewport->selection().empty(), "the selection cleared")) return;
          doubleClick(state->top);
          break;
        case 10: {
          if (!waitFor(selected(state->bossFaces), "a double-click on the boss's top selects the boss")) return;
          pass("a double-click on the boss's top selected the boss's five faces");
          doubleClick(state->top);  // again: edit it
          break;
        }
        case 11: {
          if (!waitFor(w.m_design->featureActive() && w.m_design->editingOp() == state->boss, "a second double-click on the boss edits it")) return;
          require(!area->chip()->isVisible(), "no chip while the feature is edited");
          pass("a second double-click on a boss face opened the boss for editing");
          w.m_design->escape();
          break;
        }
        case 12: {
          if (w.m_design->featureActive()) return;
          w.action("select.edges")->trigger();
          break;
        }
        case 13: {
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Edge) return;
          // The base's top edge along x at the back (y = 40): the loop of the top face (facing up, at the viewer).
          TopTools_IndexedMapOfShape edges;
          TopExp::MapShapes(shape(), TopAbs_EDGE, edges);
          int back = -1;
          for (int i = 1; i <= edges.Extent(); ++i) {
            BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
            if (c.Value((c.FirstParameter() + c.LastParameter()) / 2).Distance(gp_Pnt(20, 40, 10)) < 1e-6) back = i - 1;
          }
          require(back >= 0, "the base's back top edge");
          opad::Ref edge;
          edge.body = state->body;
          edge.kind = opad::Ref::Kind::Edge;
          edge.index = back;
          pick({edge});
          area->doubleClicked(false);
          break;
        }
        case 14: {
          const auto picks = w.m_viewport->selection();
          if (!waitFor(picks.size() == 4, "a double-click on an edge selects its loop")) return;
          require(std::all_of(picks.begin(), picks.end(), [](const opad::Ref& r) { return r.kind == opad::Ref::Kind::Edge; }), "the loop is edges");
          pass("a double-click on the base's top edge selected the top face's outer loop (4 edges)");
          pick({picks.front()});
          area->doubleClicked(true);  // Alt: the tangent chain of a straight edge between square corners is itself
          break;
        }
        case 15: {
          if (!waitFor(w.statusBar()->currentMessage() == SmartSelect::tr("No edge continues this one tangentially."), "Alt+double-click on a straight edge: no tangent chain")) return;
          pass("Alt+double-click on a straight edge says no edge continues it tangentially");
          w.action("select.faces")->trigger();
          break;
        }
        case 16:
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Face) return;
          pick(state->two);
          w.action("edit.selectparent")->trigger();  // waits for the chip's answer, then climbs
          break;
        case 17: {
          if (!waitFor(selected(state->bossFaces) && area->found().active >= 0, "the boss selected again")) return;
          QToolButton* del = nullptr;
          for (QToolButton* b : area->chip()->actionButtons())
            if (b->defaultAction()->objectName() == "smartDelete") del = b;
          require(del, "the chip's Delete");
          del->click();
          break;
        }
        case 18: {
          QMenu* question = area->openMenu();
          if (!waitFor(question && question->isVisible() && question->objectName() == "smartDeleteQuestion", "deleting the boss asks about Round")) return;
          QAction* all = question->findChild<QAction*>("deleteWithDependents");
          require(all && all->text() == SmartSelect::tr("Delete %1 and %2").arg("Boss", "Round"), "the question names Round");
          require(w.m_viewport->previewBodyCount() > 0, "the result is previewed while it asks");
          question->grab().save(prefix + ".question.png");
          pass("Delete on the boss asks \"" + all->text() + "\" with the result previewed");
          all->trigger();
          question->close();
          break;
        }
        case 19: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "the deletes are appended")) return;
          const auto& deleted = w.m_doc->scene.deleted_ops;
          require(std::count(deleted.begin(), deleted.end(), state->boss) && std::count(deleted.begin(), deleted.end(), state->round), "Boss and Round tombstoned");
          GProp_GProps g;
          BRepGProp::VolumeProperties(shape(), g);
          require(faceCount() == 6 && std::abs(g.Mass() - 16000) < 1e-3, "the base alone remains (" + std::to_string(faceCount()) + " faces)");
          require(w.m_viewport->previewBodyCount() == 0, "the preview is gone");
          Toast* toast = w.m_toasts->toasts().isEmpty() ? nullptr : w.m_toasts->toasts().back();
          require(toast && toast->text() == SmartSelect::tr("Deleted %1 and %2 feature(s) using it").arg("Boss").arg(1) && toast->actionButton(), "a toast says what went, with Undo");
          pass("the boss and its round are deleted as one step, the base remains; the toast offers Undo");
          toast->actionButton()->click();
          break;
        }
        case 20: {
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops, "the toast's Undo takes the step back")) return;
          require(faceCount() == state->faces && w.m_doc->scene.feature(state->boss) && w.m_doc->scene.feature(state->round), "Boss and Round are back");
          pass("the toast's Undo brought the boss and its round back");
          pick({state->roundFaces[0]});
          w.action("edit.selectparent")->trigger();
          break;
        }
        case 21: {
          if (!waitFor(selected(state->roundFaces) && area->found().active >= 0, "Ctrl+Up on a round face selects Round")) return;
          require(area->chip()->text() == faces("Round", 4), "the chip names Round");
          for (QToolButton* b : area->chip()->actionButtons())
            if (b->defaultAction()->objectName() == "smartDelete") b->click();
          break;
        }
        case 22: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Round, used by nothing, is deleted without a question")) return;
          require(!area->openMenu(), "no question");
          require(faceCount() == state->faces - 4, "the boss's top edges are sharp again");
          pass("Round, which nothing uses, was deleted at once");
          w.m_doc->undo();
          break;
        }
        case 23:
          if (!waitFor(w.m_doc->doc.ops.size() == state->ops, "undo brings Round back")) return;
          pick(state->two);
          w.action("edit.selectparent")->trigger();
          break;
        case 24: {
          if (!waitFor(selected(state->bossFaces) && area->found().active >= 0, "the boss selected again")) return;
          auto button = [&](const char* name) {
            for (QToolButton* b : area->chip()->actionButtons())
              if (b->defaultAction()->objectName() == name) return b;
            throw opad::Error(std::string("the chip's ") + name);
          };
          w.m_timeline->setCurrentOp({});
          button("smartFind")->click();
          require(w.m_timeline->currentOp() == state->boss && w.m_timeline->pulsing() == state->boss, "Find in timeline marks the boss's marker");
          button("smartIsolate")->click();
          require(w.m_viewport->isIsolated() && w.m_viewport->isolatedNodes() == std::vector<std::string>{state->body}, "Isolate shows the body alone");
          w.m_viewport->isolate({});
          pass("the chip's Find in timeline marks and pulses the boss's marker, Isolate shows its body alone");
          button("smartSuppress")->click();
          break;
        }
        case 25: {
          const opad::Feature* boss = w.m_doc->scene.feature(state->boss);
          if (!waitFor(boss && boss->suppressed, "Suppress suppresses the boss")) return;
          Toast* toast = w.m_toasts->toasts().isEmpty() ? nullptr : w.m_toasts->toasts().back();
          require(toast && toast->text() == SmartSelect::tr("Suppressed %1").arg("Boss") && toast->actionButton(), "a toast says it is suppressed, with Undo");
          pass("the chip's Suppress suppressed the boss (" + QString::number(faceCount()) + " faces left)");
          toast->actionButton()->click();
          break;
        }
        case 26: {
          const opad::Feature* boss = w.m_doc->scene.feature(state->boss);
          if (!waitFor(boss && !boss->suppressed && faceCount() == state->faces, "the toast's Undo brings the boss back")) return;
          pass("the toast's Undo unsuppressed the boss");
          timer->stop();
          QCoreApplication::exit(0);
          return;
        }
      }
      ++state->phase;
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: smartselect FAIL: %1").arg(e.what()));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
