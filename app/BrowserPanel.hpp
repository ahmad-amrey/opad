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
 signals:
  void reparentRequested(const std::vector<std::string>& ids, const std::string& parent, int index);
  void eyeClicked(const std::string& id);
  void swatchClicked(const std::string& id);
 protected:
  void dropEvent(QDropEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const override;
 private:
  std::function<void()> badgeClick(const QPoint& pos) const;  // a decorator's clickable badge under pos
  AppDocument* m_doc;
};

class BrowserPanel : public QWidget {
  Q_OBJECT
 public:
  explicit BrowserPanel(AppDocument* doc, QWidget* parent = nullptr);
  std::vector<std::string> selectedIds() const;
  void setSelectedIds(const std::vector<std::string>& ids);
  void startRename(const std::string& id);
  void focusFilter();
  void selectIds(const std::vector<std::string>& ids);  // like a click: selects and emits selectionChanged
  void selectParent();  // replaces the selection by its parents
  void expandAll();
  void collapseAll();  // everything but the document row
  void scrollToSelected();
  void setViewerMode(bool on);  // no rename or drag-to-reparent (colours stay: a view setting)
  void setEditedSketch(const std::string& id,const QString& name,bool visible);
  // Feature areas (BrowserDelegate.hpp): badges, icons and name styles per row, and top-level folders of their own.
  void addDecorator(browser::Decorator decorator);  // repaints
  void addFolder(browser::Folder folder);           // after the ones added before; rebuilds
  void refreshDecorations();                        // repaints: a decorator's answer changed (a folder's items: rebuild())
  BrowserTree* tree() const { return m_tree; }      // benches

 signals:
  void selectionChanged(const std::vector<std::string>& ids);
  void contextMenuRequested(const QPoint& globalPos, const std::vector<std::string>& ids);
  void fitRequested(const std::vector<std::string>& ids);
  void autoHideChanged(bool on);
  void sketchActivated(const std::string& sketchId);  // double-click on a sketch row: edit it
  void editedSketchVisibilityRequested();

 public slots:
  void rebuild();

 private:
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
  bool m_viewer = false;
  std::string m_editedSketch;
  QString m_editedName;
  bool m_editedVisible=true;
  std::vector<browser::Folder> m_folders;  // provided (addFolder)
};
