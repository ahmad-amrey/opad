// OPAD_BENCH_NAMEDVIEWS=<prefix> (case named-views in tools/bench_cases/views.py, three boxes along X): the report "named view
// shortcuts, and offer to preserve hidden / shown objects". Named view 1-9 are commands with keys (Shift+Alt+1..9 at first,
// Save view Shift+Alt+V), listed in the shortcut editor and with help, pressed here as the keyboard sends them:
//   - Named view 1 with no view saved changes nothing and says how to save one, with Save view's key;
//   - Save view's dialog offers "Also keep which objects are hidden and shown", off at first and remembered: ticked, the
//     view op keeps the nodes hidden now (display.hidden); unticked, a camera only (as every view before it);
//   - Named view 1 turns the camera to it and hides what it kept hidden, shows everything else, in one step ("restore
//     view") that undo takes back; Named view 2 (a camera only) leaves what is shown as it is; Named view 3 (none) says so;
//   - the Named views list shows each entry's key, the user's binding after a remap; the keys reach the list's own handlers
//     (an exploded view explodes again);
//   - saved and read back, the view keeps what it hid.
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "KeyText.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"
#include "opad/document.hpp"
#include "opad/scene.hpp"

namespace {
bool settled(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_NAMEDVIEWS, namedviews) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: named-views: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  Viewport* v = w.m_viewport;
  AppDocument* doc = w.m_doc;
  const bool shown = settled([&w, v] { return !w.m_loadJob && v->displayedCount() > 0 && v->remainingBodies() == 0; }, 120000);
  std::vector<std::string> boxes = doc->scene.all_bodies();
  if (!require(shown && boxes.size() == 3, "the three boxes are displayed")) {
    QCoreApplication::exit(2);
    return true;
  }
  auto visible = [doc](const std::string& id) { const opad::Node* n = doc->scene.node(id); return n && n->visible; };
  auto direction = [v] {
    const opad::json c = v->cameraJson();
    const gp_Vec d(gp_Pnt(c["eye"][0], c["eye"][1], c["eye"][2]), gp_Pnt(c["target"][0], c["target"][1], c["target"][2]));
    return gp_Dir(d);
  };
  auto key = [&w, v](int n, Qt::KeyboardModifiers modifiers = Qt::ShiftModifier | Qt::AltModifier) {  // as the keyboard sends it
    QApplication::setActiveWindow(&w);
    v->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_0 + n, modifiers);
    QApplication::sendEvent(v, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_0 + n, modifiers);
    QApplication::sendEvent(v, &release);
  };
  // Save view through its dialog: what the box says when it opens, then the name typed and the box set.
  auto saveThroughDialog = [&w](const QString& name, bool keep, bool& offered) {
    offered = false;
    QTimer::singleShot(0, &w, [&w, name, keep, &offered] {
      auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
      auto* box = dialog ? dialog->findChild<QCheckBox*>("saveViewVisibility") : nullptr;
      auto* edit = dialog ? dialog->findChild<QLineEdit*>("saveViewName") : nullptr;
      if (!dialog || !box || !edit) {
        if (dialog) dialog->reject();
        return;
      }
      offered = box->isChecked();
      edit->setText(name);
      box->setChecked(keep);
      dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    w.action("view.saveview")->trigger();
  };
  auto lastView = [doc]() -> const opad::Op* { return !doc->doc.ops.empty() && doc->doc.ops.back().type == "view" ? &doc->doc.ops.back() : nullptr; };
  int menuTriggers = 0;
  QObject::connect(w.m_viewsMenu, &QMenu::triggered, &w, [&menuTriggers] { ++menuTriggers; });

  require(w.action("view.named1") && w.action("view.named9") && w.action("view.named1")->shortcut() == QKeySequence("Shift+Alt+1") &&
              w.action("view.named9")->shortcut() == QKeySequence("Shift+Alt+9") && w.action("view.saveview")->shortcut() == QKeySequence("Shift+Alt+V"),
          "Named view 1-9 are commands on Shift+Alt+1..9, Save view on Shift+Alt+V");

  // None saved yet.
  v->standardView("iso");
  const size_t ops = doc->doc.ops.size();
  key(1);
  const QString none = w.statusBar()->currentMessage();
  require(doc->doc.ops.size() == ops && direction().IsEqual(gp_Dir(-1, 1, -1), 1e-6) && none.contains("no named view 1") && none.contains(keys::text("view.saveview")),
          "Named view 1 with none saved changes nothing and says how to save one: " + none);

  // Top, the middle box hidden, kept with it.
  v->standardView("top");
  const gp_Dir top = direction();
  doc->run("appearance", opad::json{{"targets", {boxes[1]}}, {"visible", false}});
  bool offered = true;
  saveThroughDialog("Top without the middle box", true, offered);
  const opad::Op* first = lastView();
  require(!offered && first && first->data.value("name", "") == "Top without the middle box" && first->data.contains("display") &&
              first->data["display"].value("hidden", opad::json()) == opad::json::array({boxes[1]}) && QSettings().value("view/namedViewVisibility").toBool(),
          "Save view offers to keep what is hidden (off at first); ticked, the view keeps the hidden box and the choice is remembered");
  // Front, everything shown, a camera only.
  v->standardView("front");
  const gp_Dir front = direction();
  w.action("edit.showall")->trigger();
  offered = false;
  saveThroughDialog("Front", false, offered);
  const opad::Op* second = lastView();
  require(offered && second && second->data.value("name", "") == "Front" && !second->data.contains("display") && !QSettings().value("view/namedViewVisibility").toBool(),
          "the next Save view has the box ticked as left; unticked, the view is a camera only, as before");
  require(w.m_viewsMenu->actions().size() == 2 && w.m_viewsMenu->actions()[0]->text() == keys::menuText("Top without the middle box", "view.named1") &&
              w.m_viewsMenu->actions()[1]->text().endsWith(keys::plain(QKeySequence("Shift+Alt+2"))),
          "the Named views list shows each entry's key: " + w.m_viewsMenu->actions()[0]->text());

  // Recalled by key.
  doc->run("appearance", opad::json{{"targets", {boxes[2]}}, {"visible", false}});
  v->standardView("iso");
  const QStringList steps = doc->undoLabels();
  key(1);
  require(direction().IsEqual(top, 1e-6) && visible(boxes[0]) && !visible(boxes[1]) && visible(boxes[2]) && doc->undoLabels().size() == steps.size() + 1 &&
              doc->undoLabel() == MainWindow::tr("restore view") && menuTriggers == 1,
          "Named view 1 turns to its camera, hides the box it kept hidden and shows the others, in one step through the list's own entry");
  doc->undo();
  require(visible(boxes[1]) && !visible(boxes[2]) && direction().IsEqual(top, 1e-6), "undo brings what was shown back, the camera stays");
  key(2);
  require(direction().IsEqual(front, 1e-6) && visible(boxes[1]) && !visible(boxes[2]) && doc->undoLabels().size() == steps.size() && menuTriggers == 2,
          "Named view 2, a camera only, leaves what is shown as it is");
  key(3);
  require(direction().IsEqual(front, 1e-6) && w.statusBar()->currentMessage().contains("no named view 3"), "Named view 3, none saved, says so");

  // Remapped: the list and the key follow the user's binding.
  QAction* one = w.action("view.named1");
  one->setShortcut(QKeySequence("Ctrl+Alt+7"));
  keys::announce();
  require(w.m_viewsMenu->actions()[0]->text() == keys::menuText("Top without the middle box", "view.named1") &&
              w.m_viewsMenu->actions()[0]->text().contains(keys::plain(QKeySequence("Ctrl+Alt+7"))),
          "remapped, the list shows the new key: " + w.m_viewsMenu->actions()[0]->text());
  v->standardView("iso");
  key(7, Qt::ControlModifier | Qt::AltModifier);
  require(direction().IsEqual(top, 1e-6) && !visible(boxes[1]), "and the new key recalls it");
  one->setShortcut(QKeySequence("Shift+Alt+1"));
  keys::announce();

  // Saved and read back.
  const QString path = value + ".opad";
  require(doc->saveAs(path), "saved");
  try {
    const opad::Document back = opad::Document::load(std::filesystem::path(path.toStdU16String()));
    const opad::Scene scene = opad::resolve(back);
    bool kept = false, plain = false;
    for (const auto& view : scene.views) {
      kept = kept || (view.name == "Top without the middle box" && view.display.value("hidden", opad::json()) == opad::json::array({boxes[1]}));
      plain = plain || (view.name == "Front" && view.display.is_null());
    }
    require(kept && plain, "read back, the first view keeps what it hid, the second is a camera only");
  } catch (const std::exception& e) {
    require(false, QString("read back: ") + e.what());
  }
  trace::log(QString("bench: named-views: %1").arg(all ? "PASS" : "FAIL"));
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
