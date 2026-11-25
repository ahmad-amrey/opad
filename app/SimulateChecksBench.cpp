// Simulate's guided checks, driven as a user would through the area's own widgets; case in tools/bench_cases/simulate.py.
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabBar>
#include <QTimer>
#include <functional>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "GuidedSteps.hpp"
#include "MainWindow.hpp"
#include "Ribbon.hpp"
#include "SimChecks.hpp"
#include "SimulateArea.hpp"
#include "SimulateCheck.hpp"
#include "SimulateCooling.hpp"
#include "SimulatePanel.hpp"
#include "SimulateSolver.hpp"
#include "SimulateStart.hpp"
#include "ToolPanel.hpp"
#include "Tracking.hpp"
#include "opad/sim/joints.hpp"

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
void settle(int ms) { until([] { return false; }, ms); }
}  // namespace

// OPAD_BENCH_SIMCHECKS=<prefix> on an empty document: the Simulate workspace's goal tabs; the start page in the view's place
// with a card per check (picture, question, time, readiness from the solvers found on a worker), its search and goal chips;
// a strength check on a block opened from its card: OPAD proposes the face it rests on and the top face, the load typed in
// kg, one study with its own case and loads written (once: the same again writes nothing), Run greyed without CalculiX or
// run with it to a verdict in plain words, a copy kept to compare; a motion check on a hinge (always runs) with its
// verdict; a heat check that finds the part named like a chip; an airflow check (Coming while the flow study is not in this
// build: nothing written); the missing-solver window; Thermal setup on the same guided frame; the Advanced tab's panel with
// its Mechanism / Structure / Thermal bar. Shots: <prefix>.start.png, .bend-*.png, .solver.png, .advanced.png.
OPAD_BENCH(OPAD_BENCH_SIMCHECKS, simchecks) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: simulate checks: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  const QString prefix = value;
  AppDocument* doc = w.m_doc;
  Simulate* area = w.findChild<Simulate*>();
  if (!area || !doc->hasDocument) {
    require(false, "an empty document and the Simulate area");
    return QCoreApplication::exit(2), true;
  }
  AreaServices& services = area->services();
  // A command of the bench's own once the document is free (a check reads it on a worker for its proposals meanwhile).
  auto run = [doc](const std::string& command, const opad::json& args) {
    until([doc] { return !doc->designBusy && !doc->snapshotBusy(); }, 10000);
    return doc->run(command, args);
  };
  auto shot = [&prefix](QWidget* widget, const QString& name) {
    if (widget && !prefix.isEmpty() && prefix != "1") widget->grab().save(prefix + "." + name + ".png");
  };

  // ---- the goal tabs, the start page
  w.setWorkspace("simulate");
  for (const char* tab : {"simulate.home", "simulate.goal.air", "simulate.goal.heat", "simulate.goal.strength", "simulate.goal.movement", "simulate.advanced"})
    require(w.m_ribbon->setCurrentTabId(tab), QString("the ribbon has the %1 tab").arg(tab));
  require(!w.m_ribbon->setCurrentTabId("simulate.mechanism"), "the old Mechanism tab is gone (its tools are on Advanced)");
  w.m_ribbon->setCurrentTabId("simulate.home");
  QCoreApplication::processEvents();
  SimulateStart* start = area->start();
  require(start && services.centralPage() == start, "Start shows the page of checks in the view's place");
  require(start && start->cards().size() == simchecks::all().size() && start->shown().size() == simchecks::all().size(), "a card per check, every one shown");
  require(until([area] { return area->engines().contains("motion"); }, 20000), "the solvers are looked for on a worker");
  const opad::json engines = area->engines();
  const bool fea = engines.value("static", false), cfd = engines.value("cfd", false);
  trace::log(QString("bench: simulate checks: engines static=%1 cfd=%2 dynamic=%3").arg(fea).arg(cfd).arg(engines.value("dynamic", false)));
  const auto& kinds = opad::sim::study_kinds();
  const bool flowKind = std::find(kinds.begin(), kinds.end(), "flow") != kinds.end();
  for (CheckCard* c : start->cards()) {
    const auto r = simchecks::readiness(*simchecks::find(c->check()), engines);
    require(c->chip() == r.chip && !c->chip().isEmpty(), QString("the %1 card says %2").arg(c->check(), c->chip()));
  }
  require(simchecks::readiness(*simchecks::find("clearance"), engines).state == simchecks::Readiness::Ready, "the motion check is always ready");
  require(flowKind || start->findChild<CheckCard*>("checkCard_fitting")->chip() == simchecks::readiness(*simchecks::find("fitting"), {}).chip,
          "without the flow study the airflow cards say Coming");
  shot(start, "start");
  const bool arabic = qEnvironmentVariable("OPAD_LANG") == "ar";  // described in the user's own language
  start->search()->setText(arabic ? QString::fromUtf8("هواء عبر أنبوب يضيق") : QString("air through a pipe that gets narrower"));
  QCoreApplication::processEvents();
  require(!start->shown().isEmpty() && start->shown().front() == "fitting", "describing it finds the fitting check first (" + start->shown().join(", ") + ")");
  start->search()->setText(arabic ? QString::fromUtf8("علبة Raspberry Pi تسخن") : QString("my raspberry pi case gets hot"));
  QCoreApplication::processEvents();
  require(!start->shown().isEmpty() && start->shown().front() == "overheat", "a Raspberry Pi case finds the overheat check first");
  start->search()->clear();
  if (auto* chip = start->findChild<QPushButton*>("simulateChip_heat")) chip->click();
  QCoreApplication::processEvents();
  bool heatOnly = !start->shown().isEmpty();
  for (const QString& id : start->shown()) heatOnly = heatOnly && simchecks::find(id)->goal == simchecks::Goal::Heat;
  require(heatOnly, "the Heat chip leaves the heat checks");
  shot(start, "start-heat");
  if (auto* chip = start->findChild<QPushButton*>("simulateChip_all")) chip->click();

  // ---- a strength check on a block from its card
  const opad::json block = run("feature", {{"kind", "box"}, {"name", "Shelf bracket"},
                                                {"inputs", {{"plane", {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}}}, {"length", 100}, {"width", 20}, {"height", 10}}}});
  const std::string blockId = block["body_ids"][0].get<std::string>();
  settle(300);
  start->findChild<CheckCard*>("checkCard_bend")->click();
  QCoreApplication::processEvents();
  GuidedCheck* check = area->check();
  require(area->checkPanel()->isVisible() && check->check() == "bend", "the card opens its check in a panel");
  require(services.ribbonTab() == "simulate.goal.strength" && services.centralPage() != start, "on its goal's tab, with the view back");
  require(check->stepNames() == QStringList({"part", "held", "load", "run", "results"}), "its steps: " + check->stepNames().join(", "));
  if (!prefix.isEmpty() && prefix != "1") w.grab().save(prefix + ".strength-window.png");
  require(check->listed("bodies").size() == 1 && check->listed("bodies")[0].body == blockId, "the part is the block");
  require(until([check] { return check->proposed("supports") && check->proposed("load"); }, 20000), "OPAD finds the face it rests on and the top face (a worker)");
  require(check->listed("supports").size() == 1, "the face it rests on is taken as its support at once");
  check->frame()->setCurrent(check->stepNames().indexOf("held"));
  shot(area->checkPanel(), "bend-held");
  check->useProposal("load");
  check->setValue("weight", 50);
  check->setValue("need", 2);
  check->flush();
  const opad::Study* st = doc->scene.study(check->study());
  require(st && st->def.value("settings", opad::json::object()).value("check", "") == "bend" && st->kind == "static", "one static study, marked as the bend check");
  const std::string caseName = st ? st->def["settings"].value("case", std::string()) : std::string();
  int fixed = 0, force = 0;
  double fz = 0;
  for (const auto& l : doc->scene.loads)
    if (l.load_case == caseName) {
      fixed += l.kind == "fixed";
      if (l.kind == "force") ++force, fz = l.def["vector"][2].get<double>();
    }
  require(!caseName.empty() && fixed == 1 && force == 1 && std::fabs(fz + 50 * 9.80665) < 1e-6, QString("its own case holds a support and 50 kg down (%1 N)").arg(fz));
  const size_t ops = doc->doc.ops.size();
  check->setValue("weight", 50);
  check->flush();
  require(doc->doc.ops.size() == ops, "the same choices again write nothing");
  check->frame()->setCurrent(check->stepNames().indexOf("load"));
  shot(area->checkPanel(), "bend-load");
  check->frame()->setCurrent(check->stepNames().indexOf("run"));
  require(check->runButton()->isEnabled() == fea, fea ? "Run is ready" : "Run is greyed without CalculiX");
  if (!fea) {
    require(check->runNote()->isVisible() && check->runNote()->text().contains("CalculiX"), "the Run step says CalculiX is missing and where to get it");
    shot(area->checkPanel(), "bend-run");
  } else {
    check->runButton()->click();
    require(until([area, check] { return !area->running() && check->hasResult(); }, 300000), "the check runs to a result");
    require(check->frame()->current() == check->stepNames().indexOf("results"), "the results step shows itself");
    require(!check->verdict().isEmpty(), "a verdict in plain words: " + check->verdict());
    shot(area->checkPanel(), "bend-results");
    // A copy kept to compare (its result the same run again, from the cache, on a worker).
    if (auto* keep = check->findChild<QPushButton*>("checkKeep")) keep->click();
    require(until([area, check] {
              auto* with = check->findChild<QComboBox*>("checkCompareWith");
              return !area->running() && with && with->count() > 0;
            }, 120000),
            "the kept copy is listed to compare");
    check->showStep("results");
    shot(area->checkPanel(), "bend-compare");
  }

  try {  // what throws (a command refused) is a FAIL with its words, not a silent exit
  // ---- a motion check on a hinge: always runs
  const std::string lid = run("feature", {{"kind", "box"}, {"name", "Lid"},
                                               {"inputs", {{"plane", {{"origin", {0, 60, 0}}, {"normal", {0, 0, 1}}}}, {"length", 60}, {"width", 40}, {"height", 4}}}})["body_ids"][0];
  const opad::json joint = run("joint", {{"kind", "revolute"}, {"part", lid}, {"at", {{"origin", {0, 40, 2}}, {"z", {1, 0, 0}}}}, {"base", nullptr}});
  require(joint.contains("id"), "a hinge joint on the lid");
  area->openCheck("clearance");
  require(check->check() == "clearance" && check->stepNames() == QStringList({"motion", "clear", "run", "results"}), "the motion check: " + check->stepNames().join(", "));
  check->setValue("travel", 60);
  check->frame()->setCurrent(check->stepNames().indexOf("run"));
  require(check->runButton()->isEnabled(), "a joint to drive: Run is ready");
  check->runButton()->click();
  require(until([area, check] { return !area->running() && check->hasResult(); }, 120000), "the motion check runs");
  require(!check->verdict().isEmpty(), "its verdict: " + check->verdict());
  shot(area->checkPanel(), "clearance-results");

  // ---- a heat check: the part named like a chip is proposed as the heat source
  run("feature", {{"kind", "box"}, {"name", "CPU chip"}, {"inputs", {{"plane", {{"origin", {0, 0, 10}}, {"normal", {0, 0, 1}}}}, {"length", 10}, {"width", 10}, {"height", 2}}}});
  settle(200);
  services.select({});
  area->openCheck("overheat");
  bool chip = false;
  for (const auto& r : check->listed("heat")) chip = chip || doc->scene.node(r.body)->name == "CPU chip";
  require(chip, "the part named like a chip is proposed as making heat");
  check->setValue("watts", 6);
  check->setValue("limit", 70);
  check->flush();
  int heat = 0, air = 0;
  if (const opad::Study* t = doc->scene.study(check->study()))
    for (const auto& l : doc->scene.loads)
      if (l.load_case == t->def["settings"].value("case", std::string())) heat += l.kind == "heat", air += l.kind == "convection";
  require(heat == 1 && air == 1, "its case: 6 W of heat and still air around");
  check->frame()->setCurrent(check->stepNames().indexOf("heat"));
  shot(area->checkPanel(), "overheat-heat");
  if (engines.value("thermal", false)) {  // the heat check runs to its verdict (CalculiX)
    check->frame()->setCurrent(check->stepNames().indexOf("run"));
    require(check->runButton()->isEnabled(), "the heat check is ready to run");
    check->runButton()->click();
    require(until([area, check] { return !area->running() && check->hasResult(); }, 300000), "the heat check runs");
    require(check->verdict().contains(QString::fromUtf8("°C")) || check->verdict().contains(QString::fromUtf8("°م")), "its verdict names a temperature: " + check->verdict());
    shot(area->checkPanel(), "overheat-results");
  }

  // ---- an airflow check: Coming until the flow study is in this build (nothing written), else set up
  const size_t before = doc->scene.studies.size();
  area->openCheck("fitting");
  require(check->stepNames() == QStringList({"part", "ends", "air", "run", "results"}), "the fitting check: " + check->stepNames().join(", "));
  check->setValue("flow", 350);
  check->flush();
  if (!flowKind) {
    require(doc->scene.studies.size() == before && !check->runButton()->isEnabled(), "Coming: nothing written, Run greyed");
  }
  require(until([check] { return check->proposed("inlet") && check->proposed("outlet"); }, 20000), "OPAD proposes where the air goes in and out");
  check->frame()->setCurrent(check->stepNames().indexOf("air"));
  shot(area->checkPanel(), "fitting-air");

  // ---- the missing-solver window (never its download: that opens the browser)
  {
    SolverWindow window("openfoam", {}, &w);
    window.show();
    QCoreApplication::processEvents();
    require(window.findChild<QPushButton*>("solverGet") && window.findChild<QPushButton*>("solverFind") && window.findChild<QPushButton*>("solverLater"),
            "the solver window offers Download and install, I already have it, Later");
    require(!SolverWindow::check("openfoam", "C:/no/such/folder").isEmpty() && !SolverWindow::check("calculix", "C:/Windows/notepad.exe").isEmpty(),
            "a folder or program without the solver is refused, saying why");
    shot(&window, "solver");
    window.close();
    SolverWindow chrono("chrono", {}, &w);
    require(!chrono.findChild<QPushButton*>("solverFind"), "a solver built into OPAD cannot be found by hand");
  }

  // ---- Thermal setup on the shared guided frame
  area->openCooling(0);
  auto* assistant = w.findChild<CoolingAssistant*>();
  auto* steps = assistant ? assistant->findChild<QTabBar*>("coolingSteps") : nullptr;
  require(steps && steps->count() == 4 && assistant->findChild<GuidedSteps*>(), "Thermal setup's four steps are the guided frame's");
  if (auto* next = assistant ? assistant->findChild<QPushButton*>("coolingNext") : nullptr) next->click();
  require(assistant && assistant->step() == 1, "its Next goes to the Heat step");
  if (assistant) assistant->dismiss();

  // ---- Advanced: every raw tool, the panel choosing its subject
  w.m_ribbon->setCurrentTabId("simulate.advanced");
  w.action("simulate.panel")->trigger();
  SimulatePanel* form = area->form();
  auto* modes = form->findChild<QTabBar*>("simModes");
  require(area->panel()->isVisible() && modes && modes->count() == 3, "the Simulation panel opens with its Mechanism / Structure / Thermal bar");
  if (modes) modes->setCurrentIndex(1);
  require(form->mode() == SimulatePanel::Mode::Structure, "its Structure tab shows the structural studies");
  for (const char* id : {"simulate.joint", "simulate.fixed", "simulate.force", "simulate.heat", "simulate.fan", "simulate.cooling", "simulate.static", "simulate.modal",
                         "simulate.thermal", "simulate.print", "simulate.motion", "simulate.dynamic"})
    require(w.action(id) != nullptr, QString("%1 is still a command").arg(id));
  shot(area->panel(), "advanced");
  } catch (const std::exception& e) {
    require(false, QString("no step throws: %1").arg(QString::fromStdString(e.what())));
  }
  if (const QString ui = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !ui.isEmpty()) w.grab().save(ui);

  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
