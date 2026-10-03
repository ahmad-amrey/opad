#pragma once
// The one footer of every tool panel (UI-120 c). Leading side: secondary actions ("Copy", "Undo point"), in the order
// added. Trailing side: Cancel, then the primary button, each with its key ("Esc", "Enter"). The primary is accent-styled
// and named by what it does: OK when it commits and closes the panel, Apply when it commits and the panel stays open, or
// a verb of its own ("Place", "Pin to document"). A right-to-left UI mirrors it (secondary right, primary far left).
//
// The Esc ladder, a contract for every tool, panel and overlay. Each Esc does the first of these that applies, and the
// prompt bar or the footer's Cancel says what the next one will do:
//   1. close a popup, or cancel a drag in progress (the widget that owns it);
//   2. cancel the current step: a chain being drawn, a value being typed (the tool);
//   3. clear the picks: take back the last pick of the step that waits (a guided tool goes one step back);
//   4. close the tool: its command is cancelled and nothing is committed;
//   5. close an unpinned panel (MainWindow::closeTopPanel);
//   6. clear the selection.
// A panel's Esc handler (ToolPanel::setEscapeHandler) covers steps 2 to 4, and the footer's cancelled() is connected to the
// same handler, so the key and the button always do the same thing.
#include <QPushButton>
#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;

class PanelFooter : public QWidget {
  Q_OBJECT
 public:
  enum class Primary { Close, Stay };  // OK: commits and closes the panel; Apply: commits, the panel stays open
  explicit PanelFooter(QWidget* parent = nullptr);
  QPushButton* addSecondary(const QString& text, const QString& key = QString());  // leading side, after the ones before
  void setPrimary(Primary kind, const QString& key = QStringLiteral("Enter"));
  void setPrimary(const QString& verb, const QString& key = QStringLiteral("Enter"));
  void setCancel(const QString& text, const QString& key = QStringLiteral("Esc"));  // "Cancel" unless set: "Clear" where Esc clears
  void setPrimaryVisible(bool on);
  void setCancelVisible(bool on);
  void setPrimaryEnabled(bool on);
  // Off: Cancel and the primary never take the focus, so Esc, Enter and the tool's keys stay with the window (guided tools).
  void setKeysStayWithWindow(bool on);
  QPushButton* primary() const { return m_primary; }
  QPushButton* cancel() const { return m_cancel; }
  QString primaryText() const;  // the label without its key
  QString cancelText() const;
  static QString text(QPushButton* button);  // a footer button's label
  static QString key(QPushButton* button);   // and its key
 signals:
  void accepted();
  void cancelled();
 protected:
  void paintEvent(QPaintEvent*) override;  // the divider above it
 private:
  QPushButton* button(const QString& text, const QString& key);
  void relabel(QPushButton* button, const QString& text, const QString& key);
  QHBoxLayout* m_row;
  int m_secondaries = 0;
  QPushButton* m_cancel;
  QPushButton* m_primary;
};
