#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>

#include <tuple>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SmartSelect.hpp"
#include "opad/design/provenance.hpp"
#include "opad/geometry.hpp"

// OPAD_BENCH_CONTEXT=<prefix> (TODO 11 UI-100; case context-menus in tools/bench_cases/smart.py) on the 40 mm base with a
// 10 mm boss joined on top, the body put in a component "Assembly", a sketch "Profile" beside it. The context menu as built
// for each kind of selection (filled, never shown): nothing (view entries and New sketch), Repeat Distance first once the
// Distance tool was started; a boss face (its title, the boss's Select / Edit / Find in timeline right after it, Sketch on
// and Look at the face, Press pull, Select similar and Remove faces, Select the body, no body entries), Find Boss in the
// timeline; a base face picked just now (Edit the feature that made it waits for the answer, then edits Base); one edge
// (Fillet, Chamfer, its loop, which it selects); one vertex (measuring, Properties, no Delete); the body (Edit Base, Find,
// Move, Remove Part), Edit Base; the component (Find, New component, Remove); the sketch (Edit, Redefine plane, Find,
// Export, Delete Profile); the open sketch (Finish and Cancel first). Shots: <prefix>.face.png, .edge.png, .body.png,
// .sketch.png.
OPAD_BENCH(OPAD_BENCH_CONTEXT, contextmenus) {
  struct State {
    int phase = 0, ticks = 0, wait = 0;
    std::string body, base, boss, component, sketch;
    std::vector<opad::Ref> bossFaces, baseFaces;
  };
  auto state = std::make_shared<State>();
  SmartSelect* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* smart = dynamic_cast<SmartSelect*>(a)) area = smart;
  if (!area) {
    trace::log("bench: context FAIL: the smart selection area is off");
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
      auto pass = [](const QString& what) { trace::log("bench: context: " + what + " PASS"); };
      auto waitFor = [&](bool ok, const std::string& why) {
        if (ok) {
          state->wait = 0;
          return true;
        }
        require(++state->wait < 80, why);
        return false;
      };
      // The menu as a right-click on the view (or the browser) builds it now: its entries by object name, or text.
      struct Built {
        std::unique_ptr<QMenu> menu = std::make_unique<QMenu>();
        QStringList names;
        QAction* find(const QString& name) const {
          for (QAction* a : menu->actions())
            if (a->objectName() == name) return a;
          return nullptr;
        }
        QString text(const QString& name) const { return find(name) ? find(name)->text() : QString(); }
      };
      auto build = [&](const std::vector<std::string>& ids) {
        Built b;
        w.buildContextMenu(*b.menu, ids);
        for (QAction* a : b.menu->actions())
          if (!a->isSeparator() || a->objectName() == "contextTitle") b.names << (a->objectName().isEmpty() ? a->text() : a->objectName());  // a title is a section
        trace::log("context menu: " + b.names.join(", "));
        return b;
      };
      auto pick = [&](const std::vector<opad::Ref>& refs) {
        w.m_viewport->selectRefs(refs);
        w.onViewportSelection();
      };
      auto ref = [&](opad::Ref::Kind kind, int index) {
        opad::Ref r;
        r.body = state->body;
        r.kind = kind;
        r.index = index;
        return r;
      };
      auto has = [&](const Built& b, const QStringList& names) {
        for (const QString& n : names)
          if (!b.names.contains(n)) return false;
        return true;
      };
      auto missing = [&](const Built& b, const QStringList& names) {
        QStringList out;
        for (const QString& n : names)
          if (!b.names.contains(n)) out << n;
        return out.join(",").toStdString();
      };
      switch (state->phase) {
        case 0: {
          require(w.m_doc->scene.all_bodies().size() == 1, "one body");
          state->body = w.m_doc->scene.all_bodies().front();
          for (const auto& f : w.m_doc->scene.features) {
            if (f.name == "Base") state->base = f.id;
            if (f.name == "Boss") state->boss = f.id;
          }
          w.m_doc->run("rename", opad::json{{"target", state->body}, {"name", "Part"}});
          state->component = w.m_doc->run("component", opad::json{{"name", "Assembly"}}).value("id", "");
          w.m_doc->run("reparent", opad::json{{"target", state->body}, {"parent", state->component}});
          const opad::json circle = opad::json::array({{{"kind", "circle"}, {"picks", {{60, 0}}}, {"options", {{"radius", 5.0}}}}});
          state->sketch = w.m_doc->run("sketch", opad::json{{"geometry", {{"shapes", circle}}}, {"name", "Profile"}}).value("sketch_id", "");
          require(!state->base.empty() && !state->boss.empty() && !state->component.empty() && w.m_doc->scene.sketch(state->sketch), "the fixture, a component and a sketch");
          opad::design::Provenance provenance(w.m_doc->doc);
          const auto owners = provenance.face_owners(state->body);
          for (size_t i = 0; i < owners.size(); ++i) {
            if (owners[i].op == state->boss) state->bossFaces.push_back(ref(opad::Ref::Kind::Face, int(i)));
            if (owners[i].op == state->base) state->baseFaces.push_back(ref(opad::Ref::Kind::Face, int(i)));
          }
          require(state->bossFaces.size() == 5 && !state->baseFaces.empty(), "the boss's and the base's faces");
          w.setWorkspace("design");
          w.m_viewport->standardView("iso");
          // Nothing selected: the view's entries.
          Built none = build({});
          require(has(none, {"view.fit", "view.home", "design.sketch", "file.import"}) && !none.names.contains("edit.repeat"), "nothing selected: the view's entries (" + missing(none, {"view.fit", "view.home", "design.sketch", "file.import"}) + ")");
          pass("nothing selected: Fit, Home, New sketch, Import");
          w.action("inspect.distance")->trigger();
          require(w.m_tool.id == "distance", "Distance started");
          w.cancelTool();
          Built again = build({});
          require(!again.names.isEmpty() && again.names.front() == "edit.repeat" && again.menu->actions().front()->text() == MainWindow::tr("Repeat %1").arg(MainWindow::tr("Distance")),
                  "Repeat Distance first: " + again.names.join(",").toStdString());
          again.menu->actions().front()->trigger();
          require(w.m_tool.id == "distance", "Repeat starts Distance again");
          w.cancelTool();
          pass("after a tool every menu starts with \"" + again.menu->actions().front()->text() + "\", which starts it again");
          w.action("select.faces")->trigger();
          break;
        }
        case 1:
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Face) return;
          pick({state->bossFaces[1]});
          break;
        case 2: {
          if (!waitFor(area->found().ready && smart::sameRefs(area->found().picks, {state->bossFaces[1]}), "the chip's answer for a boss face")) return;
          Built b = build(w.currentNodeIds());
          const QStringList want = {"contextTitle", "smartSelectBest", "smartEdit", "smartFind", "contextSketchOn", "contextLookAt", "design.offset_face", "inspect.distance",
                                    "inspect.properties", "select.similar", "design.remove_faces", "contextSelectBody", "contextDelete"};
          require(has(b, want), "a face's entries, missing " + missing(b, want));
          require(b.names.indexOf("smartSelectBest") == b.names.indexOf("contextTitle") + 1, "the face's history right after its title");
          require(b.text("smartEdit").startsWith(SmartSelect::tr("Edit %1").arg("Boss")) && b.text("contextTitle") == MainWindow::tr("Face of %1").arg("Part"), "Edit Boss, under \"Face of Part\"");
          require(!b.names.contains("edit.rename") && !b.names.contains("contextColour") && !b.names.contains("contextFit"), "no body entries on a face");
          b.menu->grab().save(prefix + ".face.png");
          pass("a boss face: " + b.names.join(", "));
          b.find("smartFind")->trigger();
          require(w.m_timeline->currentOp() == state->boss && w.m_timeline->pulsing() == state->boss, "Find Boss in the timeline points at its marker");
          pass("Find Boss in the timeline points at its marker");
          pick({state->baseFaces.front()});
          Built now = build(w.currentNodeIds());  // before the chip's answer: entries that wait for it
          require(now.find("smartEditOwner"), "Edit the feature that made it, before the answer: " + now.names.join(",").toStdString());
          now.find("smartEditOwner")->trigger();
          break;
        }
        case 3: {
          if (!waitFor(w.m_design->featureActive() && w.m_design->editingOp() == state->base, "Edit the feature that made it edits Base once the answer is there")) return;
          pass("Edit the feature that made it, asked before the answer, edits Base");
          w.m_design->escape();
          w.action("select.edges")->trigger();
          break;
        }
        case 4: {
          if (w.m_design->featureActive() || w.m_viewport->selectionFilter() != Viewport::SelFilter::Edge) return;
          pick({ref(opad::Ref::Kind::Edge, 0)});
          Built b = build(w.currentNodeIds());
          const QStringList want = {"contextTitle", "smartLoop", "smartTangent", "design.fillet", "design.chamfer", "inspect.radius", "inspect.properties", "contextDelete"};
          require(has(b, want), "an edge's entries, missing " + missing(b, want));
          b.menu->grab().save(prefix + ".edge.png");
          pass("one edge: " + b.names.join(", "));
          b.find("smartLoop")->trigger();
          break;
        }
        case 5: {
          const auto sel = w.m_viewport->selection();
          if (!waitFor(sel.size() >= 3 && std::all_of(sel.begin(), sel.end(), [](const opad::Ref& r) { return r.kind == opad::Ref::Kind::Edge; }), "Select its loop selects the loop")) return;
          pass(QString("Select its loop selected %1 edges").arg(sel.size()));
          w.action("select.vertices")->trigger();
          break;
        }
        case 6: {
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Vertex) return;
          pick({ref(opad::Ref::Kind::Vertex, 0)});
          Built b = build(w.currentNodeIds());
          require(has(b, {"contextTitle", "inspect.distance", "inspect.properties", "contextSelectBody"}) && !b.names.contains("contextDelete") &&
                      b.text("contextTitle") == MainWindow::tr("Vertex of %1").arg("Part"), "a vertex's entries: " + b.names.join(",").toStdString());
          pass("one vertex: " + b.names.join(", "));
          w.action("select.bodies")->trigger();
          break;
        }
        case 7: {
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Body) return;
          w.m_areaServices.select({ref(opad::Ref::Kind::Body, -1)});
          Built b = build({state->body});
          const QStringList want = {"contextTitle", "contextFit", "view.isolate", "contextHideOthers", "edit.hide", "contextEditSource", "contextFind", "edit.rename",
                                    "contextColour", "contextLock", "design.move", "edit.selectparent", "contextExport", "inspect.properties", "contextDelete"};
          require(has(b, want), "a body's entries, missing " + missing(b, want));
          require(b.text("contextEditSource") == MainWindow::tr("Edit %1").arg("Base") && b.text("contextDelete").startsWith(MainWindow::tr("Remove %1").arg("Part")),
                  "Edit Base and Remove Part: " + b.text("contextEditSource").toStdString() + " / " + b.text("contextDelete").toStdString());
          b.menu->grab().save(prefix + ".body.png");
          pass("the body: " + b.names.join(", "));
          b.find("contextEditSource")->trigger();
          break;
        }
        case 8: {
          if (!waitFor(w.m_design->featureActive() && w.m_design->editingOp() == state->base, "Edit Base from the body's menu")) return;
          pass("the body's Edit Base opens the base for editing");
          w.m_design->escape();
          break;
        }
        case 9: {
          if (w.m_design->featureActive()) return;
          Built b = build({state->component});
          require(has(b, {"contextTitle", "contextFit", "contextFind", "design.newcomponent", "edit.rename", "contextDelete"}) && !b.names.contains("design.move") &&
                      b.text("contextFind") == MainWindow::tr("Find %1 in the timeline").arg("Assembly") && b.text("contextDelete").startsWith(MainWindow::tr("Remove %1").arg("Assembly")),
                  "a component's entries: " + b.names.join(",").toStdString());
          pass("the component: " + b.names.join(", "));
          w.m_browser->setSelectedIds({state->sketch});
          w.onBrowserSelection({state->sketch});
          Built s = build({state->sketch});
          require(has(s, {"contextTitle", "contextEditSketch", "contextReplane", "contextFind", "contextExportSketch", "contextDelete"}) &&
                      s.text("contextDelete").startsWith(MainWindow::tr("Delete %1").arg("Profile")) && !s.names.contains("contextEditSource"),
                  "a sketch's entries: " + s.names.join(",").toStdString());
          s.menu->grab().save(prefix + ".sketch.png");
          pass("the sketch: " + s.names.join(", "));
          s.find("contextEditSketch")->trigger();
          break;
        }
        case 10: {
          if (!waitFor(w.m_design->sketchActive(), "Edit sketch opens it")) return;
          Built b = build({});
          QStringList first = b.names.mid(0, 2);
          require(first == QStringList{"sketch.finish", "sketch.cancel"}, "the open sketch's menu starts with Finish and Cancel: " + b.names.join(",").toStdString());
          pass("the open sketch's menu starts with Finish sketch and Cancel sketch");
          b.find("sketch.finish")->trigger();
          break;
        }
        case 11:
          if (!waitFor(!w.m_design->sketchActive(), "Finish sketch from the menu")) return;
          pass("Finish sketch from the menu leaves the sketch");
          timer->stop();
          QCoreApplication::exit(0);
          return;
      }
      ++state->phase;
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: context FAIL: %1").arg(e.what()));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}

