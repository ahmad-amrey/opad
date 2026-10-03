#pragma once
// Tabbed ribbon (28 px tab row + 80 px tool strip) with the right-hand cluster: Select segmented control,
// "Search commands" field and settings, per the design handoff. The tab set belongs to a workspace (Review,
// Design, ...): one document and one timeline, a workspace only changes which tabs and tools the ribbon shows.
// The switcher chip sits left of the tabs and opens the workspace list.
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

#include "Theme.hpp"

class SegmentButton : public QToolButton {
  Q_OBJECT
 public:
  SegmentButton(QAction* action, const QString& hint, bool primary, QWidget* parent = nullptr);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  QString m_hint;
};

class SearchField : public QAbstractButton {
  Q_OBJECT
 public:
  explicit SearchField(QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(200, 28); }

 protected:
  void paintEvent(QPaintEvent*) override;
};

struct Workspace {
  QString name, icon, key;   // key: the shortcut as shown, e.g. "Ctrl+1"
  QString description, ops;  // dropdown copy: what it is for, and the op types it writes
  bool contextual = false;   // entered by the app (sketch mode), never offered in the switcher's list
};

// The ribbon as data, before it is built (MainWindow::buildRibbon): the built-in workspaces and tabs, then what the
// feature areas add (AreaController::ribbon), then RibbonBar is made from it in this order. Workspaces have ids ("review",
// "design", "sketch"), tabs "<workspace>.<name>" ("review.view", "design.assemble", "sketch.constrain"), titled groups
// "<tab>.<name>" ("design.solid.create"). Null actions are left out, and so is a group left empty. Every workspace that
// is not contextual is switched to by the command "workspace.<id>": the window makes it (key = its shortcut, in the View
// menu after the others) unless the area added one of its own, and keeps it checked while the workspace is shown; the
// last one is remembered by id (setting ui/workspace). A contextual tab (addContextualTab) stays hidden until the app
// shows it (MainWindow::setContextualTab, AreaServices::setContextualTab): then it comes first in its workspace's tab
// row, in its accent colour, and is the current tab until it is hidden again.
//   layout.addGroup("design.solid", "design.solid.create", tr("Create"));
//   layout.addAction("design.solid.create", action("design.extrude"));
//   layout.addAction("design.solid.create", action("design.hole"), RibbonLayout::Size::Small, {action("design.thread")});  // split
struct RibbonLayout {
  enum class Size { Large, Small };  // Large: icon above the label; Small: icon beside it, three to a column
  struct Item {
    QAction* action = nullptr;
    Size size = Size::Large;
    QList<QAction*> variants;  // a split button: a click runs the action, its arrow drops these down
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
  bool addAction(const QString& group, QAction* action, Size size = Size::Large, const QList<QAction*>& variants = {});  // false: no such group
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
  QString title() const { return m_title; }
  QMenu* menu() const { return m_menu; }  // every tool of the group: the title's ▾ and the collapsed button drop it
  QList<QToolButton*> buttons() const;    // the tools', one per item, in order (hidden while collapsed)
  QToolButton* titleButton() const { return m_titleButton; }
  QToolButton* collapsedButton() const { return m_collapsed; }
  static constexpr int kTop = 4, kTools = 56, kTitle = 16, kRow = 18, kHeight = kTop + kTools + kTitle + 4;
 signals:
  void widthsChanged();
 private:
  struct Slot {
    QAction* action;
    RibbonLayout::Size size;
    QToolButton* button;
  };
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
  // Steps groups down until the row fits: the rightmost of those least stepped down goes first, to its next narrower
  // level; a group with nothing narrower left stays as it is.
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
  void setSelectFilters(const QList<QAction*>& filters, const QStringList& hints);
  void setSearchAction(QAction* a);
  void setSettingsAction(QAction* a);
  void setSettingsMenu(QAction* a, QMenu* menu);
  void setCurrentTab(int index) { m_tabs->setCurrentIndex(index); }
  int currentTab() const { return m_tabs->currentIndex(); }
  QStringList tabIds() const;  // the current workspace's tabs as the row shows them
  RibbonPage* page(const QString& tabId) const;
  RibbonPage* currentPage() const;
  QTabBar* tabBar() const { return m_tabs; }

 signals:
  void workspaceChanged(int index);

 private:
  void showWorkspaceMenu();
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
  QStackedWidget* m_stack;
  QWidget* m_strip;
  QHBoxLayout* m_stripLayout;
  QHBoxLayout* m_right;
};
