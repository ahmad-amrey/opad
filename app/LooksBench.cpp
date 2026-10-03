// OPAD_BENCH_LOOKS: the per-body look compositor (UI-121, app/ViewportLooks.cpp) in the running app. Case in
// tools/bench_cases/core.py; the composition order alone is tests/test_body_look.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/inspect.hpp"

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

using Check = std::function<void(bool, const QString&)>;

// A big model (the Engine): a ghost layer over every body, ten explode ticks moving every body, then clearing, each timed
// with a 1 ms ticker whose worst gap is the longest the event loop was held (one slice: its steps and the last one).
void timeLayers(QObject* context, AppDocument* doc, Viewport* v, Check require, std::function<void()> finish) {
  struct Clock {
    QElapsedTimer gap, phase;
    qint64 worst = 0;
  };
  auto clock = std::make_shared<Clock>();
  auto* ticker = new QTimer(context);
  ticker->setTimerType(Qt::PreciseTimer);
  QObject::connect(ticker, &QTimer::timeout, context, [clock] { clock->worst = std::max(clock->worst, clock->gap.restart()); });
  auto phase = [context, v, clock, ticker, require](const QString& name, std::function<void()> apply, std::function<void()> then) {
    clock->worst = 0;
    clock->gap.start();
    clock->phase.start();
    ticker->start(1);
    apply();
    pollUntil(context, [v] { return !v->looksPending(); }, 60000, [clock, ticker, require, name, then](bool done) {
      ticker->stop();
      require(done && clock->worst < 50, QString("%1: %2 ms, worst event-loop gap %3 ms").arg(name).arg(clock->phase.elapsed()).arg(clock->worst));
      then();
    });
  };
  std::map<std::string, LookDelta> ghost;
  for (const auto& root : doc->scene.roots) ghost[root].ghost = true;
  auto tick = std::make_shared<int>(0);
  auto next = std::make_shared<std::function<void()>>();
  *next = [doc, v, phase, finish, require, tick, next] {
    if (++*tick > 10) {
      phase("clearing every layer", [v] { v->clearLookLayer(LookSource::Activation); v->clearLookLayer(LookSource::Explode); }, [doc, v, require, finish] {
        bool restored = true;
        for (const auto& id : doc->scene.all_bodies()) restored = restored && v->shownLook(id) == v->bodyLook(id);
        require(restored, "the document's look is back on every body");
        finish();
      });
      return;
    }
    std::map<std::string, LookDelta> exploded;
    for (const auto& root : doc->scene.roots) exploded[root].offset = {0, 0, 5.0 * *tick};
    phase(QString("explode tick %1, every body moved").arg(*tick), [v, exploded] { v->setLookLayer(LookSource::Explode, exploded); }, [next] { (*next)(); });
  };
  phase(QString("ghost layer over %1 bodies").arg(v->displayedCount()), [v, ghost] { v->setLookLayer(LookSource::Activation, ghost); }, [next] { (*next)(); });
}
}  // namespace

