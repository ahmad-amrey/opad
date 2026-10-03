// Benches of the core area (T0), registered through BenchRegistry; cases in tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <cmath>
#include <functional>
#include <memory>
#include <set>

#include "BenchRegistry.hpp"
#include "CheckPanel.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "MainWindow.hpp"
#include "TimelineWidget.hpp"
#include "Units.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/inspect.hpp"

// OPAD_BENCH_SEAMS=1 [OPAD_LANG=ar]: the extension seams (UI-119) in the running app. This bench is itself dispatched
// by the registry; no two files claim the same bench switch or icon name; the fragments app/i18n/<code>/*.json are
// embedded but are not languages; with a language on, tr() gives exactly the merged table of <code>.json and its
// fragments (so every fragment string came through).
OPAD_BENCH(OPAD_BENCH_SEAMS, seams) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: seams: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  require(bench::pending() == "OPAD_BENCH_SEAMS" && bench::variables().contains("OPAD_BENCH_SEAMS") && w.m_doc->hasDocument,
          "registered bench runs with the window loaded");
  auto named = [](const QStringList& names) { return names.isEmpty() ? QString() : " (" + names.join(' ') + ")"; };
  require(bench::clashes().isEmpty(), "bench switches claimed once" + named(bench::clashes()));
  require(icons::clashes().isEmpty() && icons::has("open"), "icon names taken once" + named(icons::clashes()));
  bool languages = true;
  for (const i18n::Language& l : i18n::languages())
    languages = languages && !l.code.contains('/') && (l.code == "en" || QFile::exists(":/i18n/" + l.code + ".json"));
  int fragments = 0;
  for (const QString& code : QDir(":/i18n").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    fragments += QDir(":/i18n/" + code).entryList({"*.json"}, QDir::Files).size();
    languages = languages && QFile::exists(":/i18n/" + code + ".json");
  }
  require(languages, QString("languages are <code>.json, %1 fragments are not").arg(fragments));
  if (const QString code = i18n::current(); code != "en") {
    const QHash<QString, QString> table = i18n::table(code, {":/i18n", QCoreApplication::applicationDirPath() + "/i18n"});
    int wrong = 0;
    for (auto it = table.begin(); it != table.end(); ++it)
      if (i18n::t(it.key()) != it.value()) ++wrong;
    require(table.size() > 1000 && wrong == 0, QString("%1: %2 strings of %3.json and %4 fragments translate as merged (%5 wrong)")
                                                    .arg(code).arg(table.size()).arg(code).arg(QDir(":/i18n/" + code).entryList({"*.json"}, QDir::Files).size()).arg(wrong));
  }
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}

// OPAD_BENCH_TOLERANT=<path>: a file of a newer build (tools/bench_cases/core.py adds a one-line and a multi-line record of
// unknown types) opens (UI-65): the records are reported as needing a newer OPAD, the timeline never steps onto them, an
// edit still works, and Save As writes them back as they were read.
OPAD_BENCH(OPAD_BENCH_TOLERANT, tolerant) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: tolerant: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  std::vector<const opad::Op*> opaque;
  for (const auto& op : w.m_doc->doc.ops)
    if (!opad::Document::known_type(op.type)) opaque.push_back(&op);
  int newer = 0;
  for (const auto& u : w.m_doc->scene.unresolved) newer += u.reason.find("needs a newer OPAD") != std::string::npos;
  require(w.m_doc->hasDocument && opaque.size() == 2 && newer == 2 && !w.m_doc->scene.all_bodies().empty(),
          QString("opened with %1 records of a newer build, %2 reported, %3 bodies").arg(opaque.size()).arg(newer).arg(w.m_doc->scene.all_bodies().size()));
  std::set<std::string> visited;
  w.m_timeline->setCurrentOp({});
  for (int i = 0; i <= int(w.m_doc->doc.ops.size()); ++i) {
    w.m_timeline->step(1);
    visited.insert(w.m_timeline->currentOp());
  }
  bool hidden = !visited.empty();
  for (const auto* op : opaque) hidden = hidden && !visited.count(op->id);
  require(hidden, QString("the timeline steps through %1 ops, none of a newer build").arg(visited.size()));
  std::string records;
  for (const auto* op : opaque) records += op->raw + "\n";
  const auto body = w.m_doc->scene.all_bodies().front();
  bool edited = true;
  try {
    w.m_doc->run("rename", {{"target", body}, {"name", "Renamed here"}});
  } catch (const std::exception&) {
    edited = false;
  }
  w.m_doc->saveAs(value);
  QFile file(value);
  const std::string text = file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string();
  require(edited && w.m_doc->scene.node(body)->name == "Renamed here" && !records.empty() && text.find(records) != std::string::npos &&
              opad::Document::parse(text).ops.size() == w.m_doc->doc.ops.size(),
          "an edit, then Save As writes the newer records back byte for byte");
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}

