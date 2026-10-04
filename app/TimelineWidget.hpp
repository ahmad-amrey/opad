#pragma once
#include <QRect>
#include <QWidget>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "AppDocument.hpp"
#include "Theme.hpp"

class QScrollBar;

// ---------------------------------------------------------------- timeline
// One marker per op, left to right. Markers are icons, or icons with names (setShowNames); the design history alone hides
// renames, colours, views and sections (setDesignOnly). The playhead after the last marker is the roll-back marker: drawn
// where the scene stops (AppDocument::rollback, an editor's or the user's), dragged (or moved with Shift and the arrows,
// Home and End) it asks for a roll-back (rollbackRequested). Ctrl+C copies the current marker's op id.
class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(AppDocument* doc, QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(400, height()); }  // 48 px at the usual text size (theme::px)
  QSize minimumSizeHint() const override { return QSize(100, height()); }
  void setCurrentOp(const std::string& id);
  std::string currentOp() const { return m_current; }
  // The op an open sketch or feature editor changes: marked, and what follows it dimmed, since the edit applies from
  // that point in the history.
  void setEditingOp(const std::string& id);
  // While a component is active (UI-33): the markers of these ops, which do not touch it, are dimmed, or left out when
  // `hidden` (the op being edited stays). Empty: none.
  void setDimmedOps(std::set<std::string> ops, bool hidden = false);
  const std::set<std::string>& dimmedOps() const { return m_dimmed; }
  // Ops drawn with a bar of a theme colour under their marker (Compare: what the other version has not, or has otherwise,
  // UI-58), with the colour's state named in the tooltip. Empty: none.
  void setMarkedOps(std::map<std::string, QColor Tokens::*> marks, const QString& legend = QString());
  const std::map<std::string, QColor Tokens::*>& markedOps() const { return m_marks; }
  void step(int delta);
  std::vector<std::string> shownOps() const;  // the ops drawn as markers, in order (benches)
  QString describe(const opad::Op& op) const;
  QString label(const opad::Op& op) const;  // the name a marker carries (its feature's, its file's)
  QString icon(const opad::Op& op) const { return iconFor(op); }
  // Whether an op gets a marker: every step but view state (visibility, notes, measurements); with `designOnly` only what
  // makes and places geometry.
  static bool shows(const opad::Document& doc, const opad::Op& op, bool designOnly);
  // Points at an op's marker (smart selection's hover, Find in timeline): scrolled into view, a candidate-amber ring that
  // pulses for about a second and a half. Empty: stops.
  void pulse(const std::string& id);
  std::string pulsing() const { return m_pulse; }
  void setShowNames(bool on);  // markers carry their op's name (setting timeline/names, kept by the caller)
  bool showNames() const { return m_names; }
  void setDesignOnly(bool on);  // the design history only: imports, sketches, features, moves, reparents and their deletes
  bool designOnly() const { return m_designOnly; }
  QRect markerAt(const std::string& id) const;  // where an op's marker is drawn (empty when it is not shown)
  QRect playhead() const;                       // the roll-back marker's grip (where it is drawn while dragged or moved by keys)
  bool playheadMoving() const { return m_dragging || m_keyed; }
  // The op the scene stops before for a playhead dropped after this op's marker (Roll back to here); empty: the end.
  std::string rollPointAfter(const std::string& id) const;
  // More lines for a marker's tooltip, from feature areas (an import's source file, who committed it): HTML, appended in
  // the order added; asked on every hover move, so O(1) from what the area knows. tooltip() is the whole of it (benches).
  using TipProvider = std::function<QString(const opad::Op& op)>;
  void addTipProvider(TipProvider provider) { m_tips.push_back(std::move(provider)); }
  QString tooltip(const std::string& opId) const;
  // The markers one by one, for screen readers (UI-124): how many, each one's op, place in the widget and state in words
  // (tombstoned, suppressed, failed, ...; empty when none), and the current one (-1: none).
  int markerCount() const { return int(m_shown.size()); }
  const opad::Op* markerOp(int i) const;
  QRect markerGeometry(int i) const { return markerRect(i); }
  QRect markerArea() const;  // where markers show (the rest scrolled out)
  QString markerState(int i) const;
  int currentMarker() const;
  void openMenu();  // the current marker's menu below it (the Menu key, Shift+F10)

 signals:
  void opClicked(const std::string& opId);
  void opActivated(const std::string& opId);  // double-click, Enter or F2: edit a feature or a sketch
  void contextRequested(const std::string& opId, const QPoint& globalPos);  // empty op: on no marker
  void markerHovered(const std::string& opId);      // the pointer came onto a marker; empty: off every marker
  void rollbackRequested(const std::string& opId);  // the playhead was dropped before this op; empty: at the end
  void suppressRequested(const std::string& opId);  // Space on a feature
  void deleteRequested(const std::string& opId, bool restore);  // Del (tombstone), Shift+Del (restore)

 public slots:
  void rebuild();

 protected:
  bool event(QEvent* e) override;  // Ctrl+C, Shift with the arrows and F2 are the timeline's, not the window's
  void paintEvent(QPaintEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;

 private:
  void layoutMarkers();
  void announce();  // screen readers: the current marker has the focus
  int markersLeft() const;
  int markersRight() const;
  int markerTop() const;
  void updateScrollRange();
  void ensureVisible(size_t index);
  void ensureCurrentVisible();
  void setHover(int index);
  static bool playheadKey(int key);
  void nudgePlayhead(int key);
  void dropPlayhead(size_t gap);  // the playhead let go in this gap: asks for that roll-back when it moved
  QScrollBar* m_scroll;
  QString iconFor(const opad::Op& op) const;
  int indexAt(const QPoint& p) const;
  QRect markerRect(int i) const;
  bool isUnresolved(const std::string& opId) const;
  std::string rollPoint(size_t opIndex) const;  // the first op replay applies at or after doc.ops[opIndex]; empty: none
  size_t rollbackGap() const;                   // where the playhead is: before marker n, m_shown.size() at the end
  int gapX(size_t gap) const;
  AppDocument* m_doc;
  std::set<std::string> m_deleted, m_unresolved, m_dimmed;
  bool m_hideDimmed = false;
  std::vector<size_t> m_shown;  // indices into doc.ops drawn as markers (see timelineShows)
  std::vector<int> m_left, m_width;  // per marker, in the strip (scrolled by m_scroll)
  int m_extent = 0;
  int m_hover = -1;             // marker index (into m_shown)
  std::string m_current, m_editing;
  std::map<std::string, QColor Tokens::*> m_marks;
  QString m_markLegend;
  std::string m_pulse;
  int m_pulseTick = 0;
  class QTimer* m_pulseTimer = nullptr;
  QRect m_prevBtn, m_nextBtn;
  bool m_names = false, m_designOnly = false;
  bool m_dragging = false;  // the playhead, by the mouse
  bool m_keyed = false;     // the playhead, by keys: drawn at m_dragGap until m_keyTimer drops it there
  class QTimer* m_keyTimer = nullptr;
  size_t m_dragGap = 0;
  std::vector<TipProvider> m_tips;
  qint64 m_menuAt = 0;  // when the keyboard last opened the menu: the Menu key comes as a key and as a context menu event
};

QString opTypeIcon(const std::string& type);
