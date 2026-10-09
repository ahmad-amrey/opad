// OPAD_BENCH_COOLING_WALLS=<prefix>: Thermal setup on an enclosure of two bodies, a tray and the cover on it (built here on an
// empty document: a board, a 3 W chip, a heatsink standing up past the tray's rim with a fin (the component Cooler), a fan
// block with a hub (the component Fan unit), a desk under it). Everything is picked through the app's selection (what clicks
// in the view and rows of the browser make). The box's page: the tray and the cover picked, shown selected; the parts inside
// are those within both; the cover taken out, the heatsink is no longer inside; the cover and the desk picked, a box of three.
// The Heat page picks the parts that make heat (a click ticks or unticks one), then faces (the Faces filter on): a face of the
// board (a chip joined into it) becomes a heat row with its power, kept when the filter is switched by hand and shown selected
// again when it is back. The Air page picks the fan (a component, one part), the face its air goes through (Flip turns it
// round), a custom fan with its numbers under its choice, the heatsink (a component) and its material. The Run page picks
// nothing and has no Next. Apply writes the enclosure, the heat loads, the fan load and the heatsink's material; closed, the
// view selects again with its filter as before; opened again, everything is picked from the study.
// Frames: <prefix>.box.png, .heat.png, .air.png.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QListWidget>
#include <QTableWidget>
#include <QTimer>

#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "SimulateArea.hpp"
#include "SimulateCooling.hpp"
#include "Tracking.hpp"

