// The shared UI contracts of UI-120 in the running app: the command registry, the ribbon's titled groups and adaptive
// collapse, the panel footer and toasts. Cases in tools/bench_cases/core.py; the offscreen side is tests/test_ui_contracts.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLabel>
#include <QToolButton>
#include <QMenu>
#include <QPainter>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <memory>

#include "BenchRegistry.hpp"
#include "DesignPanels.hpp"
#include "PanelFooter.hpp"
#include "CommandPalette.hpp"
#include "ShortcutEditor.hpp"
#include "MainWindow.hpp"

namespace {
struct Checks {
  QString bench;
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: %1: %2 %3").arg(bench, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
};

// Polls `done` every 50 ms until it holds or `ms` have passed, then calls `then` with the outcome.
void waitFor(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(50);
}

// The footer's buttons in reading order (leading side first) and whole: as wide as their labels and keys ask.
bool inReadingOrder(PanelFooter* footer, const QList<QPushButton*>& buttons) {
  const bool rtl = footer->layoutDirection() == Qt::RightToLeft;
  for (int i = 0; i < buttons.size(); ++i) {
    if (buttons[i]->width() < buttons[i]->sizeHint().width()) return false;
    if (i > 0 && (rtl ? buttons[i]->x() >= buttons[i - 1]->x() : buttons[i]->x() <= buttons[i - 1]->x())) return false;
  }
  return true;
}
}  // namespace

// OPAD_BENCH_COMMANDS=1 on a STEP file in viewer mode: every command of the window has its record, in command order, with
// group, menu path, workspaces and editsDocument; an area's command registered with its whole record is enabled by its
// enabledWhen as the selection moves, and as an edit it asks to save the viewed file first instead of running.
OPAD_BENCH(OPAD_BENCH_COMMANDS, commands) {
  Checks require{"commands"};
  const CommandRegistry& registry = w.m_commands;
  QStringList names;
  for (QAction* a : w.m_actions) names << a->objectName();
  require(registry.ids() == names && registry.actions() == w.m_actions && registry.clashes().isEmpty(),
          QString("every command has one record, in command order (%1)").arg(names.size()));
  bool built = true;
  for (const QString& id : names) built = built && registry.editsDocument(id) == MainWindow::isEditAction(id) && !registry.find(id)->group.isEmpty();
  require(built, "built-in records: a group each, editsDocument as isEditAction");
  const CommandInfo* extrude = registry.find("design.extrude");
  require(extrude && extrude->group == commands::defaultGroup("design.extrude") && extrude->menuPath == "design/create" &&
              extrude->workspaces == QStringList{"design"} && opGroup(w.action("design.extrude")) == extrude->group,
          "design.extrude: group " + (extrude ? extrude->group + ", menu " + extrude->menuPath + ", workspaces " + extrude->workspaces.join(' ') : QString("none")));
  const CommandInfo* fit = registry.find("view.fit");
  require(fit && fit->workspaces == QStringList({"review", "design"}) && fit->menuPath == "view" && registry.find("nav.fusion")->menuPath == "view/navigation" &&
              registry.find("sketch.line")->workspaces == QStringList{"sketch"} && registry.inWorkspace("review").contains("inspect.distance") &&
              !registry.inWorkspace("review").contains("design.extrude"),
          "workspaces from the ribbon, menu paths from the menu bar");
  // The filters are one choice among four; Select through objects is a setting of its own that they leave as it is.
  QAction* through = w.action("select.through");
  const bool wasThrough = through->isChecked();
  through->setChecked(true);
  w.action("select.faces")->trigger();
  const bool kept = through->isChecked() && w.action("select.faces")->isChecked() && !w.action("select.bodies")->isChecked();
  w.action("select.bodies")->trigger();
  require(kept && through->isChecked() && w.action("select.bodies")->isChecked() && !w.action("select.faces")->isChecked(),
          "a selection filter leaves Select through objects on");
  through->setChecked(wasThrough);
  // An area's command with its whole record.
  require(w.m_doc->browse && !w.m_doc->scene.all_bodies().empty(), "a STEP file in viewer mode");
  static int edits = 0, looks = 0;
  CommandInfo edit;
  edit.id = "bench.edit";
  edit.label = "Bench edit";
  edit.group = "Bench";
  edit.keywords = {"probe"};
  edit.editsDocument = true;
  edit.enabledWhen = [](const CommandContext& c) { return c.document && !c.selection.empty(); };
  QAction* editAction = w.m_areaServices.addCommand(edit, [] { ++edits; });
  CommandInfo look = edit;
  look.id = "bench.look";
  look.editsDocument = false;
  QAction* lookAction = w.m_areaServices.addCommand(look, [] { ++looks; });
  w.m_areaServices.updateCommands();
  require(w.action("bench.edit") == editAction && registry.find("bench.edit")->group == "Bench" && registry.inGroup("Bench").size() == 2 &&
              w.m_areaServices.commands().editsDocument("bench.edit") && !editAction->isEnabled(),
          "an area's command: registered with its record, disabled with nothing selected");
  {  // Keyboard shortcuts lists it under its record's group, a help command still under Help
    ShortcutEditor editor(w.m_actions, &w);
    QString benchGroup, helpGroup;
    for (QTreeWidgetItemIterator it(editor.findChild<QTreeWidget*>("shortcutTree")); *it; ++it) {
      if ((*it)->toolTip(0) == "bench.edit" && (*it)->parent()) benchGroup = (*it)->parent()->text(0);
      if ((*it)->toolTip(0).startsWith("help.") && (*it)->parent()) helpGroup = (*it)->parent()->text(0);
    }
    require(benchGroup == "Bench" && helpGroup == ShortcutEditor::tr("Help"), "Keyboard shortcuts: the area's command under " + benchGroup + ", help under " + helpGroup);
  }
  const std::string body = w.m_doc->scene.all_bodies().front();
  w.m_browser->selectIds({body});
  require(editAction->isEnabled() && lookAction->isEnabled(), "enabledWhen follows the selection");
  editAction->trigger();  // viewer mode: "Save first to edit" (dismissed by the bench), the command does not run
  lookAction->trigger();
  require(edits == 0 && looks == 1, QString("viewer mode: the edit asked to save first (ran %1), the other ran (%2)").arg(edits).arg(looks));
  w.m_browser->selectIds({});
  require(!editAction->isEnabled(), "a cleared selection disables it again");
  QCoreApplication::exit(require.all ? 0 : 2);
  return true;
}

