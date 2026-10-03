// OPAD_BENCH_STATE=<prefix> (UI-09): state and selection bugs found by hand. Case in tools/bench_cases/viewer.py, on a
// drawing opened in viewer mode. (1) "Edit unsaved copy" then Rename: once the copy is made the row's editor is open in
// the expanded browser with the name, and a name typed while the keyboard is elsewhere goes into it (no one-key command
// runs), Return renames. (2) The drawing viewed again (2D mode on, the viewer card), then Ctrl+N: 2D mode off, the card
// gone, and a click on a stale "Save to edit" changes nothing. (3) Three boxes: Ctrl+click adds a body to the selection
// and takes a picked one out; the status's hover text is cleared by a document change and says the new name on the next
// frame. (4) Ctrl+Shift+Z redoes. (5) V right after a modal dialog closed hides nothing; 300 ms later it hides.
// <prefix>.png is the window with the selection.
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <functional>
#include <memory>
#include <set>

#include "BenchRegistry.hpp"
#include "KeyGuard.hpp"
#include "MainWindow.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_STATE, state) {
  static bool running = false;  // the file opened again below finishes loading into runBench once more
  if (running) return true;
  running = true;
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: state: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] { QCoreApplication::exit(all ? 0 : 2); return true; };
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  auto settled = [&w, doc, v] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
    return !doc->loading && !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() + v->skippedCount() >= expected && !doc->designBusy;
  };
  auto saveToEdit = [&w]() -> QToolButton* {
    for (auto* button : w.m_chips->findChildren<QToolButton*>())
      if (button->text() == ViewportChips::tr("Save to edit")) return button;
    return nullptr;
  };
  auto cardShown = [&] { QToolButton* b = saveToEdit(); return b && !b->isHidden(); };
  auto key = [v](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {}) {  // as the shortcut bench types
    QWidget* to = QApplication::focusWidget() ? QApplication::focusWidget() : v;
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QApplication::sendEvent(to, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
    QApplication::sendEvent(to, &release);
  };
  auto commands = std::make_shared<int>(0);  // one-key commands run while a name is typed
  for (QAction* a : w.m_actions)
    if (!a->shortcut().isEmpty() && KeyGuard::oneKey(std::make_unique<QKeyEvent>(QEvent::KeyPress, a->shortcut()[0].key(), a->shortcut()[0].keyboardModifiers()).get()))
      QObject::connect(a, &QAction::triggered, &w, [commands] { ++*commands; });

  // (1) Viewer mode, Edit unsaved copy, Rename.
  const QString drawing = doc->viewing;
  if (!require(waitUntil(settled, 60000) && doc->browse && w.action("view.2d")->isChecked() && cardShown(), "a drawing in viewer mode: 2D mode on, the viewer card")) return finish();
  const std::string body = doc->scene.all_bodies().front();
  w.m_browser->setSelectedIds({body});
  w.onBrowserSelection({body});
  waitUntil([&] { return w.currentNodeIds() == std::vector<std::string>{body}; }, 10000);
  w.makeEditable({}, [&w] { w.action("edit.rename")->trigger(); });  // as "Edit unsaved copy" resumes the command
  const bool editing = waitUntil([&] { return !doc->browse && w.m_browser->renameEditor(); }, 60000);
  auto* editor = qobject_cast<QLineEdit*>(w.m_browser->renameEditor());
  require(editing && editor && !editor->isHidden() && w.m_browserOverlay->expanded() && editor->text() == doc->nodeName(body) && !cardShown(),
          QString("editable copy: the row's editor open in the expanded browser with \"%1\", the viewer card gone").arg(editor ? editor->text() : QString()));
  if (!editor) return finish();
  // Typed where the keyboard is: the editor itself, or (its window not active) the view, from where KeyGuard hands the
  // keys over (tests/test_shortcuts covers that path on its own).
  const bool focused = QApplication::focusWidget() == editor;
  *commands = 0;
  for (const QChar c : QString("Part")) key(Qt::Key_A + (c.toLower().unicode() - 'a'), c.isUpper() ? Qt::ShiftModifier : Qt::NoModifier, QString(c));
  const QString typed = editor->text();
  require(typed == "Part" && *commands == 0 && !w.m_annotationEditor,
          QString("typed into the editor (%1): \"%2\", %3 one-key commands run").arg(focused ? "it has the keyboard" : "handed over").arg(typed).arg(*commands));
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(editor, &enter);
  const bool renamed = waitUntil([&] { return doc->nodeName(body) == "Part"; }, 5000);
  require(renamed && !w.m_browser->renameEditor(), QString("Return renames: \"%1\"").arg(doc->nodeName(body)));

  // (2) Viewed again, then Ctrl+N.
  w.openPath(drawing);
  const bool reopened = waitUntil([&] { return doc->browse && settled() && doc->scene.all_bodies().size() > 0; }, 60000);
  require(reopened && w.action("view.2d")->isChecked() && cardShown(), "viewed again: 2D mode on, the viewer card");
  w.action("file.new")->trigger();
  require(!doc->browse && !w.action("view.2d")->isChecked() && !w.m_autoTwoD && !cardShown() && !v->twoDimensional() &&
              v->selectionFilter() == Viewport::SelFilter::Body && w.action("select.bodies")->isChecked(),
          "Ctrl+N: 2D mode off, the viewer card gone, bodies picked again (not the drawing's edges)");
  emit w.m_chips->saveToEditRequested();  // a stale card's click
  require(!doc->browse && !cardShown() && doc->hasDocument && doc->doc.ops.empty(), "a stale Save to edit changes nothing");

  // (3) Ctrl+click on three boxes; the hover text after a change.
  for (int i = 0; i < 3; ++i)
    doc->run("feature", {{"kind", "box"}, {"inputs", {{"x", std::to_string(40 * i) + " mm"}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}});
  const auto boxes = doc->scene.all_bodies();
  if (!require(boxes.size() == 3 && waitUntil(settled, 60000), "three boxes displayed")) return finish();
  v->fitAll();
  int ax = 0, ay = 0, bx = 0, by = 0;
  if (!require(v->benchBodyPoint(boxes[0], ax, ay) && v->benchBodyPoint(boxes[2], bx, by), "two boxes found on screen")) return finish();
  const QPointF a(ax / v->displayScale(), ay / v->displayScale()), b(bx / v->displayScale(), by / v->displayScale());
  auto selected = [v] {
    std::set<std::string> ids;
    for (const auto& r : v->selection()) ids.insert(r.body);
    return ids;
  };
  v->benchClickAt(a);
  const auto one = selected();
  v->benchClickAt(b, Qt::ControlModifier);
  const auto two = selected();
  v->benchClickAt(a, Qt::ControlModifier);
  const auto toggled = selected();
  require(one == std::set<std::string>{boxes[0]} && two == std::set<std::string>{boxes[0], boxes[2]} && toggled == std::set<std::string>{boxes[2]},
          QString("click, Ctrl+click adds (%1 selected), Ctrl+click on a picked one takes it out (%2 left)").arg(two.size()).arg(toggled.size()));
  v->benchClickAt(a, Qt::ControlModifier);  // the first one back, the pointer resting on it
  const QString before = w.m_statusHover->text();
  doc->run("rename", {{"target", boxes[0]}, {"name", "Renamed box"}});
  const QString cleared = w.m_statusHover->text();
  v->benchClickAt(a, Qt::ControlModifier);  // a frame with the pointer still there (the click takes it out again)
  const QString after = w.m_statusHover->text();
  require(!before.isEmpty() && cleared.isEmpty() && after.contains("Renamed box"),
          QString("hover text \"%1\", cleared by a change, then \"%2\"").arg(before, after));
  const QImage shot = w.grab().toImage();
  if (!shot.isNull()) shot.save(value + ".png");

  // (4) Ctrl+Shift+Z redoes.
  const auto keys = w.action("edit.redo")->shortcuts();
  doc->run("appearance", {{"target", boxes[1]}, {"visible", false}});
  doc->undo();
  const bool shownAgain = doc->scene.node(boxes[1])->visible;
  QApplication::setActiveWindow(&w);
  v->setFocus();
  key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  require(keys.contains(QKeySequence("Ctrl+Shift+Z")) && keys.contains(QKeySequence(QKeySequence::Redo)) && shownAgain && !doc->scene.node(boxes[1])->visible,
          QString("Ctrl+Shift+Z redoes (redo keys: %1)").arg(QKeySequence::listToString(keys)));
  doc->undo();

  // (5) V right after a modal dialog: nothing hidden; 300 ms later V hides.
  w.m_browser->setSelectedIds({boxes[1]});
  w.onBrowserSelection({boxes[1]});
  waitUntil([&] { return w.currentNodeIds() == std::vector<std::string>{boxes[1]}; }, 10000);
  int hides = 0;
  const auto watch = QObject::connect(w.action("edit.hide"), &QAction::triggered, &w, [&hides] { ++hides; });
  {
    QDialog dialog(&w);
    QTimer::singleShot(30, &dialog, &QDialog::accept);
    dialog.exec();
  }
  QApplication::setActiveWindow(&w);
  v->setFocus();
  key(Qt::Key_V, Qt::NoModifier, "v");
  const bool held = hides == 0 && doc->scene.node(boxes[1])->visible;
  waitUntil([] { return false; }, KeyGuard::kQuietMs + 50);
  v->setFocus();
  key(Qt::Key_V, Qt::NoModifier, "v");
  QObject::disconnect(watch);
  require(held && hides == 1 && !doc->scene.node(boxes[1])->visible, QString("V after a dialog closed: held back, then hides (%1 hide)").arg(hides));
  return finish();
}
