// The shared UI contracts of UI-120 that every track builds on: the command registry, the ribbon's titled groups,
// adaptive collapse and tab row, the panel footer, toasts and the semantic colour tokens. Offscreen; the in-app side is
// the gui_benches cases ribbon, ribbon-rtl, toast and toast-rtl (app/ContractsBench.cpp).
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <utility>

#include "Commands.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"
#include "Toast.hpp"
#include "check.hpp"

TEST(command_registry) {
  CommandRegistry r;
  QAction extrude("Extrude"), fit("Fit"), sync("Sync"), again("Again");
  CommandInfo e;
  e.id = "design.extrude";
  e.label = "Extrude";
  e.editsDocument = true;
  e.keywords = {"push pull"};
  r.add(e, &extrude);
  CommandInfo f;
  f.id = "view.fit";
  r.add(f, &fit);
  CommandInfo s;
  s.id = "assets.sync";
  s.group = "Assets";
  s.helpId = "assets.overview";
  s.workspaces = {"design"};
  s.scope = shortcuts::SketchOnly;
  bool selected = false;
  s.enabledWhen = [&selected](const CommandContext& c) { return c.document && selected; };
  r.add(s, &sync);
  CommandInfo twice;
  twice.id = "view.fit";
  r.add(twice, &again);
  CHECK(r.ids() == QStringList({"design.extrude", "view.fit", "assets.sync"}) && r.clashes() == QStringList{"view.fit"});
  CHECK(r.action("view.fit") == &fit && r.find("view.fit")->label.isEmpty() && !r.action("view.none") && !r.find("view.none"));
  CHECK(r.actions() == QList<QAction*>({&extrude, &fit, &sync}));
  // Groups: by the id's area unless given; the action carries it for the palette, with its keywords.
  CHECK(r.find("design.extrude")->group == commands::defaultGroup("design.extrude") && commands::defaultGroup("design.x") == "Design");
  CHECK(commands::defaultGroup("panel.browser") == commands::defaultGroup("workspace.review") && commands::defaultGroup("probe.command") == "Probe");
  CHECK(r.groups() == QStringList({"Design", "View", "Assets"}) && r.inGroup("Assets") == QStringList{"assets.sync"});
  CHECK(extrude.property("commandGroup").toString() == "Design" && extrude.property("commandKeywords").toStringList() == QStringList{"push pull"});
  CHECK(r.editsDocument("design.extrude") && !r.editsDocument("view.fit") && !r.editsDocument("view.none"));
  CHECK(r.helpId("assets.sync") == "assets.overview" && r.helpId("view.fit") == "view.fit");
  CHECK(shortcuts::scope("assets.sync") == shortcuts::SketchOnly && shortcuts::scope("view.fit") == shortcuts::Everywhere);
  // Workspaces: given ones, and the ribbon's placements added once each.
  r.addWorkspace("view.fit", "review");
  r.addWorkspace("view.fit", "design");
  r.addWorkspace("view.fit", "review");
  r.addWorkspace("view.none", "review");
  CHECK(r.find("view.fit")->workspaces == QStringList({"review", "design"}));
  CHECK(r.inWorkspace("design") == QStringList({"view.fit", "assets.sync"}) && r.inWorkspace("review") == QStringList{"view.fit"});
  // The first menu that shows a command is its path.
  r.setMenuPath("view.fit", "view");
  r.setMenuPath("view.fit", "view/navigation");
  CHECK(r.find("view.fit")->menuPath == "view" && r.find("design.extrude")->menuPath.isEmpty());
  // enabledWhen: only the commands that have one are touched.
  fit.setEnabled(false);
  CommandContext c;
  r.updateEnabled(c);
  CHECK(!sync.isEnabled() && !fit.isEnabled() && extrude.isEnabled());
  c.document = true;
  selected = true;
  r.updateEnabled(c);
  CHECK(sync.isEnabled() && !fit.isEnabled());
}

