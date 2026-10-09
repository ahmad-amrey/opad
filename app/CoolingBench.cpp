// The Cooling assistant as a user drives it (OPAD_BENCH_COOLING=<prefix>): opened from its command on an empty document,
// the example built from its first page (a board, two chips, a heatsink and a fan block in a vented ABS box, the exhaust
// slots' height the parameter vent_z), the box found, the heat and fan rows filled from what the example set, a power
// changed in the table and written back as the case's load, the board's copper as the study's material; four pages, no
// optimising (it sets the study up). With OPAD_BENCH_COOLING_RUN set (and OpenFOAM) it also runs the case at
// Quick quality and checks that the heat put in leaves. Screenshots of every page at <prefix>.<page>.png.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include <cmath>
#include <functional>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SimulateArea.hpp"
#include "SimulateCooling.hpp"
#include "Tracking.hpp"
#include "opad/sim/cfd.hpp"

namespace {
struct Step {
  std::function<bool()> ready;
  std::function<void(bool)> act;
  int ms = 60000;
};

void runSteps(QObject* context, std::shared_ptr<std::vector<Step>> steps, size_t i, std::function<void()> finish) {
  if (i >= steps->size()) return finish();
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [=] {
    const Step& step = (*steps)[i];
    const bool ok = !step.ready || step.ready();
    if (!ok && clock->elapsed() < step.ms) return;
    timer->stop();
    timer->deleteLater();
    step.act(ok);
    runSteps(context, steps, i + 1, finish);
  });
  timer->start(50);
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_COOLING, cooling) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: cooling: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Simulate* area = w.findChild<Simulate*>();
  AppDocument* doc = w.m_doc;
  const QString prefix = value;
  if (!area || !doc->hasDocument) {
    require(false, "an empty document and the Simulate area");
    QCoreApplication::exit(2);
    return true;
  }
  auto assistant = [&w] { return w.findChild<CoolingAssistant*>(); };
  auto shot = [assistant, prefix](const QString& name) {
    if (CoolingAssistant* a = assistant()) {
      a->grab().save(prefix + "." + name + ".png");
      trace::log("bench: cooling: shot " + name);
    }
  };
  auto loadsOf = [doc](const char* kind) {
    std::vector<const opad::Load*> out;
    for (const auto& l : doc->scene.loads)
      if (l.kind == kind && l.load_case == CoolingAssistant::kCase) out.push_back(&l);
    return out;
  };
  auto bodyNamed = [doc](const char* name) {
    for (const auto& id : doc->scene.all_bodies())
      if (doc->scene.node(id)->name == name) return id;
    return std::string();
  };
  auto steps = std::make_shared<std::vector<Step>>();
  auto& list = *steps;
  list.push_back({nullptr, [=, &w](bool) {
                    QAction* a = area->services().action("simulate.cooling");
                    require(a && a->isEnabled(), "Thermal setup is a command, enabled on a document");
                    if (a) a->trigger();
                  }});
  list.push_back({[assistant] { return assistant() && assistant()->isVisible(); },
                  [=](bool ok) {
                    require(ok, "it opens at its first page");
                    if (!ok) return;
                    CoolingAssistant* c = assistant();
                    require(c->steps() == 4 && c->step() == 0, "four steps (box, heat, air, run), at the first");
                    shot("empty");
                    c->buildExample();
                  }});
  list.push_back({[assistant] { return assistant() && assistant()->settled(); }, [=](bool ok) {
                    require(ok, "the box's bodies are found (a worker measures them)");
                    CoolingAssistant* c = assistant();
                    const std::string box = bodyNamed("Enclosure");
                    require(!box.empty() && !bodyNamed("Board").empty() && !bodyNamed("Heatsink").empty() && !bodyNamed("Fan").empty(),
                            "the example's bodies are made");
                    require(doc->scene.param("vent_z") != nullptr, "its vent height is the parameter vent_z");
                    require(loadsOf("heat").size() == 2 && loadsOf("fan").size() == 1, "two heat sources and a fan in the case Cooling");
                    require(c->pickedBodies() == std::vector<std::string>{box}, "the box's bodies picked: the enclosure alone");
                    int heated = 0;
                    for (int r = 0; r < c->heatTable()->rowCount(); ++r) heated += c->heatTable()->item(r, 0)->checkState() == Qt::Checked;
                    require(heated == 2, QString("the heat table ticks the two chips (%1)").arg(heated));
                    require(c->fanTable()->rowCount() == 1, "the fan table has the fan block");
                    shot("box");
                    c->open(1);
                    shot("heat");
                    c->open(2);
                    shot("air");
                    for (int r = 0; r < c->heatTable()->rowCount(); ++r)
                      trace::log(QString("bench: cooling: row %1 %2 W").arg(c->heatTable()->item(r, 0)->text())
                                     .arg(c->heatTable()->item(r, 1)->data(Qt::EditRole).toDouble()));
                    for (const opad::Load* l : loadsOf("heat")) trace::log(QString("bench: cooling: load %1 W").arg(l->def.value("value", 0.0)));
                    // The SoC at 5 W instead of 4, written back by Apply.
                    for (int r = 0; r < c->heatTable()->rowCount(); ++r)
                      if (c->heatTable()->item(r, 0)->text() == "SoC") c->heatTable()->item(r, 1)->setData(Qt::EditRole, 5.0);
                    require(c->apply(), "Apply writes the case");
                  }});
  list.push_back({nullptr, [=](bool) {
                    CoolingAssistant* c = assistant();
                    double total = 0;
                    for (const opad::Load* l : loadsOf("heat")) total += l->def.value("value", 0.0);
                    require(std::fabs(total - 5.5) < 1e-9, QString("the heat sources follow the table: %1 W").arg(total));
                    const opad::Study* st = nullptr;
                    for (const auto& s : doc->scene.studies)
                      if (QString::fromStdString(s.name) == CoolingAssistant::studyName()) st = &s;
                    require(st != nullptr, "the cooling study is there");
                    if (st) {
                      const opad::json set = st->def["settings"];
                      require(set.value("air", "") == "cfd" && set["cfd"].value("enclosure", "") == bodyNamed("Enclosure"), "it solves the air in the box");
                      require(set["materials"].contains(bodyNamed("Board")) && set["materials"][bodyNamed("Board")].contains("pcb"), "the board is a 4-layer PCB");
                      require(!set["cfd"].value("buoyancy", true), "with the fan, the flow is the fan's");
                    }
                    c->open(3);
                    shot("run");
                  }});
  if (!qEnvironmentVariableIsEmpty("OPAD_BENCH_COOLING_RUN") && opad::sim::openfoam().found()) {
    list.push_back({nullptr, [=](bool) {
                      CoolingAssistant* c = assistant();
                      c->open(3);
                      require(c->startRun(), "Run starts the study");
                    }});
    list.push_back({[area] { return area->shownRun() && !area->running(); },
                    [=](bool ok) {
                      require(ok, "the study finishes");
                      if (!ok) return;
                      const opad::json s = area->shownRun()->summary;
                      const double out = s["vents"].value("heat_to_air_W", 0.0) + s.value("radiated_W", 0.0);
                      require(std::fabs(out - 5.5) < 0.1 * 5.5, QString("the 5.5 W leave the box: %1 W").arg(out));
                      require(!assistant()->resultText()->text().isEmpty(), "the Run page shows the result");
                      shot("result");
                    },
                    3600000});
  }
  list.push_back({nullptr, [=](bool) {
                    if (CoolingAssistant* c = assistant()) c->close();
                  }});
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