// OPAD_BENCH_RIGHTCLICK=<prefix> (UI-100; cases right-click and right-click-rtl in tools/bench_cases/smart.py) on the 40 mm
// base with a 10 mm boss joined on top (the body renamed Part), and an empty component Spare. Right clicks are mouse events
// through the view's own handlers, OCCT picking under the pointer (Viewport::benchFlush); the menu they open is read and
// closed from inside its own event loop (it never shows: BenchQuiet). Faces: a right click on the boss's top with nothing
// selected selects that face and the menu is about it, under its drawn title "Face of Part"; again on it, or on nothing,
// the selection stays; on a face of the base it moves there. While Distance runs a right click picks nothing. Bodies: on
// the body, the body, "Part" with Remove Part. The empty component: its menu offers Remove Spare, which takes it out as
// one step (a Remove feature), undone. Shots: <prefix>.face.png, .body.png (the menus as shown).
OPAD_BENCH(OPAD_BENCH_RIGHTCLICK, rightclick) {
  struct Menu {
    bool shown = false;
    QStringList names;
    QString title, remove;
    bool titleDrawn = false;
  };
  struct State {
    int phase = 0, ticks = 0, wait = 0;
    std::string body, base, boss, spare;
    std::vector<opad::Ref> bossFaces, baseFaces;
    std::vector<opad::Ref> before;
    size_t ops = 0;
  };
  auto state = std::make_shared<State>();
  SmartSelect* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* smart = dynamic_cast<SmartSelect*>(a)) area = smart;
  if (!area) {
    trace::log("bench: rightclick FAIL: the smart selection area is off");
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
      auto pass = [](const QString& what) { trace::log("bench: rightclick: " + what + " PASS"); };
      auto waitFor = [&](bool ok, const std::string& why) {
        if (ok) {
          state->wait = 0;
          return true;
        }
        require(++state->wait < 80, why);
        return false;
      };
      // A right click as the mouse delivers it (move, press, release; each followed by what the next frame does). The menu
      // it opens is read and closed from its own event loop; `shot` saves it.
      auto rightClick = [&](const QPointF& at, const QString& shot = {}) {
        auto menu = std::make_shared<Menu>();
        auto* poll = new QTimer(&w);  // until the menu is up (in its own event loop), then once
        poll->setInterval(20);
        QObject::connect(poll, &QTimer::timeout, &w, [menu, shot, poll, tries = 0]() mutable {
          QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
          for (QWidget* top : QApplication::topLevelWidgets())
            if (auto* candidate = qobject_cast<QMenu*>(top); !m && candidate && candidate->isVisible()) m = candidate;
          if (!m && ++tries < 250) return;
          poll->deleteLater();
          poll->stop();
          if (!m) return;
          menu->shown = true;
          for (QAction* a : m->actions()) {
            if (a->isSeparator()) continue;
            menu->names << (a->objectName().isEmpty() ? a->text() : a->objectName());
            if (a->objectName() == "contextDelete") menu->remove = a->text().split('\t').front();
          }
          if (auto* label = m->findChild<QLabel*>("contextTitleLabel")) {
            menu->title = label->text();
            menu->titleDrawn = label->isVisible() && label->width() > label->fontMetrics().horizontalAdvance(label->text());
          }
          if (!shot.isEmpty()) m->grab().save(shot);
          m->close();
        });
        poll->start();
        using Step = std::tuple<QEvent::Type, Qt::MouseButton, Qt::MouseButtons>;
        for (const auto& [type, button, buttons] : {Step{QEvent::MouseMove, Qt::NoButton, Qt::NoButton}, Step{QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton},
                                                    Step{QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton}}) {
          QMouseEvent e(type, at, w.m_viewport->mapToGlobal(at), button, buttons, Qt::NoModifier);
          QApplication::sendEvent(w.m_viewport, &e);
          w.m_viewport->benchFlush();
        }
        trace::log("right click menu: " + menu->title + " | " + menu->names.join(", "));
        return *menu;
      };
      auto selection = [&] { return w.m_viewport->selection(); };
      auto one = [&](const std::vector<opad::Ref>& among) {
        const auto sel = selection();
        return sel.size() == 1 && std::any_of(among.begin(), among.end(), [&](const opad::Ref& r) { return r.body == sel.front().body && r.kind == sel.front().kind && r.index == sel.front().index; });
      };
      const QPointF top = w.m_viewport->widgetPoint({20, 20, 20}), baseTop = w.m_viewport->widgetPoint({5, 5, 10}), nothing(12, w.m_viewport->height() - 12);
      switch (state->phase) {
        case 0: {
          require(w.m_doc->scene.all_bodies().size() == 1, "one body");
          state->body = w.m_doc->scene.all_bodies().front();
          for (const auto& f : w.m_doc->scene.features) {
            if (f.name == "Base") state->base = f.id;
            if (f.name == "Boss") state->boss = f.id;
          }
          w.m_doc->run("rename", opad::json{{"target", state->body}, {"name", "Part"}});
          state->spare = w.m_doc->run("component", opad::json{{"name", "Spare"}}).value("id", "");
          opad::design::Provenance provenance(w.m_doc->doc);
          const auto owners = provenance.face_owners(state->body);
          for (size_t i = 0; i < owners.size(); ++i) {
            const opad::Ref r = opad::Ref::parse(state->body + "/face/" + std::to_string(i));
            if (owners[i].op == state->boss) state->bossFaces.push_back(r);
            if (owners[i].op == state->base) state->baseFaces.push_back(r);
          }
          require(state->bossFaces.size() == 5 && !state->baseFaces.empty() && w.m_doc->scene.node(state->spare), "the fixture, Part and an empty component Spare");
          w.setWorkspace("design");
          w.m_viewport->standardView("iso");
          w.action("select.faces")->trigger();
          break;
        }
        case 1: {
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Face) return;
          require(selection().empty(), "nothing selected");
          const Menu m = rightClick(top, prefix + ".face.png");
          require(m.shown, "a right click on the boss's top opens the menu");
          require(one(state->bossFaces), "the right click selected the boss's face under the pointer");
          require(m.title == MainWindow::tr("Face of %1").arg("Part") && m.titleDrawn && m.names.contains("contextSketchOn"), "the menu is about that face, its title drawn: \"" + m.title.toStdString() + "\"");
          pass("a right click on the boss's top selects that face; the menu is about it, under its drawn title \"" + m.title + "\"");
          state->before = selection();
          break;
        }
        case 2: {
          if (!waitFor(area->found().ready, "smart selection answers for the face")) return;
          const Menu again = rightClick(top);
          require(again.shown && smart::sameRefs(selection(), state->before) && again.names.contains("smartSelectBest"), "again on the selected face: the selection stays (" + again.names.join(",").toStdString() + ")");
          pass("again on the selected face: the selection stays, the boss's entries are there");
          const Menu empty = rightClick(nothing);
          require(empty.shown && smart::sameRefs(selection(), state->before) && empty.title == MainWindow::tr("Face of %1").arg("Part"), "on nothing: the selection and its menu stay");
          pass("a right click on nothing keeps the selection and its menu");
          const Menu base = rightClick(baseTop);
          require(base.shown && one(state->baseFaces), "on a face of the base: the selection moves there");
          pass("a right click on a face of the base selects it instead");
          w.m_viewport->clearSelection();
          break;
        }
        case 3: {
          if (!waitFor(selection().empty() && w.m_selRefs.empty(), "the selection cleared")) return;
          w.action("inspect.distance")->trigger();
          require(w.m_tool.id == "distance", "Distance started");
          const Menu m = rightClick(top);
          require(m.shown && selection().empty(), "while Distance runs a right click picks nothing");
          w.cancelTool();
          pass("while Distance runs, a right click opens the menu and picks nothing");
          w.action("select.bodies")->trigger();
          break;
        }
        case 4: {
          if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Body || !w.m_tool.id.isEmpty()) return;
          const Menu m = rightClick(top, prefix + ".body.png");
          const auto sel = selection();
          require(m.shown && sel.size() == 1 && sel.front().body == state->body && sel.front().kind == opad::Ref::Kind::Body, "a right click on the body selects it");
          require(m.title == "Part" && m.titleDrawn && m.remove == MainWindow::tr("Remove %1").arg("Part"), "the body's menu: \"" + m.title.toStdString() + "\", " + m.remove.toStdString());
          pass("bodies: a right click selects the body, its menu \"" + m.title + "\" offers " + m.remove);
          w.m_viewport->clearSelection();
          // The empty component (in the browser): Remove Spare, one step.
          QMenu menu;
          w.buildContextMenu(menu, {state->spare});
          QAction* remove = nullptr;
          for (QAction* a : menu.actions())
            if (a->objectName() == "contextDelete") remove = a;
          require(remove && remove->text().startsWith(MainWindow::tr("Remove %1").arg("Spare")), "the empty component's menu offers Remove Spare");
          state->ops = w.m_doc->doc.ops.size();
          remove->trigger();
          break;
        }
        case 5: {
          if (!waitFor(w.m_doc->doc.ops.size() > state->ops, "Remove Spare is committed")) return;
          require(!w.m_doc->scene.node(state->spare) && w.m_doc->scene.all_bodies().size() == 1 && w.m_doc->doc.ops.size() == state->ops + 1, "Spare is gone, the body stays, one op");
          pass("Remove Spare takes the empty component out as one step");
          w.m_doc->undo();
          require(w.m_doc->scene.node(state->spare) && w.m_doc->doc.ops.size() == state->ops, "undone: Spare is back");
          pass("undone: Spare is back");
          timer->stop();
          QCoreApplication::exit(0);
          return;
        }
      }
      ++state->phase;
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: rightclick FAIL: %1").arg(e.what()));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
