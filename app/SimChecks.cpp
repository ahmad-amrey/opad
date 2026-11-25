#include "SimChecks.hpp"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>

#include "Theme.hpp"
#include "opad/sim/joints.hpp"

namespace {
class SimChecksText {
  Q_DECLARE_TR_FUNCTIONS(SimChecksText)
 public:
  static QString t(const char* s) { return tr(s); }
};
}  // namespace

namespace simchecks {

const QList<Check>& all() {
  // Built at first use (after i18n::install): the texts are the user's language.
  static const QList<Check> checks = [] {
    using T = SimChecksText;
    QList<Check> c;
    c.push_back({"fitting", Goal::Air, T::tr("How much pressure does a fitting lose?"),
                 T::tr("Ducts, reducers, elbows, adapters. Shows where the air slows down or swirls."), T::tr("About 3 min"), "flow", "openfoam",
                 {T::tr("duct"), T::tr("pipe"), T::tr("reducer"), T::tr("adapter"), T::tr("elbow"), T::tr("narrow"), T::tr("pressure"), T::tr("loss"),
                  T::tr("air"), T::tr("water"), T::tr("flow"), T::tr("hose"), T::tr("nozzle"), "CFD", "Pa"}});
    c.push_back({"fan", Goal::Air, T::tr("How much air will my fan really push?"),
                 T::tr("A fan with its duct or box. Finds the airflow you actually get, not the label's."), T::tr("About 5 min"), "flow", "openfoam",
                 {T::tr("fan"), T::tr("airflow"), T::tr("blower"), T::tr("duct"), T::tr("vent"), T::tr("extractor"), T::tr("ventilation"), "CFM", "m³/h",
                  "CFD"}});
    c.push_back({"overheat", Goal::Heat, T::tr("Will my electronics box overheat?"),
                 T::tr("Boards in a case, with or without a fan. Shows the hottest part and how warm it gets."), T::tr("1 to 10 min"), "thermal",
                 "calculix",
                 {T::tr("electronics"), T::tr("box"), T::tr("case"), T::tr("enclosure"), T::tr("board"), T::tr("Raspberry Pi"), T::tr("hot"),
                  T::tr("overheat"), T::tr("temperature"), T::tr("vents"), T::tr("cooling"), "PCB"}});
    c.push_back({"heatsink", Goal::Heat, T::tr("Is my heatsink big enough?"),
                 T::tr("A chip, its heatsink and the air around it. Tells you how hot the chip runs."), T::tr("About 1 min"), "thermal", "calculix",
                 {T::tr("heatsink"), T::tr("fins"), T::tr("chip"), T::tr("CPU"), T::tr("LED"), T::tr("cooler"), T::tr("fan"), T::tr("hot"),
                  T::tr("temperature"), T::tr("watts")}});
    c.push_back({"warmup", Goal::Heat, T::tr("How fast does it warm up?"),
                 T::tr("Switch it on and watch the heat spread: how long until a part gets too hot."), T::tr("1 to 5 min"), "thermal", "calculix",
                 {T::tr("warm up"), T::tr("heat up"), T::tr("time"), T::tr("minutes"), T::tr("transient"), T::tr("switch on"), T::tr("hot"),
                  T::tr("temperature")}});
    c.push_back({"bend", Goal::Strength, T::tr("Will it bend or break under a load?"),
                 T::tr("A bracket, a shelf, a hook. Shows the weak spot and how far it is from breaking."), T::tr("Under 1 min"), "static",
                 "calculix",
                 {T::tr("bracket"), T::tr("shelf"), T::tr("hook"), T::tr("weight"), T::tr("load"), T::tr("break"), T::tr("bend"), T::tr("strong"),
                  T::tr("strength"), T::tr("stress"), T::tr("sag"), T::tr("force"), T::tr("hold"), "FEA"}});
    c.push_back({"print", Goal::Strength, T::tr("Will my 3D print hold?"),
                 T::tr("Takes your print settings (layers, walls, infill) into account. Tells you the layer it would break at."),
                 T::tr("Under 1 min"), "static", "calculix",
                 {T::tr("3D print"), T::tr("printed"), T::tr("print"), T::tr("layers"), T::tr("infill"), T::tr("PLA"), T::tr("PETG"), T::tr("slicer"),
                  T::tr("break"), T::tr("strong"), "FDM"}});
    c.push_back({"rattle", Goal::Strength, T::tr("Will it rattle or hum?"),
                 T::tr("Finds the speeds at which a part starts to shake, so a motor or fan stays clear of them."), T::tr("Under 1 min"), "modal",
                 "calculix",
                 {T::tr("rattle"), T::tr("hum"), T::tr("shake"), T::tr("vibration"), T::tr("vibrate"), T::tr("resonance"), T::tr("noise"),
                  T::tr("frequency"), T::tr("motor"), T::tr("fan"), "Hz", "rpm"}});
    c.push_back({"clearance", Goal::Movement, T::tr("Does my mechanism move freely?"),
                 T::tr("Hinges, sliders, gears. Plays the motion and flags any part that hits another."), T::tr("Seconds"), "motion", "",
                 {T::tr("mechanism"), T::tr("hinge"), T::tr("slider"), T::tr("gear"), T::tr("move"), T::tr("moves"), T::tr("hit"), T::tr("collide"),
                  T::tr("collision"), T::tr("clearance"), T::tr("interference"), T::tr("lid"), T::tr("door"), T::tr("swing"), T::tr("rotate")}});
    c.push_back({"forces", Goal::Movement, T::tr("How strong a motor do I need?"),
                 T::tr("With gravity and motors. Shows how much torque a motor needs."), T::tr("Under 1 min"), "dynamic", "chrono",
                 {T::tr("motor"), T::tr("torque"), T::tr("servo"), T::tr("force"), T::tr("gravity"), T::tr("arm"), T::tr("lift"), T::tr("fall"),
                  T::tr("swing"), T::tr("dynamics"), T::tr("power")}});
    return c;
  }();
  return checks;
}

const Check* find(const QString& id) {
  for (const auto& c : all())
    if (c.id == id) return &c;
  return nullptr;
}

QList<const Check*> ofGoal(Goal goal) {
  QList<const Check*> out;
  for (const auto& c : all())
    if (c.goal == goal) out << &c;
  return out;
}

QString goalName(Goal goal) {
  switch (goal) {
    case Goal::Air: return SimChecksText::tr("Air and fluids");
    case Goal::Heat: return SimChecksText::tr("Heat");
    case Goal::Strength: return SimChecksText::tr("Strength");
    case Goal::Movement: return SimChecksText::tr("Movement");
  }
  return {};
}

QString goalId(Goal goal) {
  switch (goal) {
    case Goal::Air: return "air";
    case Goal::Heat: return "heat";
    case Goal::Strength: return "strength";
    case Goal::Movement: return "movement";
  }
  return {};
}

QString goalTab(Goal goal) { return "simulate.goal." + goalId(goal); }

bool goalOf(const QString& tabOrId, Goal* goal) {
  const QString id = tabOrId.startsWith("simulate.goal.") ? tabOrId.mid(14) : tabOrId;
  for (Goal g : {Goal::Air, Goal::Heat, Goal::Strength, Goal::Movement})
    if (goalId(g) == id) {
      if (goal) *goal = g;
      return true;
    }
  return false;
}

QColor goalColor(Goal goal, const Tokens& t) {
  // The tokens' own hues, so a high-contrast theme keeps its system colours.
  switch (goal) {
    case Goal::Air: return t.highContrast ? t.sel : (t.dark ? QColor("#33c3d6") : QColor("#0f7d8c"));
    case Goal::Heat: return t.amber;
    case Goal::Strength: return t.highContrast ? t.fg : (t.dark ? QColor("#9cc3ff") : QColor("#2a5fb0"));
    case Goal::Movement: return t.green;
  }
  return t.fg;
}

QList<Solver> solvers() {
  using T = SimChecksText;
  QList<Solver> s;
  s.push_back({"openfoam", "OpenFOAM", T::tr("A free solver engineers use to work out how air and water move."),
               T::tr("openfoam.com, OpenCFD's official Windows build"), "https://www.openfoam.com/download", T::tr("a few hundred MB"),
               T::tr("every Air and fluids check, and heat checks that solve the air in a box"), true, "OPAD_OPENFOAM", true});
  s.push_back({"calculix", "CalculiX", T::tr("A free solver that works out how parts bend, shake and warm up."),
               T::tr("calculix.de, its official home (the Windows program ccx.exe)"), "http://www.calculix.de/", T::tr("a few MB"),
               T::tr("every Strength and Heat check"), true, "OPAD_CCX", false});
  s.push_back({"chrono", "Project Chrono", T::tr("The physics engine that moves parts under gravity and motors."),
               T::tr("built into OPAD's releases"), "https://github.com/ahmad-amrey/opad/releases/", T::tr("part of OPAD"),
               T::tr("How strong a motor do I need"), false, {}, false});
  return s;
}

const Solver* solver(const QString& id) {
  static const QList<Solver> list = solvers();  // built once, after the language is installed
  for (const auto& s : list)
    if (s.id == id) return &s;
  return nullptr;
}

Readiness readiness(const Check& check, const opad::json& engines) {
  Readiness r;
  const std::string kind = check.kind.toStdString();
  const auto& kinds = opad::sim::study_kinds();
  if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) {  // a study kind still to come in this build
    r.state = Readiness::Coming;
    r.chip = SimChecksText::tr("Coming");
    return r;
  }
  bool ok = true;
  if (kind == "flow") ok = engines.value("flow", engines.value("cfd", false));
  else if (kind != "motion") ok = engines.value(kind, false);
  if (ok) {
    r.chip = SimChecksText::tr("Ready");
    return r;
  }
  r.state = Readiness::NeedsSolver;
  r.solver = check.solver;
  if (check.solver == "openfoam") r.chip = SimChecksText::tr("Needs the airflow solver");
  else if (const Solver* s = solver(check.solver)) r.chip = SimChecksText::tr("Needs %1").arg(s->name);
  else r.chip = SimChecksText::tr("Needs a solver");
  return r;
}

