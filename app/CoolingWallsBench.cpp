// OPAD_BENCH_COOLING_WALLS=<prefix>: the Cooling assistant on an enclosure of two bodies, a tray and the cover on it (built
// here on an empty document: a board, a 3 W chip, a heatsink standing up past the tray's rim, a fan block, a desk under it).
// Opened from its command, the box found is the tray with the cover ticked as its other wall (the desk offered, not ticked);
// the parts inside are those within both, the heatsink too; the cover unticked, the heatsink is no longer inside, ticked
// again it is; Apply writes the study's enclosure as both; opened again, the cover is ticked from the study. Frame:
// <prefix>.box.png.
#include <QAction>
#include <QApplication>
#include <QListWidget>
#include <QTableWidget>
#include <QTimer>

#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SimulateArea.hpp"
#include "SimulateCooling.hpp"
#include "Tracking.hpp"

OPAD_BENCH(OPAD_BENCH_COOLING_WALLS, coolingWalls) {
  struct State {
    int phase = 0, ticks = 0;
    bool all = true;
    std::string tray, cover, board, chip, sink, fan, desk;
  };
  auto st = std::make_shared<State>();
  const QString prefix = value;
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, prefix] {
    AppDocument* doc = w.m_doc;
    Simulate* area = w.findChild<Simulate*>();
    CoolingAssistant* c = w.findChild<CoolingAssistant*>();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: cooling walls: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    if (++st->ticks > 600) {
      require(false, QString("timed out in phase %1").arg(st->phase));
      return finish();
    }
    if (doc->loading || doc->designBusy) return;
    auto box = [doc](const char* name, opad::Vec3 at, double l, double wd, double h, const std::string& cutFrom = {}) {
      opad::json inputs = {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"length", l}, {"width", wd}, {"height", h}, {"centered", false}};
      if (!cutFrom.empty()) inputs["operation"] = "cut", inputs["targets"] = {cutFrom};
      opad::json args = {{"kind", "box"}, {"inputs", inputs}};
      if (name) args["name"] = name;
      const opad::json r = doc->run("feature", args);
      return cutFrom.empty() && r.contains("body_ids") && !r["body_ids"].empty() ? r["body_ids"][0].get<std::string>() : cutFrom;
    };
    // The bodies the heat table lists: what the assistant takes as inside the box.
    auto inside = [c] {
      std::vector<std::string> out;
      for (int r = 0; r < c->heatTable()->rowCount(); ++r) out.push_back(c->heatTable()->item(r, 0)->data(Qt::UserRole).toString().toStdString());
      return out;
    };
    auto has = [](const std::vector<std::string>& v, const std::string& id) { return std::find(v.begin(), v.end(), id) != v.end(); };
    auto wall = [c](const std::string& id) -> QListWidgetItem* {
      for (int i = 0; i < c->wallChoice()->count(); ++i)
        if (c->wallChoice()->item(i)->data(Qt::UserRole).toString().toStdString() == id) return c->wallChoice()->item(i);
      return nullptr;
    };
    switch (st->phase) {
      case 0: {  // the model
        if (!require(area && doc->hasDocument, "an empty document and the Simulate area")) return finish();
        st->tray = box(nullptr, {0, 0, 0}, 100, 80, 25);
        box(nullptr, {3, 3, 3}, 94, 74, 25, st->tray);
        doc->run("rename", {{"target", st->tray}, {"name", "Tray"}});
        st->cover = box(nullptr, {0, 0, 25}, 100, 80, 15);
        box(nullptr, {3, 3, 24}, 94, 74, 13, st->cover);
        doc->run("rename", {{"target", st->cover}, {"name", "Cover"}});
        st->board = box("Board", {10, 10, 8}, 60, 50, 1.6);
        st->chip = box("Chip", {30, 30, 9.6}, 10, 10, 2);
        st->sink = box("Heatsink", {28, 28, 11.6}, 14, 14, 20);  // up to z 31.6: past the tray's rim, under the cover
        st->fan = box("Fan", {3, 30, 5}, 10, 20, 20);
        st->desk = box("Desk", {-50, -50, -10}, 200, 180, 10);
        doc->run("load", {{"kind", "heat"}, {"case", CoolingAssistant::kCase}, {"on", {st->chip}}, {"value", 3}});
        require(!st->tray.empty() && !st->cover.empty() && !st->sink.empty() && !st->desk.empty(), "a tray, its cover, a board, a chip, a heatsink, a fan block, a desk");
        st->phase = 1;
        return;
      }
      case 1: {
        QAction* a = area->services().action("simulate.cooling");
        if (!require(a && a->isEnabled(), "Cooling assistant is enabled")) return finish();
        a->trigger();
        st->phase = 2;
        return;
      }
      case 2: {  // the box found: the tray, the cover ticked as its other wall
        if (!c || !c->isVisible()) return;
        require(c->boxChoice()->currentData().toString().toStdString() == st->tray, "the box found is the tray: " + c->boxChoice()->currentText());
        QListWidgetItem* cover = wall(st->cover);
        QListWidgetItem* desk = wall(st->desk);
        require(cover && cover->checkState() == Qt::Checked, "the cover is ticked as the box's other wall");
        require(!desk || desk->checkState() == Qt::Unchecked, "the desk under it is not a wall");
        const auto in = inside();
        require(has(in, st->board) && has(in, st->chip) && has(in, st->sink) && has(in, st->fan) && !has(in, st->cover) && !has(in, st->desk),
                QString("inside both: the board, the chip, the heatsink, the fan block (%1 parts)").arg(in.size()));
        c->grab().save(prefix + ".box.png");
        cover->setCheckState(Qt::Unchecked);
        require(!has(inside(), st->sink), "the cover unticked: the heatsink, standing past the tray's rim, is no longer inside");
        cover->setCheckState(Qt::Checked);
        require(has(inside(), st->sink), "ticked again: it is");
        require(c->apply(), "Apply writes the case");
        st->phase = 3;
        return;
      }
      case 3: {  // the study names both walls; opened again, the cover ticked from it
        opad::json enclosure;
        for (const auto& s : doc->scene.studies)
          if (s.name == CoolingAssistant::studyName().toStdString()) enclosure = s.def.value("settings", opad::json::object()).value("cfd", opad::json::object()).value("enclosure", opad::json());
        require(enclosure == opad::json::array({st->tray, st->cover}), "the study's enclosure is the tray and the cover: " + QString::fromStdString(enclosure.dump()));
        c->close();
        c->open(0);
        QListWidgetItem* cover = wall(st->cover);
        require(c->boxChoice()->currentData().toString().toStdString() == st->tray && cover && cover->checkState() == Qt::Checked, "opened again: the tray, the cover ticked");
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
