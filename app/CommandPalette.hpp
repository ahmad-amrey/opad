#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QList>
#include <QListWidget>

class QAction;
class QLabel;
class CommandPreview;

// ---------------------------------------------------------------- command search (F30)
// Rows: icon, name (the letters found in bold), the help's one-line summary (for a command not available now, what it
// needs, in amber), its group ("Recent" for a command used lately) and its key now (keys::). A query that is a key
// ("ctrl+alt+f") finds the command that has it. With nothing typed the recent commands
// come first; Enter on a command not available now keeps the palette open and says why. Beside the list, the current
// command's card and clip (UI-107).
class CommandPalette : public QDialog {
  Q_OBJECT
 public:
  CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr);
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  void refill(const QString& filter);
  void runCurrent();
  void setFoot(const QString& text, bool warning);
  QList<QAction*> m_actions;
  QLineEdit* m_edit;
  QListWidget* m_list;
  QLabel* m_foot;
  CommandPreview* m_preview;  // the current command's card (UI-107)
};

QString opGroup(const QAction* a);

namespace palette {
constexpr int kRecent = 8;
// The recent command ids after `id` ran: it first, once, at most `keep` (UI-106).
QStringList remember(QStringList recent, const QString& id, int keep = kRecent);
QStringList recent();             // setting palette/recent, newest first
void noteRun(const QString& id);  // a command ran (from the palette, a button, a menu or its key)
}  // namespace palette