TEST(semantic_tokens) {
  for (const bool dark : {true, false}) {
    const Tokens t = theme::tokens(dark);
    // Tokens that stand for colours the app already draws keep their values.
    CHECK(t.hover == QColor("#ffffff") && t.candidate == t.amber && t.warning == t.amber && t.error == t.red);
    CHECK(t.selected3d.isValid() && t.selected3d != t.hover && t.ghost.alpha() < 128 && t.locked.isValid());
    // Each family stays apart for normal vision and under deuteranopia and protanopia (CIE76 >= 20 after simulation).
    const QList<QList<QColor>> families = {{t.diffAdded, t.diffRemoved, t.diffModified, t.diffMoved}, {t.assetLinked, t.assetStale, t.assetMissing}};
    for (const auto& family : families)
      for (const theme::Vision v : {theme::Vision::Normal, theme::Vision::Deuteranopia, theme::Vision::Protanopia})
        for (int i = 0; i < family.size(); ++i)
          for (int j = i + 1; j < family.size(); ++j) CHECK(theme::deltaE(theme::simulate(family[i], v), theme::simulate(family[j], v)) >= 20);
    // The check has teeth: the plain green / red / amber set the app uses elsewhere does not pass it.
    double plain = 1e9;
    for (const auto& [a, b] : {std::pair{t.green, t.red}, std::pair{t.green, t.amber}, std::pair{t.red, t.amber}})
      plain = std::min(plain, theme::deltaE(theme::simulate(a, theme::Vision::Deuteranopia), theme::simulate(b, theme::Vision::Deuteranopia)));
    CHECK(plain < 20);
  }
  // Simulation: grey stays grey, pure red and green come together for a deuteranope.
  CHECK(theme::deltaE(theme::simulate(QColor(128, 128, 128), theme::Vision::Deuteranopia), QColor(128, 128, 128)) < 1.5);
  CHECK(theme::deltaE(theme::simulate(QColor("#ff0000"), theme::Vision::Deuteranopia), theme::simulate(QColor("#00a000"), theme::Vision::Deuteranopia)) <
        theme::deltaE(QColor("#ff0000"), QColor("#00a000")) / 3);
  // Every state has its own mark and label: colour is never the only cue.
  QStringList marks, labels;
  for (const theme::Cue& c : theme::cues()) {
    CHECK(theme::cue(c.state) == &c && QString::fromUtf8(c.mark).size() == 1 && c.label[0]);
    marks << QString::fromUtf8(c.mark);
    labels << c.label;
  }
  marks.removeDuplicates();
  labels.removeDuplicates();
  CHECK(theme::cues().size() == 10 && marks.size() == 10 && labels.size() == 10 && !theme::cue("hover"));
  const Tokens dark = theme::tokens(true);
  CHECK(dark.*(theme::cue("diffAdded")->colour) == dark.diffAdded && dark.*(theme::cue("assetMissing")->colour) == dark.assetMissing);
}

TEST(panel_footer) {
  for (const Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft}) {
    QWidget panel;
    panel.setLayoutDirection(direction);
    auto* footer = new PanelFooter(&panel);
    CHECK(footer->cancelText() == "Cancel" && PanelFooter::key(footer->cancel()) == "Esc" && footer->primaryText() == "OK" &&
          PanelFooter::key(footer->primary()) == "Enter" && footer->primary()->objectName() == "primary");
    footer->setPrimary(PanelFooter::Primary::Stay);
    CHECK(footer->primaryText() == "Apply" && PanelFooter::key(footer->primary()) == "Enter");
    QPushButton* copy = footer->addSecondary("Copy");
    QPushButton* undo = footer->addSecondary("Undo point", "Ctrl+Z");
    footer->setCancel("Clear");
    footer->setPrimary("Pin to document", "P");
    CHECK(footer->cancelText() == "Clear" && PanelFooter::key(footer->cancel()) == "Esc" && PanelFooter::text(undo) == "Undo point" &&
          PanelFooter::key(undo) == "Ctrl+Z" && PanelFooter::key(copy).isEmpty() && footer->primary()->toolTip() == "Pin to document  (P)");
    const int width = footer->minimumSizeHint().width() + 40;  // a panel takes at least the footer's minimum (ToolStepsPanel)
    footer->setGeometry(0, 0, width, 44);
    panel.resize(width, 44);
    panel.show();
    QApplication::processEvents();
    // Reading order: secondaries on the leading side, then Cancel, then the primary at the trailing end.
    QList<int> x;
    for (QPushButton* b : {copy, undo, footer->cancel(), footer->primary()}) x << b->geometry().center().x();
    const bool ltr = direction == Qt::LeftToRight;
    CHECK(ltr ? std::is_sorted(x.begin(), x.end()) : std::is_sorted(x.rbegin(), x.rend()));
    CHECK(ltr ? footer->primary()->geometry().right() > width - 40 : footer->primary()->geometry().left() < 40);
    for (QPushButton* b : {copy, undo, footer->cancel(), footer->primary()}) CHECK(b->width() >= b->sizeHint().width());  // labels whole
    int accepted = 0, cancelled = 0;
    QObject::connect(footer, &PanelFooter::accepted, [&] { ++accepted; });
    QObject::connect(footer, &PanelFooter::cancelled, [&] { ++cancelled; });
    footer->primary()->click();
    footer->cancel()->click();
    footer->setPrimaryEnabled(false);
    footer->primary()->click();
    CHECK(accepted == 1 && cancelled == 1);
    CHECK(footer->primary()->focusPolicy() != Qt::NoFocus);
    footer->setKeysStayWithWindow(true);
    CHECK(footer->primary()->focusPolicy() == Qt::NoFocus && footer->cancel()->focusPolicy() == Qt::NoFocus && copy->focusPolicy() != Qt::NoFocus);
  }
}

