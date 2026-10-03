#pragma once
// Toasts (UI-120 d): short results and warnings at the bottom centre of the viewport that go away by themselves, with at
// most one action ("Undo", "Open folder", "Show") whose callback runs when it is clicked. Each toast is a native child of
// the viewport (over the OCCT surface like the chips row) or of the page an area shows in its place, not a top-level
// window: it moves and hides with the view and never takes the focus. The newest is lowest; at most kMax show, the oldest goes first. Hovering one holds its timer.
// Colours come from the theme's stylesheet (QFrame#toast), so a theme switch restyles the ones showing; a right-to-left
// UI mirrors them (text right, action and close left).
//   m_toasts->toast(text, undoLabel, [this] { m_doc->undo(); });  // text, undoLabel: translated
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
  Toast(const QString& text, const QString& actionText, std::function<void()> callback, int ms, QWidget* host);
  QString text() const;
  QToolButton* actionButton() const { return m_action; }  // null without an action
  QToolButton* closeButton() const { return m_close; }
  int timeout() const { return m_ms; }
  int naturalWidth();  // the text on one line, with the buttons and margins
  void dismiss();  // hides it now (the stack closes the gap) and deletes it later
 signals:
  void dismissed(Toast* toast);
 protected:
  void resizeEvent(QResizeEvent*) override;  // rounded by a mask: a native child cannot be translucent
 private:
  void refreshIcons();
  QLabel* m_label;
  QToolButton* m_action = nullptr;
  QToolButton* m_close;
  QTimer m_timer;
  int m_ms;
  std::function<void()> m_callback;
};

class ToastStack : public QObject {
  Q_OBJECT
 public:
  explicit ToastStack(QWidget* host);  // the viewport; its resizes re-place the toasts
  // Shows text (and an action button when actionText is set) for ms milliseconds; 0 keeps it until it is closed or its
  // action is clicked. The callback runs on the UI thread, after which the toast goes.
  Toast* toast(const QString& text, const QString& actionText = QString(), std::function<void()> callback = {}, int ms = 4000);
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
};