// OPAD_BENCH_FOOTER=1 [OPAD_LANG=ar] on an editable document: the panel footer in the feature panel (Cancel Esc and OK
// Enter at the trailing end: Cancel leaves the feature, OK commits it and closes the panel) and in the guided tools' panel
// (Copy leading, Clear Esc and Pin to document P, which keep the keys with the window: Clear measures again, Pin pins and
// the tool stays).
OPAD_BENCH(OPAD_BENCH_FOOTER, footer) {
  auto require = std::make_shared<Checks>(Checks{"footer"});
  auto finish = [require] { QCoreApplication::exit(require->all ? 0 : 2); };
  const std::string body = w.m_doc->scene.all_bodies().empty() ? std::string() : w.m_doc->scene.all_bodies().front();
  (*require)(!w.m_doc->browse && !body.empty(), "an editable document with a body");
  w.setWorkspace("design");
  w.m_design->startFeature("box");
  FeaturePanel* form = w.m_design->featurePanel();
  PanelFooter* f = form->footer();
  (*require)(w.m_design->featureActive() && w.m_featurePanel->isVisible() && f->isVisibleTo(form) && f->cancelText() == QObject::tr("Cancel") &&
                 PanelFooter::key(f->cancel()) == "Esc" && f->primaryText() == QObject::tr("OK") && PanelFooter::key(f->primary()) == "Enter" &&
                 f->primary()->objectName() == "primary",
             "feature panel: Cancel (Esc) and the primary OK (Enter)");
  (*require)(inReadingOrder(f, {f->cancel(), f->primary()}) && f->geometry().bottom() == form->height() - 1 && f->width() == form->width(),
             QString("feature panel: the footer spans the panel's bottom, buttons whole and in reading order (%1)").arg(f->layoutDirection() == Qt::RightToLeft ? "rtl" : "ltr"));
  const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT");  // <shot>.feature.png, <shot>.tool.png
  if (!shot.isEmpty()) w.m_featurePanel->grab().save(shot + ".feature.png");
  {  // a multi-page flow's Back and a hint lead the footer (mirrored in a right-to-left UI); off again for the feature
    QPushButton* back = f->setBack({}, "Alt+Left");
    f->setHint("Hint");
    f->layout()->activate();
    int backs = 0;
    const auto counted = QObject::connect(f, &PanelFooter::backRequested, [&backs] { ++backs; });
    back->click();
    QObject::disconnect(counted);
    const bool rtl = f->layoutDirection() == Qt::RightToLeft;
    const QRect hint = f->hint()->geometry();
    const bool between = rtl ? hint.right() < back->x() && hint.left() > f->cancel()->geometry().right() : hint.left() > back->geometry().right() && hint.right() < f->cancel()->x();
    (*require)(back->isVisibleTo(f) && PanelFooter::text(back) == QObject::tr("Back") && inReadingOrder(f, {back, f->cancel(), f->primary()}) && between && backs == 1,
               "feature panel: Back, then the hint, lead the footer; Back signals backRequested");
    f->setBackVisible(false);
    f->setHint({});
    f->layout()->activate();
  }
  const size_t ops = w.m_doc->doc.ops.size();
  f->cancel()->click();
  (*require)(!w.m_design->featureActive() && !w.m_featurePanel->isVisible() && w.m_doc->doc.ops.size() == ops, "Cancel leaves the feature, nothing is committed");
  w.m_design->startFeature("box");
  f->primary()->click();
  waitFor(&w, [&w, ops] { return !w.m_design->featureActive() && w.m_doc->doc.ops.size() > ops && !w.m_doc->designBusy; }, 20000, [&w, require, finish, ops, body](bool ok) {
    (*require)(ok && !w.m_featurePanel->isVisible(), QString("OK commits the feature and closes the panel (%1 ops more)").arg(w.m_doc->doc.ops.size() - ops));
    w.setWorkspace("review");
    w.startTool("bbox");
    w.m_browser->selectIds({body});
    PanelFooter* t = w.m_toolSteps->footer();
    waitFor(&w, [&w, t] { return t->isVisibleTo(w.m_toolSteps) && t->primary()->isEnabled(); }, 20000, [&w, require, finish, t, body](bool shown) {
      QPushButton* copy = nullptr;
      for (QPushButton* b : t->findChildren<QPushButton*>())
        if (PanelFooter::text(b) == QObject::tr("Copy")) copy = b;
      (*require)(shown && copy && t->cancelText() == QObject::tr("Clear") && PanelFooter::key(t->cancel()) == "Esc" && t->primaryText() == QObject::tr("Pin to document") &&
                     PanelFooter::key(t->primary()) == "P" && t->primary()->focusPolicy() == Qt::NoFocus && t->cancel()->focusPolicy() == Qt::NoFocus,
                 "tool panel: Copy, Clear (Esc) and Pin to document (P) once there is a result; Esc, P and Enter stay with the window");
      (*require)(copy && inReadingOrder(t, {copy, t->cancel(), t->primary()}), "tool panel: Copy on the leading side, Pin at the trailing end");
      if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) w.m_toolPanel->grab().save(shot + ".tool.png");
      t->cancel()->click();  // Esc: the result goes, the tool measures again
      waitFor(&w, [&w, t] { return !t->isVisibleTo(w.m_toolSteps) && w.m_lastMeasure.is_null(); }, 10000, [&w, require, finish, t, body](bool cleared) {
        (*require)(cleared && w.m_tool.id == "bbox", "Clear drops the result and the tool waits for a pick");
        w.m_browser->selectIds({body});
        waitFor(&w, [&w, t] { return t->isVisibleTo(w.m_toolSteps) && t->primary()->isEnabled(); }, 20000, [&w, require, finish, t](bool again) {
          const size_t before = w.m_doc->doc.ops.size();
          t->primary()->click();
          (*require)(again && w.m_doc->doc.ops.size() == before + 1 && w.m_doc->doc.ops.back().type == "measurement" && w.m_tool.id == "bbox",
                     "Pin to document pins the measurement and the tool stays");
          w.cancelTool();
          finish();
        });
      });
    });
  });
  return true;
}

