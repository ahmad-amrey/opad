// The feature-area seams (AreaController.hpp) in the running app: a probe area, made only for OPAD_BENCH_AREAS, records
// every hook and uses the browser's provider APIs; the bench drives the window and checks what reached it. Cases in
// tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidgetItemIterator>

#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"

namespace {
class ProbeArea : public AreaController {
 public:
  using AreaController::AreaController;
  QStringList hooks;  // construction hooks, in call order
  QAction* command = nullptr;
  QLabel* label = nullptr;
  int ran = 0, asked = 0, menuCalls = 0, badgeClicks = 0;
  bool veto = false;
  SelectionContext selected, menuSelection;
  QStringList menuEntries;
  std::vector<bool> changes;  // documentChanged(replaced)
  QRect overlays;
  std::string activated, folderMenu = "none";

  void buildActions() override {
    hooks << "buildActions";
    command = services().addAction("probe.command", "Probe command", "dot", QKeySequence(), [this] { ++ran; });
  }
  void menus(QMenuBar*, const QMap<QString, QMenu*>& byId) override {
    hooks << "menus";
    if (QMenu* tools = byId.value("tools")) tools->addAction(command);
  }
  void ribbon(RibbonLayout& layout) override {
    hooks << "ribbon";
    layout.addGroup("review.view", {command, nullptr});
    layout.addTab("review", "review.probe", "Probe", {{command}, {}});
  }
  void statusWidgets(QStatusBar* bar) override {
    hooks << "statusWidgets";
    label = new QLabel("probe", bar);
    bar->addPermanentWidget(label);
  }
  void ready() override {
    hooks << "ready";
    // Bodies get a clickable text badge (Arabic in a right-to-left UI) and an icon badge, an italic name and a tooltip line.
    const bool rtl = QGuiApplication::layoutDirection() == Qt::RightToLeft;
    services().browser()->addDecorator([this, rtl](const browser::Row& row, browser::Decoration& d) {
      if (row.kind != "body" || !row.node) return;
      browser::Badge sync;
      sync.text = rtl ? QString::fromUtf8("مزامنة") : QString("sync");
      sync.icon = "regen";
      sync.color = &Tokens::sel;
      sync.tooltip = "probe badge";
      sync.clicked = [this] { ++badgeClicks; };
      browser::Badge mark;
      mark.icon = "dot";
      mark.fill = nullptr;
      mark.tooltip = "probe mark";
      d.badges << sync << mark;
      d.italic = true;
      d.tooltip = "probe row";
    });
    browser::Folder folder;
    folder.id = "probe";
    folder.title = "Probe folder";
    folder.icon = "doc";
    folder.items = [] {
      return std::vector<browser::Item>{{"probe:a", "Probe A", "drawing", "probe item", {{"probe:a1", "Probe A1", "sketch", "", {}}}}, {"probe:b", "Probe B", "", "", {}}};
    };
    folder.contextMenu = [this](const std::string& id, QMenu& menu) {
      folderMenu = id;
      menu.addAction("Probe entry");
      QTimer::singleShot(0, &menu, &QMenu::close);
    };
    folder.activated = [this](const std::string& id) { activated = id; };
    services().browser()->addFolder(folder);
  }
  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    ++menuCalls;
    menuSelection = selection;
    menu.addAction(command);
    menuEntries.clear();
    for (QAction* a : menu.actions()) menuEntries << a->objectName();
    QTimer::singleShot(0, &menu, &QMenu::close);  // the bench's menu shows off screen: close it again
  }
  void selectionChanged(const SelectionContext& selection) override { selected = selection; }
  void positionOverlays(const QRect& viewport) override { overlays = viewport; }
  void documentChanged(bool replaced) override { changes.push_back(replaced); }
  bool maybeClose() override {
    ++asked;
    return !veto;
  }
};

[[maybe_unused]] const bool probeAdded = areas::add("Probe", [](AreaServices& s) -> AreaController* {
  return qEnvironmentVariableIsSet("OPAD_BENCH_AREAS") ? new ProbeArea(s) : nullptr;
});

QTreeWidgetItem* rowOf(QTreeWidget* tree, const QString& id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, browser::kIdRole).toString() == id) return *it;
  return nullptr;
}
}  // namespace