TEST(toasts) {
  for (const Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft}) {
    QWidget host;
    host.setLayoutDirection(direction);
    host.resize(900, 600);
    host.show();
    QTest::mouseMove(&host, QPoint(2, 2));  // the pointer on a toast would hold its timer
    ToastStack stack(&host);
    int undone = 0;
    Toast* saved = stack.toast("Saved", QString(), {}, 150);
    Toast* deleted = stack.toast("3 bodies deleted", "Undo", [&undone] { ++undone; }, 0);
    Toast* warned = stack.toast("A reference was re-picked by its nearest match after its body changed; check the fillet", QString(), {}, 0);
    QApplication::processEvents();
    // Native children of the host, not windows; centred at the bottom, the newest lowest, apart.
    CHECK(stack.toasts() == QList<Toast*>({saved, deleted, warned}));
    for (Toast* t : stack.toasts()) {
      CHECK(t->parentWidget() == &host && !t->isWindow() && t->testAttribute(Qt::WA_NativeWindow) && t->focusPolicy() == Qt::NoFocus);
      CHECK(std::abs(t->geometry().center().x() - host.width() / 2) <= 1 && t->width() <= ToastStack::kMaxWidth && !t->mask().isEmpty());
    }
    CHECK(warned->geometry().bottom() == host.height() - ToastStack::kMargin - 1);
    CHECK(deleted->geometry().bottom() + 1 + ToastStack::kGap == warned->y() && saved->geometry().bottom() + 1 + ToastStack::kGap == deleted->y());
    CHECK(warned->height() > deleted->height() || warned->width() == ToastStack::kMaxWidth);  // long text wraps at the widest
    CHECK(!saved->actionButton() && deleted->actionButton() && deleted->actionButton()->text() == "Undo");
    // Mirrored: the action and the close button on the side the text ends.
    QLabel* text = deleted->findChild<QLabel*>("toastText");
    const bool ltr = direction == Qt::LeftToRight;
    CHECK(ltr ? deleted->actionButton()->x() > text->x() && deleted->closeButton()->x() > deleted->actionButton()->x()
              : deleted->actionButton()->x() < text->x() && deleted->closeButton()->x() < deleted->actionButton()->x());
    // Gone by itself after its time; the others close the gap.
    QTest::qWait(400);
    CHECK(stack.toasts() == QList<Toast*>({deleted, warned}) && warned->geometry().bottom() == host.height() - ToastStack::kMargin - 1);
    // The pointer on a toast holds its timer; off it, the toast stays a moment longer to be read again.
    Toast* held = stack.toast("Held", QString(), {}, 150);
    QApplication::processEvents();
    QTest::mouseMove(held, held->rect().center());
    QTest::qWait(300);
    CHECK(stack.toasts().contains(held));
    QTest::mouseMove(&host, QPoint(2, 2));
    QTest::qWait(300);
    CHECK(stack.toasts().contains(held));
    held->dismiss();
    // The action runs its callback once and takes the toast away.
    deleted->actionButton()->click();
    QApplication::processEvents();
    CHECK(undone == 1 && stack.toasts() == QList<Toast*>{warned});
    // At most kMax: the oldest goes first.
    for (int i = 0; i < 4; ++i) stack.toast(QString("Toast %1").arg(i), QString(), {}, 0);
    QApplication::processEvents();
    CHECK(stack.toasts().size() == ToastStack::kMax && stack.toasts().front()->text() == "Toast 1" && stack.toasts().back()->text() == "Toast 3");
    stack.toasts().back()->closeButton()->click();
    CHECK(stack.toasts().size() == 2);
    // A host resize keeps them at the bottom centre.
    host.resize(700, 500);
    QApplication::processEvents();
    CHECK(stack.toasts().back()->geometry().bottom() == 500 - ToastStack::kMargin - 1 && std::abs(stack.toasts().back()->geometry().center().x() - 350) <= 1);
    stack.clear();
    QApplication::processEvents();
    CHECK(stack.toasts().isEmpty());
  }
}

