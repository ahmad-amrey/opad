#pragma once
// The Version control panel (UI-62), the content of the "version" ToolPanel: the branch and how it stands against its
// remote, the document's state (unsaved, not committed, in conflict, committed), a merge that stopped (Abort merge, Commit
// the merge), Pull / Push / New branch…, and two pages: History (the commits of the document, newest first: Compare,
// Restore…, Branch from here…, Open read-only, Copy hash) and Branches (local and remote: Switch, Merge into current…,
// Delete…). Outside a repository it offers Set up repository…, Clone repository…, Locate git… or Trust this folder…. The
// footer's primary is Commit…. VersionControl owns the lists and runs what the panel asks.
#include <QWidget>

#include "Git.hpp"

class PanelFooter;
class QLabel;
class QMenu;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class VersionControl;

class VersionPanel : public QWidget {
  Q_OBJECT
 public:
  explicit VersionPanel(VersionControl* vc, QWidget* parent = nullptr);
  void showState();  // the labels and buttons, from the repository and the document as they are now (cheap)
  void showLists();  // the history and branches as the controller read them last
  void setPage(int page);  // VersionControl::Page
  int page() const;
  static QString ago(const QString& iso);  // "3 h ago"
  // benches
  QTreeWidget* history() const { return m_history; }
  QTreeWidget* branches() const { return m_branches; }
  QLabel* branchLabel() const { return m_branch; }
  QLabel* syncLabel() const { return m_sync; }
  QLabel* documentLabel() const { return m_doc; }
  QLabel* emptyLabel() const { return m_emptyText; }
  QPushButton* button(const QString& action) const;  // by its "action" property
  PanelFooter* footer() const { return m_footer; }
  bool showsRepository() const;  // else the page that offers to set one up
  const git::Commit* commitOf(QTreeWidgetItem* item) const;
  const git::Branch* branchOf(QTreeWidgetItem* item) const;
  QMenu* commitMenu(const git::Commit& commit);  // the history's context menu (actions named vcs.*), for a commit
  QMenu* branchMenu(const git::Branch& branch);
 signals:
  void closeRequested();
 private:
  QPushButton* addButton(QWidget* parent, const QString& action, const QString& text, const QString& icon = {});
  void updateButtons();
  QLabel *m_branch, *m_sync, *m_doc, *m_mergeText, *m_emptyText;
  QWidget* m_mergeBar;
  QStackedWidget *m_stack, *m_pages;
  QToolButton *m_historyTab, *m_branchesTab;
  QTreeWidget *m_history, *m_branches;
  PanelFooter* m_footer;
  VersionControl* m_vc;
};