// Words for matching: lower case, split on what is not a letter or digit in any script, the Arabic article dropped.
static QStringList wordsOf(const QString& text) {
  static const QRegularExpression gaps("[^\\w³²]+", QRegularExpression::UseUnicodePropertiesOption);
  QStringList out;
  for (QString w : text.toLower().split(gaps, Qt::SkipEmptyParts)) {
    if (w.size() > 3 && w.startsWith(QString::fromUtf8("ال"))) w = w.mid(2);
    out << w;
  }
  return out;
}

int match(const Check& check, const QString& query) {
  const QStringList words = wordsOf(query);
  if (words.isEmpty()) return 1;
  static const QStringList filler = {"a", "an", "the", "my", "of", "it", "is", "to", "how", "much", "what", "does", "will", "do", "i",
                                     "in", "on", "that", "with", "and", "or", "for", "gets", "get"};
  const QStringList hay = wordsOf(check.question + " " + check.line);
  QStringList keys;
  for (const auto& w : check.words) keys << wordsOf(w);
  int score = 0, counted = 0;
  for (const QString& w : words) {
    if (filler.contains(w)) continue;
    ++counted;
    auto hit = [&](const QStringList& in) {
      // A prefix either way: "narrow" finds "narrowing", "fans" finds "fan"; short words only whole.
      for (const QString& h : in)
        if (h == w || (w.size() >= 3 && h.startsWith(w)) || (h.size() >= 3 && w.startsWith(h) && w.size() - h.size() <= 3)) return true;
      return false;
    };
    if (hit(keys)) score += 3;
    else if (hit(hay)) score += 2;
  }
  if (counted == 0) return 1;
  return score;
}

}  // namespace simchecks
