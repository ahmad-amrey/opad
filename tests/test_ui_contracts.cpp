// The shared UI contracts of UI-120 that every track builds on: the command registry, the ribbon's titled groups and
// adaptive collapse, the panel footer, toasts and the semantic colour tokens. Offscreen; the in-app side is the
// gui_benches cases ribbon, ribbon-rtl, toast and toast-rtl (app/ContractsBench.cpp).
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <utility>

#include "Commands.hpp"
#include "PanelFooter.hpp"
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

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return check::run_all(argc, argv);
}
