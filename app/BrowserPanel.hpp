#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "AppDocument.hpp"
#include "BrowserDelegate.hpp"

// ---------------------------------------------------------------- browser
class BrowserTree : public QTreeWidget {
  Q_OBJECT
 public:
  explicit BrowserTree(AppDocument* doc, QWidget* parent = nullptr);
  using QTreeWidget::indexFromItem;  // BrowserPanel selects in one batch through the selection model
  void openMenu();  // the context menu of the current row, beside it (the Menu key, Shift+F10)
  QWidget* renameEditor() const;  // the row editor while a name is being edited, else null
 signals:
  void reparentRequested(const std::vector<std::string>& ids, const std::string& parent, int index);
  void eyeClicked(const std::string& id);
  void swatchClicked(const std::string& id);
  // The keyboard (UI-124): Space shows or hides the selected rows, Enter is a double-click on the current one, F2 and Del
  // run the window's Rename and Delete on the selection (the browser is another window: its keys may not reach them).
  void visibilityKey();
  void rowActivated(QTreeWidgetItem* item);
  void commandRequested(const QString& id);
 protected:
  bool event(QEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void startDrag(Qt::DropActions actions) override;  // not with a read-only row (Decoration::readOnly)
  void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override;
 private:
  std::function<void()> badgeClick(const QPoint& pos) const;  // a decorator's clickable badge under pos
  AppDocument* m_doc;
  qint64 m_menuAt = 0;  // when the keyboard last opened the menu: the Menu key comes as a key and as a context menu event
};

class BrowserPanel : public QWidget {
  Q_OBJECT
 public:
  explicit BrowserPanel(AppDocument* doc, QWidget* parent = nullptr);
  std::vector<std::string> selectedIds() const;
  bool isProvided(const std::string& id) const;  // a row of an area's folder (addFolder), not a node or a sketch
  QString rowName(const std::string& id) const;  // as shown
  // reveal: rows in closed components are opened and the first scrolled into view (a pick in the view); the same selection
  // again changes nothing (the view stays where the user scrolled).
  void setSelectedIds(const std::vector<std::string>& ids, bool reveal = true);
  void startRename(const std::string& id);  // the row's name editor, with the keyboard (its window activated)
  QWidget* renameEditor() const { return m_tree->renameEditor(); }  // null unless a name is being edited
  void focusFilter();
  void selectIds(const std::vector<std::string>& ids);  // like a click: selects and emits selectionChanged
  void selectParent();  // replaces the selection by its parents
  void expandAll();
  void collapseAll();  // everything but the document row
  void scrollToSelected();
  void setViewerMode(bool on);  // no rename or drag-to-reparent (colours stay: a view setting)
  void setEditedSketch(const std::string& id,const QString& name,bool visible);
  void toggleVisibility(const std::vector<std::string>& ids);  // one step: hidden when any of them is shown, else shown
  // Feature areas (BrowserDelegate.hpp): badges, icons and name styles per row, and top-level folders of their own.
  void addDecorator(browser::Decorator decorator);  // repaints
  void addFolder(browser::Folder folder);           // after the ones added before; rebuilds
  void refreshDecorations();                        // repaints: a decorator's answer changed (a folder's items: rebuild())
  bool removeRows(const std::vector<std::string>& ids);  // Del on provided rows: their folders' remove; false when none took them
  BrowserTree* tree() const { return m_tree; }      // benches

 signals:
  void selectionChanged(const std::vector<std::string>& ids);
  void contextMenuRequested(const QPoint& globalPos, const std::vector<std::string>& ids);
  void documentMenuRequested(const QPoint& globalPos);  // the document row: the menu is about the document (SelectionContext::document)
  void fitRequested(const std::vector<std::string>& ids);
  void autoHideChanged(bool on);
  void decorationsChanged();  // refreshDecorations: rows look otherwise now (the collapsed browser's picture follows)
  void sketchActivated(const std::string& sketchId);  // double-click on a sketch row: edit it
  void editedSketchVisibilityRequested();
  void commandRequested(const QString& id);  // the window's command (edit.rename, edit.delete) on the selection

 public slots:
  void rebuild();  // every row made again; the scroll position, the open rows and the current row stay

 private:
  void activate(QTreeWidgetItem* it);  // a double-click or Enter: fit the object, edit the sketch, a folder's own action
  void documentChanged();  // AppDocument::changed: rows updated in place when only looks, names or places changed, else rebuild()
  bool updateRows(const std::vector<std::string>& ids);  // those rows in place; false when the tree's shape changed
  // Where the view is: the row at its top (by rowKey) and how far it is scrolled past, the scroll bars' values.
  struct ViewState {
    std::string anchor;
    int anchorTop = 0, vertical = 0, horizontal = 0;
  };
  ViewState viewState() const;
  void restoreView(const ViewState& state);
  std::string rowKey(const QTreeWidgetItem* item) const;  // a row's identity across rebuilds (node, sketch, folder, document)
  QTreeWidgetItem* itemForKey(const std::string& key) const;
  // A provided folder's rows (items) under `folder`; `expanded` and `known` keep rows open across rebuilds, a new one opens.
  void fillFolder(QTreeWidgetItem* folder, const browser::Folder& f, const std::vector<browser::Item>& items, const std::set<std::string>& expanded,
                  const std::set<std::string>& known);
  void restoreCurrent(const std::string& id, const QString& kind, const QString& folder);  // the keyboard's row after rows were made again
  static QString stateText(const opad::Node& n);  // what a node row's eye, lock and kind say, for screen readers
  void applyFilter();
  void updateBreadcrumb();
  QTreeWidgetItem* itemFor(const std::string& id) const;
  QTreeWidgetItem* build(const std::string& id, QTreeWidgetItem* parent, std::set<std::string>& expanded);
  const browser::Folder* providedFolder(const QTreeWidgetItem* item) const;  // the provided folder of a folder or provided row

  AppDocument* m_doc;
  BrowserTree* m_tree;
  QLineEdit* m_filter;
  QLabel* m_breadcrumb;
  QToolButton *m_parentBtn, *m_locateBtn, *m_expandBtn, *m_collapseBtn;
  QLabel* m_empty;
  std::unordered_map<std::string, QTreeWidgetItem*> m_index;  // node id -> item, rebuilt with the tree
  bool m_updating = false;
  unsigned long long m_syncedRevision = 0, m_syncedGeneration = 0;  // the document the rows show: the next change may be partial
  bool m_viewer = false;
  std::string m_editedSketch;
  QString m_editedName;
  bool m_editedVisible=true;
  std::vector<browser::Folder> m_folders;  // provided (addFolder)
};
