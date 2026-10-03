#pragma once
#include <QRect>
#include <QWidget>
#include <set>
#include <string>
#include <vector>

#include "AppDocument.hpp"

class QScrollBar;

// ---------------------------------------------------------------- timeline
// One marker per op, left to right. Markers are icons, or icons with names (setShowNames); the design history alone hides
// renames, colours, views and sections (setDesignOnly). The playhead after the last marker is the roll-back marker: drawn
// where the scene stops (AppDocument::rollback, an editor's or the user's), dragged it asks for a roll-back
// (rollbackRequested). Ctrl+C copies the current marker's op id.
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
  QString label(const opad::Op& op) const;  // the name a marker carries (its feature's, its file's)
  // Points at an op's marker (smart selection's hover, Find in timeline): scrolled into view, a candidate-amber ring that
  // pulses for about a second and a half. Empty: stops.
  void pulse(const std::string& id);
  std::string pulsing() const { return m_pulse; }
  void setShowNames(bool on);  // markers carry their op's name (setting timeline/names, kept by the caller)
  bool showNames() const { return m_names; }
  void setDesignOnly(bool on);  // the design history only: imports, sketches, features, moves, reparents and their deletes
  bool designOnly() const { return m_designOnly; }
  size_t markerCount() const { return m_shown.size(); }
  QRect markerAt(const std::string& id) const;  // where an op's marker is drawn (empty when it is not shown)
  QRect playhead() const;                       // the roll-back marker's grip
  // The op the scene stops before for a playhead dropped after this op's marker (Roll back to here); empty: the end.
  std::string rollPointAfter(const std::string& id) const;

 signals:
  void opClicked(const std::string& opId);
  void opActivated(const std::string& opId);  // double-click: edit a feature or a sketch
  void contextRequested(const std::string& opId, const QPoint& globalPos);  // empty op: on no marker
  void markerHovered(const std::string& opId);      // the pointer came onto a marker; empty: off every marker
  void rollbackRequested(const std::string& opId);  // the playhead was dropped before this op; empty: at the end

 public slots:
  void rebuild();

 protected:
  bool event(QEvent* e) override;
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;

 private:
  void layoutMarkers();
  void updateScrollRange();
  void ensureVisible(size_t index);
  void ensureCurrentVisible();
  void setHover(int index);
  QScrollBar* m_scroll;
  QString iconFor(const opad::Op& op) const;
  int indexAt(const QPoint& p) const;
  QRect markerRect(int i) const;
  bool isUnresolved(const std::string& opId) const;
  std::string rollPoint(size_t opIndex) const;  // the first op replay applies at or after doc.ops[opIndex]; empty: none
  size_t rollbackGap() const;                   // where the playhead is: before marker n, m_shown.size() at the end
  int gapX(size_t gap) const;
  AppDocument* m_doc;
  std::set<std::string> m_deleted, m_unresolved;
  std::vector<size_t> m_shown;  // indices into doc.ops drawn as markers (see timelineShows)
  std::vector<int> m_left, m_width;  // per marker, in the strip (scrolled by m_scroll)
  int m_extent = 0;
  int m_hover = -1;             // marker index (into m_shown)
  std::string m_current, m_editing;
  std::string m_pulse;
  int m_pulseTick = 0;
  class QTimer* m_pulseTimer = nullptr;
  QRect m_prevBtn, m_nextBtn;
  bool m_names = false, m_designOnly = false;
  bool m_dragging = false;  // the playhead
  size_t m_dragGap = 0;
};

QString opTypeIcon(const std::string& type);
