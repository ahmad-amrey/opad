#pragma once
#include <QRect>
#include <QWidget>
#include <set>
#include <string>
#include <vector>

#include "AppDocument.hpp"

class QScrollBar;

// ---------------------------------------------------------------- timeline
class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(AppDocument* doc, QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(400, 48); }
  QSize minimumSizeHint() const override { return QSize(100, 48); }
  void setCurrentOp(const std::string& id);
  std::string currentOp() const { return m_current; }
  // The op an open sketch or feature editor changes: marked, and what follows it dimmed, since the edit applies from
  // that point in the history.
  void setEditingOp(const std::string& id);
  void step(int delta);
  QString describe(const opad::Op& op) const;

 signals:
  void opClicked(const std::string& opId);
  void opActivated(const std::string& opId);  // double-click: edit a feature or a sketch
  void contextRequested(const std::string& opId, const QPoint& globalPos);

 public slots:
  void rebuild();

 protected:
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;

 private:
  void updateScrollRange();
  void ensureCurrentVisible();
  QScrollBar* m_scroll;
  QString iconFor(const opad::Op& op) const;
  int indexAt(const QPoint& p) const;
  QRect markerRect(int i) const;
  bool isUnresolved(const std::string& opId) const;
  AppDocument* m_doc;
  std::set<std::string> m_deleted, m_unresolved;
  std::vector<size_t> m_shown;  // indices into doc.ops drawn as markers (see timelineShows)
  int m_hover = -1;             // marker index (into m_shown)
  std::string m_current, m_editing;
  QRect m_prevBtn, m_nextBtn;
};

QString opTypeIcon(const std::string& type);
