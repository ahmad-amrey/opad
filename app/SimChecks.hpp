#pragma once
// Simulate's checks: each simulation OPAD has, asked as a plain question a non-engineer recognises ("Will it bend or break
// under a load?"), under a goal (Air and fluids, Heat, Strength, Movement), with a line of what it is for, how long it takes,
// the study kind and loads it sets up and the solver it needs. The start page shows them as cards (a picture each,
// SimPictures), the goal tabs as buttons; a check opens its guided panel (SimulateCheck). Pure data and Qt text: no OCCT,
// so the help test links it too.
#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>

#include "opad/json.hpp"

struct Tokens;

namespace simchecks {

enum class Goal { Air, Heat, Strength, Movement };

struct Check {
  QString id;        // "bend": its command is "simulate.check.bend", its picture "bend"
  Goal goal;
  QString question;  // "Will it bend or break under a load?"
  QString line;      // what it is for, one line
  QString time;      // "Under 1 min"
  QString kind;      // the study kind it makes: static, modal, thermal, motion, dynamic, flow
  QString solver;    // what it runs on: "calculix", "openfoam", "chrono" or "" (built in)
  QStringList words;  // what people type for it ("bracket", "shelf", "duct", "CFM")
  QString command() const { return "simulate.check." + id; }
};
const QList<Check>& all();
const Check* find(const QString& id);
QList<const Check*> ofGoal(Goal goal);

QString goalName(Goal goal);        // "Air and fluids"
QString goalId(Goal goal);          // "air", "heat", "strength", "movement"
QString goalTab(Goal goal);         // its ribbon tab: "simulate.goal.air"
QColor goalColor(Goal goal, const Tokens& t);
bool goalOf(const QString& tabOrId, Goal* goal);  // "simulate.goal.heat" or "heat"

// A solver a check runs on: what it is, where it comes from, how big it is, what uses it.
struct Solver {
  QString id, name;  // "openfoam", "OpenFOAM"
  QString what;      // one line for someone who never heard of it
  QString from;      // its official home, said in words
  QString url;       // its official download page (opened in the browser, never fetched)
  QString size;      // the download, roughly
  QString usedFor;
  bool external = true;  // false: part of OPAD's build (Chrono, Netgen): a build without it cannot get it
  QString envVar;        // where OPAD looks first once found by hand (OPAD_OPENFOAM, OPAD_CCX)
  bool folder = false;   // found by hand as a folder (OpenFOAM's) rather than a program (ccx.exe)
};
const Solver* solver(const QString& id);
QList<Solver> solvers();

// Whether a check can run here, from sim::engines() (read on a worker) and the study kinds this build has.
struct Readiness {
  enum State { Ready, NeedsSolver, Coming } state = Ready;
  QString chip;    // "Ready", "Needs the airflow solver", "Coming"
  QString solver;  // the one missing
};
Readiness readiness(const Check& check, const opad::json& engines);

// How well `query` (what the user typed) matches the check: 0 none, higher better; an empty query 1. Each word that is
// one of its keywords counts 3, one of its question or line 2 (a prefix counts: "narrow" finds "narrowing"); little
// words ("my", "the", "how") are skipped.
int match(const Check& check, const QString& query);

}  // namespace simchecks