TEST(ribbon_layout_groups) {
  QAction extrude("Extrude"), revolve("Revolve"), hole("Hole"), thread("Thread"), plain("Plain");
  RibbonLayout layout;
  layout.addWorkspace("design", {"Design", "component", "Ctrl+2", "", ""});
  layout.addTab("design", "design.solid", "Solid");
  CHECK(layout.addGroup("design.solid", "design.solid.create", "Create") && !layout.addGroup("design.none", "design.none.x", "X"));
  CHECK(layout.addAction("design.solid.create", &extrude) && layout.addAction("design.solid.create", &revolve) &&
        layout.addAction("design.solid.create", &hole, RibbonLayout::Size::Small, {&thread}) && !layout.addAction("design.none.x", &extrude));
  CHECK(layout.addGroup("design.solid", "design.solid.create", "Again") == layout.group("design.solid.create") && layout.group("design.solid.create")->title == "Create");
  CHECK(layout.addGroup("design.solid", {&plain}) && layout.tab("design.solid")->groups.size() == 2 && layout.tab("design.solid")->groups[1].title.isEmpty());
  const RibbonLayout::Group* create = layout.group("design.solid.create");
  CHECK(create->actions() == QList<QAction*>({&extrude, &revolve, &hole}) && create->items[2].variants == QList<QAction*>{&thread} &&
        create->items[2].size == RibbonLayout::Size::Small && create->items[0].size == RibbonLayout::Size::Large && !layout.group(""));
  RibbonLayout::Tab* explode = layout.addContextualTab("design", "design.explode", "Explode");
  CHECK(explode && explode->contextual && explode->accent == &Tokens::amber && !layout.tab("design.solid")->contextual && !layout.addContextualTab("none", "none.x", "X"));
}

