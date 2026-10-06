// The help shows the user's keys (TODO 11 wave 3, help audit §4.3 and §6.3 test 6), checked in the running app with a
// remapped keyboard; the key formatter is KeyText.hpp, the help area HelpArea.cpp.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "CommandHelp.hpp"
#include "CommandPalette.hpp"
#include "HelpClip.hpp"
#include "HelpReference.hpp"
#include "HelpWindows.hpp"
#include "I18n.hpp"
#include "GuidedTool.hpp"
#include "KeyText.hpp"
#include "PanelFooter.hpp"
#include "Panels.hpp"
#include "Ribbon.hpp"
#include "RichTip.hpp"
#include "ShortcutEditor.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace {
QString plain(QString s) { return s.remove(QChar(0x2066)).remove(QChar(0x2069)); }  // a key's text without its isolates
}  // namespace

// OPAD_BENCH_KEYHELP=<prefix> (a document with a box; the case's settings remap view.fit to Ctrl+Alt+F, inspect.pin to
// Ctrl+Alt+J, help.current to Ctrl+F1, help.shortcuts to Ctrl+Shift+K, tools.commands to Ctrl+Space, select.faces to
// Ctrl+Alt+2, clear view.home and view.unisolate, and save a key for Clear measurement): every help surface shows the
// user's key, never the default. The rich card's caps and hint, Ctrl+F1 expanding it (F1 no longer does) and opening the
// guide; the palette's key and search by key ("ctrl+alt+f" finds Fit); the Tool guide's key column and card; the cheat
// sheet's rows, Help for this tool's row and its closing key; the lessons and the coach card's steps; a clip's key element
// (the command's key, its name without one, a fixed key) and caption. Esc stays Clear measurement's (fixed). Then the
// shortcut editor puts Fit on F and the cheat sheet on Ctrl+Alt+K: every open surface follows without reopening. Right
// to left (OPAD_LANG=ar) the caps read Ctrl ... F left to right and the key text is isolated. Saved as
// <prefix>.card/.palette/.guide/.sheet/.start/.clip-*.png and <prefix>.after-*.png.
OPAD_BENCH(OPAD_BENCH_KEYHELP, keyhelp) {
  const QString prefix = value;
  QSettings().setValue("ui/tipAnimate", true);
  QSettings().setValue("ui/reduceMotion", false);
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: keyhelp: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  const bool rtl = QApplication::isRightToLeft();
  auto filterHints = [](MainWindow& w) {  // the ribbon's Bodies, Faces, Edges, Vertices segments: their keys as shown
    QStringList out;
    for (const char* id : {"select.bodies", "select.faces", "select.edges", "select.vertices"})
      for (SegmentButton* b : w.m_ribbon->findChildren<SegmentButton*>())
        if (b->defaultAction() == w.action(id)) out << b->hint();
    return out;
  };
  w.m_ribbon->setWorkspace(0);
  w.m_ribbon->setCurrentTab(0);  // Review > View: Fit and Home
  auto button = [&w](const char* id) -> QToolButton* {
    for (auto* b : w.m_ribbon->findChildren<QToolButton*>()) if (b->defaultAction() == w.action(id) && b->isVisibleTo(w.m_ribbon)) return b;
    return nullptr;
  };
  auto key = [](QWidget* target, int k, Qt::KeyboardModifiers m, QEvent::Type type = QEvent::KeyPress) {
    QKeyEvent e(type, k, m);
    e.ignore();  // taken only when someone accepts it
    QApplication::sendEvent(target, &e);
    return e.isAccepted();
  };
  using State = RichTip::State;
  RichTip* tip = RichTip::instance();
  // A clip of key elements (the library's clips name their commands once WP1 converts them): Fit's, Home's (none), Enter.
  const QString clipFile = QDir(QFileInfo(prefix).absolutePath()).filePath(QFileInfo(prefix).fileName() + ".keys.json");
  {
    QFile f(clipFile);
    if (f.open(QIODevice::WriteOnly))
      f.write(R"({"clips": [{"id": "keys", "duration": 3, "items": [{"el": "grid"},
        {"el": "key", "command": "view.fit", "press": 0.6, "from": 0, "to": 1},
        {"el": "key", "command": "view.home", "from": 1, "to": 2},
        {"el": "key", "fixed": "enter", "press": 2.6, "from": 2, "to": 3}],
       "steps": [{"to": 1, "caption": "{press:view.fit}"}, {"to": 2, "caption": "{press:view.home}", "captionNoKey": "Home: choose it in the View menu"},
                 {"to": 3, "caption": "Pin ({key:inspect.pin}) keeps it"}]}]})");
  }
  auto frames = [prefix](const QString& name) {
    clips::Options o;
    o.rtl = QApplication::isRightToLeft();
    for (double t : {0.6, 1.5, 2.6}) clips::frame("keys", t, {576, 324}, 1, o).save(QString("%1.%2-%3.png").arg(prefix, name).arg(t));
  };

  add(800, [=, &w] {
    // The remapped settings as the window read them; Esc stays fixed.
    check(w.action("view.fit")->shortcut() == QKeySequence("Ctrl+Alt+F") && w.action("view.home")->shortcut().isEmpty() &&
              w.action("help.current")->shortcut() == QKeySequence("Ctrl+F1") && w.action("help.shortcuts")->shortcut() == QKeySequence("Ctrl+Shift+K"),
          "the remapped keys are the window's (Fit Ctrl+Alt+F, Home none, Help for this tool Ctrl+F1, cheat sheet Ctrl+Shift+K)");
    check(w.action("inspect.clear")->shortcut() == QKeySequence("Esc") && !QSettings().contains("shortcuts/inspect.clear") && shortcuts::fixedKey(w.action("inspect.clear")),
          "Esc stays Clear measurement's: a saved key is dropped, the editor cannot change it");
    QToolButton *fit = button("view.fit"), *home = button("view.home");
    if (!fit || !home) return check(false, "Fit and Home on the ribbon");
    // The rich card.
    tip->showFor(fit, State::Compact);
    const QString helpKey = keys::text("help.current");
    check(tip->keyCaps() == QStringList({"Ctrl", "Alt", "F"}) && tip->hint() == RichTip::tr("Shift or %1 for more").arg(helpKey) && plain(helpKey) == "Ctrl+F1",
          "the card: Fit's caps Ctrl Alt F, and \"" + plain(tip->hint()) + "\"");
    const QList<QRect> caps = tip->keyRects();
    check(caps.size() == 3 && caps[0].left() < caps[1].left() && caps[1].left() < caps[2].left() && (!rtl || caps[2].right() < tip->width() / 2),
          "the caps read Ctrl, Alt, F left to right" + QString(rtl ? ", on the card's left (mirrored as a group)" : ""));
    tip->grab().save(prefix + ".card.png");
    const bool f1 = key(fit, Qt::Key_F1, Qt::NoModifier, QEvent::ShortcutOverride);
    key(fit, Qt::Key_F1, Qt::NoModifier);
    check(!f1 && tip->state() == State::Hidden, "F1 (no longer Help for this tool's) is not taken: it hides the card like any other key");
    tip->showFor(fit, State::Compact);
    const bool ctrlF1 = key(fit, Qt::Key_F1, Qt::ControlModifier, QEvent::ShortcutOverride);
    key(fit, Qt::Key_F1, Qt::ControlModifier);
    check(ctrlF1 && tip->state() == State::Expanded, "Ctrl+F1 expands the card at once");
    check(tip->hint() == RichTip::tr("%1 for the tool guide").arg(helpKey), "the expanded card names Ctrl+F1 for the guide (" + plain(tip->hint()) + ")");
    key(fit, Qt::Key_F1, Qt::ControlModifier, QEvent::ShortcutOverride);
    key(fit, Qt::Key_F1, Qt::ControlModifier);
    auto* reference = w.findChild<CommandReference*>();
    check(reference && reference->isVisible() && reference->current() == "view.fit" && tip->state() == State::Hidden, "Ctrl+F1 on the expanded card opens the tool guide at Fit");
    if (reference) reference->close();
    tip->showFor(home, State::Compact);
    check(tip->keyCaps().isEmpty(), "Home has no key: no caps");
    tip->hideTip();
    // The clip: the command's key, its name without one, a fixed key; the captions with the keys or the keyless caption.
    clips::load(clipFile);
    const auto resolved = clips::resolvedKeys("keys");
    check(resolved.size() == 3 && resolved[0] == QStringList({"Ctrl", "Alt", "F"}) && resolved[1] == QStringList({help::title("view.home")}) &&
              resolved[2] == keys::fixedCaps("enter"),
          "a clip's key elements: Fit's Ctrl Alt F, Home by its name, Enter");
    const auto clipSteps = clips::steps("keys");
    check(plain(clips::caption(clipSteps[0])) == plain(help::expand(QCoreApplication::translate("help", "Press %1").arg(keys::text("view.fit")))) &&
              clips::caption(clipSteps[1]) == i18n::t("Home: choose it in the View menu") && plain(clips::caption(clipSteps[2])).contains("Ctrl+Alt+J"),
          "its captions: \"" + plain(clips::caption(clipSteps[0])) + "\", the keyless one for Home, Pin's Ctrl+Alt+J");
    frames("clip");
    clips::load();  // the library again (the Tool guide plays its clips)
    // The library's clips name their commands: Fit's clip shows Ctrl Alt F and says so, Home's (no key) shows Home by
    // its name and says "Choose Home"; Isolate's details drop "(key)" with Exit isolate unassigned.
    const auto fitSteps = clips::steps("view.fit"), homeSteps = clips::steps("view.home");
    const QString pressFit = plain(QCoreApplication::translate("help", "Press %1").arg(keys::text("view.fit")));
    const QString chooseHome = QCoreApplication::translate("help", "Choose %1").arg(help::title("view.home"));
    check(clips::resolvedKeys("view.fit", 1.0) == QList<QStringList>({{"Ctrl", "Alt", "F"}}) && !fitSteps.isEmpty() && plain(clips::caption(fitSteps[0])) == pressFit,
          "the library's Fit clip: Ctrl Alt F, \"" + (fitSteps.isEmpty() ? QString() : plain(clips::caption(fitSteps[0]))) + "\"");
    check(clips::resolvedKeys("view.home", 0.9) == QList<QStringList>({{help::title("view.home")}}) && !homeSteps.isEmpty() && clips::caption(homeSteps[0]) == chooseHome,
          "the library's Home clip: Home by its name, \"" + (homeSteps.isEmpty() ? QString() : clips::caption(homeSteps[0])) + "\"");
    const QString isolate = help::expand(help::find("view.isolate")->details);
    check(!isolate.contains("()") && !isolate.contains(" (") && isolate.contains(help::title("view.unisolate")), "Isolate's details without Exit isolate's key: \"" + isolate + "\"");
    clips::Options o;
    o.rtl = rtl;
    clips::frame("view.fit", 1.0, {576, 324}, 1, o).save(prefix + ".library-fit.png");
    clips::frame("view.home", 0.9, {576, 324}, 1, o).save(prefix + ".library-home.png");
  });
  add(300, [=, &w] {
    // The ribbon: the search badge and tooltip, the filters' keys.
    SearchField* search = w.m_ribbon->searchField();
    check(search && search->key() == "Ctrl+Space" && plain(search->toolTip()).contains("(Ctrl+Space)"), "the ribbon's search badge and tooltip: Ctrl+Space (" + (search ? search->key() : QString()) + ")");
    check(filterHints(w) == QStringList({"1", "Ctrl+Alt+2", "3", "4"}), "the ribbon's filters: 1, Ctrl+Alt+2, 3, 4 (" + filterHints(w).join(' ') + ")");
    w.m_ribbon->grab().save(prefix + ".ribbon.png");
    // Distance: Pin's key in the footer, the filters' keys in the prompt.
    w.action("inspect.distance")->trigger();
  });
  add(400, [=, &w] {
    PanelFooter* footer = w.m_toolSteps ? w.m_toolSteps->footer() : nullptr;
    check(footer && PanelFooter::key(footer->primary()) == "Ctrl+Alt+J", "Distance's footer: Pin to document Ctrl+Alt+J (" + (footer ? PanelFooter::key(footer->primary()) : QString()) + ")");
    const QString hints = plain(w.m_prompt->hints());
    check(hints.contains("1/Ctrl+Alt+2/3/4") && !hints.contains(QString::fromUtf8("1–4")), "its prompt names the filters' keys (" + hints + ")");
    if (w.m_toolPanel) w.m_toolPanel->grab().save(prefix + ".distance.png");
    w.m_prompt->grab().save(prefix + ".prompt.png");
  });
  add(300, [=, &w] {
    // The palette: Fit's key, and a key typed finds its command.
    auto* palette = new CommandPalette(w.m_actions, &w);
    palette->setAttribute(Qt::WA_DeleteOnClose);
    palette->show();
    auto* items = palette->findChild<QListWidget*>("paletteList");
    palette->findChild<QLineEdit*>("paletteInput")->setText("ctrl+alt+f");
    auto first = [items] { return items->count() ? static_cast<QAction*>(items->item(0)->data(Qt::UserRole).value<void*>())->objectName() : QString(); };
    check(first() == "view.fit", "the palette: \"ctrl+alt+f\" finds Fit first (" + first() + ")");
    palette->findChild<QLineEdit*>("paletteInput")->setText("Ctrl+Space");
    check(first() == "tools.commands", "and Ctrl+Space Search commands");
    auto* preview = palette->findChild<CommandPreview*>();
    check(preview && preview->keyCaps() == QStringList({"Ctrl", "Space"}), "its card shows Ctrl Space");
    palette->findChild<QLineEdit*>("paletteInput")->setText("ctrl+alt+f");
  });
  add(300, [=, &w] {  // laid out: its card's clip and summary in place
    if (auto* palette = w.findChild<CommandPalette*>()) {
      palette->grab().save(prefix + ".palette.png");
      palette->close();
    }
    // The Tool guide: the key column and the card.
    w.action("help.reference")->trigger();
    auto* reference = w.findChild<CommandReference*>();
    if (!reference) return check(false, "the Tool guide");
    reference->open("view.fit");
    QTreeWidgetItem *fitRow = nullptr, *homeRow = nullptr;
    for (QTreeWidgetItemIterator it(reference->findChild<QTreeWidget*>("referenceList")); *it; ++it) {
      if ((*it)->data(0, Qt::UserRole).toString() == "view.fit") fitRow = *it;
      if ((*it)->data(0, Qt::UserRole).toString() == "view.home") homeRow = *it;
    }
    check(fitRow && plain(fitRow->text(1)) == "Ctrl+Alt+F" && homeRow && homeRow->text(1).isEmpty(), "the Tool guide's key column: Fit Ctrl+Alt+F, Home none");
    check(reference->preview()->keyCaps() == QStringList({"Ctrl", "Alt", "F"}), "its card: Ctrl Alt F");
    if (rtl) {
      QLabel *ctrl = nullptr, *f = nullptr;
      for (QLabel* l : reference->preview()->findChildren<QLabel*>("keycap")) {
        if (l->text() == "Ctrl") ctrl = l;
        if (l->text() == "F") f = l;
      }
      check(ctrl && f && ctrl->mapTo(reference, QPoint()).x() < f->mapTo(reference, QPoint()).x(), "right to left the card's caps still read Ctrl ... F");
    }
    reference->setFilter("ctrl+alt+j");
    check(reference->shown() == QStringList({"inspect.pin"}), "searching a key finds Pin (" + reference->shown().join(' ') + ")");
    reference->setFilter(QString());
    reference->open("help.current");
    const QString details = help::expand(help::find("help.current")->details);
    check(details.contains(QChar(0x2066)) && plain(details).contains("Ctrl+F1"), "Help for this tool's details name Ctrl+F1, isolated");
    reference->open("view.fit");
  });
  add(400, [=, &w] {
    if (auto* reference = w.findChild<CommandReference*>()) reference->grab().save(prefix + ".guide.png");  // laid out
    // The cheat sheet: rows, Help for this tool's row, its closing key.
    w.action("help.shortcuts")->trigger();
    auto* sheet = w.findChild<ShortcutSheet*>();
    if (!sheet) return check(false, "the cheat sheet");
    const QStringList rows = sheet->shown();
    const QString fit = help::find("view.fit")->title, home = help::find("view.home")->title;
    bool homeListed = false;
    for (const QString& r : rows) homeListed = homeListed || r.startsWith(home + ' ');
    check(rows.contains(fit + " Ctrl+Alt+F") && !homeListed, "the cheat sheet: Fit Ctrl+Alt+F, Home not listed (no key)");
    check(rows.contains(QCoreApplication::translate("help", "Guide of the tool you are using") + " Ctrl+F1"), "its Help for this tool row is Ctrl+F1");
    check(sheet->closeKey() == QKeySequence("Ctrl+Shift+K"), "it closes on Ctrl+Shift+K (the key that opened it), not on ?");
    sheet->setFilter("ctrl+alt+f");
    check(sheet->shown() == QStringList({fit + " Ctrl+Alt+F"}), "searching the key finds Fit");
    sheet->setFilter(QString());
    // Getting started and the coach card.
    w.action("help.start")->trigger();
    auto* start = w.findChild<GettingStarted*>();
    if (!start) return check(false, "Getting started");
    const QString move = plain(GettingStarted::lessons("fusion")[0].text), find = plain(GettingStarted::lessons("fusion")[5].text);
    check(move.contains("(Ctrl+Alt+F)") && !move.contains("()") && !move.contains(home + " ("), "lesson 1 names Fit's Ctrl+Alt+F and Home without a key");
    check(find.contains("Ctrl+Space") && find.contains("Ctrl+F1") && find.contains("Ctrl+Shift+K") && !find.contains("(?)"), "lesson 6: Ctrl+Space, Ctrl+F1, Ctrl+Shift+K");
    auto* coach = w.m_viewport->findChild<CoachCard*>();
    check(coach && plain(coach->steps()).contains("(" + keys::plain(keys::binding("sketch.line")) + ")") && plain(coach->steps()).contains("(" + keys::plain(keys::binding("design.extrude")) + ")"),
          "the coach card's steps name the tools' keys (" + (coach ? plain(coach->steps()) : QString()) + ")");
  });
  add(400, [=, &w] {
    w.findChild<ShortcutSheet*>()->grab().save(prefix + ".sheet.png");
    w.findChild<GettingStarted*>()->grab().save(prefix + ".start.png");
    // Leave them open, the card up: the editor changes Fit to F and the cheat sheet to Ctrl+Alt+K.
    auto* reference = w.findChild<CommandReference*>();
    if (!reference) return check(false, "the Tool guide still there");
    reference->open("view.fit");
    tip->showFor(button("view.fit"), State::Compact);
    ShortcutEditor editor(w.m_actions, &w);
    auto* tree = editor.findChild<QTreeWidget*>("shortcutTree");
    auto assign = [&](const QString& id, const QKeySequence& to) {
      for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->toolTip(0) == id && (*it)->data(0, Qt::UserRole).toInt() >= 0) tree->setCurrentItem(*it);
      editor.findChild<QKeySequenceEdit*>("shortcutBinding")->setKeySequence(to);
      editor.findChild<QPushButton*>("shortcutAssign")->click();
    };
    bool free = true;  // the editor would ask about a clash (a modal dialog): only keys nothing has
    for (QAction* a : w.m_actions)
      for (const char* key : {"F", "Ctrl+Alt+K", "Ctrl+Alt+Q", "2", "Ctrl+Shift+Space"}) free = free && a->shortcut() != QKeySequence(key);
    check(free, "F, Ctrl+Alt+K, Ctrl+Alt+Q, 2 and Ctrl+Shift+Space are free to give");
    if (!free) return;
    assign("view.fit", QKeySequence("F"));
    assign("help.shortcuts", QKeySequence("Ctrl+Alt+K"));
    assign("inspect.pin", QKeySequence("Ctrl+Alt+Q"));
    assign("select.faces", QKeySequence("2"));
    assign("tools.commands", QKeySequence("Ctrl+Shift+Space"));
    QElapsedTimer applied;
    applied.start();
    editor.accept();  // the keys change, the open help surfaces follow (one announcement)
    const qint64 editorMs = applied.elapsed();
    applied.restart();
    keys::announce();  // the help surfaces alone, once more
    trace::log(QString("bench: keyhelp: the editor applied in %1 ms; the open help windows follow a change in %2 ms").arg(editorMs).arg(applied.elapsed()));
  });
  add(200, [=, &w] {
    auto* reference = w.findChild<CommandReference*>();
    auto* sheet = w.findChild<ShortcutSheet*>();
    auto* start = w.findChild<GettingStarted*>();
    const QString fit = help::find("view.fit")->title;
    check(w.action("view.fit")->shortcut() == QKeySequence("F"), "the editor applied Fit on F");
    check(tip->state() != State::Hidden && tip->keyCaps() == QStringList({"F"}), "the card up follows: F");
    QTreeWidgetItem* fitRow = nullptr;
    for (QTreeWidgetItemIterator it(reference->findChild<QTreeWidget*>("referenceList")); *it; ++it)
      if ((*it)->data(0, Qt::UserRole).toString() == "view.fit") fitRow = *it;
    check(fitRow && plain(fitRow->text(1)) == "F" && reference->preview()->keyCaps() == QStringList({"F"}) && reference->current() == "view.fit",
          "the open Tool guide follows: F in its column and card, still at Fit");
    check(sheet && sheet->isVisible() && sheet->shown().contains(fit + " F") && sheet->closeKey() == QKeySequence("Ctrl+Alt+K"), "the open cheat sheet follows: Fit F, closing on Ctrl+Alt+K");
    check(start && plain(start->findChild<QLabel*>("secondary")->text()).contains("(F)"), "the open lesson follows: Fit (F)");
    // The window's own surfaces: Distance's footer and prompt (the tool still runs), the ribbon's filters and search badge.
    PanelFooter* footer = w.m_toolSteps ? w.m_toolSteps->footer() : nullptr;
    check(footer && PanelFooter::key(footer->primary()) == "Ctrl+Alt+Q", "Distance's open footer follows: Ctrl+Alt+Q");
    check(plain(w.m_prompt->hints()).contains(QString::fromUtf8("1–4")), "its prompt follows: 1–4 (" + plain(w.m_prompt->hints()) + ")");
    check(filterHints(w) == QStringList({"1", "2", "3", "4"}) && w.m_ribbon->searchField()->key() == "Ctrl+Shift+Space", "the ribbon follows: filters 1 2 3 4, search Ctrl+Shift+Space");
    w.m_ribbon->grab().save(prefix + ".after-ribbon.png");
    if (w.m_toolPanel) w.m_toolPanel->grab().save(prefix + ".after-distance.png");
    w.cancelTool();
    clips::load(clipFile);
    check(clips::resolvedKeys("keys", 0.6) == QList<QStringList>({{"F"}}), "the clip's key element follows: F");
    tip->grab().save(prefix + ".after-card.png");
    frames("after-clip");
    clips::load();  // the library again
    const auto fitSteps = clips::steps("view.fit");
    check(clips::resolvedKeys("view.fit", 1.0) == QList<QStringList>({{"F"}}) && plain(clips::caption(fitSteps[0])) == plain(QCoreApplication::translate("help", "Press %1").arg("F")),
          "the library's Fit clip follows: F, \"" + plain(clips::caption(fitSteps[0])) + "\"");
    QFile::remove(clipFile);
  });
  add(400, [=, &w] {
    auto* reference = w.findChild<CommandReference*>();
    auto* sheet = w.findChild<ShortcutSheet*>();
    auto* start = w.findChild<GettingStarted*>();
    reference->grab().save(prefix + ".after-guide.png");
    sheet->grab().save(prefix + ".after-sheet.png");
    start->grab().save(prefix + ".after-start.png");
    tip->hideTip();
    sheet->close();
    start->close();
    reference->close();
  });
  add(200, [=] {  // what a change costs the window's own surfaces (footers, ribbon, prompts, tooltips), the help closed
    QElapsedTimer applied;
    applied.start();
    keys::announce();
    trace::log(QString("bench: keyhelp: with the help windows closed the window follows a change in %1 ms").arg(applied.elapsed()));
  });
  add(0, [=] {
    trace::log(QString("bench: keyhelp: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [&w, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [steps, next, check, i] {
      try { (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}
