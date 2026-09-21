#pragma once
// Tabbed ribbon (28 px tab row + 64 px tool strip) with the right-hand cluster: Select segmented control,
// "Search commands" field and settings, per the design handoff. The tab set belongs to a workspace (Review,
// Design, ...): one document and one timeline, a workspace only changes which tabs and tools the ribbon shows.
// The switcher chip sits left of the tabs and opens the workspace list.
#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QMenu>
#include <QWidget>

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

class RibbonBar : public QWidget {
  Q_OBJECT
 public:
  explicit RibbonBar(QWidget* parent = nullptr);
  int addWorkspace(const Workspace& w);
  int addTab(int workspace, const QString& title, const QList<QList<QAction*>>& groups);
  void setWorkspace(int index);  // swaps the tab set; each workspace remembers its current tab
  int workspace() const { return m_workspace; }
  void setSelectFilters(const QList<QAction*>& filters, const QStringList& hints);
  void setSearchAction(QAction* a);
  void setSettingsAction(QAction* a);
  void setSettingsMenu(QAction* a, QMenu* menu);
  void setCurrentTab(int index) { m_tabs->setCurrentIndex(index); }
  int currentTab() const { return m_tabs->currentIndex(); }

 signals:
  void workspaceChanged(int index);

 private:
  void showWorkspaceMenu();
  struct Tabs {
    QStringList titles;
    QList<int> pages;  // index into m_stack
    int current = 0;
  };
  QList<Workspace> m_workspaces;
  QList<Tabs> m_tabSets;
  int m_workspace = -1;
  WorkspaceChip* m_chip;
  QTabBar* m_tabs;
  QStackedWidget* m_stack;
  QWidget* m_strip;
  QHBoxLayout* m_stripLayout;
  QHBoxLayout* m_right;
};
