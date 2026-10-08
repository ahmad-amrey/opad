// Align (bodies, components, sketches) and the speed of moving many bodies at once.
//
// OPAD_BENCH_ALIGN=<prefix> on tools/bench_cases/align.py's document (a base block, a smaller block beside it, a sketch on XY
// with a boss extruded from it): Align with the block, its bottom face and the base's top face given previews the block
// drawn moved (its own object: nothing meshed, nothing stands in), Enter commits it (on the base, the same body key: placed,
// not rebuilt); Align sketch with the base's top as To previews the sketch drawn there and the boss regenerated, Enter
// commits it (the sketch on the top, linked to that face, the boss on it); undone, both are back. Frames:
// <prefix>.align-panel.png, .sketch-panel.png, .align-view.png.
//
// OPAD_BENCH_MOVEPERF=<prefix>, meant for the Engine: once its bodies are on screen (unhidden in memory, never saved), Move
// with every root picked and then with every body picked, 10 mm up, and Align of the roots onto XY lifted 10 mm (then, for
// comparison, Move with a copy, which rebuilds every body as Move did before: its preview only): each
// preview's time from the start to every body drawn moved, none meshed; each commit's time to the bodies in place; undone.
// The stalls are the trace's (OPAD_TRACE).
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QTimer>

#include <BRepAdaptor_Surface.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"

namespace {
// The planar face of a body facing `n` (world): "uuid/face/N", empty when none.
std::string faceToward(const AppDocument* doc, const std::string& body, const gp_Dir& n) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(opad::node_world_shape(doc->doc, doc->scene, body), TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const TopoDS_Face f = TopoDS::Face(faces(i));
    const BRepAdaptor_Surface a(f);
    if (a.GetType() != GeomAbs_Plane) continue;
    gp_Dir d = a.Plane().Axis().Direction();
    if (!a.Plane().Position().Direct()) d.Reverse();
    if (f.Orientation() == TopAbs_REVERSED) d.Reverse();
    if (d.Dot(n) > 1 - 1e-9) return body + "/face/" + std::to_string(i - 1);
  }
  return {};
}

void pressReturn(QWidget* window, QWidget* view) {
  QApplication::setActiveWindow(window);
  QWidget* to = QApplication::focusWidget();
  QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
  QApplication::sendEvent(to ? to : view, &press);
}

