#pragma once
// Secondary UI: hierarchical browser (F27), properties (F24), annotations (F32), timeline (F28),
// command search (F30) and the shortcut editor (F31).
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTreeWidget>
#include <QWidget>
#include <set>
#include <string>
#include <vector>

#include "AppDocument.hpp"

class BrowserTree : public QTreeWidget {
  Q_OBJECT
 public:
  using QTreeWidget::QTreeWidget;
 signals:
  void reparentRequested(const std::vector<std::string>& ids, const std::string& parent, int index);
 protected:
  void dropEvent(QDropEvent* e) override;
};

class BrowserPanel : public QWidget {
  Q_OBJECT
 public:
  explicit BrowserPanel(AppDocument* doc, QWidget* parent = nullptr);
  std::vector<std::string> selectedIds() const;
  void setSelectedIds(const std::vector<std::string>& ids);
  void startRename(const std::string& id);

 signals:
  void selectionChanged(const std::vector<std::string>& ids);
  void contextMenuRequested(const QPoint& globalPos, const std::vector<std::string>& ids);
  void fitRequested(const std::vector<std::string>& ids);

 public slots:
  void rebuild();

 private:
  void applyFilter();
  void updateBreadcrumb();
  QTreeWidgetItem* itemFor(const std::string& id) const;
  QTreeWidgetItem* build(const std::string& id, QTreeWidgetItem* parent, std::set<std::string>& expanded);

  AppDocument* m_doc;
  BrowserTree* m_tree;
  QLineEdit* m_filter;
  QLabel* m_breadcrumb;
  bool m_updating = false;
};

class PropertiesPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PropertiesPanel(QWidget* parent = nullptr);
  void showJson(const QString& title, const opad::json& j);
  void clear();

 private:
  void add(QTreeWidgetItem* parent, const QString& key, const opad::json& v);
  QLabel* m_title;
  QTreeWidget* m_tree;
};

class AnnotationsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit AnnotationsPanel(AppDocument* doc, QWidget* parent = nullptr);
  std::string currentOpId() const;

 signals:
  void selectNode(const std::string& id);
  void addRequested();
  void resolveRequested(const std::string& opId);

 public slots:
  void rebuild();

 private:
  AppDocument* m_doc;
  QComboBox* m_author;
  QListWidget* m_list;
};

class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(AppDocument* doc, QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(400, 44); }
  QSize minimumSizeHint() const override { return QSize(100, 44); }

 signals:
  void opClicked(const std::string& opId);
  void deleteRequested(const std::string& opId);

 public slots:
  void rebuild();

 protected:
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;

 private:
  int indexAt(const QPoint& p) const;
  double spacing() const;
  AppDocument* m_doc;
  std::set<std::string> m_deleted;
  int m_hover = -1;
};

class CommandPalette : public QDialog {
  Q_OBJECT
 public:
  CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr);

 private:
  void refill(const QString& filter);
  void runCurrent();
  QList<QAction*> m_actions;
  QLineEdit* m_edit;
  QListWidget* m_list;
};

class ShortcutEditor : public QDialog {
  Q_OBJECT
 public:
  ShortcutEditor(const QList<QAction*>& actions, QWidget* parent = nullptr);
  void accept() override;

 private:
  QList<QAction*> m_actions;
  QTreeWidget* m_tree;
};