// OPAD_BENCH_TOAST=<prefix> [OPAD_LANG=ar] on an editable document: toasts are native children of the viewport at its
// bottom centre, the newest lowest; an Undo action undoes a real edit and takes its toast away; a timed one goes by
// itself and the rest close the gap; a theme switch restyles the ones showing; mirrored in a right-to-left UI.
// <prefix>.toast.png and <prefix>.toast-light.png show the stack.
OPAD_BENCH(OPAD_BENCH_TOAST, toast) {
  auto require = std::make_shared<Checks>(Checks{"toast"});
  const std::string body = w.m_doc->scene.all_bodies().empty() ? std::string() : w.m_doc->scene.all_bodies().front();
  const size_t ops = w.m_doc->doc.ops.size();
  w.m_doc->run("rename", opad::json{{"target", body}, {"name", "Bench body"}});
  ToastStack* stack = w.m_toasts;
  stack->clear();
  Toast* saved = stack->toast("Saved to box.opad", QString(), {}, 4000);  // outlives the theme switches below
  Toast* renamed = stack->toast("Renamed the body to Bench body", QObject::tr("Undo"), [&w] { w.m_doc->undo(); }, 0);
  Toast* warned = stack->toast("A reference was re-picked by its nearest match after its body changed; check the fillet before you go on", QString(), {}, 0);
  const QRect vp = w.m_viewport->rect();
  bool native = true, centred = true;
  for (Toast* t : stack->toasts()) {
    native = native && t->parentWidget() == w.m_viewport && !t->isWindow() && t->testAttribute(Qt::WA_NativeWindow) && t->isVisible();
    centred = centred && std::abs(t->geometry().center().x() - vp.center().x()) <= 1 && t->width() <= ToastStack::kMaxWidth;
  }
  (*require)(stack->toasts() == QList<Toast*>({saved, renamed, warned}) && native, "three toasts, native children of the viewport");
  (*require)(centred && warned->geometry().bottom() == vp.bottom() - ToastStack::kMargin && renamed->geometry().bottom() + 1 + ToastStack::kGap == warned->y() &&
                 saved->geometry().bottom() + 1 + ToastStack::kGap == renamed->y(),
             "bottom centre of the viewport, the newest lowest, 8 px apart");
  QLabel* text = renamed->findChild<QLabel*>("toastText");
  const bool rtl = renamed->layoutDirection() == Qt::RightToLeft;
  (*require)(rtl == (QGuiApplication::layoutDirection() == Qt::RightToLeft) && (rtl ? renamed->actionButton()->x() < text->x() : renamed->actionButton()->x() > text->x()),
             QString("the action where the text ends (%1)").arg(rtl ? "rtl" : "ltr"));
  auto shot = [&w, stack](const QString& file) {
    QRect area;
    for (Toast* t : stack->toasts()) area |= t->geometry();
    area.adjust(-24, -24, 24, 24);
    return w.grab(QRect(w.m_viewport->mapTo(&w, area.topLeft()), area.size())).save(file);
  };
  const QString prefix = value;
  (*require)(shot(prefix + ".toast.png"), "screenshot " + prefix + ".toast.png");
  // A theme switch restyles the toasts showing: their background is the new bg3.
  const bool dark = theme::current().dark;
  auto background = [](Toast* t) { return t->grab().toImage().pixelColor(t->width() / 2, 3); };
  auto similar = [](const QColor& a, const QColor& b) { return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) <= 9; };
  const QColor before = background(warned);
  w.applyTheme(!dark);
  QCoreApplication::processEvents();
  const QColor after = background(warned);
  (*require)(similar(before, theme::tokens(dark).bg3) && similar(after, theme::tokens(!dark).bg3),
             QString("theme-aware: bg3 %1 -> %2").arg(before.name(), after.name()));
  shot(prefix + ".toast-light.png");
  w.applyTheme(dark);
  renamed->actionButton()->click();  // Undo
  QCoreApplication::processEvents();
  (*require)(w.m_doc->doc.ops.size() == ops && w.m_doc->nodeName(body) != "Bench body" && stack->toasts() == QList<Toast*>({saved, warned}) &&
                 warned->geometry().bottom() == w.m_viewport->rect().bottom() - ToastStack::kMargin,
             QString("Undo undid the rename (%1 ops, was %2; name %3) and took its toast away (%4 left)").arg(w.m_doc->doc.ops.size()).arg(ops).arg(w.m_doc->nodeName(body)).arg(stack->toasts().size()));
  waitFor(&w, [stack, saved] { return !stack->toasts().contains(saved); }, 8000, [&w, require, stack, warned](bool gone) {
    const QRect vp = w.m_viewport->rect();
    (*require)(gone && stack->toasts() == QList<Toast*>{warned} && warned->geometry().bottom() == vp.bottom() - ToastStack::kMargin,
               QString("a timed toast goes by itself (%1 left)").arg(stack->toasts().size()));
    for (int i = 0; i < 4; ++i) stack->toast(QString("Toast %1").arg(i), QString(), {}, 0);
    (*require)(stack->toasts().size() == ToastStack::kMax && stack->toasts().front()->text() == "Toast 1", "at most three, the oldest goes first");
    w.resize(w.width() - 120, w.height() - 80);  // the viewport shrinks: they stay at its bottom centre
    QCoreApplication::processEvents();
    const QRect now = w.m_viewport->rect();
    Toast* last = stack->toasts().back();
    (*require)(now != vp && last->geometry().bottom() == now.bottom() - ToastStack::kMargin && std::abs(last->geometry().center().x() - now.center().x()) <= 1,
               "a resized viewport keeps them at its bottom centre");
    stack->clear();
    QCoreApplication::exit(require->all ? 0 : 2);
  });
  return true;
}