double lowest(const AppDocument* doc, const std::string& node) {
  double x0, y0, z0, x1, y1, z1;
  opad::node_tight_bbox(doc->doc, doc->scene, node).Get(x0, y0, z0, x1, y1, z1);
  return z0;
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_ALIGN, align) {
  struct State {
    int phase = 0, ticks = 0;
    bool all = true;
    std::string base, block, sketch, boss, bossOp, top, bottom, blockKey;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  const QString prefix = value;
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, prefix] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    DesignController* design = w.m_design;
    FeaturePanel* form = design->featurePanel();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: align: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    if (++st->ticks > 1200) {
      require(false, QString("timed out in phase %1").arg(st->phase));
      return finish();
    }
    if (doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending()) return;
    switch (st->phase) {
      case 0: {  // the document: two blocks and a sketch; the boss extruded from it here
        for (const auto& id : doc->scene.all_bodies()) {
          const opad::Node* n = doc->node(id);
          if (n->name.rfind("Base", 0) == 0) st->base = id;
          if (n->name.rfind("Block", 0) == 0) st->block = id;
        }
        if (!doc->scene.sketches.empty()) st->sketch = doc->scene.sketches.front().id;
        if (!require(!st->base.empty() && !st->block.empty() && !st->sketch.empty(), "the base, the block and the sketch")) return finish();
        doc->run("feature", {{"kind", "extrude"}, {"name", "Boss"}, {"inputs", {{"profiles", {{{"sketch", st->sketch}, {"at", {-34, -36}}}}}, {"distance", "4 mm"}}}});
        st->phase = 1;
        return;
      }
      case 1: {  // Align, given its picks: the block's bottom onto the base's top
        for (const auto& f : doc->scene.features)
          if (f.kind == "extrude") st->bossOp = f.id, st->boss = f.result.value("bodies", opad::json::array()).empty() ? std::string() : f.result["bodies"][0].value("id", "");
        if (!require(!st->boss.empty(), "the boss extruded from the sketch")) return finish();
        st->top = faceToward(doc, st->base, gp_Dir(0, 0, 1));
        st->bottom = faceToward(doc, st->block, gp_Dir(0, 0, -1));
        st->blockKey = doc->node(st->block)->body_key;
        QApplication::setActiveWindow(&w);
        st->clock.start();
        design->startFeature("align", {{"bodies", opad::json::array({{{"body", st->block}, {"kind", "body"}}})},
                                       {"from_plane", {{"face", st->bottom}}}, {"to_plane", {{"face", st->top}}}});
        st->phase = 2;
        return;
      }
      case 2: {  // previewed: the block's own object drawn moved, nothing meshed or stood in
        if (view->previewMovedCount() == 0 && st->clock.elapsed() < 20000) return;
        if (view->previewMovedCount() == 0)
          trace::log("bench: align: the form says \"" + form->statusText() + "\" with " + QString::fromStdString(form->inputs().dump()));
        require(design->featureActive() && view->previewMovedCount() == 1 && view->previewBodyCount() == 0,
                QString("the preview draws the block moved (%1 moved, %2 meshed) in %3 ms").arg(view->previewMovedCount()).arg(view->previewBodyCount()).arg(st->clock.elapsed()));
        require(std::abs(view->previewMotion(st->block).TranslationPart().Z() - 10) < 1e-6, "by 10 mm up, onto the base's top");
        form->window()->grab().save(prefix + ".align-panel.png");
        view->grabImage().save(prefix + ".align-view.png");
        pressReturn(&w, view);
        st->phase = 3;
        return;
      }
      case 3: {
        if (design->featureActive()) return;
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        require(f && f->kind == "align" && f->result.contains("placements"), "Enter committed an Align that places the block");
        require(std::abs(lowest(doc, st->block) - 10) < 1e-6 && doc->node(st->block)->body_key == st->blockKey, "the block on the base, its body key kept");
        if (view->benchLookState(st->sketch).empty()) {  // hidden once the boss took its profile: shown, so its move is seen
          trace::log("bench: align: the sketch is hidden, showing it");
          doc->run("appearance", {{"target", st->sketch}, {"visible", true}});
          return;
        }
        design->startSketchAlign(st->sketch);
        require(form->activeInput() == "to_plane", "Align sketch waits for To (From is the sketch's plane): " + form->activeInput());
        st->clock.start();
        st->phase = 4;
        return;
      }
      case 4: {  // its To box opened the plane picker: the base's top chosen there, as a click on it does
        if (!design->planePicker()->active() && st->clock.elapsed() < 5000) return;
        if (!require(design->planePicker()->active(), "the plane picker is open for To")) return finish();
        design->planePicker()->choose({{"face", opad::Ref::parse(st->top).to_json()}});
        st->clock.start();
        st->phase = 5;
        return;
      }
      case 5: {  // the sketch drawn on the top, the boss previewed there
        if ((!view->previewSketchMoved() || view->previewBodyCount() == 0) && st->clock.elapsed() < 20000) return;
        if (!view->previewSketchMoved() || view->previewBodyCount() == 0)
          trace::log("bench: align: the form says \"" + form->statusText() + "\" with " + QString::fromStdString(form->inputs().dump()) +
                     (view->previewSketchMoved() ? " (the sketch drawn moved)" : ""));
        require(design->aligningSketch() == st->sketch && view->previewSketchMoved() && view->previewBodyCount() >= 1,
                QString("Align sketch previews the sketch moved and the boss on it (%1 previewed) in %2 ms").arg(view->previewBodyCount()).arg(st->clock.elapsed()));
        form->window()->grab().save(prefix + ".sketch-panel.png");
        pressReturn(&w, view);
        st->phase = 6;
        return;
      }
      case 6: {
        if (design->featureActive()) return;
        const opad::SketchItem* sk = doc->scene.sketch(st->sketch);
        require(sk && std::abs(sk->frame.origin[2] - 10) < 1e-6 && sk->plane.contains("support"), "the sketch on the base's top, linked to that face");
        require(std::abs(lowest(doc, st->boss) - 10) < 1e-6, "the boss followed it");
        doc->undo(3);  // the sketch aligned, shown, the block aligned
        st->phase = 7;
        return;
      }
      case 7: {
        const opad::SketchItem* sk = doc->scene.sketch(st->sketch);
        require(std::abs(lowest(doc, st->block)) < 1e-6 && sk && std::abs(sk->frame.origin[2]) < 1e-6, "undone: the block and the sketch back");
        return finish();
      }
    }
  });
  timer->start();
  return true;
}