namespace {
// Polls `done` every 50 ms until it holds or `ms` have passed, then calls `then` with the outcome.
void pollUntil(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(50);
}
}  // namespace

// OPAD_BENCH_UNITS=<prefix> on the box (30 x 20 x 10 mm): the status bar's unit is live (UI-123). Choosing Inches in its
// menu appends one units op; a Distance between the two end faces then reads 1.181 in in the tool's result and in the
// view's label, Properties shows inches, a precision change and fractional inches redraw the result at once, and undo
// brings millimetres back; the check panel's overhang follows radians; a new feature's and a sketch tool's defaults are
// offered in inches and a sketch dimension reads in inches. <prefix>.status.png / .panel.png / .sketch.png: the status
// bar, the tool panel and the sketch in inches.
OPAD_BENCH(OPAD_BENCH_UNITS, units) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: units: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  auto finish = [all] { QCoreApplication::exit(*all ? 0 : 2); };
  auto result = [&w](const QString& key) {
    const auto* grid = w.m_toolSteps->findChild<QTreeWidget*>();
    for (int i = 0; grid && i < grid->topLevelItemCount(); ++i)
      if (grid->topLevelItem(i)->text(0) == key) return grid->topLevelItem(i)->toolTip(1);
    return QString();
  };
  const auto bodies = w.m_doc->scene.all_bodies();
  require(w.m_statusUnits->text() == "mm" && units::current().length == "mm" && bodies.size() == 1, "a millimetre document shows mm: " + w.m_statusUnits->text());
  if (bodies.size() != 1) return finish(), true;
  // The two end faces (normals along X), 30 mm apart.
  std::vector<opad::Ref> ends;
  for (int i = 0; i < 6; ++i) {
    opad::Ref r;
    r.body = bodies.front();
    r.kind = opad::Ref::Kind::Face;
    r.index = i;
    const auto info = opad::inspect_ref(w.m_doc->doc, w.m_doc->scene, r);
    if (info.contains("normal") && std::abs(std::abs(info["normal"][0].get<double>()) - 1) < 1e-9) ends.push_back(r);
  }
  QMenu* menu = w.m_statusUnits->menu();
  emit menu->aboutToShow();
  emit menu->aboutToShow();  // opened again: rebuilt, not piled up
  require(menu->findChildren<QMenu*>(Qt::FindDirectChildrenOnly).size() == 1, "the menu reopened holds one Decimal places submenu");
  QAction* inches = menu->findChild<QAction*>("unit.in");
  require(ends.size() == 2 && inches && inches->isEnabled() && !inches->isChecked(), "the menu offers inches");
  if (ends.size() != 2 || !inches) return finish(), true;
  const size_t ops = w.m_doc->doc.ops.size();
  inches->trigger();
  pollUntil(&w, [&w] { return w.m_doc->scene.units == "in" && !w.m_doc->designBusy; }, 20000, [&w, require, finish, result, ends, ops](bool switched) {
    const auto& last = w.m_doc->doc.ops.back();
    require(switched && w.m_doc->doc.ops.size() == ops + 1 && last.type == "units" && last.data.value("length", "") == "in" &&
                w.m_statusUnits->text() == "in" && units::current().length == "in",
            "Inches appends one units op and the status bar follows: " + w.m_statusUnits->text());
    w.startTool("distance");
    w.toolPicksChanged(ends, false);
    pollUntil(&w, [&w] { return !w.m_lastMeasure.is_null(); }, 20000, [&w, require, finish, result](bool measured) {
      const QString value = result(w.m_tool.title);
      const QStringList captions = w.m_viewport->measurementCaptions();
      require(measured && value == "1.181 in" && result(QString::fromUtf8("ΔX")).endsWith(" in"), "the Distance result reads " + value);
      require(captions.contains(QString::fromUtf8("ΔX +1.181 in")), "the view's label reads " + captions.join(" | "));  // along X only: the axis label
      QCoreApplication::processEvents();  // the panel fits its result rows
      w.statusBar()->grab().save(qEnvironmentVariable("OPAD_BENCH_UNITS") + ".status.png");
      w.m_toolPanel->grab().save(qEnvironmentVariable("OPAD_BENCH_UNITS") + ".panel.png");
      w.m_props->showEntity("Probe", {}, {}, opad::json{{"volume", 6000.0}, {"center", {25.4, 50.8, 0.0}}, {"normal", {1.0, 0.0, 0.0}}});
      QStringList shown;
      for (const auto* item : w.m_props->findChildren<QTreeWidget*>().value(0)->findItems("*", Qt::MatchWildcard)) shown << item->text(1).remove(QChar(0x202A)).remove(QChar(0x202C));
      require(shown.contains(QString::fromUtf8("0.366 in³")) && shown.contains("(1, 2, 0) in") && shown.contains("(1, 0, 0)"), "Properties in inches: " + shown.join(" | "));
      const auto typed = units::parse(units::Kind::Length, "1/2");
      require(typed && std::abs(*typed - 12.7) < 1e-9, "a typed 1/2 reads as half an inch");
      units::setPrecision(1, false, 0);
      require(result(w.m_tool.title) == "1.2 in" && w.m_viewport->measurementCaptions().contains(QString::fromUtf8("ΔX +1.2 in")), "one decimal redraws the result: " + result(w.m_tool.title));
      units::setPrecision(3, false, 64);
      require(result(w.m_tool.title) == "1 3/16 in", "fractional inches: " + result(w.m_tool.title));
      const CheckPanel::Mode mode = w.m_checks->mode();
      w.m_checks->begin(CheckPanel::Mode::Print);
      units::setPrecision(3, true, 0);
      const opad::json checks = w.m_checks->options();
      w.m_checks->begin(mode);
      const QDoubleSpinBox* overhang = nullptr;
      for (const auto* box : w.m_checks->findChildren<QDoubleSpinBox*>())
        if (box->property("stored").toDouble() == 45) overhang = box;
      require(overhang && overhang->suffix() == " rad" && std::abs(overhang->value() - 0.785) < 1e-9 && checks.value("overhang_deg", 0.0) == 45,
              "the overhang box in radians: " + (overhang ? overhang->text() : QString()));
      units::setPrecision(3, false, 0);
      w.cancelTool();
      // Defaults and sketch dimensions in inches: a new fillet offers 0.1 in, a sketch tool's 2 mm default reads 0.1 in,
      // and a sketch line of 1.5 in is dimensioned "1.5 in" in the editor (1 1/2 in with fractions on).
      w.m_design->startFeature("fillet");
      const opad::json radius = w.m_design->featurePanel()->inputs().value("radius", opad::json());
      w.m_design->escape();
      require(radius == "0.1 in" && w.m_design->sketch()->option("radius", "2 mm") == "0.1 in", "defaults in inches: " + QString::fromStdString(radius.dump()));
      opad::design::Sketch sk;
      const int line = sk.add_line(sk.add_point(0, 0), sk.add_point(38.1, 0));
      sk.add_constraint(opad::design::SkConstraint::Type::Horizontal, {line});
      sk.add_constraint(opad::design::SkConstraint::Type::Distance, {line}, 38.1);
      w.m_design->applyOps({opad::design::make_sketch_op("Inch sketch", {{"base", "xy"}}, sk.to_json())}, "bench sketch", [&w, require, finish](bool ok, const QString& error) {
        require(ok && !w.m_doc->scene.sketches.empty(), "a sketch with a 1.5 in line " + error);
        if (!ok || w.m_doc->scene.sketches.empty()) return finish();
        w.m_design->editOp(w.m_doc->scene.sketches.back().id);
        auto dimension = [&w] {
          for (auto* tree : w.findChildren<QTreeWidget*>())
            for (int i = 0; tree->columnCount() == 3 && i < tree->topLevelItemCount(); ++i)  // the constraint list: "d<n>" rows
              if (tree->topLevelItem(i)->text(0).startsWith('d')) return tree->topLevelItem(i)->text(2);
          return QString();
        };
        pollUntil(&w, [&w, dimension] { return w.m_design->sketchActive() && !dimension().isEmpty(); }, 20000, [&w, require, finish, dimension](bool editing) {
          require(editing && dimension() == "1.5 in", "the sketch dimension reads " + dimension());
          w.m_viewport->grabImage().save(qEnvironmentVariable("OPAD_BENCH_UNITS") + ".sketch.png");
          units::setPrecision(3, false, 16);
          require(dimension() == "1 1/2 in", "with fractions it reads " + dimension());
          units::setPrecision(3, false, 0);
          require(!w.m_design->sketch()->modified(), "looking changed nothing");
          if (w.m_design->sketch()->modified()) w.m_design->sketch()->end();  // never the discard question
          w.m_design->cancelSketch();
          w.m_doc->setRollback({});
          w.m_doc->undo();  // the sketch
          w.m_doc->undo();  // the units
          require(w.m_doc->scene.units == "mm" && w.m_statusUnits->text() == "mm" && units::current().length == "mm", "undo brings millimetres back: " + w.m_statusUnits->text());
          finish();
        });
      });
    });
  });
  return true;
}
