// OPAD_BENCH_COOLING_WALLS=<prefix>: the Cooling assistant on an enclosure of two bodies, a tray and the cover on it (built
// here on an empty document: a board, a 3 W chip, a heatsink standing up past the tray's rim, a fan block, a desk under it).
// Opened from its command, the box's bodies picked are the tray and the cover, shown selected, the page picking; the parts
// inside are those within both, the heatsink too. Picked through the app's selection (what clicks in the view and rows of the
// browser make): the cover taken out, the heatsink is no longer inside; the cover and the desk picked, a box of three; the
// Heat page picks faces (the Faces filter on): a face of the board (a chip joined into it) becomes a heat row with its power,
// kept when the filter is switched by hand and shown selected again when it is back; the Air page picks nothing and the
// filter is the one before; Apply writes the study's enclosure as the bodies picked and a heat load on the face; closed, the
// view selects again; opened again, the bodies and the face are picked from the study. Frames: <prefix>.box.png, .heat.png.
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QTableWidget>
#include <QTimer>

#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "Jobs.hpp"
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
    auto face = [](const std::string& body, int index) {
      opad::Ref r;
      r.body = body;
      r.kind = opad::Ref::Kind::Face;
      r.index = index;
      return r;
    };
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
      case 5: {  // the Heat page picks faces: a chip joined into the board has none of its own body
        if (c->pickedBodies().size() != 2) return;
        c->open(1);
        require(c->pickingFaces() && w.action("select.faces")->isChecked(), "the Heat page picks faces (the Faces filter on)");
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
        const bool box = c->picking() && !c->pickingFaces() && w.action("select.bodies")->isChecked() && c->pickedBodies().size() == 2;
        c->open(2);
        require(box && !c->picking() && w.action("select.bodies")->isChecked(),
                "the box's page picks bodies again; the Air page picks nothing, the view's filter as it was");
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