// OPAD_BENCH_RIBBON=<widths> (default 1280,1600) [OPAD_BENCH_UISHOT=<prefix>] [OPAD_LANG=ar] on an editable document: at
// each window width every tab of every workspace (Sketch's too) fits without shortening a label: each tool is as wide as it
// asks, every group lies inside the row, groups stepped down are the rightmost and at most one level apart, tabs never
// elide, and a right-to-left UI starts at the right; from 1600 px Review and Design show every group large. The tab row
// shows every tab whole and then its cluster in reading order (quick access Save, Undo ▾, Redo ▾, search, settings); the
// strip ends in the compact Select control. Narrower, with an area's widget in the cluster, search gives up its field
// before a tab is cut. Undo ▾ / Redo ▾ list the steps and take several at once. <prefix>.<width>.<workspace>.png stacks
// the tabs of a workspace, <prefix>.narrow.png shows the compact tab row. Then a contextual tab with a split button: hidden
// until shown, then first and current; hidden again, the tab before comes back.
OPAD_BENCH(OPAD_BENCH_RIBBON, ribbon) {
  Checks require{"ribbon"};
  const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT");
  const bool rtl = QGuiApplication::layoutDirection() == Qt::RightToLeft;
  QList<int> widths;
  for (const QString& part : QString(value == "1" ? "1280,1600" : value).split(',', Qt::SkipEmptyParts)) widths << part.toInt();
  const QSize size = w.size();
  const QString start = w.workspaceId();
  RibbonBar* ribbon = w.m_ribbon;
  require(ribbon->tabBar()->elideMode() == Qt::ElideNone, "tab titles never elide");
  auto quickButton = [ribbon](QAction* a) -> QToolButton* {
    for (QToolButton* b : ribbon->cluster()->findChildren<QToolButton*>("ribbonQuick"))
      if (b->defaultAction() == a) return b;
    return nullptr;
  };
  QToolButton* save = quickButton(w.action("file.save"));
  QToolButton* undo = quickButton(w.action("edit.undo"));
  QToolButton* redo = quickButton(w.action("edit.redo"));
  QToolButton* gear = ribbon->cluster()->findChild<QToolButton*>("ribbonSettings");
  SearchField* search = ribbon->searchField();
  QWidget* segments = ribbon->strip()->findChild<QWidget*>("segmented");
  QToolButton* select = ribbon->selectButton();
  if (!save || !undo || !redo || !gear || !search || !segments || !select) {
    require(false, "tab row: quick access, search and settings; strip: the Select control");
    QCoreApplication::exit(2);
    return true;
  }
  require(!save->menu() && undo->menu() && redo->menu() && undo->popupMode() == QToolButton::MenuButtonPopup && ribbon->tabRow()->isAncestorOf(search) &&
              ribbon->tabRow()->isAncestorOf(gear) && !ribbon->strip()->isAncestorOf(search) && ribbon->strip()->isAncestorOf(select),
          "tab row: Save, Undo ▾, Redo ▾, search and settings; the strip keeps only the tools and the Select control");
  const QList<SegmentButton*> filters = segments->findChildren<SegmentButton*>();
  bool iconSegments = filters.size() == 4;
  for (int i = 0; i < filters.size(); ++i)
    iconSegments = iconSegments && filters[i]->iconOnly() && filters[i]->width() >= filters[i]->sizeHint().width() &&
                   filters[i]->toolTip().contains(QString::number(i + 1)) && filters[i]->defaultAction() == w.action(QStringList({"select.bodies", "select.faces", "select.edges", "select.vertices"})[i]);
  require(iconSegments && select->menu() &&
              select->menu()->actions() == QList<QAction*>({w.action("edit.selectall"), w.action("edit.invert"), w.action("select.through"), w.action("select.geometry"), w.action("edit.selectparent")}) &&
              select->text().startsWith(QObject::tr("Select")),
          "Select control: the four filters as icons with their keys (the name in the tooltip), the rest under Select ▾");
  // What is wrong with the tab row as it shows: tabs cut, the cluster over them or outside the row, or out of reading order.
  auto tabRow = [&](QWidget* extra) {
    QStringList wrong;
    QTabBar* bar = ribbon->tabBar();
    if (bar->width() < bar->sizeHint().width() || bar->tabRect(bar->count() - 1).right() >= bar->width()) wrong << "tabs cut";
    const QRect cluster = ribbon->cluster()->geometry();
    if (rtl ? cluster.right() >= bar->x() : cluster.x() <= bar->geometry().right()) wrong << "cluster over the tabs";
    if (!ribbon->tabRow()->rect().contains(cluster)) wrong << "cluster outside the row";
    QList<QWidget*> order{save, undo, redo, search};
    if (extra) order << extra;
    order << gear;
    for (int i = 1; i < order.size(); ++i)
      if (rtl ? order[i]->geometry().right() >= order[i - 1]->x() : order[i]->x() <= order[i - 1]->geometry().right()) wrong << "cluster out of order";
    return wrong.join(", ");
  };
  for (const int width : widths) {
    w.resize(width, 1000);
    QCoreApplication::processEvents();
    int tabs = 0, collapsed = 0;
    QStringList narrow, outside, unordered, rows, row, small;
    for (int ws = 0; ws < w.m_workspaceIds.size(); ++ws) {
      ribbon->setWorkspace(ws);
      QCoreApplication::processEvents();
      if (const QString wrong = tabRow(nullptr); !wrong.isEmpty() || search->compact()) row << w.m_workspaceIds[ws] + ": " + (wrong.isEmpty() ? QString("search compact") : wrong);
      QList<QImage> strips;
      for (int t = 0; t < ribbon->tabBar()->count(); ++t) {
        ribbon->setCurrentTab(t);
        QCoreApplication::processEvents();
        RibbonPage* page = ribbon->currentPage();
        ++tabs;
        const QList<int> levels = page->levels();
        QString levelText;
        // Stepped down from the end of the row, one level at a time: a group left of another is never further down unless
        // that one has nothing narrower left; every step saved room.
        const QList<RibbonGroup*> groups = page->groups();
        auto steps = [](RibbonGroup* g, int level) {  // narrower levels taken to get there
          int n = 0;
          for (int l = 0; l >= 0 && l < level; l = g->nextLevel(l)) ++n;
          return n;
        };
        int lowest = RibbonGroup::Collapsed;
        for (int i = 0; i < levels.size(); ++i) {
          levelText += QString::number(levels[i]);
          collapsed += levels[i] > 0;
          for (int j = i + 1; j < levels.size(); ++j)
            if (steps(groups[i], levels[i]) > steps(groups[j], levels[j]) && groups[j]->nextLevel(levels[j]) >= 0) unordered << page->id();
          if (levels[i] > 0 && groups[i]->widthAt(levels[i]) >= groups[i]->widthAt(0)) unordered << page->id();
          if (groups[i]->nextLevel(levels[i]) >= 0) lowest = std::min(lowest, steps(groups[i], levels[i]));
        }
        for (int i = 0; i < levels.size(); ++i)
          if (steps(groups[i], levels[i]) > lowest + 1) unordered << page->id();
        rows << page->id() + " " + levelText;
        if (qEnvironmentVariableIsSet("OPAD_BENCH_RIBBON_DEBUG")) {
          QStringList ws0;
          for (RibbonGroup* g : page->groups()) ws0 << QString("%1:%2/%3/%4/%5").arg(g->title()).arg(g->widthAt(0)).arg(g->widthAt(1)).arg(g->widthAt(2)).arg(g->widthAt(3));
          trace::log(QString("ribbon debug %1 page %2 row0 %3 groups %4").arg(page->id()).arg(page->width()).arg(page->widthAt(QList<int>(page->groups().size(), 0))).arg(ws0.join(' ')));
        }
        for (RibbonGroup* g : page->groups()) {
          if (g->x() < 0 || g->geometry().right() >= page->width()) outside << page->id() + ":" + g->title();
          for (QToolButton* b : g->findChildren<QToolButton*>())
            if (b->isVisibleTo(page) && b->width() < b->sizeHint().width()) narrow << page->id() + ":" + b->text();
        }
        if (page->groups().size() > 1 && rtl != (page->groups().first()->x() > page->groups().last()->x())) outside << page->id() + " (direction)";
        // The Select control at the strip's end, after the tools in reading order.
        const QRect tools = page->parentWidget()->geometry(), control = select->geometry() | segments->geometry();
        if (tools.intersects(control) || (rtl ? segments->x() >= select->x() || control.right() >= tools.x() : segments->x() <= select->x() || control.x() <= tools.right()))
          outside << page->id() + " (Select control)";
        if (width >= 1600 && (w.m_workspaceIds[ws] == "review" || w.m_workspaceIds[ws] == "design") && std::any_of(levels.begin(), levels.end(), [](int l) { return l > 0; }))
          small << page->id() + " " + levelText;
        strips << ribbon->grab().toImage();
      }
      if (!shot.isEmpty() && !strips.isEmpty()) {
        QImage sheet(strips.first().width(), strips.first().height() * static_cast<int>(strips.size()), QImage::Format_ARGB32);
        sheet.fill(Qt::transparent);
        QPainter p(&sheet);
        for (int i = 0; i < strips.size(); ++i) {
          QImage strip = strips[i];
          strip.setDevicePixelRatio(1);  // pixel for pixel: the sheet has no scale of its own
          p.drawImage(0, i * strips.first().height(), strip);
        }
        p.end();
        sheet.save(QString("%1.%2.%3.png").arg(shot).arg(width).arg(w.m_workspaceIds[ws]));
      }
    }
    trace::log(QString("bench: ribbon: %1 px levels %2").arg(width).arg(rows.join(", ")));
    require(w.width() == width && narrow.isEmpty(), QString("%1 px: every tool as wide as its label asks over %2 tabs (%3 groups stepped down)%4")
                                                      .arg(width).arg(tabs).arg(collapsed).arg(narrow.isEmpty() ? QString() : ": " + narrow.join(", ")));
    require(outside.isEmpty(), QString("%1 px: every group inside its row, the row from the %2%3").arg(width).arg(rtl ? "right" : "left").arg(outside.isEmpty() ? QString() : ": " + outside.join(", ")));
    unordered.removeDuplicates();
    require(unordered.isEmpty(), QString("%1 px: the end of the row steps down first, one level at a time%2").arg(width).arg(unordered.isEmpty() ? QString() : ": " + unordered.join(", ")));
    require(row.isEmpty(), QString("%1 px: every tab whole, then quick access, search and settings in reading order%2").arg(width).arg(row.isEmpty() ? QString() : ": " + row.join("; ")));
    if (width >= 1600) require(small.isEmpty(), QString("%1 px: every group of Review and Design large%2").arg(width).arg(small.isEmpty() ? QString() : ": " + small.join(", ")));
  }
  // Narrower, with an area's widget in the cluster (after search, before settings): search shows as its icon before any
  // tab is cut, and the row stays whole down to there and beyond. The window keeps 1280 px at least, where more tabs or
  // area widgets get there: the sweep lifts that limit to reach it with these.
  auto* branch = new QLabel("Bench branch: main");
  w.m_areaServices.addTabRowWidget(branch);
  w.setWorkspace("design");
  w.statusBar()->clearMessage();  // the switch's message sits in the status bar's prompt, whose least width it would set
  const QSize least = w.minimumSize();
  w.setMinimumSize(0, 0);
  int compactAt = 0, reached = 0;
  QStringList shrinking;
  for (int width = 1280; width >= 760 && (!compactAt || width >= compactAt - 100); width -= 20) {
    w.resize(width, 1000);
    QCoreApplication::processEvents();
    if (w.width() != width) break;  // the window's least width (reported below)
    reached = width;
    if (const QString wrong = tabRow(branch); !wrong.isEmpty()) shrinking << QString("%1 px: %2").arg(width).arg(wrong);
    if (search->compact() && !compactAt) compactAt = width;
    if (!search->compact() && compactAt) shrinking << QString("%1 px: search whole again").arg(width);
  }
  require(compactAt && shrinking.isEmpty() && search->width() == SearchField::kCompact && branch->parentWidget() == ribbon->cluster(),
          QString("narrower: search shows as its icon from %1 px (down to %2, the window's least %3), every tab whole all the way, the area's widget before settings%4")
              .arg(compactAt).arg(reached).arg(w.minimumSizeHint().width()).arg(shrinking.isEmpty() ? QString() : ": " + shrinking.join("; ")));
  if (!shot.isEmpty()) ribbon->grab().save(shot + ".narrow.png");
  delete branch;
  w.setMinimumSize(least);
  w.resize(size);
  QCoreApplication::processEvents();
  require(!search->compact() && tabRow(nullptr).isEmpty(), "wide again: search whole");
  // Undo ▾ / Redo ▾: the document's steps, the next one first; a click on one takes it and every step before it in one go.
  const std::string body = w.m_doc->scene.all_bodies().empty() ? std::string() : w.m_doc->scene.all_bodies().front();
  require(!body.empty() && !w.m_doc->browse, "an editable document with a body");
  const size_t ops = w.m_doc->doc.ops.size();
  const QString name = w.m_doc->nodeName(body);
  w.m_doc->run("rename", opad::json{{"target", body}, {"name", "Ribbon one"}});
  w.m_doc->run("rename", opad::json{{"target", body}, {"name", "Ribbon two"}});
  w.m_doc->run("appearance", opad::json{{"target", body}, {"visible", false}});
  std::vector<std::string> ids;
  for (const opad::Op& op : w.m_doc->doc.ops) ids.push_back(op.id);
  auto shown = [](QToolButton* b) {
    emit b->menu()->aboutToShow();
    QStringList out;
    for (QAction* a : b->menu()->actions()) out << a->text();
    return out;
  };
  const QString hide = AppDocument::tr("hide"), rename = AppDocument::tr("rename");
  const QStringList undoSteps = shown(undo);
  require(w.m_doc->undoLabels().mid(0, 3) == QStringList({hide, rename, rename}) && undoSteps.mid(0, 3) == QStringList({MainWindow::tr("&Undo %1").arg(hide), MainWindow::tr("&Undo %1").arg(rename), MainWindow::tr("&Undo %1").arg(rename)}) &&
              undo->toolTip().startsWith(MainWindow::tr("&Undo %1").arg(hide).remove('&')),
          "Undo ▾ lists the steps, the next one first; the button's tooltip names it: " + undoSteps.mid(0, 3).join(" | "));
  const auto revision = w.m_doc->revision;
  undo->menu()->actions().value(1)->trigger();  // the second: hide and the last rename
  require(w.m_doc->doc.ops.size() == ops + 1 && w.m_doc->nodeName(body) == "Ribbon one" && w.m_doc->scene.node(body)->visible && w.m_doc->revision == revision + 1,
          QString("a click on the second step undoes two in one refresh (%1 ops, was %2)").arg(w.m_doc->doc.ops.size()).arg(ids.size()));
  const QStringList redoSteps = shown(redo);
  require(redoSteps == QStringList({MainWindow::tr("&Redo %1").arg(rename), MainWindow::tr("&Redo %1").arg(hide)}), "Redo ▾ lists them back: " + redoSteps.join(" | "));
  redo->menu()->actions().value(1)->trigger();
  std::vector<std::string> again;
  for (const opad::Op& op : w.m_doc->doc.ops) again.push_back(op.id);
  require(again == ids && w.m_doc->nodeName(body) == "Ribbon two" && !w.m_doc->scene.node(body)->visible, "redoing both brings the same ops back");
  undo->click();
  require(w.m_doc->doc.ops.size() == ids.size() - 1 && w.m_doc->scene.node(body)->visible && undo->toolTip().startsWith(MainWindow::tr("&Undo %1").arg(rename).remove('&')),
          "the quick-access Undo undoes one step: " + undo->toolTip());
  w.m_doc->undo(2);
  require(w.m_doc->doc.ops.size() == ops && w.m_doc->nodeName(body) == name, "back to the document as it was");
  w.setWorkspace("design");
  QCoreApplication::processEvents();
  // A contextual tab with a split button, as an area adds one (RibbonLayout::addContextualTab).
  RibbonLayout layout;
  layout.addWorkspace("design", Workspace());
  layout.addContextualTab("design", "design.benchContext", "Bench context");
  layout.addGroup("design.benchContext", "design.benchContext.view", "View");
  layout.addAction("design.benchContext.view", w.action("view.fit"), RibbonLayout::Size::Large, {w.action("view.home"), w.action("view.iso")});
  layout.addAction("design.benchContext.view", w.action("view.ortho"), RibbonLayout::Size::Small);
  ribbon->addTab(static_cast<int>(w.m_workspaceIds.indexOf("design")), *layout.tab("design.benchContext"));
  const QStringList before = ribbon->tabIds();
  const QString current = before.value(ribbon->currentTab());
  require(!before.contains("design.benchContext") && !ribbon->contextualTabShown("design.benchContext"), "a contextual tab is hidden until shown");
  require(w.setContextualTab("design.benchContext", true) && ribbon->tabIds().first() == "design.benchContext" && ribbon->tabIds().mid(1) == before &&
              ribbon->currentPage()->id() == "design.benchContext" && !ribbon->tabBar()->tabIcon(0).isNull(),
          "shown: first in the row, current, with its accent dot");
  QCoreApplication::processEvents();
  RibbonGroup* group = ribbon->currentPage()->groups().value(0);
  QToolButton* split = group ? group->buttons().value(0) : nullptr;
  require(split && split->popupMode() == QToolButton::MenuButtonPopup && split->menu() &&
              split->menu()->actions() == QList<QAction*>({w.action("view.home"), w.action("view.iso")}) && split->width() >= split->sizeHint().width() &&
              group->menu()->actions().size() == 4 && group->titleButton()->isVisible(),
          "split button: the action, its arrow drops the variants; the group's title lists all four");
  if (!shot.isEmpty()) ribbon->grab().save(shot + ".contextual.png");
  w.setWorkspace("review");
  require(!ribbon->tabIds().contains("design.benchContext") && ribbon->contextualTabShown("design.benchContext"), "another workspace does not show it");
  w.setWorkspace("design");
  require(ribbon->tabIds().first() == "design.benchContext", "back in its workspace it is there again");
  require(w.setContextualTab("design.benchContext", false) && ribbon->tabIds() == before && ribbon->tabIds().value(ribbon->currentTab()) == current &&
              !w.setContextualTab("design.none", true),
          "hidden: the row and the current tab as before");
  w.setWorkspace(start);
  QCoreApplication::exit(require.all ? 0 : 2);
  return true;
}
