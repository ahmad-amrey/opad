#pragma once
// Rich hover card for commands (UI-106, design notes B §3). Hover an attached widget for 450 ms: a compact card (icon,
// title, key caps, summary, and in amber what a disabled command needs). Keep hovering for ui/tipExpandMs (1200) more:
// it grows (120 ms) to show the details and the animated clip slot (UI-107 plugs the player in with setClipFactory).
// Shift or F1 while hovering shows the expanded card at once. Moving to another attached widget while a card is up
// swaps it at once ("browse mode", 300 ms grace); a press, a key, a wheel, a drag, leaving or deactivating hides it.
// The pointer may rest on the card to read it. One top-level ToolTip window, never focused; painted from the theme
// tokens on every paint and mirrored for right-to-left languages. Qt's own tooltip is held back on attached widgets.
// Setting ui/tips: 0 off (Qt tooltip), 1 basic (Qt tooltip), 2 rich (default).
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

class QAction;
class QVariantAnimation;
struct CommandHelp;

class RichTip : public QWidget {
  Q_OBJECT
 public:
  enum class State { Hidden, Compact, Expanded };
  // Hover help for `w` from the CommandHelp record `commandId` (a ribbon button, a status toggle, a panel button).
  // Works on disabled widgets too: the filter is application-wide.
  static void attach(QWidget* w, const QString& commandId);
  static void detach(QWidget* w);
  static RichTip* instance();
  // The QAction of a command id (MainWindow::action): its icon, live shortcut and enabled state. A QToolButton's
  // default action is used without it.
  static void setActionLookup(std::function<QAction*(const QString&)> lookup);
  // UI-107: the expanded card's clip, made for the record's clip id (nullptr: no slot); `has` says which ids have one
  // (all when not given), so a command without a clip keeps the narrower card.
  using ClipFactory = std::function<QWidget*(const QString& clip, QWidget* parent)>;
  static void setClipFactory(ClipFactory factory, std::function<bool(const QString&)> has = {});
  static constexpr int kMargin = 6;    // translucent rim for the shadow, as ToolPanel
  static constexpr QSize kClip{288, 162};

  State state() const { return m_state; }
  QString commandId() const { return m_id; }
  QWidget* target() const { return m_target; }
  bool showsRequirement() const { return !m_requirement.isEmpty(); }
  QWidget* clip() const { return m_clip; }
  // At once, for a target (F1 help, benches); `state` Hidden hides.
  void showFor(QWidget* target, State state);
  void hideTip();

 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
  void paintEvent(QPaintEvent*) override;

 private:
  explicit RichTip(QWidget* owner);
  QWidget* attachedAt(QObject* o) const;
  void hover(QWidget* target);
  void dismiss();
  bool hovering() const { return m_state != State::Hidden || (m_target && m_suppressed != m_target); }  // Shift and F1 count
  void present(State state);
  void relayout(State state);
  void place(bool animate);
  QAction* actionFor() const;
  static int mode();

  QHash<QWidget*, QString> m_attached;
  QPointer<QWidget> m_target, m_suppressed, m_clip;
  QString m_id;
  State m_state = State::Hidden;
  QTimer m_show, m_expand, m_hide;
  QElapsedTimer m_lastShown;  // browse mode: a card hidden less than 300 ms ago comes back at once
  QPointer<QVariantAnimation> m_grow;
  // Laid out by relayout(), in left-to-right coordinates (painted mirrored in RTL).
  QString m_title, m_summary, m_details, m_requirement, m_hint, m_icon;
  QStringList m_keys;
  QRect m_iconRect, m_titleRect, m_summaryRect, m_detailsRect, m_clipRect, m_requirementRect, m_hintRect;
  QList<QRect> m_keyRects;
  bool m_below = true;
  bool m_expandable = false;  // details or a clip: the compact card grows after a while
  QRect m_anchor;  // the target, global
  // Mouse events reach the filter once per widget they propagate through: only the first one counts.
  QPointer<QObject> m_lastReceiver;
  quint64 m_lastStamp = 0;
  QPointF m_lastGlobal;
};
