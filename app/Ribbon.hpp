#pragma once
// Tabbed ribbon (28 px tab row + 64 px tool strip) with the right-hand cluster: Select segmented control,
// "Search commands" field and settings, per the design handoff.
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

class RibbonBar : public QWidget {
  Q_OBJECT
 public:
  explicit RibbonBar(QWidget* parent = nullptr);
  int addTab(const QString& title, const QList<QList<QAction*>>& groups);
  void setSelectFilters(const QList<QAction*>& filters, const QStringList& hints);
  void setSearchAction(QAction* a);
  void setSettingsAction(QAction* a);
  void setSettingsMenu(QAction* a, QMenu* menu);
  void setCurrentTab(int index) { m_tabs->setCurrentIndex(index); }
  int currentTab() const { return m_tabs->currentIndex(); }

 private:
  QTabBar* m_tabs;
  QStackedWidget* m_stack;
  QWidget* m_strip;
  QHBoxLayout* m_stripLayout;
  QHBoxLayout* m_right;
};
