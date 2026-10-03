// The feature-area seams (AreaController.hpp) in the running app: a probe area, made only for OPAD_BENCH_AREAS, records
// every hook and uses the browser's provider APIs; the bench drives the window and checks what reached it. Cases in
// tools/bench_cases/core.py.
#include <QActionGroup>
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
#include "ViewportChips.hpp"

namespace {
class ProbeArea : public AreaController {
 public:
  using AreaController::AreaController;
  QStringList hooks;  // construction hooks, in call order
  QAction* command = nullptr;
  QLabel* label = nullptr;
  QLabel* chip = nullptr;
  int ran = 0, asked = 0, menuCalls = 0, badgeClicks = 0, leadClicks = 0, propertyActions = 0, sectionRows = 1;
  bool veto = false;
  SelectionContext selected, menuSelection;
  QStringList menuEntries;
  std::vector<bool> changes;  // documentChanged(replaced)
  QRect overlays;
  std::string activated, folderMenu = "none";
  QString startWorkspace;  // services().workspace() in ready()
  QStringList workspaces;  // workspaceChanged
  PropertySubject subject;  // the last one the section provider saw

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
    layout.addWorkspace("probe", {"Probe", "dot", "Ctrl+Shift+9", "Probe workspace", "ops: none"});
    layout.addTab("probe", "probe.tools", "Probe tools", {{services().action("view.fit")}});
  }
  void statusWidgets(QStatusBar* bar) override {
    hooks << "statusWidgets";
    label = new QLabel("probe", bar);
    bar->addPermanentWidget(label);
  }
  void ready() override {
    hooks << "ready";
    startWorkspace = services().workspace();
    chip = new QLabel("Probe chip");
    chip->setObjectName("chipSel");
    chip->hide();
    services().chips()->addChip(chip);
    // Bodies get a clickable text badge (Arabic in a right-to-left UI) and an icon badge, an italic name and a tooltip line.
    const bool rtl = QGuiApplication::layoutDirection() == Qt::RightToLeft;
    services().browser()->addDecorator([this, rtl](const browser::Row& row, browser::Decoration& d) {
      if (row.kind == "document") {  // a clickable lead (as an activation radio would be); a provided row's has no click
        d.lead.icon = "dot";
        d.lead.color = &Tokens::sel;
        d.lead.tooltip = "probe lead";
        d.lead.clicked = [this] { ++leadClicks; };
      } else if (row.id == "probe:b") {
        d.lead.icon = "check";
        d.lead.tooltip = "probe row lead";
      }
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
    // A section for bodies: rows and a link.
    services().properties()->addSectionProvider([this](const PropertySubject& s, const opad::json&, QList<PropertySection>& out) {
      subject = s;
      if (s.refs.empty() || s.refs.front().kind != opad::Ref::Kind::Body) return;
      PropertySection section;
      section.title = "Probe section";
      section.rows = {{"Probe key", "probe value"}};
      for (int i = 1; i < sectionRows; ++i) section.rows.append({QString("Probe row %1").arg(i), "x"});
      section.actions = {{"Probe action", [this] { ++propertyActions; }}};
      out << section;
    });
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
  void workspaceChanged(const QString& id) override { workspaces << id; }
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

// OPAD_BENCH_AREAS=<prefix> on a document with one body: the hooks in order, the workspace the settings start in
// (OPAD_BENCH_AREAS_WORKSPACE), the command in the Tools menu, the ribbon and the status bar, documentChanged on edits and
// loads, selectionChanged from the browser, the context menu's entry and selection, positionOverlays with the viewport; the
// probe's workspace (its command, switching by id, workspaceChanged); its chip in the chips row (saved as
// <prefix>.chips.png); the browser's decorations (badges right of the name and left of the built-in ones, a badge click
// that runs its callback and selects nothing, tooltips, a lead in the swatch's column) and the probe's folder (rows after
// Sketches, selection, double-click, its own context menu, no eye, open or closed across rebuilds, breadcrumb), the
// browser saved as <prefix>.browser.png; the probe's Properties section (after the built-in rows, its link, refresh, an
// op as the subject; the panel saved as <prefix>.properties.png); last maybeClose keeping the document, then letting New replace it.
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
  // OPAD_BENCH_AREAS_WORKSPACE: the workspace the case's settings start in (by id, or Design saved as 1 by earlier builds).
  const QString start = qEnvironmentVariable("OPAD_BENCH_AREAS_WORKSPACE", "review");
  require(probe->startWorkspace == start && w.workspaceId() == start && w.action("workspace." + start) && w.action("workspace." + start)->isChecked() && probe->workspaces.isEmpty(),
          "workspace restored at startup, not reported: " + probe->startWorkspace);
  w.setWorkspace("review");  // the ribbon checks below look at Review's tabs
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

  // The probe's chip: after the built-in ones; the row follows it as it shows, grows and hides.
  ViewportChips* chips = w.m_chips;
  auto fit = [chips] { QCoreApplication::sendPostedEvents(chips, QEvent::LayoutRequest); };
  fit();
  const int narrow = chips->width();
  probe->chip->show();
  fit();
  const int wide = chips->width();
  const bool rtl = chips->layoutDirection() == Qt::RightToLeft;
  bool after = true;
  for (QWidget* other : chips->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
    if (other != probe->chip && other->isVisibleTo(chips))
      after = after && (rtl ? probe->chip->geometry().right() < other->geometry().left() : probe->chip->geometry().left() > other->geometry().right());
  chips->grab().save(value + ".chips.png");
  probe->chip->setText("Probe chip with a longer text");
  fit();
  const int longer = chips->width();
  const bool inside = chips->rect().contains(probe->chip->geometry());
  probe->chip->hide();
  fit();
  require(probe->chip->parentWidget() == chips && after && wide > narrow + 20 && longer > wide && chips->width() == narrow && inside,
          QString("chips: an area's chip after the built-in ones, the row fits it (%1, %2, %3, %4 px)").arg(narrow).arg(wide).arg(longer).arg(chips->width()));

  // The probe's workspace: a switcher command made by the window, remembered by id, reported to the areas.
  QAction* space = w.action("workspace.probe");
  QAction* review = w.action("workspace.review");
  const QList<QAction*> viewMenu = w.m_viewMenu->actions();
  const int at = viewMenu.indexOf(space);
  require(space && space->isCheckable() && space->shortcut() == QKeySequence("Ctrl+Shift+9") && space->actionGroup() == review->actionGroup() &&
              review->actionGroup()->isExclusive() && at > 0 && viewMenu[at - 1] == w.action("workspace.design") && w.m_actions.contains(space),
          "workspace: its command, shortcut, in the View menu after Design, one of the switcher's group");
  if (!space) {
    QCoreApplication::exit(2);
    return true;
  }
  const qsizetype reported = probe->workspaces.size();
  space->trigger();
  bool tools = false;
  for (QTabBar* bar : w.m_ribbon->findChildren<QTabBar*>())
    for (int i = 0; i < bar->count(); ++i) tools = tools || bar->tabText(i) == "Probe tools";
  require(w.workspaceId() == "probe" && probe->services().workspace() == "probe" && probe->workspaces.size() == reported + 1 && probe->workspaces.back() == "probe" &&
              space->isChecked() && !review->isChecked() && w.m_settings.value("ui/workspaceId").toString() == "probe" &&
              w.m_settings.value("ui/workspace").toInt() == 0 && tools &&
              w.statusBar()->currentMessage().contains("Ctrl+1 / 2 / Ctrl+Shift+9"),
          "workspace: the command shows its tabs, checks it, saves it by id and tells the areas (" + w.statusBar()->currentMessage() + ")");
  probe->services().setWorkspace("design");
  require(w.workspaceId() == "design" && probe->workspaces.back() == "design" && w.action("workspace.design")->isChecked() && !space->isChecked() &&
              w.m_settings.value("ui/workspaceId").toString() == "design" && w.m_settings.value("ui/workspace").toInt() == 1,
          "workspace: an area switches by id (and an earlier build reads Design as 1)");
  const qsizetype before = probe->workspaces.size();
  probe->services().setWorkspace("sketch");
  probe->services().setWorkspace("no-such-workspace");
  require(w.workspaceId() == "design" && probe->workspaces.size() == before, "workspace: the sketch's and unknown ids change nothing");
  review->trigger();
  require(w.workspaceId() == "review" && probe->workspaces.back() == "review" && review->isChecked(), "workspace: back to Review");

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
    {  // ... and is no node: Del does not fall back to the timeline's marker, V does nothing, Properties shows the sections alone
      std::string marker;
      for (const auto& op : w.m_doc->doc.ops)
        if (op.type == "feature") marker = op.id;
      w.m_timeline->setCurrentOp(marker);  // a marker clicked earlier
      const size_t ops = w.m_doc->doc.ops.size();
      bool refused = false;
      try { w.deleteCurrent(); } catch (const std::exception&) { refused = true; }
      const size_t afterDelete = w.m_doc->doc.ops.size();
      w.action("edit.hide")->trigger();
      w.action("inspect.properties")->trigger();
      const QTreeWidget* table = w.m_props->table();
      const bool sections = w.m_propsPanel->isVisible() && table->topLevelItemCount() > 0 && table->topLevelItem(0)->text(0).contains("PROBE SECTION") &&
                            w.m_props->subject().refs.size() == 1 && w.m_props->subject().refs.front().body == "probe:b";
      require(!marker.empty() && w.currentNodeIds().empty() && w.m_selRefs.empty() && w.m_statusSel->text() == MainWindow::tr("%1 selected").arg(1),
              "folder: a row is no node for the view or the edit commands (status " + w.m_statusSel->text() + ")");
      require(refused && afterDelete == ops && w.m_doc->doc.ops.size() == ops,
              QString("folder: Del on a row is refused, not the timeline's marker (%1, %2 ops more); V does nothing (%3 more)")
                  .arg(refused ? "refused" : "ran").arg(afterDelete - ops).arg(w.m_doc->doc.ops.size() - afterDelete));
      require(sections, QString("folder: Properties shows the areas' sections alone (shown %1, %2 rows, first '%3')")
                            .arg(w.m_propsPanel->isVisible()).arg(table->topLevelItemCount()).arg(table->topLevelItemCount() ? table->topLevelItem(0)->text(0) : QString()));
      w.m_propsPanel->hide();
      w.m_timeline->setCurrentOp({});
    }
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
    // The lead: in the swatch's column, clickable without selecting; a provided row's shows its tooltip, the body keeps its swatch.
    const QModelIndex docIndex = tree->indexFromItem(root), bIndex = tree->indexFromItem(rowOf(tree, "probe:b"));
    const QRect docRect = tree->visualRect(docIndex), bRect = tree->visualRect(bIndex);
    QRect leadRect, bLeadRect;
    const browser::Decoration docDecoration = delegate->decoration(docIndex), bDecoration = delegate->decoration(bIndex);
    const browser::Badge* lead = delegate->badgeAt(docDecoration, docIndex, docRect, QPoint(docRect.left() + browser::kSwatchX + 5, docRect.center().y()), &leadRect);
    const browser::Badge* bLead = delegate->badgeAt(bDecoration, bIndex, bRect, QPoint(bRect.left() + browser::kSwatchX + 5, bRect.center().y()), &bLeadRect);
    require(lead && lead->tooltip == "probe lead" && leadRect.left() >= docRect.left() + browser::kEyeX + 18 && leadRect.right() < docRect.left() + browser::kTypeX && bLead && !bLead->clicked &&
                !delegate->badgeAt(d, index, r, QPoint(r.left() + browser::kSwatchX + 5, r.center().y())),
            QString("lead: between the eye and the type icon (%1 to %2 px), not on the body").arg(leadRect.left() - docRect.left()).arg(leadRect.right() - docRect.left()));
    mouse(QEvent::MouseButtonPress, leadRect.center(), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, leadRect.center(), Qt::LeftButton);
    require(probe->leadClicks == 1 && w.m_browser->selectedIds() == std::vector<std::string>{"probe:b"} && tip(docIndex, leadRect.center()) == "probe lead" &&
                tip(bIndex, bLeadRect.center()) == "probe row lead",
            "lead: a click runs its callback and selects nothing; tooltips");

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

    // Properties: the probe's section under the built-in rows, its link, a refill, an op as the subject.
    w.m_browser->selectIds({body});
    w.action("inspect.properties")->trigger();
    QTreeWidget* table = w.m_props->table();
    auto rowAt = [table](const QString& text, int column) {
      for (int i = 0; i < table->topLevelItemCount(); ++i)
        if (table->topLevelItem(i)->text(column).contains(text)) return i;
      return -1;
    };
    const int header = rowAt("PROBE SECTION", 0), key = rowAt("Probe key", 0), link = rowAt("Probe action", 1);
    require(w.m_propsPanel->isVisible() && w.m_props->subject().refs.size() == 1 && w.m_props->subject().refs.front().body == body && probe->subject.refs.size() == 1 && probe->subject.refs.front().str() == w.m_props->subject().refs.front().str() &&
                header > 2 && key == header + 1 && table->topLevelItem(key)->text(1).contains("probe value") && link == key + 1,
            QString("properties: the section after %1 built-in rows").arg(header));
    QRect linkRect;
    if (link >= 0) {
      table->scrollToItem(table->topLevelItem(link));
      linkRect = table->visualItemRect(table->topLevelItem(link));
      const QPoint at = linkRect.center();
      for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, QPointF(at), QPointF(table->viewport()->mapToGlobal(at)), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(table->viewport(), &e);
      }
    }
    require(probe->propertyActions == 1, QString("properties: the link runs its action (at %1,%2 in %3x%4)").arg(linkRect.center().x()).arg(linkRect.center().y()).arg(table->viewport()->width()).arg(table->viewport()->height()));
    probe->sectionRows = 3;
    w.m_props->refresh();
    require(rowAt("Probe row 2", 0) == rowAt("Probe key", 0) + 2, "properties: refresh() asks the providers again");
    table->scrollToBottom();
    w.m_propsPanel->grab().save(prefix + ".properties.png");
    for (QToolButton* b : w.m_propsPanel->findChildren<QToolButton*>())
      if (b->isCheckable()) b->setChecked(true);  // pinned: it stays open for the op
    std::string feature;
    for (const auto& op : w.m_doc->doc.ops)
      if (op.type == "feature") feature = op.id;
    w.selectOpTargets(feature);
    require(!feature.empty() && w.m_props->subject().op == feature && probe->subject.op == feature && rowAt("PROBE SECTION", 0) < 0, "properties: a timeline op as the subject");

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
