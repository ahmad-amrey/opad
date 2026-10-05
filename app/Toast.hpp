#pragma once
// Toasts (UI-120 d): short results and warnings at the bottom centre of the viewport that go away by themselves, with at
// most one action ("Undo", "Open folder", "Show") whose callback runs when it is clicked. Each toast is a native child of
// the viewport (over the OCCT surface like the chips row) or of the page an area shows in its place, not a top-level
// window: it moves and hides with the view and never takes the focus. The newest is lowest; at most kMax show, the oldest goes
// first (a question only when nothing else is left to go: other toasts never push it out unanswered). Hovering one holds its timer.
// Colours come from the theme's stylesheet (QFrame#toast), so a theme switch restyles the ones showing; a right-to-left
// UI mirrors them (text right, action and close left).
//   m_toasts->toast(text, undoLabel, [this] { m_doc->undo(); });  // text, undoLabel: translated
// A question (ToastStack::ask) has a line of detail under its text and its answers on a row below, at the end.
#include <QFrame>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <functional>

class QLabel;
class QToolButton;

class Toast : public QFrame {
  Q_OBJECT
 public:
  struct Action {
    QString text;  // translated
    std::function<void()> callback;
  };
  Toast(const QString& text, const QString& actionText, std::function<void()> callback, int ms, QWidget* host);
  Toast(const QString& text, const QString& detail, const QList<Action>& actions, int ms, QWidget* host);
  QString text() const;
  QString detail() const;
  QToolButton* actionButton(int i = 0) const { return m_actions.value(i); }  // null without that action
  QToolButton* closeButton() const { return m_close; }
  int timeout() const { return m_ms; }
  int naturalWidth();  // the text on one line, with the buttons and margins
  void setWrapped(bool on);  // the text (and detail) wrapped at the toast's width
  void dismiss();  // hides it now (the stack closes the gap) and deletes it later
  bool isQuestion() const { return m_question; }
 signals:
  void dismissed(Toast* toast);  // any way it went: an action, ×, its timeout, the stack
  void closed();  // × clicked (before dismissed): the user put it away
 protected:
  void resizeEvent(QResizeEvent*) override;  // rounded by a mask: a native child cannot be translucent
 private:
  void refreshIcons();
  QLabel* m_label;
  QLabel* m_detail = nullptr;
  QList<QToolButton*> m_actions;
  QToolButton* m_close;
  QTimer m_timer;
  int m_ms;
  bool m_stacked = false;  // text, detail and answers in rows (a question)
  bool m_question = false;  // made by ToastStack::ask
  friend class ToastStack;
};

class ToastStack : public QObject {
  Q_OBJECT
 public:
  explicit ToastStack(QWidget* host);  // the viewport; its resizes re-place the toasts
  // Shows text (and an action button when actionText is set) for ms milliseconds; 0 keeps it until it is closed or its
  // action is clicked. The callback runs on the UI thread, after which the toast goes.
  Toast* toast(const QString& text, const QString& actionText = QString(), std::function<void()> callback = {}, int ms = 4000);
  // A question: text, a line of detail under it and its answers (each runs its callback, then the toast goes). Closed or
  // timed out it just goes; dismissed() comes either way, closed() only for ×.
  Toast* ask(const QString& text, const QString& detail, const QList<Toast::Action>& answers, int ms = 0);
  QList<Toast*> toasts() const;  // showing, oldest first
  void clear();
  void place();  // bottom centre of the host, newest lowest, 8 px apart
  // The widget they show over: the viewport, or the page an area shows in its place (the sheet canvas); those showing move.
  void setHost(QWidget* host);
  QWidget* host() const { return m_host; }
  static constexpr int kMax = 3, kMargin = 16, kGap = 8, kMaxWidth = 480;
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  QWidget* m_host;
  QList<QPointer<Toast>> m_toasts;
  Toast* add(Toast* t);  // sized, listed, shown and placed
};