OPAD_BENCH(OPAD_BENCH_MOVEPERF, movePerf) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, run = 0;
    bool all = true;
    std::vector<std::string> picks;
    size_t expect = 0;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  auto* timer = new QTimer(&w);
  timer->setInterval(50);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    DesignController* design = w.m_design;
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: move perf: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    if (++st->ticks > 24000) {
      require(false, QString("timed out in phase %1").arg(st->phase));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending();
    // The last: Move with a copy, which still rebuilds every body as Move did before it placed them (previewed, then cancelled).
    static const char* runs[] = {"Move, every root", "Move, every body", "Align, every root", "Move with a copy, every root (rebuilt, as Move was before)"};
    switch (st->phase) {
      case 0: {  // its bodies on screen
        if (busy) return;
        const auto bodies = doc->scene.all_bodies();
        if (std::none_of(bodies.begin(), bodies.end(), [doc](const std::string& id) { return doc->scene.effectively_visible(id); })) {
          trace::log("bench: move perf: nothing visible, unhiding (in memory, never saved)");
          std::vector<std::string> hidden;
          for (const auto& [id, node] : doc->scene.nodes)
            if (!node.visible) hidden.push_back(id);
          for (const auto& id : hidden) doc->run("appearance", {{"target", id}, {"visible", true}});
          return;
        }
        if (++st->settled < 20) return;
        st->phase = 1;
        return;
      }
      case 1: {  // the next run started, given its picks
        if (busy) return;
        if (st->run >= 4) return finish();
        st->picks.clear();
        st->expect = 0;
        const bool everyBody = st->run == 1;
        for (const auto& id : everyBody ? doc->scene.all_bodies() : doc->scene.roots)
          if (doc->scene.effectively_visible(id) && !doc->scene.effectively_locked(id) && !doc->node(id)->linked) {
            st->picks.push_back(id);
            st->expect += everyBody || doc->node(id)->kind == opad::Node::Kind::Body ? 1 : doc->scene.bodies_under(id).size();
          }
        opad::json refs = opad::json::array();
        for (const auto& id : st->picks) refs.push_back({{"body", id}, {"kind", "body"}});
        QApplication::setActiveWindow(&w);
        st->clock.start();
        if (st->run == 2)
          design->startFeature("align", {{"bodies", refs}, {"from_plane", {{"base", "xy"}}}, {"to_plane", {{"base", "xy"}}}, {"offset", "10 mm"}, {"flip", true}});
        else
          design->startFeature("move", {{"bodies", refs}, {"dz", "10 mm"}, {"copy", st->run == 3}});
        trace::log(QStringLiteral("bench: move perf: %1: %2 picked, %3 bodies, started in %4 ms").arg(runs[st->run]).arg(st->picks.size()).arg(st->expect).arg(st->clock.elapsed()));
        st->phase = 2;
        return;
      }
      case 2: {  // previewed: every body drawn moved, none meshed
        if (st->run == 3) {  // the baseline: every copy rebuilt and meshed for its preview
          if (view->previewBodyCount() < st->expect && st->clock.elapsed() < 600000) return;
          trace::log(QStringLiteral("bench: move perf: %1: previewed in %2 ms (%3 meshed)").arg(runs[st->run]).arg(st->clock.elapsed()).arg(view->previewBodyCount()));
          design->escape();
          ++st->run;
          st->phase = 1;
          return;
        }
        if (view->previewMovedCount() < st->expect && st->clock.elapsed() < 120000) return;
        const qint64 ms = st->clock.elapsed();
        require(view->previewMovedCount() >= st->expect && view->previewBodyCount() == 0,
                QStringLiteral("%1: previewed in %2 ms (%3 drawn moved, %4 meshed)").arg(runs[st->run]).arg(ms).arg(view->previewMovedCount()).arg(view->previewBodyCount()));
        st->clock.start();
        pressReturn(&w, view);
        st->phase = 3;
        return;
      }
      case 3: {  // committed: in place
        if (design->featureActive() || busy) return;
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        require(f && f->result.contains("placements") && !f->result.contains("bodies"),
                QStringLiteral("%1: committed in %2 ms, placed (no body rebuilt)").arg(runs[st->run]).arg(st->clock.elapsed()));
        st->clock.start();
        doc->undo();
        st->phase = 4;
        return;
      }
      case 4: {
        if (busy) return;
        trace::log(QStringLiteral("bench: move perf: %1: undone in %2 ms").arg(runs[st->run]).arg(st->clock.elapsed()));
        ++st->run;
        st->phase = 1;
        return;
      }
    }
  });
  timer->start();
  return true;
}
