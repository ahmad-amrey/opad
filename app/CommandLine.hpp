#pragma once
// The sketch's command line (TODO 11 UI-133): a strip at the bottom left of the view while a sketch is open, for drafting by
// the keyboard. A word runs a tool or an editing command (SketchCommands.hpp: the tool's name, its id or a short form: L, C,
// REC, TR, O, M ...); anything else is typed into the step's value boxes as keys over the view are (SketchEditor::enter:
// x,y, @dx,dy, @len<ang, a bare length or size), then Enter. The prompt is the prompt bar's step; the lines above it are
// what was entered and what the sketch said back. Up and Down recall earlier entries, Enter on an empty line ends the step
// (repeats the last command when no tool runs), Esc clears the line, then is the sketch's Esc. A native child of the view
// (over the OCCT surface, as the toasts are), rounded by a mask.
#include <QFrame>
#include <QPointer>
#include <QStringList>

class QLabel;
class QLineEdit;
class QToolButton;

class CommandLine : public QFrame {
  Q_OBJECT
 public:
  explicit CommandLine(QWidget* view);
  QLineEdit* edit() const { return m_edit; }
  void setPrompt(const QString& prompt, const QString& hints);  // the step that waits, and what the keys do now
  QString prompt() const;
  void log(const QString& line, bool problem = false);  // a line above the input (the last kLines show)
  QStringList lines() const { return m_lines; }
  QStringList history() const { return m_history; }  // what was entered, oldest first
  void remember(const QString& entry);
  void focusLine();  // takes the keyboard (shown first)
  void place();      // bottom left of the view
  static constexpr int kLines = 3, kMargin = 8, kMaxWidth = 620;
 signals:
  void entered(const QString& text);  // Enter (empty: Enter on an empty line)
  void escaped();                     // Esc on an empty line
  void undoPoint();                   // Backspace on an empty line
  void closeRequested();              // its close button
 protected:
  bool eventFilter(QObject* target, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
 private:
  void restyle();
  void layoutLines();
  void showMatch();
  QWidget* m_view;
  QLabel* m_prompt;
  QLabel* m_match;
  QLabel* m_log;
  QLineEdit* m_edit;
  QToolButton* m_close;
  QStringList m_lines, m_problems, m_history;
  int m_recall = 0;     // the history entry Up/Down show (m_history.size(): the line being typed)
  QString m_draft;      // the line being typed while Up/Down show older entries
};