OPAD_BENCH(OPAD_BENCH_COOLING_WALLS, coolingWalls) {
  struct State {
    int phase = 0, ticks = 0;
    bool all = true;
    std::string tray, cover, board, chip, sink, fan, desk, hub, fin, fanUnit, sinkUnit;
    opad::Vec3 way{0, 0, 0};
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
    if (++st->ticks > 900) {
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
    auto pick = [area](const std::vector<std::string>& ids) {  // as clicks in the view or rows of the browser select them
      std::vector<opad::Ref> refs;
      for (const auto& id : ids) {
        opad::Ref r;
        r.body = id;
        r.kind = opad::Ref::Kind::Body;
        refs.push_back(r);
      }
      area->services().select(refs);
    };
    auto body = [](const std::string& id) {
      opad::Ref r;
      r.body = id;
      return r;
    };
    auto face = [](const std::string& body, int index) {
      opad::Ref r;
      r.body = body;
      r.kind = opad::Ref::Kind::Face;
      r.index = index;
      return r;
    };
    auto ticked = [c] {  // the heat table's parts ticked as making heat
      std::vector<std::string> out;
      for (int r = 0; r < c->heatTable()->rowCount(); ++r)
        if (const QString id = c->heatTable()->item(r, 0)->data(Qt::UserRole).toString(); !id.isEmpty() && c->heatTable()->item(r, 0)->checkState() == Qt::Checked)
          out.push_back(id.toStdString());
      return out;
    };
    auto pickBox = [c](const char* name) { return c->findChild<PickBox*>(name); };
    using Pick = CoolingAssistant::Pick;
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
        st->hub = box("Hub", {5, 38, 13}, 4, 4, 4);
        st->fin = box("Fin", {28, 42, 11.6}, 14, 2, 18);
        st->fanUnit = doc->run("component", {{"name", "Fan unit"}}).value("id", "");
        doc->run("reparent", {{"targets", {st->fan, st->hub}}, {"parent", st->fanUnit}});
        st->sinkUnit = doc->run("component", {{"name", "Cooler"}}).value("id", "");
        doc->run("reparent", {{"targets", {st->sink, st->fin}}, {"parent", st->sinkUnit}});
        doc->run("load", {{"kind", "heat"}, {"case", CoolingAssistant::kCase}, {"on", {st->chip}}, {"value", 3}});
        require(!st->tray.empty() && !st->cover.empty() && !st->sink.empty() && !st->desk.empty() && !st->fanUnit.empty() && !st->sinkUnit.empty(),
                "a tray, its cover, a board, a chip, a heatsink and a fin (a component), a fan block and a hub (a component), a desk");
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
      case 2: {  // the box found (by a worker): the tray and the cover picked, shown selected, the page picking
        if (!c || !c->isVisible() || !c->settled()) return;
        require(c->pickedBodies() == std::vector<std::string>{st->tray, st->cover}, QString("the box's bodies picked: the tray and the cover (%1 picked)").arg(c->pickedBodies().size()));
        require(c->picking(), "the box's page picks in the view and the browser");
        const auto sel = area->services().selection().ids;
        require(has(sel, st->tray) && has(sel, st->cover) && sel.size() == 2, QString("they are what is selected (%1 selected)").arg(sel.size()));
        const auto in = inside();
        require(has(in, st->board) && has(in, st->chip) && has(in, st->sink) && has(in, st->fan) && !has(in, st->cover) && !has(in, st->desk),
                QString("inside both: the board, the chip, the heatsink, the fan block (%1 parts)").arg(in.size()));
        c->grab().save(prefix + ".box.png");
        pick({st->tray});  // the cover clicked again: out
        st->phase = 3;
        return;
      }
      case 3: {
        if (c->pickedBodies() != std::vector<std::string>{st->tray}) return;
        require(!has(inside(), st->sink), "the cover taken out: the heatsink, standing past the tray's rim, is no longer inside");
        pick({st->tray, st->cover, st->desk});  // the cover and the desk picked (as rows of the browser with Ctrl)
        st->phase = 4;
        return;
      }
      case 4: {
        if (c->pickedBodies().size() != 3) return;
        require(c->pickedBodies() == std::vector<std::string>{st->tray, st->cover, st->desk} && has(inside(), st->sink) && has(inside(), st->board),
                "the cover and the desk picked: a box of three bodies, the parts inside");
        pick({st->tray, st->cover});
        st->phase = 5;
        return;
      }
      case 5: {  // the Heat page picks the parts that make heat (bodies), then faces: a chip joined into the board has no body
        if (c->pickedBodies().size() != 2) return;
        c->open(1);
        require(c->pickingWhat() == Pick::Parts && w.action("select.bodies")->isChecked(), "the Heat page picks the parts that make heat (the Bodies filter)");
        const auto sel = area->services().selection().ids;
        require(sel == std::vector<std::string>{st->chip}, QString("the chip, making heat, shown selected (%1 selected)").arg(sel.size()));
        pick({st->chip, st->board});  // the board clicked
        st->phase = 50;
        return;
      }
      case 50: {
        if (ticked().size() != 2) return;
        require(has(ticked(), st->board), "the board clicked: ticked as making heat");
        pick({st->chip});  // clicked again: out
        st->phase = 501;
        return;
      }
      case 501: {
        if (ticked() != std::vector<std::string>{st->chip}) return;
        pickBox("coolingHeatFaces")->click();
        require(c->pickingFaces() && w.action("select.faces")->isChecked(), "Faces that make heat clicked: it picks faces (the Faces filter on)");
        require(c->pickedBodies().size() == 2, "the box's bodies kept through the filter's switch");
        area->services().select({face(st->board, 5)});
        st->phase = 51;
        return;
      }
      case 51: {
        if (c->heatFaces().empty()) {  // the faces' pick targets come once the filter is on (a sliced job): clicked then
          area->services().select({face(st->board, 5)});
          return;
        }
        const int r = c->heatTable()->rowCount() - 1;
        require(c->heatTable()->item(r, 0)->text().contains("Board") && !(c->heatTable()->item(r, 2)->flags() & Qt::ItemIsEnabled),
                "the face a row of the heat table, named after its body, no board tick: " + c->heatTable()->item(r, 0)->text());
        c->heatTable()->item(r, 1)->setData(Qt::EditRole, 2.5);
        w.action("select.bodies")->trigger();  // the filter switched by hand: the selection cleared, the face kept
        require(c->heatFaces().size() == 1, "the Bodies filter switched to by hand: the face picked kept");
        w.action("select.faces")->trigger();
        st->phase = 52;
        return;
      }
      case 52: {  // back in the Faces filter, the face is shown selected again (a moment later)
        const auto refs = area->services().selection().refs;
        if (refs.size() != 1) return;
        require(refs[0].kind == opad::Ref::Kind::Face && refs[0].body == st->board && refs[0].index == 5 && c->heatFaces().size() == 1,
                "back in the Faces filter: the face shown selected again");
        require(c->heatTable()->item(c->heatTable()->rowCount() - 1, 1)->data(Qt::EditRole).toDouble() == 2.5, "its power kept");
        c->grab().save(prefix + ".heat.png");
        c->open(0);
        require(c->pickingWhat() == Pick::Box && w.action("select.bodies")->isChecked() && c->pickedBodies().size() == 2, "the box's page picks bodies again");
        c->open(2);
        require(c->pickingWhat() == Pick::Fan && w.action("select.bodies")->isChecked(), "the Air page picks the fan (the Bodies filter)");
        require(c->fans().size() == 1 && c->fans()[0].on == std::vector<std::string>{st->fan}, "a first guess: the body called Fan");
        st->phase = 53;
        return;
      }
      case 53: {  // the fan a component: one part (its row in the browser clicked, once the filter is applied)
        if (c->fans()[0].on != std::vector<std::string>{st->fanUnit}) {
          area->services().select({body(st->fanUnit)});
          return;
        }
        require(pickBox("coolingFanPick")->what().contains("component"), "the fan is the component Fan unit, one part: " + pickBox("coolingFanPick")->what());
        pickBox("coolingWayPick")->click();
        require(c->pickingWhat() == Pick::Way && w.action("select.faces")->isChecked(), "Air goes through clicked: it picks a face");
        st->phase = 54;
        return;
      }
      case 54: {  // a face of the fan block, once the faces are pickable
        if (c->fans()[0].across.body.empty()) {
          area->services().select({face(st->fan, 0)});
          return;
        }
        const opad::Vec3 v = c->fans()[0].vector();
        if (std::hypot(c->fans()[0].normal[0], c->fans()[0].normal[1], c->fans()[0].normal[2]) < 0.5) return;  // measured on a worker
        st->way = v;
        require(std::fabs(std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) - 1) < 1e-6, QString("the air goes across the face: along an axis of the block (%1, %2, %3)").arg(v[0]).arg(v[1]).arg(v[2]));
        c->findChild<QPushButton*>("coolingWayFlip")->click();
        const opad::Vec3 f = c->fans()[0].vector();
        require(f[0] == -v[0] && f[1] == -v[1] && f[2] == -v[2], "Flip turns the way round");
        auto* model = c->findChild<QComboBox*>("coolingFanModel");
        auto* flow = c->findChild<QDoubleSpinBox*>("coolingFanFlow");
        require(model->isEditable() && !flow->isVisibleTo(c), "the fan model a drop list to search; a library fan hides the custom numbers");
        model->setCurrentIndex(model->count() - 1);  // Custom
        require(flow->isVisibleTo(c) && flow->isEnabled(), "Custom shows its free flow and shut-off pressure under it");
        flow->setValue(12);
        c->findChild<QDoubleSpinBox*>("coolingFanPressure")->setValue(30);
        pickBox("coolingSinkPick")->click();
        require(c->pickingWhat() == Pick::Sink && w.action("select.bodies")->isChecked(), "Heatsink clicked: it picks bodies or a component");
        st->phase = 55;
        return;
      }
      case 55: {
        if (c->fans()[0].sink != std::vector<std::string>{st->sinkUnit}) {
          area->services().select({body(st->sinkUnit)});
          return;
        }
        auto* material = c->findChild<QComboBox*>("coolingSinkMaterial");
        require(material->isEnabled() && material->isEditable(), "the heatsink's material: a drop list to search, once a heatsink is picked");
        material->setCurrentIndex(material->findData("copper"));
        require(c->fans()[0].material == "copper", "copper chosen for the whole heatsink");
        c->grab().save(prefix + ".air.png");
        c->open(3);
        require(!c->picking() && w.action("select.bodies")->isChecked() && !c->findChild<QPushButton*>("coolingNext")->isVisible(),
                "the Run page picks nothing, the view's filter as it was; no Next on the last page");
        c->open(0);
        require(c->apply(), "Apply writes the case");
        st->phase = 6;
        return;
      }
      case 6: {  // the study names both walls, the face makes its heat; opened again, they are picked from it
        opad::json enclosure;
        for (const auto& study : doc->scene.studies)
          if (study.name == CoolingAssistant::studyName().toStdString()) enclosure = study.def.value("settings", opad::json::object()).value("cfd", opad::json::object()).value("enclosure", opad::json());
        require(enclosure == opad::json::array({st->tray, st->cover}), "the study's enclosure is the tray and the cover: " + QString::fromStdString(enclosure.dump()));
        int onFace = 0, onChip = 0;
        for (const auto& l : doc->scene.loads)
          if (l.kind == "heat" && l.load_case == CoolingAssistant::kCase && l.refs.size() == 1) {
            const auto& r = l.refs.front();
            onFace += r.kind == opad::Ref::Kind::Face && r.body == st->board && r.index == 5 && l.def.value("value", 0.0) == 2.5;
            onChip += r.kind == opad::Ref::Kind::Body && r.body == st->chip && l.def.value("value", 0.0) == 3;
          }
        require(onFace == 1 && onChip == 1, QString("heat loads: 2.5 W on the board's face, 3 W in the chip (%1, %2)").arg(onFace).arg(onChip));
        const opad::Load* fan = nullptr;
        for (const auto& l : doc->scene.loads)
          if (l.kind == "fan" && l.load_case == CoolingAssistant::kCase) fan = &l;
        require(fan && fan->refs.size() == 1 && fan->refs[0].body == st->fanUnit && fan->def.value("heatsink", opad::json()) == opad::json::array({st->sinkUnit}) &&
                    fan->def.value("fan", opad::json()) == opad::json{{"flow", 12.0}, {"pressure", 30.0}} && fan->def.contains("across") &&
                    fan->def.value("vector", opad::Vec3{}) == opad::Vec3{-st->way[0], -st->way[1], -st->way[2]},
                "the fan load: on the component, through the heatsink component, custom 12 m3/h at 30 Pa, the way flipped: " +
                    QString::fromStdString(fan ? fan->def.dump() : std::string()));
        opad::json mats;
        for (const auto& study : doc->scene.studies)
          if (study.name == CoolingAssistant::studyName().toStdString()) mats = study.def.value("settings", opad::json::object()).value("materials", opad::json::object());
        require(mats.value(st->sink, opad::json()).value("material", "") == "copper" && mats.value(st->fin, opad::json()).value("material", "") == "copper",
                "the heatsink's two bodies both copper in the study");
        c->close();
        require(!c->picking(), "closed: the view selects again");
        c->open(1);
        st->phase = 7;
        return;
      }
      case 7: {
        if (!c->settled()) return;
        require(c->pickedBodies() == std::vector<std::string>{st->tray, st->cover}, "opened again: the tray and the cover picked");
        const int r = c->heatTable()->rowCount() - 1;
        require(c->heatFaces().size() == 1 && c->pickingFaces() && c->heatTable()->item(r, 1)->data(Qt::EditRole).toDouble() == 2.5,
                "opened again on the Heat page: the face picked, with its 2.5 W");
        const auto& f = c->fans();
        require(f.size() == 1 && f[0].on == std::vector<std::string>{st->fanUnit} && f[0].sink == std::vector<std::string>{st->sinkUnit} && f[0].material == "copper" &&
                    f[0].model.isEmpty() && f[0].flow == 12 && !f[0].across.body.empty() && f[0].vector() == opad::Vec3{-st->way[0], -st->way[1], -st->way[2]},
                "opened again: the fan as set (its component, the face and way, custom numbers, the heatsink and its copper)");
        c->close();
        require(w.action("select.bodies")->isChecked(), "closed: the Bodies filter as before");
        return finish();
      }
    }
  });
  timer->start();
  return true;
}

