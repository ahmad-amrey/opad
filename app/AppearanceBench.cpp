// OPAD_BENCH_BATCH=<prefix> (UI-02): commands on several objects are one run, one refresh and one undo step. Cases in
// tools/bench_cases/viewer.py: the as1 assembly (editable), three parts with a design history, and the Engine (its hidden
// root shown in memory first), where Hide others used to be ~1,294 commands, ~100 s frozen and the undo history gone.
// Hide others (view.hideothers) hides the fewest nodes (Scene::others_to_hide), Lock and Opacity on three bodies, the
// browser's document eye, and Delete of two operations (a batch, or one design plan); each step undone.
#include <QApplication>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "MainWindow.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_BATCH, batch) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: batch: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] { QCoreApplication::exit(all ? 0 : 2); return true; };
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
    if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
  auto settled = [&w, doc, v] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0 && !doc->designBusy;
  };
  const bool shown = waitUntil(settled, 240000);
  if (!require(shown && !doc->browse, QString("%1 bodies displayed, editable").arg(v->displayedCount()))) return finish();
  int changes = 0;
  QObject::connect(doc, &AppDocument::changed, &w, [&changes] { ++changes; });
  auto select = [&](const std::vector<std::string>& ids) {
    w.m_browser->setSelectedIds(ids);
    w.onBrowserSelection(ids);
    return waitUntil([&] { const auto now = w.currentNodeIds(); return std::set<std::string>(now.begin(), now.end()) == std::set<std::string>(ids.begin(), ids.end()); }, 30000);
  };
  // One command: `fn` adds `ops` ops in one step named `label`, with one refresh; `check` holds after it and its undo
  // brings back the visibility, lock and opacity of every node.
  auto step = [&](const QString& what, size_t ops, const QString& label, const std::function<void()>& fn, const std::function<bool()>& check, int budget = 3000) {
    std::map<std::string, std::tuple<bool, bool, double>> looks;
    for (const auto& [id, n] : doc->scene.nodes) looks[id] = {n.visible, n.locked, n.opacity};
    const size_t before = doc->doc.ops.size(), steps = doc->undoLabels().size();
    changes = 0;
    QElapsedTimer clock;
    clock.start();
    try {
      fn();
    } catch (const std::exception& e) {
      require(false, QString("%1: %2").arg(what, QString::fromUtf8(e.what())));
    }
    const bool done = waitUntil([doc] { return !doc->designBusy; }, 120000);
    const qint64 ms = clock.elapsed();
    const bool ok = done && check();
    require(ok && doc->doc.ops.size() >= before + ops && doc->undoLabels().size() == steps + 1 && doc->undoLabel() == label && changes == 1 && ms < budget,
            QString("%1: %2 ops (%3 wanted), %4 step(s) \"%5\", %6 refresh(es), %7 ms")
                .arg(what).arg(doc->doc.ops.size() - before).arg(ops).arg(doc->undoLabels().size() - steps).arg(doc->undoLabel()).arg(changes).arg(ms));
    changes = 0;
    doc->undo();
    bool back = doc->doc.ops.size() == before && changes == 1;
    for (const auto& [id, n] : doc->scene.nodes) back = back && looks.count(id) && looks[id] == std::make_tuple(n.visible, n.locked, n.opacity);
    require(back, QString("%1 undone in one step").arg(what));
  };
  std::vector<std::string> bodies = doc->scene.all_bodies();
  if (!require(bodies.size() >= 3, QString("%1 bodies").arg(bodies.size()))) return finish();
  // Hide others on the deepest body: only it stays shown, and with components fewer commands than other bodies.
  const std::string keep = *std::max_element(bodies.begin(), bodies.end(), [doc](const auto& a, const auto& b) { return doc->scene.path_to(a).size() < doc->scene.path_to(b).size(); });
  const auto hide = doc->scene.others_to_hide({keep});
  const bool nested = doc->scene.path_to(keep).size() > 2;
  require(select({keep}), "the deepest body selected");
  step(QString("Hide others (%1 nodes for %2 other bodies)").arg(hide.size()).arg(bodies.size() - 1), hide.size(), MainWindow::tr("hide others"),
       [&] { w.action("view.hideothers")->trigger(); },
       [&] {
         bool only = !nested || hide.size() < bodies.size() - 1;
         for (const auto& b : bodies) only = only && doc->scene.effectively_visible(b) == (b == keep);
         return only;
       });
  // Lock and Opacity on three bodies.
  const std::vector<std::string> three(bodies.begin(), bodies.begin() + 3);
  require(select(three), "three bodies selected");
  step("Lock three bodies", 3, AppDocument::tr("lock"), [&] { w.action("design.lock")->trigger(); },
       [&] { return std::all_of(three.begin(), three.end(), [doc](const auto& id) { return doc->scene.node(id)->locked; }); });
  step("Opacity of three bodies", 3, AppDocument::tr("opacity"),
       [&] {
         QTimer::singleShot(200, &w, [] {  // the dialog, answered (benches keep other windows off screen)
           if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
             dialog->setIntValue(50);
             dialog->accept();
           }
         });
         w.action("design.opacity")->trigger();
       },
       [&] { return std::all_of(three.begin(), three.end(), [doc](const auto& id) { return std::abs(doc->scene.node(id)->opacity - 0.5) < 1e-9; }); });
  // The browser's document eye: every root that changes, in one step.
  if (auto* tree = w.m_browser->findChild<BrowserTree*>(); tree && doc->scene.roots.size() > 1) {
    const auto roots = doc->scene.roots;
    step(QString("the document eye on %1 roots").arg(roots.size()), roots.size(), AppDocument::tr("hide"), [&] { emit tree->eyeClicked(""); },
         [&] { return std::none_of(roots.begin(), roots.end(), [doc](const auto& id) { return doc->scene.node(id)->visible; }); });
  }
  // Delete two operations: a batch of tombstones, or one design plan when there is a history.
  std::vector<std::string> ops;  // features that made bodies, else new components
  for (const auto& f : doc->scene.features) if (ops.size() < 2 && !f.result.value("bodies", opad::json::array()).empty()) ops.push_back(f.id);
  for (const char* name : {"First", "Second"})
    if (ops.size() < 2) ops.push_back(doc->run("component", {{"name", name}}).value("op", ""));
  if (require(ops.size() == 2 && doc->doc.find_op(ops[0]) && doc->doc.find_op(ops[1]), "two operations to delete"))
    step(doc->scene.features.empty() ? "Delete two operations (a batch)" : "Delete two operations (one design plan)", 2, MainWindow::tr("delete"), [&] { w.deleteOps(ops); },
         [&] { return std::all_of(ops.begin(), ops.end(), [doc](const auto& id) { return doc->doc.is_deleted(id); }); });
  return finish();
}
