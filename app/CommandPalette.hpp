#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QList>
#include <QListWidget>

class QAction;

// ---------------------------------------------------------------- command search (F30)
class CommandPalette : public QDialog {
  Q_OBJECT
 public:
  CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr);
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  void refill(const QString& filter);
  void runCurrent();
  QList<QAction*> m_actions;
  QLineEdit* m_edit;
  QListWidget* m_list;
};

QString opGroup(const QAction* a);
