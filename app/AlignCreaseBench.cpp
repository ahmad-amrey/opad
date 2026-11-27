// OPAD_BENCH_ALIGN_CREASE=<prefix> on the align-pairs document (a block "Block" and its noisy scan moved away; gui case
// align-crease, tools/bench_cases/align.py): a scan's edge taken as a line in Align. A box over a run of the scan's sides
// along its top front edge gives the line through them (the fallback). Then the pointer rests on one side near that edge:
// the crease (the top and the front flats grown, fitted and met) is fitted on a worker and drawn along its whole length,
// close to the true edge; a click on that side takes it, fitted (no second fit). To = the block's own edge; previewed,
// committed; the crease, fitted again on the moved scan, lies on the block's edge. Every step under the stall limit:
// "bench: align crease: ... PASS".
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "FitPick.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/scan_fit.hpp"

namespace {
constexpr qint64 kStallLimitMs = 250;

gp_Trsf scanMove() {  // as tools/bench_cases/align.py _pairs_move
  gp_Trsf r, t;
  r.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 1, 2)), 25 * M_PI / 180);
  t.SetTranslation(gp_Vec(180, 30, 20));
  return t * r;
}
double lineDistance(const gp_Pnt& p, const gp_Pnt& o, const gp_Dir& d) { return gp_Vec(o, p).Crossed(gp_Vec(d)).Magnitude(); }
}  // namespace