// A sketch takes the entries under its own id: a compare tint under an activation ghost draws its lines in the tint faded
// towards the background (line aspects ignore alpha) and leaves them unpickable, a hidden entry erases it, clearing brings
// the sketch blue back, drawn and pickable.
void sketchLooks(QObject* context, AppDocument* doc, DesignController* design, Viewport* v, Check require, std::function<void()> finish) {
  opad::design::Sketch sk;
  const int a = sk.add_point(0, 0), b = sk.add_point(10, 0), c = sk.add_point(10, 10);
  sk.add_line(a, b);
  sk.add_line(b, c);
  design->applyOps({opad::design::make_sketch_op("Look sketch", {{"base", "xy"}}, sk.to_json())}, "bench sketch", [=](bool ok, const QString& error) {
    const std::string id = ok && !doc->scene.sketches.empty() ? doc->scene.sketches.back().id : std::string();
    pollUntil(context, [v, id] { return !id.empty() && !v->benchLookState(id).empty() && !v->looksPending(); }, 10000, [=](bool shown) {
      require(shown, "a sketch is drawn " + error);
      if (!shown) return finish();
      const BodyLook base = v->shownLook(id);
      LookDelta ghost, red, hidden;
      ghost.ghost = true;
      red.color = std::array<double, 3>{0.9, 0.15, 0.1};
      hidden.visible = false;
      v->setLookLayer(LookSource::Compare, {{id, red}});
      v->setLookLayer(LookSource::Activation, {{id, ghost}});
      pollUntil(context, [v] { return !v->looksPending(); }, 10000, [=](bool) {
        const auto state = v->benchLookState(id);
        const QColor bg = v->tokens().vp;
        const auto want = looks::mix(*red.color, {bg.redF(), bg.greenF(), bg.blueF()}, 1 - v->tokens().ghost.alphaF());
        bool faded = state.contains("color");
        for (int i = 0; i < 3 && faded; ++i) faded = std::abs(state["color"][i].get<double>() - want[i]) < 1e-3;
        require(faded && state.value("activated", -1) == 0 && state.value("displayed", false) && v->shownLook(id).ghost,
                "a ghosted, tinted sketch: lines faded towards the background, not pickable: " + QString::fromStdString(state.dump()));
        v->setLookLayer(LookSource::Lock, {{id, hidden}});
        pollUntil(context, [v] { return !v->looksPending(); }, 10000, [=](bool) {
          require(!v->benchLookState(id).value("displayed", true), "a hidden entry erases the sketch");
          v->clearLookLayer(LookSource::Compare);
          v->clearLookLayer(LookSource::Activation);
          v->clearLookLayer(LookSource::Lock);
          pollUntil(context, [v] { return !v->looksPending(); }, 10000, [=](bool) {
            const auto back = v->benchLookState(id);
            const QColor sel = v->tokens().sel;
            require(back.value("displayed", false) && back.value("activated", 0) > 0 && v->shownLook(id) == base &&
                        base.color == std::array<double, 3>{sel.redF(), sel.greenF(), sel.blueF()} && std::abs(back["color"][2].get<double>() - sel.blueF()) < 1e-3,
                    "cleared: the sketch drawn in its blue and pickable again");
            finish();
          });
        });
      });
    });
  });
}

