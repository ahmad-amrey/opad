// OPAD_BENCH_ALIGN_SEQUENCE=<prefix> on the align-pairs document (a block "Block" and its noisy scan moved away; gui case
// align-321, tools/bench_cases/align.py): Align's 3-2-1 steps on the scan. Primary: the scan's top (a flat fitted where a
// click on it would grow it, through the plane picker) flush onto the block's top; the panel then says what is still free
// (two slides and the turn about the top's normal). Secondary: the scan's front flat onto the block's front: one slide left.
// Tertiary: the scan's left flat onto the block's left: fully fixed. Each step previewed, then committed, and the scan's
// nodes measured on a worker lie on the block. Every step under the stall limit: "bench: align 321: ... PASS".
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <BRepAdaptor_Surface.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "FitPick.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "Viewport.hpp"
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
}  // namespace

OPAD_BENCH(OPAD_BENCH_ALIGN_SEQUENCE, alignSequence) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, step = 0;
    bool all = true, jobDone = false, jobOk = false;
    std::string scan, block;
    opad::json found;  // per side (top, front, left): the scan's seed triangle and the block's face ordinal
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
    PlanePicker* picker = design->planePicker();
    auto require = [st](bool ok, const QString& what) {
      trace::log(QString("bench: align 321: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    if (++st->ticks > 30000) {
      require(false, QString("timed out in phase %1 (%2)").arg(st->phase).arg(st->stepName));
      return finish();
    }
    const bool busy = doc->loading || doc->designBusy || w.m_displayJob || w.m_meshRemaining > 0 || view->looksPending() || view->filterSwitching();
    // The steps: input pairs and the side (top, front, left) each takes.
    static const char* inputs[3][2] = {{"primary_from", "primary_to"}, {"secondary_from_plane", "secondary_to_plane"}, {"tertiary_from_plane", "tertiary_to_plane"}};
    static const char* sides[3] = {"top", "front", "left"};
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
        w.m_jobs->async(QStringLiteral("Finding the sides"), [docCopy, scene, scan, block, out](Progress) {
          const auto m = opad::scanfit::fit_mesh(opad::node_world_shape(*docCopy, *scene, scan));
          TopTools_IndexedMapOfShape fs;
          TopExp::MapShapes(opad::node_world_shape(*docCopy, *scene, block), TopAbs_FACE, fs);
          const std::vector<std::pair<gp_Pnt, gp_Dir>> at = {{gp_Pnt(50, 30, 40), gp_Dir(0, 0, 1)}, {gp_Pnt(50, 0, 20), gp_Dir(0, -1, 0)}, {gp_Pnt(0, 30, 20), gp_Dir(-1, 0, 0)}};
          *out = opad::json::object();
          for (size_t s = 0; s < at.size(); ++s) {
            const gp_Pnt p = at[s].first.Transformed(scanMove());
            const gp_Dir n = at[s].second.Transformed(scanMove());
            int seed = -1;
            double best = 1e300;
            for (int t = 0; t < m->triangles(); ++t)
              if (m->normal[size_t(t)].Magnitude() > 0 && m->normal[size_t(t)].Dot(gp_Vec(n)) > 0.95)
                if (const double d = m->centre[size_t(t)].Distance(p); d < best) best = d, seed = t;
            int face = -1;
            for (int i = 1; i <= fs.Extent(); ++i) {
              const TopoDS_Face f = TopoDS::Face(fs(i));
              const BRepAdaptor_Surface a(f);
              if (a.GetType() != GeomAbs_Plane) continue;
              gp_Dir d = a.Plane().Axis().Direction();
              if (!a.Plane().Position().Direct()) d.Reverse();
              if (f.Orientation() == TopAbs_REVERSED) d.Reverse();
              if (d.Dot(at[s].second) > 1 - 1e-9) face = i - 1;
            }
            (*out)[sides[s]] = {{"seed", seed}, {"face", face}};
          }
        }, [st, out](bool ok, const QString&) {
          st->jobDone = true;
          st->jobOk = ok;
          st->found = *out;
        });
        st->phase = 1;
        return;
      }
      case 1: {
        if (!st->jobDone) return;
        if (!require(st->jobOk && st->found.size() == 3, "the scan's flats and the block's faces")) return finish();
        QApplication::setActiveWindow(&w);
        begin("Align started on the scan, 3-2-1 steps");
        design->startFeature("align", {{"bodies", opad::json::array({{{"body", st->scan}, {"kind", "body"}}})}, {"from", "sequence"}, {"flip", true},
                                       {"secondary", "plane"}, {"tertiary", "plane"}});
        st->phase = 2;
        return;
      }
      case 2: {
        if ((busy || !design->featureActive()) && st->clock.elapsed() < 20000) return;
        end();
        require(form->choiceText("from") == i18n::t("3-2-1 steps"), "the mode is called 3-2-1 steps: " + form->choiceText("from"));
        require(form->nextPlane() == "primary_from" && !picker->active(), "the primary plane of the scan is the next step, no picker opened by itself");
        st->step = 0;
        st->phase = 10;
        return;
      }
      // Each step: its From box opens the picker, the scan's flat chosen (fitted on a worker); its To box, the block's face.
      case 10: {
        if (busy) return;
        begin(QStringLiteral("%1: the scan's %2").arg(inputs[st->step][0]).arg(sides[st->step]));
        form->activate(inputs[st->step][0]);
        st->phase = 11;
        return;
      }
      case 11: {
        if (!picker->active() && st->clock.elapsed() < 20000) return;
        if (!require(picker->active(), "its box opens the picker")) return finish();
        picker->choose({{"fit", {{"body", st->scan}, {"seed", st->found[sides[st->step]]["seed"]}, {"of", "plane"}, {"tolerance", "10 deg"}}}});
        st->phase = 12;
        return;
      }
      case 12: {
        const opad::json from = form->picks(inputs[st->step][0]);
        if ((picker->active() || !fitpick::fitted(from) || busy) && st->clock.elapsed() < 30000) return;
        end(fitpick::quality(from));
        require(fitpick::fitted(from) && fitpick::grade(from) == "good", QStringLiteral("the scan's %1 fitted: %2").arg(sides[st->step]).arg(fitpick::quality(from)));
        begin(QStringLiteral("%1: the block's %2").arg(inputs[st->step][1]).arg(sides[st->step]));
        form->activate(inputs[st->step][1]);
        st->phase = 13;
        return;
      }
      case 13: {
        if (!picker->active() && st->clock.elapsed() < 20000) return;
        if (!require(picker->active(), "its box opens the picker")) return finish();
        opad::Ref r;
        r.body = st->block, r.kind = opad::Ref::Kind::Face, r.index = st->found[sides[st->step]]["face"].get<int>();
        picker->choose({{"face", r.to_json()}});
        st->phase = 14;
        return;
      }
      case 14: {  // previewed, and the panel says what is still free
        static const char* expect[3] = {"turn about", "slide along", "Fully fixed"};
        const QString note = form->resultNote();
        const bool ready = !picker->active() && !form->picks(inputs[st->step][1]).is_null() && !design->previewing() && view->previewMovedCount() > 0 && note.contains(expect[st->step]);
        if ((!ready || busy) && st->clock.elapsed() < 30000) return;
        end();
        require(ready, QStringLiteral("step %1 previewed; the panel says: %2").arg(st->step + 1).arg(note));
        if (st->step == 0) require(note.count(',') == 2, "the primary leaves two slides and a turn: " + note);
        if (st->step == 1) require(!note.contains("turn") && note.count(',') == 0, "the secondary leaves one slide: " + note);
        form->window()->grab().save(prefix + QStringLiteral(".step%1.png").arg(st->step + 1));
        if (++st->step < 3) {
          st->phase = 10;
          return;
        }
        begin("committed");
        emit form->accepted();
        st->phase = 20;
        return;
      }
      case 20: {
        if ((design->featureActive() || busy) && st->clock.elapsed() < 30000) return;
        end();
        const opad::Feature* f = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
        if (!require(f && f->kind == "align" && f->inputs.value("from", "") == "sequence" && f->result.value("free", opad::json::array({1})).empty(),
                     "a fully fixed 3-2-1 Align was committed"))
          return finish();
        st->jobDone = false;
        auto docCopy = std::make_shared<opad::Document>(doc->doc);
        auto scene = std::make_shared<opad::Scene>(doc->scene);
        auto out = std::make_shared<opad::json>();
        const std::string scan = st->scan;
        begin("the scan measured (worker)");
        w.m_jobs->async(QStringLiteral("Measuring the scan"), [docCopy, scene, scan, out](Progress) {
          const auto m = opad::scanfit::fit_mesh(opad::node_world_shape(*docCopy, *scene, scan));
          double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
          for (const auto& p : m->nodes)
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p.Coord(k + 1)), hi[k] = std::max(hi[k], p.Coord(k + 1));
          *out = {{"lo", {lo[0], lo[1], lo[2]}}, {"hi", {hi[0], hi[1], hi[2]}}};
        }, [st, out](bool ok, const QString&) {
          st->jobDone = true;
          st->jobOk = ok;
          st->found = *out;
        });
        st->phase = 21;
        return;
      }
      case 21: {
        if (!st->jobDone) return;
        end();
        const auto& lo = st->found.value("lo", opad::json::array({0, 0, 0})), &hi = st->found.value("hi", opad::json::array({0, 0, 0}));
        double worst = 0;
        const double want[6] = {0, 0, 0, 100, 60, 40};
        for (int k = 0; k < 3; ++k) worst = std::max({worst, std::fabs(lo[k].get<double>() - want[k]), std::fabs(hi[k].get<double>() - want[k + 3])});
        require(st->jobOk && worst < 0.3, QStringLiteral("the scan lies on the block: its box within %1 mm").arg(worst, 0, 'f', 3));
        return finish();
      }
    }
  });
  timer->start();
  return true;
}