// OPAD_BENCH_COOLING_PERF=<prefix>, meant for the Engine (or any big model): once its bodies are on screen (unhidden in memory,
// never saved), Thermal setup opened from its command: the command returns at once and nothing stalls the UI over 250 ms while
// the bodies' boxes are measured and the box is found (a worker); then a body picked and taken out takes a few milliseconds
// (from the boxes kept). Opening it used to measure every body's shape again per candidate box on the UI thread: a minute.
OPAD_BENCH(OPAD_BENCH_COOLING_PERF, coolingPerf) {
  struct State {
    int phase = 0, ticks = 0, settled = 0;
    bool all = true;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  auto* timer = new QTimer(&w);
  timer->setInterval(50);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer] {
    AppDocument* doc = w.m_doc;
    Simulate* area = w.findChild<Simulate*>();
    CoolingAssistant* c = w.findChild<CoolingAssistant*>();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: cooling perf: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    if (++st->ticks > 12000) {
      require(false, QString("timed out in phase %1").arg(st->phase));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0;
    switch (st->phase) {
      case 0: {  // its bodies on screen
        if (busy) return;
        const auto bodies = doc->scene.all_bodies();
        if (std::none_of(bodies.begin(), bodies.end(), [doc](const std::string& id) { return doc->scene.effectively_visible(id); })) {
          trace::log("bench: cooling perf: nothing visible, unhiding (in memory, never saved)");
          std::vector<std::string> hidden;
          for (const auto& [id, node] : doc->scene.nodes)
            if (!node.visible) hidden.push_back(id);
          for (const auto& id : hidden) doc->run("appearance", {{"target", id}, {"visible", true}});
          return;
        }
        if (++st->settled < 20 || w.m_jobs->busy()) return;
        trace::resetStalls();
        QElapsedTimer t;
        t.start();
        area->services().action("simulate.cooling")->trigger();
        const qint64 call = t.elapsed();
        require(call < 250, QString("the command returns in %1 ms with %2 bodies").arg(call).arg(doc->scene.all_bodies().size()));
        st->clock.start();
        st->phase = 1;
        return;
      }
      case 1: {  // the box found on a worker, the UI never held
        if (!c || !c->settled()) return;
        const trace::Stalls stalls = trace::stalls();
        require(stalls.longest < 250, QString("the box found in %1 ms, the longest stall meanwhile %2 ms").arg(st->clock.elapsed()).arg(stalls.longest));
        require(c->picking() && !c->pickedBodies().empty(), QString("the box's page picks, %1 bodies picked").arg(c->pickedBodies().size()));
        // One more body picked, then taken out again (what the app's selection hands it): what is inside follows from the boxes.
        std::vector<std::string> more = c->pickedBodies();
        for (const auto& id : doc->scene.all_bodies())
          if (std::find(more.begin(), more.end(), id) == more.end()) {
            more.push_back(id);
            break;
          }
        QElapsedTimer t;
        t.start();
        const std::vector<std::string> before = c->pickedBodies();
        c->selectionChanged(more, {});
        c->selectionChanged(before, {});
        require(t.elapsed() < 250 && c->pickedBodies() == before, QString("a body picked and taken out in %1 ms (what is inside follows from the boxes)").arg(t.elapsed()));
        c->close();
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