// OPAD_BENCH_LOOKS=<prefix>. On a small document (the overlap fixture: a box and a cylinder, the cylinder moved into a
// component here) an activation ghost and a compare tint on the same body give the documented look (the tint at the
// ghost's opacity, not pickable, hovered as inactive, drawn so: the pixel changes and comes back), a tint on the
// component reaches its body, ghosts become pickable on request, an explode offset moves the body where picking finds it
// and Fit frames it and takes the note pinned to it along, a
// candidate's layer gives way to the X-ray Topmost while selected and comes back after, clearing every layer
// restores the document's look, and a sketch follows the entries under its id (sketchLooks). On a big document (the Engine) the layers are timed (timeLayers). <prefix>.ghost.png
// is the view with the ghost and the tints.
OPAD_BENCH(OPAD_BENCH_LOOKS, looks) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: looks: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  std::function<void()> finish = [all] { QCoreApplication::exit(*all ? 0 : 2); };
  Viewport* v = w.m_viewport;
  for (const auto& root : w.m_doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
    if (const auto* n = w.m_doc->scene.node(root); n && !n->visible) w.m_doc->run("appearance", {{"target", root}, {"visible", true}});
  auto settled = [&w, v] {
    int expected = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) expected += w.m_doc->scene.effectively_visible(id) && !w.m_doc->scene.node(id)->body_missing;
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0 && !v->looksPending();
  };
  pollUntil(&w, settled, 220000, [&w, v, require, finish, settled](bool shown) {
    const auto bodies = w.m_doc->scene.all_bodies();
    require(shown, QString("%1 of %2 bodies displayed").arg(v->displayedCount()).arg(bodies.size()));
    if (!shown) return finish();
    if (bodies.size() > 16) return timeLayers(&w, w.m_doc, v, require, finish);
    if (bodies.size() < 2) return require(false, "two bodies"), finish();
    const std::string a = bodies[0], b = bodies[1];
    const std::string component = w.m_doc->run("component", {{"name", "Group"}}).value("id", "");
    w.m_doc->run("reparent", {{"target", b}, {"parent", component}});
    const std::string note = w.m_doc->run("annotate", {{"anchor", b}, {"text", "Follows its part"}}).value("id", "");
    pollUntil(&w, settled, 20000, [&w, v, require, finish, a, b, component, note](bool ready) {
      int ax = 0, ay = 0;
      const bool found = ready && v->benchBodyPoint(a, ax, ay);
      require(found && w.m_doc->scene.node(b)->parent == component, "the box is picked, the cylinder sits in a component");
      if (!found) return finish();
      auto pixel = [v, ax, ay] { const QImage image = v->grabImage(); return image.isNull() ? QColor() : image.pixelColor(ax, ay); };
      auto apart = [](const QColor& p, const QColor& q) { return std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) + std::abs(p.blue() - q.blue()); };
      const QColor before = pixel();
      const BodyLook base = v->shownLook(a);
      LookDelta ghost, red, green;
      ghost.ghost = true;
      red.color = std::array<double, 3>{0.9, 0.15, 0.1};
      green.color = std::array<double, 3>{0.1, 0.75, 0.25};
      v->setLookLayer(LookSource::Activation, {{a, ghost}});
      v->setLookLayer(LookSource::Compare, {{a, red}, {component, green}});
      pollUntil(&w, [v] { return !v->looksPending(); }, 10000, [&w, v, require, finish, a, b, component, note, ax, ay, pixel, apart, before, base, red, green](bool applied) {
        const BodyLook la = v->shownLook(a), lb = v->shownLook(b);
        const auto sa = v->benchLookState(a), sb = v->benchLookState(b);
        const double ghostAlpha = v->tokens().ghost.alphaF();  // the theme's ghost role
        require(applied && la.color == *red.color && std::abs(la.opacity - ghostAlpha) < 1e-9 && la.ghost && !la.pickable && la == v->bodyLook(a),
                "ghost under a compare tint: the tint at the theme ghost's opacity, not pickable");
        require(std::abs(sa.value("transparency", 0.0) - (1 - ghostAlpha)) < 1e-6 && sa.value("activated", -1) == 0 && sa.value("displayed", false) &&
                    std::abs(sa["color"][0].get<double>() - 0.9) < 1e-3,
                "the box's AIS: " + QString::fromStdString(sa.dump()));
        require(lb.color == *green.color && lb.opacity == 1 && lb.pickable && sb.value("activated", 0) > 0, "the component's tint reaches its cylinder, which stays pickable");
        require(v->benchPickAt(ax, ay) != a, "picking passes through the ghost");
        require(v->hoverName(a).endsWith("(inactive)") && !v->hoverName(b).endsWith("(inactive)"), "hovered, a ghost says it is inactive: " + v->hoverName(a));
        v->grabImage().save(qEnvironmentVariable("OPAD_BENCH_LOOKS") + ".ghost.png");
        const QColor ghosted = pixel();
        require(apart(ghosted, before) > 40, QString("drawn so: %1 -> %2").arg(before.name(), ghosted.name()));
        // The other theme's ghost: its colour and alpha, and the looks are applied again; then back.
        const bool dark = v->tokens().dark;
        w.applyTheme(!dark);
        const double otherAlpha = v->tokens().ghost.alphaF();
        const BodyLook other = v->bodyLook(a);
        const bool rescheduled = v->looksPending();
        w.applyTheme(dark);
        require(std::abs(other.opacity - otherAlpha) < 1e-9 && std::abs(otherAlpha - ghostAlpha) > 0.01 && rescheduled &&
                    std::abs(v->bodyLook(a).opacity - ghostAlpha) < 1e-9,
                QString("a theme switch: the ghost at %1 opacity in the other theme, %2 back").arg(otherAlpha, 0, 'f', 3).arg(ghostAlpha, 0, 'f', 3));
        v->setGhostsPickable(true);
        pollUntil(&w, [v] { return !v->looksPending(); }, 10000, [&w, v, require, finish, a, b, component, note, ax, ay, pixel, apart, before, base](bool) {
          require(v->shownLook(a).pickable && v->benchLookState(a).value("activated", 0) > 0 && v->benchPickAt(ax, ay) == a, "ghosts pickable on request");
          v->setGhostsPickable(false);
          v->clearLookLayer(LookSource::Activation);
          v->clearLookLayer(LookSource::Compare);
          pollUntil(&w, [v] { return !v->looksPending(); }, 10000, [&w, v, require, finish, a, b, component, note, ax, ay, pixel, apart, before, base](bool) {
            require(v->shownLook(a) == base && v->benchPickAt(ax, ay) == a && apart(pixel(), before) < 8,
                    QString("cleared: the document's look, picked and drawn again (%1)").arg(pixel().name()));
            LookDelta up;
            up.offset = {0, 0, 40};
            QPoint pinned;
            const bool anchored = v->noteAnchor(note, pinned);
            const opad::json info = opad::inspect_ref(w.m_doc->doc, w.m_doc->scene, opad::Ref::parse(b));  // where updateAnnotations pins it
            const opad::json centre = info.contains("center") ? info["center"] : info.value("bbox", opad::json::object()).value("center", opad::json());
            auto moved = std::make_shared<int>(0);
            QObject::connect(v, &Viewport::notesMoved, &w, [moved] { ++*moved; });
            v->setLookLayer(LookSource::Explode, {{component, up}});
            pollUntil(&w, [v, moved] { return !v->looksPending() && *moved > 0; }, 10000, [&w, v, require, finish, a, b, note, pinned, anchored, centre](bool) {
              // The note pinned to the cylinder follows it: its anchor is drawn 40 mm up, and the cards were told.
              QPoint drawn;
              const opad::Vec3 up = centre.is_array() ? opad::Vec3{centre[0].get<double>(), centre[1].get<double>(), centre[2].get<double>() + 40} : opad::Vec3{0, 0, 0};
              const bool shown = v->noteAnchor(note, drawn);
              require(anchored && shown && centre.is_array() && (drawn - v->widgetPoint(up)).manhattanLength() <= 2 && (drawn - pinned).manhattanLength() > 10,
                      QString("the note follows its exploded part: (%1, %2) -> (%3, %4)").arg(pinned.x()).arg(pinned.y()).arg(drawn.x()).arg(drawn.y()));
              const auto sb = v->benchLookState(b);
              v->fitAll();
              int bx = 0, by = 0;
              opad::Vec3 at{0, 0, 0};
              const bool picked = v->benchBodyPoint(b, bx, by) && v->benchPickAt(bx, by, &at) == b;
              require(std::abs(sb["translation"][2].get<double>() - 40) < 1e-9 && picked && at[2] > 30,
                      QString("explode offset: moved 40 mm up, picked there (z %1) after Fit").arg(at[2]));
              LookDelta amber;
              amber.color = std::array<double, 3>{1.0, 0.7, 0.1};
              amber.layer = Graphic3d_ZLayerId_Top;
              v->setLookLayer(LookSource::Candidate, {{b, amber}});
              pollUntil(&w, [v] { return !v->looksPending(); }, 10000, [&w, v, require, finish, a, b](bool) {
                const bool top = v->benchLookState(b).value("layer", 0) == Graphic3d_ZLayerId_Top;
                v->selectNodes({b});
                pollUntil(&w, [v, b] { return v->benchLookState(b).value("selected", false); }, 10000, [&w, v, require, finish, a, b, top](bool selected) {
                  require(top && selected && v->benchLookState(b).value("layer", 0) == Graphic3d_ZLayerId_Topmost, "a candidate's layer, then Topmost while selected (X-ray last)");
                  v->clearSelection();
                  pollUntil(&w, [v, b] { return !v->benchLookState(b).value("selected", true); }, 10000, [&w, v, require, finish, a, b](bool) {
                    require(v->benchLookState(b).value("layer", 0) == Graphic3d_ZLayerId_Top, "deselected: back in the candidate's layer");
                    v->clearLookLayer(LookSource::Candidate);
                    v->clearLookLayer(LookSource::Explode);
                    pollUntil(&w, [v] { return !v->looksPending(); }, 10000, [&w, v, require, finish, a, b](bool) {
                      const auto sb = v->benchLookState(b);
                      require(sb.value("layer", 1) == Graphic3d_ZLayerId_Default && std::abs(sb["translation"][2].get<double>()) < 1e-9 && sb.value("activated", 0) > 0 &&
                                  v->shownLook(b) == v->bodyLook(b) && v->shownLook(a) == v->bodyLook(a),
                              "every layer cleared: placed, layered and pickable as the document says");
                      sketchLooks(&w, w.m_doc, w.m_design, v, require, finish);
                    });
                  });
                });
              });
            });
          });
        });
      });
    });
  });
  return true;
}