// OPAD_BENCH_AREAS=<prefix> on a document with one body: the hooks in order, the command in the Tools menu, the ribbon
// and the status bar, documentChanged on edits and loads, selectionChanged from the browser, the context menu's entry
// and selection, positionOverlays with the viewport; the browser's decorations (badges right of the name and left of the
// built-in ones, a badge click that runs its callback and selects nothing, tooltips) and the probe's folder (rows after
// Sketches, selection, double-click, its own context menu, no eye, open or closed across rebuilds, breadcrumb), the
// browser saved as <prefix>.browser.png; last maybeClose keeping the document, then letting New replace it.
OPAD_BENCH(OPAD_BENCH_AREAS, areas) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: areas: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  ProbeArea* probe = nullptr;
  for (AreaController* area : w.m_areas)
    if (auto* p = dynamic_cast<ProbeArea*>(area)) probe = p;
  require(probe && areas::names().contains("Probe") && areas::clashes().isEmpty() && probe->objectName() == "Probe", "probe area registered and made");
  if (!probe || w.m_doc->scene.all_bodies().size() != 1) {
    QCoreApplication::exit(2);
    return true;
  }
  require(probe->hooks == QStringList({"buildActions", "menus", "ribbon", "statusWidgets", "ready"}), "construction hooks in order: " + probe->hooks.join(' '));
  bool inTools = false;
  for (QAction* menu : w.menuBar()->actions())
    inTools = inTools || (menu->text() == QObject::tr("&Tools") && menu->menu()->actions().contains(probe->command));
  require(w.action("probe.command") == probe->command && w.m_actions.contains(probe->command) && inTools, "command registered, in the Tools menu");
  int buttons = 0;
  for (QToolButton* b : w.m_ribbon->findChildren<QToolButton*>())
    if (b->defaultAction() == probe->command) ++buttons;
  bool tab = false;
  for (QTabBar* bar : w.m_ribbon->findChildren<QTabBar*>())
    for (int i = 0; i < bar->count(); ++i) tab = tab || bar->tabText(i) == "Probe";
  require(buttons == 2 && tab, QString("ribbon: a group on Review > View and a Probe tab (%1 buttons)").arg(buttons));
  require(probe->label && probe->label->parentWidget() == w.statusBar() && probe->label->isVisibleTo(w.statusBar()), "status widget in the status bar");
  require(!probe->changes.empty() && probe->changes.back(), "documentChanged: the load replaced the document");
  const std::string body = w.m_doc->scene.all_bodies().front();
  w.m_doc->run("rename", opad::json{{"target", body}, {"name", "Probe body"}});
  require(!probe->changes.back(), "documentChanged: an edit is not a replacement");
  w.m_browser->selectIds({body});
  require(probe->selected.ids == std::vector<std::string>{body} && probe->selected.refs.size() == 1 && !probe->selected.sketching, "selectionChanged from the browser");
  w.showContextMenu(w.mapToGlobal(QPoint(200, 200)), {body});
  require(probe->menuSelection.ids == std::vector<std::string>{body} && probe->menuEntries.contains("probe.command") && probe->menuEntries.contains("edit.hide"),
          "context menu: the probe's entry after the built-in ones, for the right object");
  w.positionOverlays();
  require(probe->overlays == QRect(w.m_viewport->mapToGlobal(QPoint(0, 0)), w.m_viewport->size()) && !probe->overlays.isEmpty(), "positionOverlays: the viewport, global");
  probe->command->trigger();
  require(probe->ran == 1, "the command runs");

  // The browser, opened (its rows get their width when it is shown).
  w.m_browserOverlay->setAutoHide(false);
  w.m_browserOverlay->reveal();
  QTimer::singleShot(500, &w, [&w, probe, body, require, all, prefix = value] {
    BrowserTree* tree = w.m_browser->tree();
    auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
    QTreeWidgetItem* root = tree->topLevelItem(0);
    QTreeWidgetItem* folder = root && root->childCount() ? root->child(0) : nullptr;  // no sketches in this document
    QTreeWidgetItem* a = rowOf(tree, "probe:a");
    const bool rows = folder && folder->data(0, browser::kFolderRole).toString() == "probe" && folder->text(0) == "Probe folder" && folder->childCount() == 2 &&
                      a && a->parent() == folder && a->childCount() == 1 && a->child(0)->text(0) == "Probe A1" && folder->child(1)->text(0) == "Probe B" &&
                      root->childCount() == 2 && root->child(1)->data(0, browser::kIdRole).toString().toStdString() == body && folder->isExpanded();
    require(rows, "folder: its rows first under the document, then the nodes");
    if (!rows || !delegate) return QCoreApplication::exit(2);

    QTreeWidgetItem* bodyRow = rowOf(tree, QString::fromStdString(body));
    const QModelIndex index = tree->indexFromItem(bodyRow);
    const QRect r = tree->visualRect(index);
    const browser::Decoration d = delegate->decoration(index);
    QRect syncRect, markRect;
    for (int x = r.right(); x > r.left(); --x) {
      QRect hit;
      if (const browser::Badge* b = delegate->badgeAt(d, index, r, QPoint(x, r.center().y()), &hit)) (b->clicked ? syncRect : markRect) = hit;
    }
    require(d.italic && d.badges.size() == 2 && !syncRect.isEmpty() && !markRect.isEmpty() && markRect.right() < syncRect.left() &&
                syncRect.right() <= r.right() - 6 && markRect.left() > r.left() + browser::kNameX,
            QString("decoration: two badges right to left, clear of the name (row %1 px wide, badges at %2 and %3)").arg(r.width()).arg(syncRect.left() - r.left()).arg(markRect.left() - r.left()));
    w.m_browser->selectIds({"probe:b"});
    require(probe->selected.ids == std::vector<std::string>{"probe:b"} && w.m_browser->selectedIds() == std::vector<std::string>{"probe:b"}, "folder: a row selects like a node");
    auto mouse = [tree](QEvent::Type type, const QPoint& at, Qt::MouseButton button) {
      QMouseEvent e(type, QPointF(at), QPointF(tree->viewport()->mapToGlobal(at)), button, type == QEvent::MouseButtonRelease ? Qt::NoButton : button, Qt::NoModifier);
      QCoreApplication::sendEvent(tree->viewport(), &e);
    };
    mouse(QEvent::MouseButtonPress, syncRect.center(), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, syncRect.center(), Qt::LeftButton);
    require(probe->badgeClicks == 1 && w.m_browser->selectedIds() == std::vector<std::string>{"probe:b"}, "badge click runs its callback, selects nothing");
    auto tip = [tree, delegate](const QModelIndex& at, const QPoint& p) {
      QToolTip::hideText();
      QHelpEvent e(QEvent::ToolTip, p, tree->viewport()->mapToGlobal(p));
      QStyleOptionViewItem option;
      option.rect = tree->visualRect(at);
      delegate->helpEvent(&e, tree, option, at);
      return QToolTip::text();
    };
    const QString badgeTip = tip(index, syncRect.center()), rowTip = tip(index, QPoint(r.left() + browser::kNameX + 4, r.center().y()));
    require(badgeTip == "probe badge" && rowTip.startsWith("Probe body\n") && rowTip.endsWith("\nprobe row"), "tooltips: the badge's, the row's with the decorators' line");

    const size_t ops = w.m_doc->doc.ops.size();
    const QRect b = tree->visualRect(tree->indexFromItem(rowOf(tree, "probe:b")));
    mouse(QEvent::MouseButtonPress, QPoint(b.left() + browser::kEyeX + 6, b.center().y()), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, QPoint(b.left() + browser::kEyeX + 6, b.center().y()), Qt::LeftButton);
    require(w.m_doc->doc.ops.size() == ops, "folder: a row has no eye");
    const QPoint at = tree->visualRect(tree->indexFromItem(a)).center();
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease, QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) mouse(type, at, Qt::LeftButton);
    require(probe->activated == "probe:a", "folder: double-click activates the row");
    const int menus = probe->menuCalls;
    emit tree->customContextMenuRequested(tree->visualRect(tree->indexFromItem(rowOf(tree, "probe:b"))).center());  // A closed by the double-click
    const bool rowMenu = probe->folderMenu == "probe:b";
    emit tree->customContextMenuRequested(tree->visualRect(tree->indexFromItem(folder)).center());
    require(rowMenu && probe->folderMenu.empty() && probe->menuCalls == menus, "folder: its own context menu on a row and on the folder, not the objects' one");
    w.m_browser->selectIds({"probe:a1"});
    bool crumb = false;
    for (QLabel* label : w.m_browser->findChildren<QLabel*>()) crumb = crumb || (label->text().contains("Probe folder") && label->text().contains("Probe A1"));
    require(crumb, "folder: breadcrumb through the folder");
    w.m_browser->grab().save(prefix + ".browser.png");
    w.m_browser->selectIds({body});  // a selected row keeps its parents open
    folder->setExpanded(false);
    w.m_browser->rebuild();
    QTreeWidgetItem* again = tree->topLevelItem(0)->child(0);
    require(again && again->text(0) == "Probe folder" && !again->isExpanded() && rowOf(tree, "probe:a1"), "folder: closed stays closed across a rebuild");

    const auto generation = w.m_doc->generation;
    const int asked = probe->asked;  // opening the file on the command line asked once already
    probe->veto = true;
    w.action("file.new")->trigger();
    require(probe->asked == asked + 1 && w.m_doc->generation == generation && w.m_doc->scene.all_bodies().size() == 1, "maybeClose false keeps the document");
    probe->veto = false;
    w.action("file.new")->trigger();
    require(probe->asked == asked + 2 && w.m_doc->generation != generation && w.m_doc->scene.all_bodies().empty() && probe->changes.back(), "maybeClose true lets New replace it");
    QCoreApplication::exit(*all ? 0 : 2);
  });
  return true;
}
