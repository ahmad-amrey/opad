// Move of a linked file's part (the user's report: "'XYZ' is part of a linked file and cannot be changed"): the whole file
// moves as one, its top node placed by the Move's result, its parts the file's. Case "linked-move" in
// tools/bench_cases/assets.py; also run on any document with a linked file (the first one's part is picked).
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <Bnd_Box.hxx>

#include <cmath>
#include <functional>
#include <set>

#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "MainWindow.hpp"
#include "TranslateTriad.hpp"
#include "Viewport.hpp"
#include "opad/assets.hpp"
#include "opad/geometry.hpp"

namespace {
bool settle(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  QCoreApplication::processEvents();
  return done();
}

// The translation part of a world matrix.
opad::Vec3 at(const opad::Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

bool closeTo(const opad::Vec3& a, const opad::Vec3& b, double tol = 1e-6) {
  return std::abs(a[0] - b[0]) <= tol && std::abs(a[1] - b[1]) <= tol && std::abs(a[2] - b[2]) <= tol;
}

QString text(const opad::Vec3& v) { return QString("(%1, %2, %3)").arg(v[0], 0, 'f', 3).arg(v[1], 0, 'f', 3).arg(v[2], 0, 'f', 3); }
}  // namespace

// OPAD_BENCH_LINKED_MOVE=<prefix> on a document linking a file (a KiCad board in the case) beside an ordinary body: Move with
// one part of the file and the body picked in the browser says the file moves as one; 15 mm up along Z its preview draws
// every part of the file and the body moved (their own objects: none stands in, none is meshed), the triad at
// the middle of the file and the body moved; committed, the file's top node is placed (the Move's result "placements"), its
// parts keep their keys, the body moved, one undo step. Edited to 25 mm (rolled back, previewed from where the file was),
// undo and redo, saved and read again (the same text, the same places), synced from next/<file> when there is one (the
// place kept). A copy of the file is refused, saying why. Frames: <prefix>.preview.png, .panel.png.
OPAD_BENCH(OPAD_BENCH_LINKED_MOVE, linkedMove) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: linked move: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] { QCoreApplication::exit(all ? 0 : 2); return true; };
  const QString prefix = value;
  AppDocument* doc = w.m_doc;
  Viewport* view = w.m_viewport;
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  AssetsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
  AssetMonitor* monitor = area ? area->monitor() : nullptr;
  std::string import;
  for (const auto& o : doc->doc.ops)
    if (import.empty() && o.type == "import" && o.data.contains("asset") && o.data["asset"].value("storage", "linked") != "embedded") import = o.id;
  auto loaded = [doc, import] {
    if (doc->loading || doc->designBusy) return false;
    for (const auto& id : doc->scene.all_bodies())
      if (const opad::Node* n = doc->node(id); n->source_op == import && n->body_missing) return false;
    return true;
  };
  auto settled = [&w, view] { return !w.m_displayJob && w.m_meshRemaining == 0 && !view->looksPending(); };
  require(monitor && !import.empty() && settle([&] { return loaded() && settled() && !monitor->checking(); }, 600000),
          QString("a linked file, read and displayed (%1 bodies)").arg(doc->scene.all_bodies().size()));
  if (import.empty() || !loaded()) return finish();
  const std::string source = doc->doc.find_op(import)->data.value("source", "");

  // A part of it (the deepest body: a footprint's model rather than the board), another part, and a body of the user's.
  std::string part, other, mine;
  for (const auto& id : doc->scene.all_bodies()) {
    const opad::Node* n = doc->node(id);
    if (n->source_op == import && doc->scene.effectively_visible(id)) {
      if (part.empty() || doc->scene.path_to(id).size() > doc->scene.path_to(part).size()) part = id;
    } else if (!n->linked && mine.empty() && doc->scene.effectively_visible(id) && n->representation == "solid") {
      mine = id;
    }
  }
  for (const auto& id : doc->scene.all_bodies())
    if (id != part && doc->node(id)->source_op == import && other.empty()) other = id;
  const std::vector<std::string> tops = opad::linked_tops(doc->scene, part);
  require(!part.empty() && !other.empty() && tops.size() >= 1, QString("a part of %1, another part, %2 top node(s)%3")
                                                                   .arg(QString::fromStdString(source)).arg(tops.size()).arg(mine.empty() ? ", no body of its own" : ", a body of its own"));
  if (part.empty() || other.empty() || tops.empty()) return finish();
  const opad::Vec3 part0 = at(doc->scene.world(part)), other0 = at(doc->scene.world(other)), top0 = at(doc->scene.world(tops[0]));
  const opad::Vec3 mine0 = mine.empty() ? opad::Vec3{0, 0, 0} : at(doc->scene.world(mine));
  const std::string partKey = doc->node(part)->body_key, otherKey = doc->node(other)->body_key;
  auto box = [doc](const std::string& id) {
    Bnd_Box b;
    try { b = opad::node_world_bbox(doc->doc, doc->scene, id); } catch (const std::exception&) {}
    return b;
  };
  auto middle = [](const Bnd_Box& b) {
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return opad::Vec3{(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2};
  };
  Bnd_Box picked;
  for (const auto& top : tops)
    for (const auto& b : doc->scene.bodies_under(top))
      if (const Bnd_Box one = box(b); !one.IsVoid()) picked.Add(one);
  if (!mine.empty())
    if (const Bnd_Box one = box(mine); !one.IsVoid()) picked.Add(one);
  const opad::Vec3 pickedMiddle = middle(picked);
  const size_t ops0 = doc->doc.ops.size();
  const std::string mineKey = mine.empty() ? std::string() : doc->node(mine)->body_key;
  const opad::Vec3 mineMiddle0 = mine.empty() ? opad::Vec3{0, 0, 0} : middle(box(mine));

  // 1. Move, the part and the body picked in the browser: the file moves as one.
  QApplication::setActiveWindow(&w);
  design->startFeature("move");
  settle([&] { return design->featureActive() && form->activeInput() == "bodies"; }, 5000);
  std::vector<std::string> rows{part};
  if (!mine.empty()) rows.push_back(mine);
  emit w.m_browser->selectionChanged(rows);  // what clicks on the rows send
  settle([&] { const opad::json p = form->picks("bodies"); return p.is_array() && p.size() == rows.size(); }, 5000);
  const QString said = form->pickText("bodies");
  const QString file = QFileInfo(QString::fromStdString(source)).fileName();
  require(said == (mine.empty() ? QString("%1 moves as one").arg(file) : QString("1 selected · %1 moves as one").arg(file)), "the Bodies box says: " + said);
  form->setValue("dz", "15 mm");
  auto previewed = [&](double dz) {
    return !design->previewing() && design->readyPreview() && std::abs(view->previewMotion(part).TranslationPart().Z() - dz) < 1e-9;
  };
  const bool shown = settle([&] { return previewed(15); }, 60000);
  auto plan = design->readyPreview();
  size_t movedTops = 0;
  if (plan)
    for (const auto& m : plan->moved) movedTops += std::find(tops.begin(), tops.end(), m.node) != tops.end();
  const size_t underTops = [&] { size_t n = 0; for (const auto& t : tops) n += doc->scene.bodies_under(t).size(); return n; }();
  const size_t drawnMoved = underTops + (mine.empty() ? 0 : 1);  // the body of its own is placed as it is too
  require(shown && movedTops == tops.size() && view->previewMovedCount() == drawnMoved && view->previewMotion(other).TranslationPart().Z() == 15 &&
              !view->previewStandsIn(part) && !view->previewStandsIn(other),
          QString("preview: every part of the file drawn 15 mm up as it is (%1 drawn moved for %2 parts%3, none stands in)")
              .arg(view->previewMovedCount()).arg(underTops).arg(mine.empty() ? "" : " and the body"));
  if (!mine.empty())
    require(!view->previewStandsIn(mine) && view->previewBodyCount() == 0 && std::abs(view->previewMotion(mine).TranslationPart().Z() - 15) < 1e-9,
            "preview: the body of its own drawn 15 mm up as it is, nothing meshed");
  TranslateTriad* triad = design->moveTriad();
  require(triad && triad->shown() && closeTo(triad->at(), {pickedMiddle[0], pickedMiddle[1], pickedMiddle[2] + 15}, 1e-3),
          QString("the triad at the middle of the file and the body, moved: %1, expected %2").arg(triad ? text(triad->at()) : "none", text({pickedMiddle[0], pickedMiddle[1], pickedMiddle[2] + 15})));
  view->grabImage().save(prefix + ".preview.png");
  if (QWidget* panel = form->window()) panel->grab().save(prefix + ".panel.png");

  // 2. Committed: the top node placed, the parts the file's, the body moved; one undo step.
  emit form->accepted();
  settle([&] { return !design->featureActive() && !doc->designBusy && doc->doc.ops.size() > ops0; }, 60000);
  const opad::Feature* move = doc->scene.features.empty() ? nullptr : &doc->scene.features.back();
  const std::string moveId = move && move->kind == "move" ? move->id : std::string();
  const opad::json placements = move ? move->result.value("placements", opad::json::array()) : opad::json::array();
  require(!moveId.empty() && placements.size() == tops.size() + (mine.empty() ? 0 : 1) && move->error.empty() && doc->doc.ops[ops0].id == moveId && view->previewMovedCount() == 0,
          QString("committed: the feature op placing the file's %1 top node(s) and the body %2 (%3 ops)").arg(tops.size()).arg(QString::fromStdString(placements.dump()).left(160)).arg(doc->doc.ops.size() - ops0));
  auto moved = [&](double dz) {
    return closeTo(at(doc->scene.world(part)), {part0[0], part0[1], part0[2] + dz}) && closeTo(at(doc->scene.world(other)), {other0[0], other0[1], other0[2] + dz}) &&
           closeTo(at(doc->scene.world(tops[0])), {top0[0], top0[1], top0[2] + dz}) &&
           (mine.empty() || closeTo(middle(box(mine)), {mineMiddle0[0], mineMiddle0[1], mineMiddle0[2] + dz}, 0.5));
  };
  const bool keys = doc->node(part)->body_key == partKey && doc->node(other)->body_key == otherKey && doc->doc.body(partKey) && doc->doc.body(partKey)->external;
  require(moved(15) && keys, QString("the whole file moved 15 mm up, its parts' keys kept and not stored (%1): the part at %2, the other part at %3, the body's middle at %4")
                                 .arg(keys)
                                 .arg(text(at(doc->scene.world(part))), text(at(doc->scene.world(other))), mine.empty() ? QString() : text(middle(box(mine)))));
  if (!mine.empty()) require(doc->node(mine)->body_key == mineKey, "the body of its own placed as it is (its body key kept)");
  require(doc->undoLabels().value(0).contains("move"), "one undo step: " + doc->undoLabels().value(0));
  require(doc->scene.unresolved.empty(), QString("nothing unresolved (%1)").arg(doc->scene.unresolved.size()));

  // 3. Edited: rolled back (the file where it was), 25 mm previewed from there and committed.
  design->editOp(moveId);
  const bool rolled = settle([&] { return design->featureActive() && closeTo(at(doc->scene.world(part)), part0) && !design->previewing(); }, 30000);
  require(rolled && form->pickText("bodies").contains("moves as one"), "editing: rolled back to where the file was, the box says it moves as one");
  form->setValue("dz", "25 mm");
  const bool edited = settle([&] { return previewed(25); }, 60000);
  require(edited, QString("edit preview: the file 25 mm up from where it was (%1)").arg(view->previewMotion(part).TranslationPart().Z()));
  emit form->accepted();
  settle([&] { return !design->featureActive() && !doc->designBusy && moved(25); }, 60000);
  require(moved(25), QString("edited: the file 25 mm up (%1)").arg(text(at(doc->scene.world(part)))));
  doc->undo();
  const bool undone = moved(15);
  doc->redo();
  require(undone && moved(25), "undo puts it back 15 mm up, redo 25 mm");
  doc->undo(2);
  const bool gone = closeTo(at(doc->scene.world(part)), part0) && closeTo(at(doc->scene.world(other)), other0);
  doc->redo(2);
  require(gone && moved(25), "the Move undone: the file where it was; redone");
  if (!mine.empty()) require(closeTo(at(doc->scene.world(mine)), {mine0[0], mine0[1], mine0[2] + 25}), "the body's node placed 25 mm up (its shape as it was)");

  // 4. Saved and read again: the same text, the same places, nothing to regenerate.
  const QString saved = QFileInfo(doc->path()).absolutePath() + "/linked-move-saved.opad";
  const bool wrote = doc->saveAs(saved);
  bool same = false;
  try {
    opad::Document again = opad::Document::load(saved.toStdU16String());
    opad::AssetOptions trust;
    trust.trust_all = true;  // the file the app read (trusted there)
    opad::load_assets(again, trust);
    const opad::Scene s = opad::resolve(again);
    same = again.serialize() == doc->doc.serialize() && s.world(part).m == doc->scene.world(part).m && s.world(other).m == doc->scene.world(other).m &&
           opad::design::plan_regenerate(again).ops.empty();
  } catch (const std::exception& e) {
    trace::log(QString("bench: linked move: reading it again: %1").arg(e.what()));
  }
  require(wrote && same, "saved and read again: the same text and places, nothing to regenerate");

  // 5. Synced from next/<file> (a newer board) when there is one: the place the Move gave it is kept.
  const QString dir = QFileInfo(QString::fromStdString(opad::asset_of(doc->doc, import).value("abs", ""))).absolutePath();
  const QString next = QFileInfo(doc->path()).absolutePath() + "/next/" + file;
  if (QFileInfo::exists(next) && area) {
    // Written anew (now as its time: a copy keeps the time, and a hash remembered by path, size and time would match).
    QFile in(next), out(dir + "/" + file);
    const QByteArray bytes = in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    const bool copied = !bytes.isEmpty() && out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(bytes) == bytes.size();
    out.close();
    settle([&] { const opad::json* s = monitor->state(import); return s && s->value("state", "") == "changed" && !monitor->checking(); }, 20000);
    const size_t before = doc->doc.ops.size();
    area->sync({import});
    const bool synced = settle([&] { return !area->busy() && doc->doc.ops.size() > before && !doc->designBusy && loaded(); }, 60000);
    const opad::Feature* f = doc->scene.feature(moveId);
    require(copied && synced && f && f->error.empty() && closeTo(at(doc->scene.world(tops[0])), {top0[0], top0[1], top0[2] + 25}) &&
                closeTo(at(doc->scene.world(other)), {other0[0], other0[1], other0[2] + 25}),
            QString("synced from next/%1: the file stays 25 mm up (top node at %2)").arg(file, text(at(doc->scene.world(tops[0])))));
  }

  // 6. A copy of the file is refused, saying why.
  design->startFeature("move");
  settle([&] { return design->featureActive() && form->activeInput() == "bodies"; }, 5000);
  emit w.m_browser->selectionChanged({part});
  settle([&] { const opad::json p = form->picks("bodies"); return p.is_array() && p.size() == 1; }, 5000);
  form->setValues({{"dz", "5 mm"}, {"copy", true}});
  settle([&] { return !design->previewing() && form->statusText().contains("cannot be copied"); }, 30000);
  require(form->statusText().startsWith("A linked file cannot be copied"), "a copy refused: " + form->statusText());
  design->escape();
  settle([&] { return !design->featureActive(); }, 5000);
  return finish();
}
