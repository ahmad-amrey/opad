#pragma once
// The Simulation guide (simulate.guide: the Simulate ribbon, the Simulation panel, the Help menu): use cases taken step by
// step, from a hinge turned with the slider to a 3D-printed part's weakest layer. Each names the keys the user has now
// (help::expand) and lists the tools it uses as buttons that run them, enabled when they can run.
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <functional>

class QAction;
class QGridLayout;
class QLabel;
class QListWidget;
class QPushButton;

class SimulateGuide : public QWidget {
  Q_OBJECT
 public:
  struct UseCase {
    QString title, goal;
    QStringList steps;
    QStringList commands;  // the tools it uses, in the order the steps use them
    QString tip;
  };
  static QList<UseCase> useCases();  // with the user's keys now

  // `lookup`: a command's QAction (null: none); `run` runs one (in the Simulate workspace).
  SimulateGuide(std::function<QAction*(const QString&)> lookup, std::function<void(const QString&)> run, QWidget* parent = nullptr);
  void open(int useCase);  // shown and raised at that use case
  int current() const;
  int count() const;
  QList<QPushButton*> toolButtons() const { return m_buttons; }
  QLabel* steps() const { return m_steps; }

 protected:
  void keyPressEvent(class QKeyEvent* e) override;

 private:
  void refresh();
  std::function<QAction*(const QString&)> m_lookup;
  std::function<void(const QString&)> m_run;
  QList<UseCase> m_cases;
  QListWidget* m_list;
  QLabel *m_title, *m_goal, *m_steps, *m_tip;
  QGridLayout* m_tools;
  QList<QPushButton*> m_buttons;
  QList<QMetaObject::Connection> m_watch;
};
