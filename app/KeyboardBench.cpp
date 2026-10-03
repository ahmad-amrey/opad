// Bench of keyboard-only use of the browser and the timeline, and of the items screen readers find in painted widgets
// (UI-124). The keys are BrowserTree's and TimelineWidget's; the items are AccessibilityArea.cpp's.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"

#include <QAccessible>
#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QVector3D>

// OPAD_BENCH_KEYBOARD=<prefix> (a box and the sketch "Plate"): in the browser, Space hides and shows the body row, then
// the sketch row; Enter fits the view to the body; F2 starts renaming it (Esc leaves the name); Del asks the window to
// delete it; the Menu key and Shift+F10 open its menu. On the timeline, Space suppresses the box and brings it back, Del
// tombstones it and Shift+Del restores it, Enter edits it, the Menu key opens its menu. A screen reader finds the markers
// as list items named as the tooltips name them, the current one selected, Press selecting what a marker touches; and the
// view cube's six faces as buttons named as the views (Top view ...), three facing an iso camera, Press on Front turning
// the view to the front.
OPAD_BENCH(OPAD_BENCH_KEYBOARD, keyboard) {
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: keyboard: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; std::function<bool()> until; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn, std::function<bool()> until = {}) { steps->push_back({delay, std::move(fn), std::move(until)}); };
  auto idle = [&w] { return !w.m_jobs->busy() && !w.m_design->busy(); };
  auto key = [](QWidget* to, int k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, k, mods);
    QApplication::sendEvent(to, &press);
  };
  // The menu runs its own loop: look at it from inside, then close it.
  auto menuAfter = [&w](std::shared_ptr<QStringList> seen) {
    QTimer::singleShot(300, &w, [seen] {
      for (QWidget* top : QApplication::topLevelWidgets())
        if (auto* m = qobject_cast<QMenu*>(top); m && m->isVisible()) {
          for (QAction* a : m->actions())
            if (!a->isSeparator()) *seen << QString(a->text()).remove('&').section('\t', 0, 0);
          m->close();
        }
    });
  };
  BrowserTree* tree = w.m_browser->tree();
  auto row = [tree](const QString& kind) -> QTreeWidgetItem* {
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
      if ((*it)->data(0, Qt::UserRole).toString() == kind) return *it;
    return nullptr;
  };
  auto opOf = [&w](const char* type) {
    for (const auto& op : w.m_doc->doc.ops)
      if (op.type == type) return op.id;
    return std::string();
  };
  const std::string feature = opOf("feature"), sketch = opOf("sketch");
  auto fits = std::make_shared<int>(0);
  auto commands = std::make_shared<QStringList>();
  QObject::connect(w.m_browser, &BrowserPanel::fitRequested, &w, [fits](const std::vector<std::string>&) { ++*fits; });
  QObject::connect(w.m_browser, &BrowserPanel::commandRequested, &w, [commands](const QString& id) { *commands << id; });
  auto deleted = [&w](const std::string& id) { return std::find(w.m_doc->scene.deleted_ops.begin(), w.m_doc->scene.deleted_ops.end(), id) != w.m_doc->scene.deleted_ops.end(); };
  auto suppressed = [&w, feature] { const opad::Feature* f = w.m_doc->scene.feature(feature); return f && f->suppressed; };

  // ---- the browser
  add(600, [=, &w] {
    QTreeWidgetItem* body = row("body");
    QTreeWidgetItem* plate = row("sketch");
    check(body && plate && !feature.empty() && !sketch.empty(), "the box's body and the sketch are in the browser, their ops on the timeline");
    if (!body || !plate) return;
    const std::string id = body->data(0, browser::kIdRole).toString().toStdString(), name = w.m_doc->node(id)->name;
    tree->setCurrentItem(body);  // every change rebuilds the rows: look them up again after one
    key(tree, Qt::Key_Space);
    const bool hidden = !w.m_doc->node(id)->visible;
    key(tree, Qt::Key_Space);
    check(hidden && w.m_doc->node(id)->visible, "Space hides the body's row and shows it again");
    check(tree->currentItem() && tree->currentItem()->data(0, browser::kIdRole).toString().toStdString() == id, "the keyboard's row stays current while the rows are rebuilt");
    tree->setCurrentItem(row("sketch"));
    key(tree, Qt::Key_Space);
    const bool sketchHidden = !w.m_doc->scene.sketch(sketch)->visible;
    key(tree, Qt::Key_Space);
    check(sketchHidden && w.m_doc->scene.sketch(sketch)->visible, "Space hides the sketch and shows it again");
    tree->setCurrentItem(row("body"));
    key(tree, Qt::Key_Return);
    check(*fits == 1, "Enter on the body fits the view to it");
    key(tree, Qt::Key_F2);
    QWidget* editor = tree->indexWidget(tree->currentIndex());
    if (editor) key(editor, Qt::Key_Escape);
    check(commands->contains("edit.rename") && editor && !tree->indexWidget(tree->currentIndex()) && w.m_doc->node(id)->name == name,
          "F2 renames the row (Rename), Esc leaves the name");
    key(tree, Qt::Key_Delete);  // asks first: the bench's guard answers Cancel
    check(commands->contains("edit.delete") && w.m_doc->node(id) && !deleted(w.m_doc->node(id)->source_op), "Del runs Delete on the row (its question cancelled)");
    auto seen = std::make_shared<QStringList>();
    menuAfter(seen);
    key(tree, Qt::Key_Menu);
    check(!seen->isEmpty(), "the Menu key opens the row's menu (" + seen->join(", ") + ")");
  }, idle);
  add(500, [=] {  // past the guard against the Menu key's second event
    auto seen = std::make_shared<QStringList>();
    menuAfter(seen);
    tree->setCurrentItem(row("body"));
    key(tree, Qt::Key_F10, Qt::ShiftModifier);
    check(!seen->isEmpty(), "Shift+F10 opens it too");
  }, idle);

  // ---- the timeline
  add(200, [=, &w] {
    w.m_timeline->setCurrentOp(feature);
    key(w.m_timeline, Qt::Key_Space);
  }, idle);
  add(100, [=, &w] {
    check(suppressed(), "Space on the box's marker suppresses it");
    key(w.m_timeline, Qt::Key_Space);
  }, [=] { return idle() && suppressed(); });
  add(100, [=, &w] {
    check(!suppressed(), "Space again brings it back");
    key(w.m_timeline, Qt::Key_Delete);
  }, [=] { return idle() && !suppressed(); });
  add(100, [=, &w] {
    check(deleted(feature), "Del tombstones the marker's op");
    key(w.m_timeline, Qt::Key_Delete, Qt::ShiftModifier);
  }, [=] { return idle() && deleted(feature); });
  add(100, [=, &w] {
    check(!deleted(feature), "Shift+Del restores it");
    w.m_timeline->setCurrentOp(feature);
    key(w.m_timeline, Qt::Key_Return);
  }, [=] { return idle() && !deleted(feature); });
  add(300, [=, &w] {
    check(w.m_design->featureActive(), "Enter edits the feature");
    w.m_design->escape();
  }, [&w] { return w.m_design->featureActive() && !w.m_design->busy(); });
  add(300, [=, &w] {
    auto seen = std::make_shared<QStringList>();
    menuAfter(seen);
    w.m_timeline->setCurrentOp(feature);
    key(w.m_timeline, Qt::Key_Menu);
    const QString edit = QCoreApplication::translate("MainWindow", "Edit feature"), suppress = QCoreApplication::translate("MainWindow", "Suppress");
    check(seen->contains(edit) && seen->contains(suppress), "the Menu key opens the marker's menu (" + seen->join(", ") + ")");
  }, [=, &w] { return idle() && !w.m_design->featureActive(); });

  // ---- what a screen reader finds
  add(200, [=, &w] {
    QAccessibleInterface* line = QAccessible::queryAccessibleInterface(w.m_timeline);
    const int markers = w.m_timeline->markerCount();
    QAccessibleInterface* first = line && markers > 0 ? line->child(0) : nullptr;
    const int at = w.m_timeline->currentMarker();
    QAccessibleInterface* current = line && at >= 0 ? line->child(at) : nullptr;
    check(line && line->role() == QAccessible::List && markers >= 2 && line->childCount() >= markers && first && first->role() == QAccessible::ListItem &&
              first->text(QAccessible::Name) == w.m_timeline->describe(*w.m_timeline->markerOp(0)) && line->indexOfChild(first) == 0,
          QString("the timeline is a list of its %1 markers, named as their tooltips (%2)").arg(markers).arg(first ? first->text(QAccessible::Name) : QString()));
    check(current && current->state().selected && w.m_timeline->rect().contains(w.m_timeline->mapFromGlobal(current->rect().center())), "the current marker is selected and placed on the strip");
    QAccessibleInterface* other = line && markers >= 2 ? line->child(at == 0 ? 1 : 0) : nullptr;
    if (auto* action = other ? other->actionInterface() : nullptr) action->doAction(QAccessibleActionInterface::pressAction());
    check(other && w.m_timeline->currentMarker() == (at == 0 ? 1 : 0) && other->state().selected, "Press on another marker makes it current");
    QAccessibleInterface* view = QAccessible::queryAccessibleInterface(w.m_viewport);
    QStringList names;
    int facing = 0;
    QAccessibleInterface* front = nullptr;
    const QString frontName = QCoreApplication::translate("MainWindow", "Front view");
    for (int i = 0; view && i < view->childCount(); ++i) {
      QAccessibleInterface* c = view->child(i);
      if (!c || c->object() || c->role() != QAccessible::PushButton) continue;
      names << c->text(QAccessible::Name);
      if (!c->state().invisible && w.m_viewport->rect().contains(w.m_viewport->mapFromGlobal(c->rect().center()))) ++facing;
      if (c->text(QAccessible::Name) == frontName) front = c;
    }
    check(names.size() == 6 && names.contains(QCoreApplication::translate("MainWindow", "Top view")) && front, "the view cube's six faces are buttons named as the views (" + names.join(", ") + ")");
    check(facing == 3, QString("three of them face the iso camera (%1)").arg(facing));
    if (front && front->actionInterface()) front->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    const opad::json camera = w.m_viewport->cameraJson();
    const QVector3D eye(camera["eye"][0].get<float>(), camera["eye"][1].get<float>(), camera["eye"][2].get<float>()),
        target(camera["target"][0].get<float>(), camera["target"][1].get<float>(), camera["target"][2].get<float>());
    const QVector3D toEye = (eye - target).normalized();
    check(toEye.y() < -0.99f, QString("Press on Front view turns the view to the front (eye direction %1, %2, %3)").arg(toEye.x()).arg(toEye.y()).arg(toEye.z()));
    w.m_timeline->grab().save(value + ".timeline.png");
    trace::log(QString("bench: keyboard: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  }, idle);

  auto next = std::make_shared<std::function<void(size_t, int)>>();
  *next = [&w, steps, next, check](size_t i, int waited) {
    if (i >= steps->size()) return;
    QTimer::singleShot(waited ? 50 : (*steps)[i].delay, &w, [steps, next, check, i, waited] {
      const Step& s = (*steps)[i];
      if (s.until && !s.until() && waited < 300) return (*next)(i, waited + 1);
      try { s.fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1, 0);
    });
  };
  (*next)(0, 0);
  return true;
}