OPAD_BENCH(OPAD_BENCH_ALIGN_CREASE, alignCrease) {
  struct State {
    int phase = 0, ticks = 0, settled = 0;
    bool all = true, jobDone = false, jobOk = false;
    std::string scan, block;
    std::vector<int> sides;  // the scan's sides along its top front edge
    int blockEdge = -1;      // the block's edge (0,0,40)-(100,0,40)
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
      trace::log(QString("bench: align crease: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    // The true edge, moved with the scan.
    const gp_Pnt edgeMid = gp_Pnt(50, 0, 40).Transformed(scanMove());
    const gp_Dir edgeDir = gp_Dir(1, 0, 0).Transformed(scanMove());
    if (++st->ticks > 30000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->stepName));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || view->filterSwitching();
    switch (st->phase) {
      case 0: {
        if (busy || ++st->settled < 25) return;
        for (const auto& id : doc->scene.all_bodies()) {
          const opad::Node* n = doc->node(id);
          if (n && n->representation == "mesh") st->scan = id;
          else if (n && n->name == "Block") st->block = id;
        }
        if (!require(!st->scan.empty() && !st->block.empty(), "a scan mesh and the Block solid")) return finish();
        auto docCopy = std::make_shared<opad::Document>(doc->doc);
        auto scene = std::make_shared<opad::Scene>(doc->scene);
        auto out = std::make_shared<opad::json>();
        const std::string scan = st->scan, block = st->block;
        w.m_jobs->async(QStringLiteral("Finding the edge"), [docCopy, scene, scan, block, out, edgeMid, edgeDir](Progress) {
          const auto m = opad::scanfit::fit_mesh(opad::node_world_shape(*docCopy, *scene, scan));
          opad::json sides = opad::json::array();
          for (int t = 0; t < m->triangles(); ++t)
            for (int k = 0; k < 3; ++k) {
              const gp_Pnt p = m->nodes[size_t(m->tri[size_t(t)][size_t(k)])], q = m->nodes[size_t(m->tri[size_t(t)][size_t((k + 1) % 3)])];
              if (lineDistance(p, edgeMid, edgeDir) < 0.2 && lineDistance(q, edgeMid, edgeDir) < 0.2 && std::fabs(gp_Vec(edgeMid, p).Dot(gp_Vec(edgeDir))) < 30) sides.push_back(3 * t + k);
            }
          TopTools_IndexedMapOfShape es;
          TopExp::MapShapes(opad::node_world_shape(*docCopy, *scene, block), TopAbs_EDGE, es);
          int edge = -1;
          for (int i = 1; i <= es.Extent(); ++i) {
            TopoDS_Vertex a, b;
            TopExp::Vertices(TopoDS::Edge(es(i)), a, b);
            const gp_Pnt p = BRep_Tool::Pnt(a), q = BRep_Tool::Pnt(b);
            if (std::fabs(p.Y()) < 1e-6 && std::fabs(q.Y()) < 1e-6 && std::fabs(p.Z() - 40) < 1e-6 && std::fabs(q.Z() - 40) < 1e-6) edge = i - 1;
          }
          *out = {{"sides", sides}, {"edge", edge}};
        }, [st, out](bool ok, const QString&) {
          st->jobDone = true;
          st->jobOk = ok;
          st->measured = *out;
        });
        st->phase = 1;
        return;
      }
      case 1: {
        if (!st->jobDone) return;
        for (const auto& s : st->measured["sides"]) st->sides.push_back(s.get<int>());
        st->blockEdge = st->measured.value("edge", -1);
        if (!require(st->sides.size() >= 10 && st->blockEdge >= 0, QStringLiteral("the scan's sides along the edge (%1) and the block's edge").arg(st->sides.size()))) return finish();
        QApplication::setActiveWindow(&w);
        begin("Align started on the scan, line onto line");
        design->startFeature("align", {{"bodies", opad::json::array({{{"body", st->scan}, {"kind", "body"}}})}, {"from", "line"}, {"to", "line"}});
        st->phase = 2;
        return;
      }
      case 2: {  // From line takes the clicks (Edge filter)
        if ((busy || form->activeInput() != "from_line") && st->clock.elapsed() < 20000) return;
        end();
        if (!require(form->activeInput() == "from_line", "From edge or axis takes the picks: " + form->activeInput())) return finish();
        // A box over the run of sides: the line through them.
        begin("a box over the sides along the edge");
        std::vector<opad::Ref> refs;
        for (int s : st->sides) {
          opad::Ref r;
          r.body = st->scan, r.kind = opad::Ref::Kind::Edge, r.index = s;
          refs.push_back(r);
        }
        view->selectRefs(refs);
        design->viewportSelectionChanged();
        st->phase = 3;
        return;
      }
      case 3: {
        const opad::json from = form->picks("from_line");
        if ((!fitpick::fitted(from) || busy) && st->clock.elapsed() < 30000) return;
        end(fitpick::title(from));
        require(fitpick::fitted(from) && from["fit"].contains("sides"), "a box of sides gives the line through them: " + fitpick::title(from) + " · " + fitpick::quality(from));
        // Back to From for the crease: the pointer rests on a side of the top near the front edge.
        form->setPicks("from_line", opad::json());
        form->activate(QString());
        form->activate("from_line");
        st->settled = 0;
        st->phase = 4;
        return;
      }
      case 4: {
        if (busy || ++st->settled < 10) return;
        const gp_Pnt at = gp_Pnt(50, 0.9, 40).Transformed(scanMove());
        begin("the pointer resting near the edge");
        view->benchHoverAt(QPointF(view->widgetPoint({at.X(), at.Y(), at.Z()})));
        st->phase = 5;
        return;
      }
      case 5: {  // the crease, fitted on a worker and drawn
        const auto line = view->guideLine();
        if ((!line || design->creaseFitting()) && st->clock.elapsed() < 20000) return;
        end();
        if (!require(line.has_value() && fitpick::fitted(design->creaseOffered()), "the crease is offered and drawn while hovering: " + fitpick::quality(design->creaseOffered()))) return finish();
        const gp_Pnt a(line->first[0], line->first[1], line->first[2]), b(line->second[0], line->second[1], line->second[2]);
        const gp_Dir d(gp_Vec(a, b));
        const double off = lineDistance(edgeMid, a, d), angle = std::acos(std::min(1.0, std::fabs(d.Dot(edgeDir)))) * 180 / M_PI, length = a.Distance(b);
        require(off < 0.1 && angle < 0.3, QStringLiteral("it lies on the true edge: %1 mm off, %2 degrees").arg(off, 0, 'f', 4).arg(angle, 0, 'f', 3));
        require(length > 85 && length < 101, QStringLiteral("it runs the edge's length: %1 mm of 100").arg(length, 0, 'f', 2));
        begin("the side clicked");
        const gp_Pnt at = gp_Pnt(50, 0.9, 40).Transformed(scanMove());
        view->benchClickAt(QPointF(view->widgetPoint({at.X(), at.Y(), at.Z()})));
        st->phase = 6;
        return;
      }
      case 6: {
        const opad::json from = form->picks("from_line");
        if ((!fitpick::isFit(from) || busy) && st->clock.elapsed() < 20000) return;
        end(fitpick::title(from));
        require(fitpick::fitted(from) && from["fit"].value("of", "") == "crease", "the click takes the crease, fitted already: " + fitpick::title(from) + " · " + fitpick::quality(from));
        require(!view->guideLine().has_value(), "the guide line goes with the pick");
        begin("To: the block's edge");
        form->activate("to_line");
        st->phase = 7;
        return;
      }
      case 7: {
        if (busy || form->activeInput() != "to_line") return;
        opad::Ref r;
        r.body = st->block, r.kind = opad::Ref::Kind::Edge, r.index = st->blockEdge;
        view->selectRefs({r});
        design->viewportSelectionChanged();
        st->phase = 8;
        return;
      }
      case 8: {
        const bool ready = form->picks("to_line").contains("edge") && !design->previewing() && view->previewMovedCount() > 0;
        if ((!ready || busy) && st->clock.elapsed() < 30000) return;
        end();
        require(ready, "previewed: the scan moved onto the block's edge");
        begin("committed");
        emit form->accepted();
        st->phase = 9;
        return;
      }
      case 9: {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 30000) return;
        end();
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        if (!require(f && f->kind == "align" && f->inputs.value("from_line", opad::json()).value("fit", opad::json()).value("of", "") == "crease", "an Align step on the crease was committed")) return finish();
        // The crease fitted again on the moved scan (worker): on the block's edge.
        st->jobDone = false;
        auto docCopy = std::make_shared<opad::Document>(doc->doc);
        auto scene = std::make_shared<opad::Scene>(doc->scene);
        auto out = std::make_shared<opad::json>();
        const opad::json ask = {{"fit", f->inputs["from_line"]["fit"]}};
        begin("the crease fitted on the moved scan (worker)");
        w.m_jobs->async(QStringLiteral("Fitting"), [docCopy, scene, ask, out](Progress) { *out = opad::design::fit_reference(*docCopy, *scene, ask, "axis"); },
                        [st, out](bool ok, const QString&) {
                          st->jobDone = true;
                          st->jobOk = ok;
                          st->measured = *out;
                        });
        st->phase = 10;
        return;
      }
      case 10: {
        if (!st->jobDone) return;
        end();
        const opad::json wr = st->measured.value("world", opad::json::object());
        bool ok = st->jobOk && wr.contains("origin") && wr.contains("dir");
        double off = 1e9, angle = 1e9;
        if (ok) {
          const gp_Pnt o(wr["origin"][0], wr["origin"][1], wr["origin"][2]);
          const gp_Dir d(wr["dir"][0], wr["dir"][1], wr["dir"][2]);
          off = lineDistance(o, gp_Pnt(0, 0, 40), gp_Dir(1, 0, 0));
          angle = std::acos(std::min(1.0, std::fabs(d.X()))) * 180 / M_PI;
        }
        require(ok && off < 0.1 && angle < 0.3, QStringLiteral("the scan's edge now lies on the block's: %1 mm off, %2 degrees").arg(off, 0, 'f', 4).arg(angle, 0, 'f', 3));
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
