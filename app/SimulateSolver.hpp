#pragma once
// The window a check opens when its solver is not on this computer (SimChecks::Solver): what the solver is, where it comes
// from and how big it is, what uses it, and three answers. "Download and install" opens the solver's official download page
// in the browser (OPAD never downloads or runs an installer itself); "I already have it: find it" asks for its folder
// (OpenFOAM) or program (ccx.exe), checks it, and remembers it (QSettings simulate/solvers/<id>, set as the solver's
// variable for this run: OPAD_OPENFOAM, OPAD_CCX); "Later" closes it: the check stays set up and runs once the solver is
// there. A solver that is part of OPAD's build (Project Chrono) says so and points at OPAD's releases.
#include <QDialog>

#include <functional>

class QLabel;

class SolverWindow : public QDialog {
  Q_OBJECT
 public:
  // `found`: after a solver was found by hand (the engines are looked for again).
  SolverWindow(const QString& solver, std::function<void()> found, QWidget* parent);
  QString solver() const { return m_id; }
  QLabel* note() const { return m_note; }

  // The solvers found by hand before: their variables set for this run (from main's start, before the engines are read).
  static void applySaved();
  // A folder or program given for that solver, checked: an empty string when it holds the solver, else why not.
  static QString check(const QString& solver, const QString& path);
  // Remembers it and sets the variable (check first).
  static void remember(const QString& solver, const QString& path);

 private:
  void find();
  QString m_id;
  std::function<void()> m_found;
  QLabel* m_note = nullptr;
};
