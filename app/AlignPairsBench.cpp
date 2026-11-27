// OPAD_BENCH_ALIGN_PAIRS=<prefix> on a document holding a block ("Block", a solid from the origin) and its scan (a noisy mesh
// moved away by a known move; gui case align-pairs, tools/bench_cases/align.py): Align is started on the scan in Point
// pairs; the first point is clicked in the view, the rest picked as clicks pick them (the selection, then the controller's
// selection handler), corner by corner, the scan's then the block's. Along the way: the picks alternate between the sides,
// numbered markers stand at both points of every pair, no preview before three whole pairs, then the scan drawn moved with
// each marker line as short as the noise; a pick waiting for its match keeps the preview; a fifth pair picked wrong is left
// out and named (red marker, "left out" in the list); Esc takes the last picks back one at a time; Remove in the list
// drops a pair; Enter commits, and the scan's nodes measured on a worker lie on the block. Every step keeps the UI thread
// under the stall limit (250 ms): "bench: align pairs: ... PASS".
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QTimer>
#include <QToolButton>

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <BRep_Tool.hxx>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"
#include "opad/scan_fit.hpp"

namespace {
constexpr qint64 kStallLimitMs = 250;

// The scan's known move (tools/bench_cases/align.py _pairs_move): 25 degrees about (1, 1, 2) through the origin, then a shift.
gp_Trsf scanMove() {
  gp_Trsf r, t;
  r.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 1, 2)), 25 * M_PI / 180);
  t.SetTranslation(gp_Vec(180, 30, 20));
  return t * r;
}

int count(const opad::json& p) { return p.is_array() ? int(p.size()) : p.is_null() ? 0 : 1; }
}  // namespace

