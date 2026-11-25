// The Simulation panel's subject follows its own bar (the Advanced tab holds every raw tool); case in tools/bench_cases/simulate.py.
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPushButton>
#include <QTabBar>
#include <QTimer>
#include <functional>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Ribbon.hpp"
#include "SimulateArea.hpp"
#include "SimulateCooling.hpp"
#include "SimulatePanel.hpp"
#include "ToolPanel.hpp"
#include "Tracking.hpp"

namespace {
bool until(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return done();
}

QStringList kinds(QComboBox* box) {
  QStringList out;
  for (int i = 0; i < box->count(); ++i) out << box->itemData(i).toString();
  return out;
}
}  // namespace

// OPAD_BENCH_SIMTABS=1 on an empty document with a motion, a static and a thermal study (not run): the Simulation panel opened
// from Simulate's Advanced tab, its bar on Mechanism, shows the mechanism, its joints and only the motion study, New study offering motion and dynamic; on
// Structure no mechanism, the static study, static and vibration; on Thermal the thermal study, Thermal setup (its button opens
// its panel in the Simulation panel's place), thermal alone. The panel stays open across the tabs, and each tab's study is the one chosen.
OPAD_BENCH(OPAD_BENCH_SIMTABS, simtabs) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: simulate tabs: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  AppDocument* doc = w.m_doc;
  Simulate* area = w.findChild<Simulate*>();
  if (!area || !doc->hasDocument) {
    require(false, "an empty document and the Simulate area");
    return QCoreApplication::exit(2), true;
  }
  auto study = [doc](const char* kind, const char* name) { return doc->run("study", {{"kind", kind}, {"name", name}, {"run", false}}).value("id", std::string()); };
  const std::string motion = study("motion", "Travel"), stat = study("static", "Bracket"), thermal = study("thermal", "Cooling");
  w.setWorkspace("simulate");
  require(w.m_ribbon->setCurrentTabId("simulate.advanced"), "the Simulate workspace, its Advanced tab");
  w.action("simulate.panel")->trigger();
  SimulatePanel* form = area->form();
  require(until([area] { return area->panel()->isVisible(); }, 5000), "the Simulation panel opens");
  auto* studies = form->findChild<QComboBox*>("simStudies");
  auto* newKind = form->findChild<QComboBox*>("simNewKind");
  auto* joints = form->findChild<QComboBox*>("simJoints");
  auto* cooling = form->findChild<QPushButton*>("simCooling");
  auto listed = [studies] {
    std::vector<std::string> out;
    for (int i = 0; i < studies->count(); ++i) out.push_back(studies->itemData(i).toString().toStdString());
    return out;
  };
  auto* modes = form->findChild<QTabBar*>("simModes");
  require(modes && modes->count() == 3, "the panel's Mechanism / Structure / Thermal bar");
  if (!modes) return QCoreApplication::exit(2), true;
  struct Tab {
    const char* id;
    SimulatePanel::Mode mode;
    std::string study;
    QStringList kinds;
    bool mechanism, cooling;
  };
  const Tab tabs[] = {{"simulate.mechanism", SimulatePanel::Mode::Mechanism, motion, {"motion", "dynamic"}, true, false},
                      {"simulate.thermal", SimulatePanel::Mode::Thermal, thermal, {"thermal"}, false, true},
                      {"simulate.structure", SimulatePanel::Mode::Structure, stat, {"static", "modal"}, false, false},
                      {"simulate.mechanism", SimulatePanel::Mode::Mechanism, motion, {"motion", "dynamic"}, true, false}};
  for (const Tab& t : tabs) {
    modes->setCurrentIndex(int(t.mode));
    QCoreApplication::processEvents();
    const QString on = QString(t.id).mid(9) + ": ";
    require(area->panel()->isVisible() && form->mode() == t.mode, on + "the panel stays open, about this tab");
    require(listed() == std::vector<std::string>{t.study} && area->study() == t.study, on + "only this tab's study listed, and chosen");
    require(kinds(newKind) == t.kinds, on + "New study offers " + t.kinds.join(", ") + " (" + kinds(newKind).join(", ") + ")");
    require(joints->isVisibleTo(form) == t.mechanism, on + (t.mechanism ? "the mechanism and its joints shown" : "no mechanism"));
    require(cooling->isVisibleTo(form) == t.cooling, on + (t.cooling ? "Thermal setup shown" : "no Thermal setup"));
    if (t.cooling) {
      cooling->click();
      auto* assistant = w.findChild<CoolingAssistant*>();
      require(assistant && assistant->isVisible() && !area->panel()->isVisible(), on + "Thermal setup opens from the panel, a panel in its place");
      if (assistant) assistant->dismiss();
      w.action("simulate.panel")->trigger();  // the Simulation panel again
    }
  }
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