TEST(ribbon_collapse) {
  QList<QAction*> tools;
  auto add = [&tools](RibbonLayout& layout, const QString& group, const QString& text, RibbonLayout::Size size = RibbonLayout::Size::Large) {
    tools << new QAction(text);
    layout.addAction(group, tools.last(), size);
    return tools.last();
  };
  RibbonLayout layout;
  layout.addWorkspace("design", {"Design", "component", "Ctrl+2", "", ""});
  layout.addTab("design", "design.solid", "Solid");
  layout.addGroup("design.solid", "design.solid.create", "Create");
  QAction* extrude = add(layout, "design.solid.create", "Extrude");
  add(layout, "design.solid.create", "Revolve");
  add(layout, "design.solid.create", "Sweep along a path");
  QAction* thread = new QAction("Thread");
  QAction* holeTool = new QAction("Hole");
  tools << thread << holeTool;
  layout.addAction("design.solid.create", holeTool, RibbonLayout::Size::Small, {thread});  // a small split button
  layout.addGroup("design.solid", "design.solid.modify", "Modify");
  for (const char* t : {"Fillet", "Chamfer", "Shell", "Draft angle", "Scale"}) add(layout, "design.solid.modify", t);
  layout.addAction("design.solid.modify", nullptr);  // left out
  layout.addGroup("design.solid", "design.solid.pattern", "Pattern");
  for (const char* t : {"Mirror", "Rectangular pattern", "Circular pattern"}) add(layout, "design.solid.pattern", t);
  for (const Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft}) {
    RibbonPage page(*layout.tab("design.solid"), nullptr);
    page.setLayoutDirection(direction);
    const QList<RibbonGroup*> groups = page.groups();
    CHECK(groups.size() == 3 && groups[0]->title() == "Create" && groups[0]->buttons().size() == 4 && groups[1]->buttons().size() == 5);
    QToolButton* hole = groups[0]->buttons()[3];
    CHECK(hole->popupMode() == QToolButton::MenuButtonPopup && hole->menu()->actions() == QList<QAction*>{thread});
    CHECK(groups[0]->menu()->actions().size() == 5 && groups[0]->menu()->actions().contains(thread));  // the title lists the variants too
    const int full = page.widthAt({0, 0, 0});
    CHECK(page.minimumSizeHint().width() == page.widthAt({3, 3, 3}) && page.minimumSizeHint().width() < full);
    page.show();
    auto fitsAt = [&](int width) {
      page.resize(width, RibbonGroup::kHeight);
      QApplication::processEvents();
      const QList<int> levels = page.levels();
      bool whole = page.widthAt(levels) <= width || levels == QList<int>(3, RibbonGroup::Collapsed);
      for (RibbonGroup* g : groups) {
        whole = whole && g->x() >= 0 && g->geometry().right() < std::max(width, page.widthAt(levels));
        for (QToolButton* b : g->findChildren<QToolButton*>())
          if (b->isVisibleTo(&page)) whole = whole && b->width() >= b->sizeHint().width() && b->geometry().right() < g->width();
      }
      return whole ? levels : QList<int>();
    };
    CHECK(fitsAt(full + 40) == QList<int>({0, 0, 0}));
    const QList<int> one = fitsAt(full - 1);  // the end of the row steps down first
    CHECK(!one.isEmpty() && one.first() == 0 && one.last() > 0);
    const QList<int> two = fitsAt(page.widthAt({1, 1, 1}));
    CHECK(two == QList<int>({1, 1, 1}) || (!two.isEmpty() && two[0] <= two[1] && two[1] <= two[2]));
    const QList<int> last = fitsAt(page.minimumSizeHint().width());
    CHECK(!last.isEmpty() && std::all_of(last.begin(), last.end(), [](int l) { return l >= RibbonGroup::Icons; }));
    // Collapsed: one button, the tools hidden, and its menu has them all.
    fitsAt(page.minimumSizeHint().width() - 20);
    CHECK(page.levels() == QList<int>(3, RibbonGroup::Collapsed) && groups[1]->collapsedButton()->isVisibleTo(&page) &&
          !groups[1]->buttons()[0]->isVisibleTo(&page) && !groups[1]->titleButton()->isVisibleTo(&page) && groups[1]->menu()->actions().size() == 5);
    // Mirrored: the row starts at the right in a right-to-left UI.
    fitsAt(full + 40);
    CHECK(direction == Qt::LeftToRight ? groups[0]->x() < groups[2]->x() : groups[0]->x() > groups[2]->x());
    // A hidden tool leaves the row narrower; shown again it is back.
    extrude->setVisible(false);
    QApplication::processEvents();
    CHECK(!groups[0]->buttons()[0]->isVisibleTo(&page) && page.widthAt({0, 0, 0}) < full);
    extrude->setVisible(true);
    QApplication::processEvents();
    CHECK(groups[0]->buttons()[0]->isVisibleTo(&page) && page.widthAt({0, 0, 0}) == full);
  }
  qDeleteAll(tools);
}

TEST(ribbon_contextual_tab) {
  QAction fit("Fit"), explode("Explode"), steps("Steps");
  RibbonLayout layout;
  layout.addWorkspace("review", {"Review", "eye", "Ctrl+1", "", ""});
  layout.addWorkspace("design", {"Design", "component", "Ctrl+2", "", ""});
  layout.addTab("review", "review.view", "View", {{&fit}});
  layout.addTab("design", "design.solid", "Solid", {{&fit}});
  layout.addTab("design", "design.view", "View", {{&fit}});
  layout.addContextualTab("design", "design.explode", "Explode");
  layout.addGroup("design.explode", "design.explode.steps", "Steps");
  layout.addAction("design.explode.steps", &explode, RibbonLayout::Size::Large, {&steps});
  RibbonBar bar;
  for (const RibbonLayout::Space& space : layout.spaces) {
    const int index = bar.addWorkspace(space.workspace);
    for (const RibbonLayout::Tab& tab : space.tabs) bar.addTab(index, tab);
  }
  bar.setWorkspace(1);
  bar.setCurrentTab(1);
  CHECK(bar.tabIds() == QStringList({"design.solid", "design.view"}) && bar.currentPage()->id() == "design.view" && !bar.contextualTabShown("design.explode"));
  CHECK(bar.setContextualTab("design.explode", true) && bar.tabIds() == QStringList({"design.explode", "design.solid", "design.view"}) &&
        bar.currentPage()->id() == "design.explode" && bar.tabBar()->currentIndex() == 0 && !bar.tabBar()->tabIcon(0).isNull());
  bar.setWorkspace(0);
  CHECK(bar.tabIds() == QStringList{"review.view"} && bar.contextualTabShown("design.explode"));
  bar.setWorkspace(1);
  CHECK(bar.tabIds().first() == "design.explode" && bar.currentPage()->id() == "design.explode");
  CHECK(bar.setContextualTab("design.explode", false) && bar.tabIds() == QStringList({"design.solid", "design.view"}) && bar.currentPage()->id() == "design.view");
  CHECK(!bar.setContextualTab("design.solid", true) && !bar.setContextualTab("design.none", true) && bar.tabBar()->elideMode() == Qt::ElideNone);
}