OPAD_BENCH(OPAD_BENCH_ALIGN_PAIRS, alignPairs) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, step = 0;
    bool all = true, jobDone = false, jobOk = false;
    std::string scan, block;
    // Pair by pair: the scan's node and the block's vertex ("kind": "vertex"), and the block's corner (world).
    std::vector<opad::Ref> from, to;
    std::vector<gp_Pnt> corners;
    opad::json measured;
    QString stepName;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  const QString prefix = value;
  auto* timer = new QTimer(&w);
  timer->setInterval(20);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, prefix] {
    AppDocument* doc = w.m_doc;
    Viewport* view = w.m_viewport;
    DesignController* design = w.m_design;
    FeaturePanel* form = design->featurePanel();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: align pairs: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
      st->all = st->all && ok;
      return ok;
    };
    auto finish = [timer, st] {
      timer->stop();
      QCoreApplication::exit(st->all ? 0 : 2);
    };
    auto begin = [st](const QString& step) {
      st->stepName = step;
      trace::resetStalls();
      st->clock.start();
    };
    auto end = [st, require](const QString& extra = {}) {
      const trace::Stalls s = trace::stalls();
      require(s.longest < kStallLimitMs, QStringLiteral("%1: %2 ms, longest stall %3 ms (%4 stalls)%5")
                                             .arg(st->stepName).arg(st->clock.elapsed()).arg(s.longest).arg(s.count).arg(extra.isEmpty() ? QString() : ", " + extra));
    };
    auto key = [&w, form](int k) {
      QApplication::setActiveWindow(&w);
      QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, k == Qt::Key_Return ? "\r" : QString());
      QApplication::sendEvent(form, &press);
    };
    // A pick as a click makes it: the active side's picks and this one selected, then the controller's handler.
    auto pick = [view, design, form](const opad::Ref& r) {
      std::vector<opad::Ref> refs;
      const opad::json had = form->picks(form->activeInput());
      for (const auto& p : had.is_array() ? had : opad::json::array())
        try {
          refs.push_back(opad::Ref::from_json(p));
        } catch (const std::exception&) {
        }
      refs.push_back(r);
      view->selectRefs(refs);
      design->viewportSelectionChanged();
    };
    auto markers = [design] { return design->benchPairMarkers(); };
    // Waiting for the preview and the markers to settle.
    auto settled = [design, view] { return !design->previewing() && !design->pairsResolving() && !view->looksPending(); };
    if (++st->ticks > 30000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->stepName));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || view->filterSwitching();
    switch (st->phase) {
      case 0: {  // loaded and drawn: the scan and the block
        if (busy || ++st->settled < 25) return;
        for (const auto& id : doc->scene.all_bodies()) {
          const opad::Node* n = doc->node(id);
          if (!n) continue;
          if (n->representation == "mesh") st->scan = id;
          else if (n->name == "Block") st->block = id;
        }
        if (!require(!st->scan.empty() && !st->block.empty(), "a scan mesh and the Block solid")) return finish();
        // The corners to pair, found on a worker: the scan's node nearest each moved corner, the block's vertex at it. The
        // fifth pair is wrong on purpose: the scan's (100, 60, 0) against the block's (0, 0, 40).
        begin("corners found (worker)");
        auto docCopy = std::make_shared<opad::Document>(doc->doc);
        auto scene = std::make_shared<opad::Scene>(doc->scene);
        auto out = std::make_shared<opad::json>();
        const std::string scan = st->scan, block = st->block;
        w.m_jobs->async(QStringLiteral("Finding the corners"), [docCopy, scene, scan, block, out](Progress) {
          const auto m = opad::scanfit::fit_mesh(opad::node_world_shape(*docCopy, *scene, scan));
          TopTools_IndexedMapOfShape vs;
          TopExp::MapShapes(opad::node_world_shape(*docCopy, *scene, block), TopAbs_VERTEX, vs);
          const std::vector<std::pair<gp_Pnt, gp_Pnt>> pairs = {{gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 0)},       {gp_Pnt(100, 0, 0), gp_Pnt(100, 0, 0)},
                                                                {gp_Pnt(100, 60, 40), gp_Pnt(100, 60, 40)}, {gp_Pnt(0, 60, 0), gp_Pnt(0, 60, 0)},
                                                                {gp_Pnt(100, 60, 0), gp_Pnt(0, 0, 40)}};
          *out = opad::json::array();
          for (const auto& [a, b] : pairs) {
            const gp_Pnt moved = a.Transformed(scanMove());
            size_t node = 0;
            for (size_t i = 1; i < m->nodes.size(); ++i)
              if (m->nodes[i].Distance(moved) < m->nodes[node].Distance(moved)) node = i;
            int vertex = 0;
            for (int i = 1; i <= vs.Extent(); ++i)
              if (BRep_Tool::Pnt(TopoDS::Vertex(vs(i))).Distance(b) < 1e-6) vertex = i - 1;
            out->push_back({{"node", node}, {"vertex", vertex}, {"corner", {b.X(), b.Y(), b.Z()}}, {"off", m->nodes[node].Distance(moved)}});
          }
        }, [st, out](bool ok, const QString& error) {
          st->jobDone = true;
          st->jobOk = ok;
          st->measured = *out;
          if (!ok) trace::log("bench: align pairs: corners failed: " + error);
        });
        st->phase = 1;
        return;
      }
      case 1: {
        if (!st->jobDone) return;
        end();
        if (!require(st->jobOk && st->measured.size() == 5, "five pairs of corners")) return finish();
        for (const auto& c : st->measured) {
          opad::Ref a, b;
          a.body = st->scan, a.kind = opad::Ref::Kind::Vertex, a.index = c["node"].get<int>();
          b.body = st->block, b.kind = opad::Ref::Kind::Vertex, b.index = c["vertex"].get<int>();
          st->from.push_back(a), st->to.push_back(b);
          st->corners.push_back(gp_Pnt(c["corner"][0].get<double>(), c["corner"][1].get<double>(), c["corner"][2].get<double>()));
        }
        require(st->measured[0]["off"].get<double>() < 0.2, "the scan's corner nodes are where the move puts the corners");
        QApplication::setActiveWindow(&w);
        begin("Align started on the scan, Point pairs");
        design->startFeature("align", {{"bodies", opad::json::array({{{"body", st->scan}, {"kind", "body"}}})}, {"from", "point_pairs"}});
        st->phase = 2;
        return;
      }
      case 2: {  // the bodies' side takes the clicks
        if ((busy || !design->featureActive() || form->activeInput() != "from_points") && st->clock.elapsed() < 20000) return;
        end(QStringLiteral("active %1").arg(form->activeInput()));
        if (!require(form->activeInput() == "from_points" && form->pairInputs().first == "from_points", "the points on the bodies are picked first")) return finish();
        // The first point clicked in the view: the scan's corner nearest the eye (nothing in front of it).
        const opad::Vec3 dir = view->viewDirection();
        size_t nearest = 0;
        double best = 1e300;
        for (size_t i = 0; i < 4; ++i) {
          const gp_Pnt p = st->corners[i].Transformed(scanMove());
          if (const double d = p.X() * dir[0] + p.Y() * dir[1] + p.Z() * dir[2]; d < best) best = d, nearest = i;
        }
        std::swap(st->from[0], st->from[nearest]), std::swap(st->to[0], st->to[nearest]), std::swap(st->corners[0], st->corners[nearest]);
        const gp_Pnt at = st->corners[0].Transformed(scanMove());
        begin("the first corner clicked on the scan");
        view->benchClickAt(QPointF(view->widgetPoint({at.X(), at.Y(), at.Z()})));
        st->phase = 3;
        return;
      }
      case 3: {  // picked: the target's side next, a marker at the point
        const opad::json m = markers();
        const bool ready = count(form->picks("from_points")) == 1 && m.size() == 1 && m[0].contains("from") && settled();
        if (!ready && st->clock.elapsed() < 20000) return;
        end();
        if (!require(ready, QStringLiteral("the click picked a point on the scan (%1)").arg(QString::fromStdString(form->picks("from_points").dump())))) return finish();
        const gp_Pnt want = st->corners[0].Transformed(scanMove());
        const opad::json f = m[0]["from"];
        const double off = gp_Pnt(f[0].get<double>(), f[1].get<double>(), f[2].get<double>()).Distance(want);
        // A click takes the node nearest the pointer that picking finds: the corner or one a cell (2 mm) from it.
        require(off < 3, QStringLiteral("marker 1 stands at the clicked corner (%1 mm off)").arg(off, 0, 'f', 3));
        require(form->activeInput() == "to_points", "its match on the target is asked next");
        require(view->previewMovedCount() == 0, "no preview before three pairs");
        // The From box clicked again while it is a pick ahead: the next pick replaces that one (the corner node itself).
        begin("the From box again: the exact corner replaces the click");
        form->activate("from_points");
        st->phase = 30;
        return;
      }
      case 30: {
        if (busy || form->activeInput() != "from_points") return;
        pick(st->from[0]);
        st->phase = 31;
        return;
      }
      case 31: {
        const opad::json f = form->picks("from_points");
        const bool ready = count(f) == 1 && opad::Ref::from_json(f[0]).index == st->from[0].index && form->activeInput() == "to_points" && settled();
        if (!ready && st->clock.elapsed() < 20000) return;
        end();
        require(ready && count(form->picks("to_points")) == 0, "replaced, not added: one point on the bodies, the target asked next");
        st->step = 1;  // the picks made: from[0]
        begin("pairs 1 to 3 picked in turn");
        st->phase = 4;
        return;
      }
      case 4: {  // one pick per tick, checking the hand-over each time, until three whole pairs
        if (busy || design->pairsResolving()) return;
        const int k = st->step;  // picks so far: 2 per pair
        if (k == 4 && !settled()) return;  // two whole pairs: what the panel says once the preview step ran
        if (k == 4 && st->settled != -4) {
          st->settled = -4;
          require(view->previewMovedCount() == 0 && form->completePairs() == 2, "two whole pairs: still no preview");
          require(form->statusText().contains("three pairs"), "the panel says three pairs are needed: " + form->statusText());
        }
        if (k < 6) {
          const bool onTarget = k % 2 == 1;
          if (!require(form->activeInput() == (onTarget ? "to_points" : "from_points"), QStringLiteral("pick %1 goes to the %2").arg(k + 1).arg(onTarget ? "target" : "bodies"))) return finish();
          pick(onTarget ? st->to[size_t(k / 2)] : st->from[size_t(k / 2)]);
          ++st->step;
          return;
        }
        st->phase = 5;
        return;
      }
      case 5: {  // three whole pairs: the preview moves the scan, the marker lines are as short as the noise
        const opad::json m = markers();
        const bool ready = view->previewMovedCount() > 0 && settled() && m.size() == 3 && m[2].contains("line") && form->pairListText().contains("rms");
        if (!ready && st->clock.elapsed() < 60000) return;
        end(QStringLiteral("%1 drawn moved").arg(view->previewMovedCount()));
        require(view->previewMovedCount() > 0, "three pairs: the preview moves the scan");
        double longest = 0;
        for (const auto& p : m) longest = std::max(longest, p.value("line", 1e9));
        require(longest < 0.5, QStringLiteral("the markers meet: longest line %1 mm").arg(longest, 0, 'f', 3));
        require(form->pairListText().count('\n') >= 3, "the list has a row per pair and the fit's rms: " + form->pairListText().replace('\n', " | "));
        form->window()->grab().save(prefix + ".pairs-panel.png");
        begin("pair 4: its point on the scan, waiting for its match");
        pick(st->from[3]);
        st->phase = 6;
        return;
      }
      case 6: {
        const opad::json m = markers();
        const bool ready = m.size() == 4 && m[3].contains("from") && settled();
        if (!ready && st->clock.elapsed() < 20000) return;
        end();
        require(ready && !m[3].contains("to"), "marker 4 on the scan, none on the target yet");
        require(view->previewMovedCount() > 0, "the preview stays while a pick waits for its match");
        begin("pair 4's match, then a wrong fifth pair");
        pick(st->to[3]);
        st->phase = 7;
        return;
      }
      case 7: {  // the fifth pair, one side then the other
        if (busy || design->pairsResolving()) return;
        const int nf = count(form->picks("from_points")), nt = count(form->picks("to_points"));
        if (nf == 4 && nt == 4 && form->activeInput() == "from_points") pick(st->from[4]);
        else if (nf == 5 && nt == 4 && form->activeInput() == "to_points") {
          pick(st->to[4]);
          st->phase = 8;
        } else if (st->clock.elapsed() > 20000) {
          require(false, QStringLiteral("the fifth pair picked in turn (%1 / %2, active %3)").arg(nf).arg(nt).arg(form->activeInput()));
          return finish();
        }
        return;
      }
      case 8: {
        const opad::json m = markers();
        const bool ready = m.size() == 5 && settled() && view->previewMovedCount() > 0 && m[4].value("stray", false);
        if (!ready && st->clock.elapsed() < 60000) return;
        end();
        require(ready, QStringLiteral("the wrong pair 5 is left out, its markers red (%1)").arg(QString::fromStdString(m.dump())));
        require(form->pairListText().contains(QStringLiteral("left out")), "the list names it: " + form->pairListText().replace('\n', " | "));
        double longest = 0;
        for (size_t i = 0; i < 4 && i < m.size(); ++i) longest = std::max(longest, m[i].value("line", 1e9));
        require(longest < 0.5, QStringLiteral("the other four still meet: longest line %1 mm").arg(longest, 0, 'f', 3));
        form->window()->grab().save(prefix + ".pairs-stray.png");
        begin("Esc takes back the last pick");
        key(Qt::Key_Escape);
        st->phase = 9;
        return;
      }
      case 9: {
        if (busy) return;
        require(design->featureActive() && count(form->picks("to_points")) == 4 && count(form->picks("from_points")) == 5 && form->activeInput() == "to_points",
                "Esc took the target's last point back; its box picks again");
        key(Qt::Key_Escape);
        st->phase = 10;
        return;
      }
      case 10: {
        const opad::json m = markers();
        const bool ready = count(form->picks("from_points")) == 4 && m.size() == 4 && settled() && view->previewMovedCount() > 0 && !form->pairListText().contains(QStringLiteral("left out"));
        if (!ready && st->clock.elapsed() < 60000) return;
        end();
        require(ready && form->activeInput() == "from_points", "Esc again took the scan's point back: four pairs, none left out");
        // Remove in the list: the second pair goes, both of its points.
        QList<QToolButton*> removes;
        for (QToolButton* b : form->findChildren<QToolButton*>())
          if (b->parentWidget() && b->parentWidget()->objectName() == "pairRow" && !b->isHidden()) removes << b;
        if (!require(removes.size() == 4, QStringLiteral("a Remove per pair (%1)").arg(removes.size()))) return finish();
        begin("Remove on pair 2");
        removes[1]->click();
        st->phase = 11;
        return;
      }
      case 11: {
        const opad::json m = markers();
        const bool ready = count(form->picks("from_points")) == 3 && count(form->picks("to_points")) == 3 && m.size() == 3 && settled() && view->previewMovedCount() > 0;
        if (!ready && st->clock.elapsed() < 60000) return;
        end();
        require(ready, "pair 2 removed from both sides; three pairs previewed");
        const opad::json f = form->picks("from_points");
        require(f.is_array() && f.size() == 3 && opad::Ref::from_json(f[1]).index == st->from[2].index, "the third pair is the second now");
        begin("committed");
        key(Qt::Key_Return);
        st->phase = 12;
        return;
      }
      case 12: {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 60000) return;
        end();
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        if (!require(f && f->kind == "align" && f->inputs.value("from", "") == "point_pairs", "an Align step from point pairs was committed")) return finish();
        require(design->benchPairMarkers().empty(), "the markers went with the panel");
        // The scan's box measured on a worker: on the block.
        begin("the scan measured (worker)");
        st->jobDone = false;
        auto docCopy = std::make_shared<opad::Document>(doc->doc);
        auto scene = std::make_shared<opad::Scene>(doc->scene);
        auto out = std::make_shared<opad::json>();
        const std::string scan = st->scan;
        w.m_jobs->async(QStringLiteral("Measuring the scan"), [docCopy, scene, scan, out](Progress) {
          const auto m = opad::scanfit::fit_mesh(opad::node_world_shape(*docCopy, *scene, scan));
          double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
          for (const auto& p : m->nodes)
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p.Coord(k + 1)), hi[k] = std::max(hi[k], p.Coord(k + 1));
          *out = {{"lo", {lo[0], lo[1], lo[2]}}, {"hi", {hi[0], hi[1], hi[2]}}};
        }, [st, out](bool ok, const QString&) {
          st->jobDone = true;
          st->jobOk = ok;
          st->measured = *out;
        });
        st->phase = 13;
        return;
      }
      case 13: {
        if (!st->jobDone) return;
        end();
        const auto& lo = st->measured.value("lo", opad::json::array({0, 0, 0})), &hi = st->measured.value("hi", opad::json::array({0, 0, 0}));
        double worst = 0;
        const double want[6] = {0, 0, 0, 100, 60, 40};
        for (int k = 0; k < 3; ++k) worst = std::max({worst, std::fabs(lo[k].get<double>() - want[k]), std::fabs(hi[k].get<double>() - want[k + 3])});
        require(st->jobOk && worst < 0.4, QStringLiteral("the scan lies on the block: its box within %1 mm").arg(worst, 0, 'f', 3));
        trace::log(QStringLiteral("bench: align pairs: %1").arg(trace::stallHistogram()));
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
