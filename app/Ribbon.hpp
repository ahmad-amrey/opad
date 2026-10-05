#pragma once
// Tabbed ribbon (28 px tab row + 80 px tool strip). The tab set belongs to a workspace (Review, Design, ...): one
// document and one timeline, a workspace only changes which tabs and tools the ribbon shows. The switcher chip sits left
// of the tabs and opens the workspace list; the tab row ends in one cluster (quick access Save, Undo ▾, Redo ▾, "Search
// commands", the areas' widgets such as a branch chip, settings), and the strip ends in the compact Select control
// ("Select ▾" and the filters as icons with their keys), so the tools get the rest of the strip (UI-103).
// A tab is a row of titled groups (UI-120 b). A label is never shortened: when a tab is narrower than its tools, groups
// step down, the rightmost first and all of them one step before any takes the next: large tools become small ones
// (icon beside the label, three to a column), then icons only (the label in the tooltip), then the whole group one
// button that drops its tools down. A group's title under it ("CREATE ▾") lists every tool of the group.
#include <QAbstractButton>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QMenu>
#include <QWidget>

#include <array>
#include <functional>

#include "Theme.hpp"

class SegmentButton : public QToolButton {
  Q_OBJECT
 public:
  // hint: the key shown after the label, as text, a fixed key's name or a command id (its key now: keys::spec).
  SegmentButton(QAction* action, const QString& hint, bool primary, QWidget* parent = nullptr);
  void setIconOnly(bool on);  // the action's icon (its name in QAction::data) and the key, the label in the tooltip
  QString hint() const { return m_hint; }  // the key as shown now
  bool iconOnly() const { return m_iconOnly; }
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  QString m_spec, m_hint;
  bool m_iconOnly = false;
};

class SearchField : public QAbstractButton {
  Q_OBJECT
 public:
  explicit SearchField(QWidget* parent = nullptr);
  void setCompact(bool on);  // the icon alone: the tab row is short of room
  void setCommand(const QString& id);  // the command it opens: its key now in the badge and tooltip (none: no badge)
  QString key() const { return m_key; }
  bool compact() const { return m_compact; }
  QSize sizeHint() const override;
  int fullWidth() const;  // with its words
  static constexpr int kFull = 200, kCompact = 28, kHeight = 24;

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  bool m_compact = false;
  QString m_command, m_key;
};

struct Workspace {
  // Initialised positionally ({name, icon, key, description, ops}): new members go at the end.
  QString name, icon, key;   // key: its default shortcut, e.g. "Ctrl+1" (the command's when made by the window)
  QString description, ops;  // the dropdown row's hint and the chip's tooltip: what it is for, and the op types it writes
  bool contextual = false;   // entered by the app (sketch mode), never offered in the switcher's list
  QString command;           // its command ("workspace.design"): the key shown is the one bound now; empty: key
};

// The ribbon as data, before it is built (MainWindow::buildRibbon): the built-in workspaces and tabs (the ribbon table,
// MainWindowRibbonTable.cpp), then what the feature areas add (AreaController::ribbon), then RibbonBar is made from it in
// this order. Workspaces have ids ("review", "design", "drafting"), tabs "<workspace>.<name>" ("review.view",
// "design.assemble", the contextual "design.sketch"), titled groups "<tab>.<name>" ("design.solid.create"). Null actions are left out, and so is a group left empty. Every workspace that
// is not contextual is switched to by the command "workspace.<id>": the window makes it (key = its shortcut, in the View
// menu after the others) unless the area added one of its own, and keeps it checked while the workspace is shown; the
// last one is remembered by id (setting ui/workspaceId). A contextual tab (addContextualTab) stays hidden until the app
// shows it (MainWindow::setContextualTab, AreaServices::setContextualTab): then it comes first in its workspace's tab
// row, in its accent colour, and is the current tab until it is hidden again.
//   layout.addGroup("design.solid", "design.solid.create", tr("Create"));
//   layout.addAction("design.solid.create", action("design.extrude"));
//   layout.addAction("design.solid.create", action("design.hole"), RibbonLayout::Size::Small, {action("design.thread")});  // split
struct RibbonLayout {
  // Large: icon above the label; Small: icon beside it, three to a column; Icon: the icon alone at every level, three to a
  // column (the label in the tooltip and its accessible name): glyphs that read at a glance, the sketch's constraints.
  enum class Size { Large, Small, Icon };
  struct Item {
    QAction* action = nullptr;
    Size size = Size::Large;
    QList<QAction*> variants;  // a split button: a click runs the action, its arrow drops these down
    bool primary = false;      // the tab's main verb (Finish sketch): filled in the accent colour, its size at every level
  };
  struct Group {
    QString id, title;  // untitled (an area's plain group, addGroup(tab, actions)): no title row, collapses to "More ▾"
    QList<Item> items;
    QList<QAction*> actions() const;  // the items' actions, in order
  };
  struct Tab {
    QString id, title;
    QList<Group> groups;
    bool contextual = false;
    QColor Tokens::* accent = nullptr;  // a contextual tab's title colour
  };
  struct Space {
    QString id;
    Workspace workspace;
    QList<Tab> tabs;
  };
  QList<Space> spaces;  // the switcher's order
  Space& addWorkspace(const QString& id, const Workspace& workspace);  // at the end; an id that is there already: that one
  Tab* addTab(const QString& workspace, const QString& id, const QString& title, const QList<QList<QAction*>>& groups = {});  // null: no such workspace
  Tab* addContextualTab(const QString& workspace, const QString& id, const QString& title, QColor Tokens::* accent = &Tokens::amber);
  bool addGroup(const QString& tab, const QList<QAction*>& actions);  // an untitled group after the tab's groups; false: no such tab
  Group* addGroup(const QString& tab, const QString& id, const QString& title);  // a titled one; null: no such tab; an id there: that one
  bool addAction(const QString& group, QAction* action, Size size = Size::Large, const QList<QAction*>& variants = {}, bool primary = false);  // false: no such group
  Space* workspace(const QString& id);
  Tab* tab(const QString& id);
  Group* group(const QString& id);
  int index(const QString& workspace) const;  // its RibbonBar index once built; -1 if none
};

