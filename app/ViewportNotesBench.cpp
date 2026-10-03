// OPAD_BENCH_NOTEANCHORS=<prefix> (UI-03): notes pinned to bodies and a face cost a sync nothing. Cases in
// tools/bench_cases/viewer.py: three parts (CI) and the Engine beside the repository (its hidden root shown in memory), as
// the evaluation measured it (two body notes: every sync 15-18 s, hide + undo + redo 46 s). Two notes on the largest
// bodies and one on a face: adding each is timed, the anchors are measured once on a worker while the event loop keeps
// turning, they sit where opad::annotation_anchor says; a hide, its undo and redo and a sync without a change are timed
// and measure nothing; moving a pinned body measures that note again, where the body went; a resolved note drops out.
// <prefix>.png is the view with the anchors.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "opad/inspect.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_NOTEANCHORS, noteanchors) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: note anchors: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
    if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
  auto settled = [&w, doc, v] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0;
  };
  const auto finish = [&all] { QCoreApplication::exit(all ? 0 : 2); return true; };
  const bool shown = waitUntil(settled, 240000);
  if (!require(shown, QString("%1 bodies displayed").arg(v->displayedCount()))) return finish();
  std::vector<std::string> bodies = doc->scene.all_bodies();
  auto size = [doc](const std::string& id) { const opad::BodyEntry* e = doc->doc.body(doc->scene.node(id)->body_key); return e ? e->brep.size() : 0; };
  std::stable_sort(bodies.begin(), bodies.end(), [&](const auto& a, const auto& b) { return size(a) > size(b); });  // the heaviest first
  if (!require(bodies.size() >= 3, QString("%1 bodies, three wanted").arg(bodies.size()))) return finish();
  const bool big = bodies.size() > 100;
  // Seconds before (15-18 s a sync on the Engine); the Engine's own hide / undo / redo take ~100 ms. Generous, as other
  // builds may share the machine: what counts exactly is what is measured, and when.
  const int runBudget = big ? 3000 : 2000, syncBudget = big ? 1000 : 500;
  auto timed = [&](const QString& what, const std::function<void()>& fn, int budget) {
    QElapsedTimer clock;
    clock.start();
    fn();
    const qint64 ms = clock.elapsed();
    require(ms < budget, QString("%1: %2 ms (under %3)").arg(what).arg(ms).arg(budget));
    return ms;
  };
  auto ticks = std::make_shared<int>(0);
  QObject::connect(v, &Viewport::notesMoved, &w, [ticks] { ++*ticks; });
  std::vector<std::string> notes;
  const std::vector<std::string> anchors = {bodies[0], bodies[1], bodies[0] + "/face/0"};
  for (const auto& anchor : anchors)
    timed(QString("a note pinned to %1").arg(QString::fromStdString(anchor.size() > 36 ? anchor.substr(0, 8) + anchor.substr(36) : anchor.substr(0, 8))),
          [&] { notes.push_back(doc->run("annotate", {{"anchor", anchor}, {"text", "Check this"}}).value("id", "")); }, runBudget);
  // Measured on a worker: the event loop keeps turning (the worst time between two turns), and each anchor once. A 1 ms
  // timer never fired inside this nested loop, so a measure longer than 100 ms under load failed with no gap seen.
  QElapsedTimer gap, measuring;
  qint64 worst = 0;
  int turns = 0;
  gap.start();
  measuring.start();
  while (v->notesPending() && measuring.elapsed() < 120000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    worst = std::max(worst, gap.restart());
    ++turns;
  }
  const bool measured = !v->notesPending() && v->anchorsMeasured() == 3;
  require(measured && worst < 2000, QString("three anchors measured on a worker in %1 ms (%2 measured), worst event-loop gap %3 ms over %4 turns")
                                        .arg(measuring.elapsed()).arg(v->anchorsMeasured()).arg(worst).arg(turns));
  // Where annotation_anchor puts a note (body notes: cached boxes by now; the face once, here: on the Engine's largest
  // body that is seconds of the bench's own time), shifted by `by`.
  auto expect = [doc](const std::string& anchor) {
    try {
      return std::make_pair(true, opad::annotation_anchor(doc->doc, doc->scene, opad::Ref::parse(anchor)));
    } catch (const std::exception&) {
      return std::make_pair(false, opad::Vec3{0, 0, 0});
    }
  };
  const auto face = expect(anchors[2]);
  auto at = [&](const std::string& note, const std::string& anchor, opad::Vec3* shown = nullptr, const opad::Vec3& by = {0, 0, 0}) {
    opad::Vec3 drawn{0, 0, 0};
    const auto [ok, expected] = anchor == anchors[2] ? face : expect(anchor);
    if (!ok || !v->noteAnchorPoint(note, drawn)) return false;
    if (shown) *shown = drawn;
    return std::abs(drawn[0] - expected[0] - by[0]) + std::abs(drawn[1] - expected[1] - by[1]) + std::abs(drawn[2] - expected[2] - by[2]) < 1e-6;
  };
  opad::Vec3 before{0, 0, 0};
  require(at(notes[0], anchors[0], &before) && at(notes[1], anchors[1]) && at(notes[2], anchors[2]), "each note sits where annotation_anchor puts it");
  QPoint pinned;
  const bool onScreen = v->noteAnchor(notes[0], pinned);  // (the message is made before a condition in the same call)
  require(onScreen, QString("the first note is on screen at (%1, %2)").arg(pinned.x()).arg(pinned.y()));
  // Changes that do not touch what the notes are pinned to: nothing measured, nothing laid out again on a mere sync.
  const std::string other = bodies.back();
  timed("hide another body", [&] { doc->run("appearance", {{"target", other}, {"visible", false}}); }, runBudget);
  timed("undo", [&] { doc->undo(); }, runBudget);
  timed("redo", [&] { doc->redo(); }, runBudget);
  timed("undo again", [&] { doc->undo(); }, runBudget);
  waitUntil(settled, 60000);
  QCoreApplication::processEvents();
  const int laidOut = *ticks;
  timed("a sync without a document change (a batch of meshes)", [&] { v->sync(); }, syncBudget);
  QCoreApplication::processEvents();
  require(v->anchorsMeasured() == 3 && !v->notesPending() && *ticks == laidOut,
          QString("nothing measured again (%1 measured), the notes not laid out again on that sync").arg(v->anchorsMeasured()));
  // Moving a pinned body: that note is measured again, where the body went; undo brings it back.
  const opad::Node* moved = doc->scene.node(bodies[0]);
  opad::Mat4 shifted = moved->local;
  shifted.m[3] += 50;
  timed("move a pinned body", [&] { doc->run("transform", {{"target", bodies[0]}, {"matrix", shifted.to_json()}}); }, runBudget);
  const bool remeasured = waitUntil([v] { return !v->notesPending(); }, 60000) && v->anchorsMeasured() == 5;
  require(remeasured, QString("its two notes measured again (%1 measured)").arg(v->anchorsMeasured()));
  opad::Vec3 after{0, 0, 0};
  const bool followed = at(notes[0], anchors[0], &after) && at(notes[1], anchors[1]) &&
                        at(notes[2], anchors[2], nullptr, {after[0] - before[0], after[1] - before[1], after[2] - before[2]}) &&
                        std::abs(std::hypot(after[0] - before[0], after[1] - before[1], after[2] - before[2]) - 50) < 1e-6;
  require(followed,
          QString("the notes follow the body 50 mm (%1, %2, %3 -> %4, %5, %6)").arg(before[0]).arg(before[1]).arg(before[2]).arg(after[0]).arg(after[1]).arg(after[2]));
  const QImage frame = v->grabImage();
  if (!frame.isNull()) frame.save(value + ".png");
  doc->undo();
  const bool back = waitUntil([v] { return !v->notesPending(); }, 60000) && at(notes[0], anchors[0], &after) && at(notes[2], anchors[2]) &&
                    std::abs(after[0] - before[0]) + std::abs(after[1] - before[1]) + std::abs(after[2] - before[2]) < 1e-6;
  require(back, QString("undone: back where it was (%1, %2, %3)").arg(after[0]).arg(after[1]).arg(after[2]));
  // A resolved note leaves nothing behind; the others keep their places without a measurement.
  const int count = v->anchorsMeasured();
  timed("resolve the face note", [&] { doc->run("delete_annotation", {{"target", notes[2]}}); }, runBudget);
  opad::Vec3 gone;
  require(!v->noteAnchorPoint(notes[2], gone) && at(notes[0], anchors[0]) && v->anchorsMeasured() == count && !v->notesPending(), "resolved: dropped, the rest kept");
  return finish();
}