TEST(ribbon_tab_row) {
  QAction fit("Fit"), save("Save"), undo("Undo"), search("Search"), settings("Settings"), bodies("Bodies"), faces("Faces"), through("Through");
  RibbonLayout layout;
  layout.addWorkspace("design", {"Design", "component", "Ctrl+2", "", ""});
  for (const char* t : {"Solid", "Modify", "Construct", "Assemble", "View", "Export"}) layout.addTab("design", QString("design.") + t, t, {{&fit}});
  for (const Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft}) {
    QWidget host;  // a window would keep the bar at its layout's least width: in the app it sits in the ribbon's tool bar
    host.resize(1700, 200);
    auto* ribbon = new RibbonBar(&host);
    RibbonBar& bar = *ribbon;
    bar.setLayoutDirection(direction);
    const int space = bar.addWorkspace(layout.spaces[0].workspace);
    for (const RibbonLayout::Tab& tab : layout.spaces[0].tabs) bar.addTab(space, tab);
    // The cluster's parts land in their slots whichever order they come in: quick access, search, areas' widgets, settings.
    auto* branch = new QLabel("branch: main");
    bar.addTabRowWidget(branch);
    bar.setSettingsMenu(&settings);
    bar.setSearchAction(&search);
    QMenu steps;
    QToolButton* undoButton = bar.addQuickAction(&undo, &steps);
    QToolButton* saveButton = bar.addQuickAction(&save);
    QMenu more;
    more.addAction(&through);
    bar.setSelectFilters({&bodies, &faces}, {"1", "2"}, &more);
    bar.setWorkspace(space);
    host.show();
    auto settle = [&bar](int width) {
      bar.resize(width, bar.sizeHint().height());
      QApplication::processEvents();
    };
    settle(1600);
    QToolButton* gear = bar.cluster()->findChild<QToolButton*>("ribbonSettings");
    const QList<QWidget*> order{undoButton, saveButton, bar.searchField(), branch, gear};
    const bool rtl = direction == Qt::RightToLeft;
    auto inOrder = [&] {
      for (int i = 1; i < order.size(); ++i)
        if (rtl ? order[i]->geometry().right() >= order[i - 1]->x() : order[i]->x() <= order[i - 1]->geometry().right()) return false;
      return true;
    };
    QTabBar* tabs = bar.tabBar();
    auto whole = [&] { return tabs->width() >= tabs->sizeHint().width() && (rtl ? bar.cluster()->geometry().right() < tabs->x() : bar.cluster()->x() > tabs->geometry().right()); };
    CHECK(tabs->count() == 6 && gear && branch->parentWidget() == bar.cluster() && undoButton->popupMode() == QToolButton::MenuButtonPopup && undoButton->menu() == &steps && !saveButton->menu());
    CHECK(!bar.searchField()->compact() && bar.searchField()->width() == SearchField::kFull && inOrder() && whole());
    CHECK(bar.selectButton()->menu() == &more && bar.strip()->isAncestorOf(bar.selectButton()) && !bar.strip()->isAncestorOf(bar.searchField()));
    for (SegmentButton* b : bar.strip()->findChildren<SegmentButton*>()) {
      SegmentButton labelled(b->defaultAction(), "1", false);  // as the filters showed before: the name and the key
      CHECK(b->iconOnly() && b->sizeHint().width() < labelled.sizeHint().width());
    }
    // The width that just fits every tab and the whole cluster: a pixel less and search shows as its icon, the tabs whole.
    const int lead = rtl ? bar.width() - tabs->geometry().right() - 1 : tabs->x();
    const int need = lead + tabs->sizeHint().width() + RibbonBar::kClusterGap + bar.cluster()->width() + RibbonBar::kRowMargin;
    settle(need);
    CHECK(bar.width() == need && !bar.searchField()->compact() && whole() && inOrder());
    settle(need - 1);
    CHECK(bar.searchField()->compact() && bar.searchField()->width() == SearchField::kCompact && whole() && inOrder());
    settle(need - 120);
    CHECK(bar.searchField()->compact() && whole() && inOrder());
    settle(1600);
    CHECK(!bar.searchField()->compact() && inOrder());
  }
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return check::run_all(argc, argv);
}