// Switcher chip: 26 px, bg2, 1 px line, icon in sel + name 500 + mono key + chevron.
class WorkspaceChip : public QAbstractButton {
  Q_OBJECT
 public:
  explicit WorkspaceChip(QWidget* parent = nullptr);
  void setWorkspace(const Workspace& w);
  QSize sizeHint() const override;
 protected:
  void paintEvent(QPaintEvent*) override;
 private:
  Workspace m_ws;
};

// One group of a tab: its tool buttons (one per item) at a level, and its title under them.
class RibbonGroup : public QWidget {
  Q_OBJECT
 public:
  enum Level { Large, Small, Icons, Collapsed };  // as laid out; every tool small; icons only; one button with a menu
  RibbonGroup(const RibbonLayout::Group& group, QWidget* parent);
  int widthAt(int level);  // remembered until a tool's label or visibility changes
  int nextLevel(int level);  // the next level that is narrower (one tool small can be wider than large); -1: none
  int level() const { return m_level; }
  void setLevel(int level);  // places the tools for that level and resizes the group
  void remeasure() { m_widths.fill(-1); }  // the text size changed
  QString title() const { return m_title; }
  QMenu* menu() const { return m_menu; }  // every tool of the group: the title's ▾ and the collapsed button drop it
  QList<QToolButton*> buttons() const;    // the tools', one per item, in order (hidden while collapsed)
  QToolButton* titleButton() const { return m_titleButton; }
  QToolButton* collapsedButton() const { return m_collapsed; }
  static constexpr int kTop = 4, kTools = 56, kTitle = 16, kRow = 18, kHeight = kTop + kTools + kTitle + 4;  // at 100 % text
  // At the text size (theme::textScale, UI-124): a small tool's row, the tools' box (three rows at least), the title's.
  static int toolRow();
  static int toolsBox();
  static int titleBox();
  static int stripHeight();
 signals:
  void widthsChanged();
 private:
  struct Slot {
    QAction* action;
    RibbonLayout::Size size;
    QToolButton* button;
    bool pinned = false;  // keeps its size whatever the level (the primary verb); its group never collapses
  };
  int modeAt(const Slot& s, int level) const;  // how a tool shows at a level: Large, Small or Icons
  void style(QToolButton* b, int mode) const;  // mode: Large, Small or Icons
  QSize measure(const Slot& s, int mode);
  void actionChanged();
  QList<Slot> m_slots;
  QString m_title;
  QMenu* m_menu;
  QToolButton* m_titleButton;
  QToolButton* m_collapsed;
  QToolButton* m_probe;  // hidden: sizes of the tools at the other levels
  QString m_signature;   // labels and visibility the widths were measured for
  std::array<int, 4> m_widths{{-1, -1, -1, -1}};
  int m_level = Large;
};

// A tab's page: its groups left to right (mirrored right to left), each at the level that lets the row fit the width.
class RibbonPage : public QWidget {
  Q_OBJECT
 public:
  explicit RibbonPage(const RibbonLayout::Tab& tab, QWidget* parent);
  QString id() const { return m_id; }
  QList<RibbonGroup*> groups() const { return m_groups; }
  QList<int> levels() const;
  int widthAt(const QList<int>& levels);  // the row with these levels, padding and separators included
  // Steps groups down until the row fits: the rightmost of those that took the fewest steps goes first, to its next
  // narrower level (a group of small tools skips the small level: one step); a group with nothing narrower left stays.
  void fit();
  QSize sizeHint() const override { return minimumSizeHint(); }
  QSize minimumSizeHint() const override;  // every group collapsed
  static constexpr int kPad = 4, kSeparator = 9;
 protected:
  void resizeEvent(QResizeEvent*) override;
  void paintEvent(QPaintEvent*) override;  // separators between groups
 private:
  QString m_id;
  QList<RibbonGroup*> m_groups;
};

class RibbonBar : public QWidget {
  Q_OBJECT
 public:
  explicit RibbonBar(QWidget* parent = nullptr);
  int addWorkspace(const Workspace& w);
  int addTab(int workspace, const RibbonLayout::Tab& tab);  // contextual ones stay hidden until setContextualTab
  void setWorkspace(int index);  // swaps the tab set; each workspace remembers its current tab
  int workspace() const { return m_workspace; }
  const Workspace& workspaceAt(int index) const { return m_workspaces[index]; }
  // Shows a contextual tab first in its workspace's row (and makes it the current tab) or hides it (the tab that was
  // current before comes back). False: no such contextual tab.
  bool setContextualTab(const QString& id, bool shown);
  bool contextualTabShown(const QString& id) const;
  QStringList contextualTabs(int workspace) const;  // the ids of a workspace's contextual tabs, shown or not
  // The tab row's cluster, in this order whichever order they come in: quick access, search, the areas' widgets,
  // settings. It is never squeezed: when the row cannot show every tab whole beside it, search shows as its icon alone.
  QToolButton* addQuickAction(QAction* a, QMenu* steps = nullptr);  // an icon; with steps a split button (Undo ▾)
  void addTabRowWidget(QWidget* w);  // an area's (a branch chip, AreaServices::addTabRowWidget), before settings
  void setSearchAction(QAction* a);
  void setSettingsMenu(QAction* a, QMenu* menu = nullptr);
  // The strip's trailing end: "Select ▾" (dropping `more`), then the filters as icon segments with their keys (hints).
  void setSelectFilters(const QList<QAction*>& filters, const QStringList& hints, QMenu* more = nullptr);
  QWidget* tabRow() const { return m_tabRow; }
  QWidget* cluster() const { return m_cluster; }
  SearchField* searchField() const { return m_search; }
  QToolButton* selectButton() const { return m_selectButton; }  // "Select ▾"
  QWidget* strip() const { return m_strip; }
  static constexpr int kRowMargin = 8, kChipGap = 12, kClusterGap = 16;  // tab row: its ends, after the chip, before the cluster
  void setCurrentTab(int index) { m_tabs->setCurrentIndex(index); }
  int currentTab() const { return m_tabs->currentIndex(); }
  QStringList tabIds() const;  // the current workspace's tabs as the row shows them
  RibbonPage* page(const QString& tabId) const;
  RibbonPage* currentPage() const;
  QTabBar* tabBar() const { return m_tabs; }
  // Every command button the ribbon makes (tools and menu buttons, filters, quick access, search, settings) is handed to
  // this with its command id as it is made: the window attaches the hover cards there (RichTip::attach, UI-106).
  using CommandButtonHook = std::function<void(QWidget* button, const QString& commandId)>;
  static void setCommandButtonHook(CommandButtonHook hook);
  static void commandButton(QWidget* button, const QString& commandId);  // runs the hook (none set: nothing)

 signals:
  void workspaceChanged(int index);

 protected:
  void resizeEvent(QResizeEvent*) override;

 private:
  void showWorkspaceMenu();
  void fitTabRow();  // search whole or as its icon, whichever lets every tab show whole
  void refillTabs();  // the current workspace's row: shown contextual tabs first, then the others
  struct Entry {
    QString id, title;
    RibbonPage* page;
    bool contextual = false, shown = true;
    QColor Tokens::* accent = nullptr;
  };
  struct Tabs {
    QList<Entry> entries;
    QString current, beforeContextual;  // tab ids
    QList<int> row;                     // entries as the tab bar shows them
  };
  QList<Workspace> m_workspaces;
  QList<Tabs> m_tabSets;
  int m_workspace = -1;
  bool m_filling = false;
  WorkspaceChip* m_chip;
  QTabBar* m_tabs;
  QWidget* m_tabRow;
  QWidget* m_cluster;
  QHBoxLayout *m_quick, *m_searchSlot, *m_corner, *m_settingsSlot;  // the cluster's parts, in order
  SearchField* m_search = nullptr;
  QToolButton* m_selectButton = nullptr;
  QStackedWidget* m_stack;
  QWidget* m_strip;
  QHBoxLayout* m_stripLayout;
  QHBoxLayout* m_right;
};
