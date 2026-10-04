#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QMenu>
#include <QMouseEvent>

#include <cmath>
#include <set>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "SheetAnnotate.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/drawing/tables.hpp"

// OPAD_BENCH_SHEET_TABLES=<prefix> (UI-84): a plate with four pins and a bracket (one row of four pins) drawn front, top and
// iso. Parts list: the tool plans it as it starts, the preview (header and rows) follows the pointer, a click places it
// with its numbers settled (1 to 3). Balloon: a click on a pin's edge in the front view plans its number (the pins' row), a
// click places it. Auto-balloon on the iso view: a balloon for each row, outside the view, one step (Ctrl+Z takes all
// three back, Ctrl+Y returns them). Quantity from the bar on the selected balloon; the parts list dragged by its corner; a
// washer added later is numbered after the highest; the bracket left out of the BoM keeps its number until the list's menu
// renumbers it. <prefix>.tables.png.
OPAD_BENCH(OPAD_BENCH_SHEET_TABLES, sheetTables) {
  using opad::drawing::Vec2;
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage();
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: tables: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: tables: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  SheetPage* page = docs->sheetPage();
  SheetCanvas* canvas = page->canvas();
  SheetAnnotator* tools = page->annotator();
  AppDocument* doc = w.m_doc;
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !tools->busy() && !doc->designBusy && !states.empty() &&
           std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return v.final || !v.error.isEmpty(); });
  };
  const auto mouse = [&](QEvent::Type type, const QPointF& scene) {
    QWidget* vp = canvas->viewport();
    const QPoint p = canvas->mapFromScene(scene);
    QMouseEvent e(type, QPointF(p), QPointF(vp->mapToGlobal(p)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease || type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &e);
  };
  const auto moveTo = [&](Vec2 paper) { mouse(QEvent::MouseMove, canvas->toScene(paper)); };
  const auto click = [&](Vec2 paper) {
    const QPointF s = canvas->toScene(paper);
    mouse(QEvent::MouseMove, s);
    mouse(QEvent::MouseButtonPress, s);
    mouse(QEvent::MouseButtonRelease, s);
  };
  const auto items = [&](const std::string& kind) {
    std::vector<const opad::SheetItem*> out;
    if (const opad::Sheet* s = doc->scene.sheet(page->sheet()))
      for (const auto& id : s->items)
        if (const opad::SheetItem* t = doc->scene.sheet_item(id); t && t->kind == kind) out.push_back(t);
    return out;
  };
  const auto added = [&](const std::string& kind, size_t n) {
    return waitFor([&] { return items(kind).size() >= n && !doc->designBusy; }, 15000) && waitFor(settled, 30000);
  };
  const auto planned = [&] { return waitFor([&] { return !tools->busy(); }, 15000); };
  const auto previewText = [&](const std::string& text) {
    const auto& p = canvas->preview();
    return p && std::any_of(p->prims.begin(), p->prims.end(), [&](const opad::drawing::Prim& x) { return x.kind == opad::drawing::Prim::Kind::Text && x.text == text; });
  };
  try {
    doc->newDocument();
    const auto made = [&](const opad::json& out) { return out["body_ids"][0].get<std::string>(); };
    const std::string plate = made(doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "5 mm"}}}}));
    std::vector<std::string> pins;
    for (const auto& [x, y] : std::initializer_list<std::pair<double, double>>{{-20, -10}, {20, -10}, {-20, 10}, {20, 10}})
      pins.push_back(made(doc->run("feature", {{"kind", "cylinder"},
                                              {"inputs", {{"plane", {{"origin", {0, 0, 5}}, {"normal", {0, 0, 1}}}}, {"x", x}, {"y", y}, {"diameter", 6}, {"height", 20}, {"operation", "new"}}}})));
    const std::string bracket = made(doc->run("feature", {{"kind", "box"},
                                                          {"inputs", {{"plane", {{"origin", {-45, 0, 0}}, {"normal", {0, 0, 1}}}}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "30 mm"}, {"operation", "new"}}}}));
    doc->run("rename", {{"target", plate}, {"name", "Plate"}});
    doc->run("rename", {{"target", bracket}, {"name", "Bracket"}});
    doc->run("rename", {{"targets", pins}, {"name", "Pin"}});
    w.action("workspace.drawings")->trigger();
    std::string sheet;
    docs->createDrawing({{"size", "A3"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "1:1"}, {"views", {"front", "top", "iso"}}},
                        [&](const std::string& id) { sheet = id; });
    check(waitFor([&] { return !sheet.empty(); }, 20000) && waitFor(settled, 30000), "a drawing of the assembly: front, top and iso views drawn");
    const opad::Sheet* sh = doc->scene.sheet(sheet);
    if (!sh || sh->views.size() != 3) throw opad::Error("no sheet");
    const std::string front = sh->views[0], iso = sh->views[2];
    const auto paper = [&](const std::string& view, opad::Vec3 p) {
      const opad::drawing::ViewFrame* f = canvas->frame(view);
      return f ? f->paper(p) : Vec2{0, 0};
    };

    // The parts list: planned as the tool starts, it follows the pointer; a click places it with its numbers settled.
    w.action("drawings.partsList")->trigger();
    check(tools->tool() == SheetAnnotator::Tool::PartsList && tools->listModeBox()->isVisible(), "Parts list starts its tool with the Lists option");
    planned();
    const Vec2 corner{sh->width - 10, 10 + 36};
    moveTo(corner);
    check(previewText("ITEM") && previewText("Plate") && previewText("Pin") && previewText("Bracket") && previewText("4"),
          "the preview draws the header and a row each for the plate, the four pins and the bracket");
    const size_t ops0 = doc->doc.ops.size();
    click(corner);
    check(added("parts_list", 1) && doc->doc.ops.size() == ops0 + 1, "a click places it: one step");
    const std::string list = items("parts_list").empty() ? std::string() : items("parts_list")[0]->id;
    const auto numberOf = [&](const std::string& node) {
      const opad::SheetItem* t = doc->scene.sheet_item(list);
      const opad::json rows = opad::drawing::parts_rows(doc->doc, doc->scene, *doc->scene.sheet(sheet), t->def)["rows"];
      const opad::json* r = opad::drawing::row_of(doc->scene, rows, node);
      return r ? r->value("number", 0) : 0;
    };
    if (!list.empty()) {
      const opad::json at = doc->scene.sheet_item(list)->def["at"];
      const double px = 1.5 / canvas->pixelsPerMm();  // the pointer is in whole pixels
      check(doc->scene.sheet_item(list)->def["numbers"].size() == 3 && std::fabs(at[0].get<double>() - corner[0]) < px &&
                std::fabs(at[1].get<double>() - corner[1]) < px,
            "numbers 1 to 3 settled, its corner where it was clicked: " + QString::fromStdString(at.dump() + " " + doc->scene.sheet_item(list)->def["numbers"].dump()));
      check(numberOf(plate) == 1 && numberOf(pins[2]) == 2 && numberOf(bracket) == 3, "the plate 1, the pins 2, the bracket 3");
      check(canvas->itemBox(list).has_value(), "the canvas can pick it");
    }
    tools->cancel();

    // A balloon on a pin's top edge in the front view: the pins' number.
    w.action("drawings.balloon")->trigger();
    check(tools->tool() == SheetAnnotator::Tool::Balloon && tools->qtyBox()->isVisible() && !tools->prompt().isEmpty(), "Balloon starts its tool: a prompt and Quantity");
    const Vec2 pinTop = paper(front, {20, -10, 25});
    click(pinTop);
    check(planned() && tools->plan().value("measured", opad::json::object()).value("number", "") == "2", "a pin picked: its row's number, 2");
    moveTo({pinTop[0] + 12, pinTop[1] + 18});
    check(previewText("2"), "the balloon follows the pointer");
    click({pinTop[0] + 12, pinTop[1] + 18});
    check(added("balloon", 1) && items("balloon")[0]->def["result"]["shown"] == "2", "a click places it, showing 2");
    tools->cancel();

    // Auto-balloon on the iso view: one balloon per row, around the view, one step.
    canvas->selectViews({iso});
    const size_t before = doc->doc.ops.size();
    w.action("drawings.autoBalloon")->trigger();
    check(added("balloon", 4) && doc->doc.ops.size() == before + 3, "Auto-balloon puts three balloons on the iso view in one step");
    {
      std::set<std::string> numbers;
      bool outside = true;
      const opad::drawing::ViewFrame* f = canvas->frame(iso);
      for (const auto* b : items("balloon")) {
        if (b->view != iso) continue;
        numbers.insert(b->def["result"]["shown"].get<std::string>());
        const Vec2 at{f->at[0] + b->def["place"]["text"][0].get<double>(), f->at[1] + b->def["place"]["text"][1].get<double>()};
        outside = outside && (at[0] < f->box[0] || at[0] > f->box[2] || at[1] < f->box[1] || at[1] > f->box[3]);
      }
      check(numbers == std::set<std::string>({"1", "2", "3"}) && outside, "numbered 1, 2, 3, each outside the view");
    }
    waitFor([&] { return doc->canUndo(); }, 10000);  // the canvas reads the document between edits
    doc->undo();
    check(waitFor([&] { return items("balloon").size() == 1; }, 5000), "Ctrl+Z takes the three back");
    waitFor([&] { return doc->canRedo(); }, 10000);
    doc->redo();
    check(waitFor([&] { return items("balloon").size() == 4; }, 5000) && waitFor(settled, 30000), "Ctrl+Y returns them");

    // The selected balloon's quantity from the bar.
    const std::string first = items("balloon")[0]->id;
    canvas->selectItems({first});
    tools->itemsSelected({first});
    check(tools->qtyBox()->isVisible(), "the bar edits the selected balloon: Quantity");
    tools->qtyBox()->setChecked(true);
    check(waitFor([&] { return doc->scene.sheet_item(first)->def.value("qty", false); }, 5000) && waitFor(settled, 30000), "ticked: the balloon writes 4×");

    // The parts list dragged by its corner: one edit of where it stands.
    const opad::json was = doc->scene.sheet_item(list)->def["at"];
    canvas->benchDragItem(list, {-20, 30});
    check(waitFor([&] {
            const opad::json at = doc->scene.sheet_item(list)->def["at"];
            return std::fabs(at[0].get<double>() - (was[0].get<double>() - 20)) < 0.01 && std::fabs(at[1].get<double>() - (was[1].get<double>() + 30)) < 0.01;
          }, 10000),
          "dragged: its corner moves with it");

    // A washer added later: numbered after the highest. The bracket left out keeps 3 until the list is renumbered.
    std::string washer;
    const auto edit = [&](const std::string& command, const opad::json& args, std::function<void(const opad::json&)> then = {}) {  // after the canvas's read
      bool done = false;
      docs->run(command, args, [&](const opad::json& out) {
        if (then && !out.is_null()) then(out);
        done = true;
      });
      waitFor([&] { return done; }, 15000);
    };
    edit("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"origin", {0, 30, 0}}, {"normal", {0, 0, 1}}}}, {"diameter", 10}, {"height", 2}, {"operation", "new"}}}},
         [&](const opad::json& out) { washer = made(out); });
    edit("rename", {{"target", washer}, {"name", "Washer"}});
    edit("part_properties", {{"target", bracket}, {"set", {{"bom", "exclude"}}}});
    check(waitFor(settled, 30000) && numberOf(washer) == 4 && numberOf(bracket) == 0, "a washer added later is 4; the bracket left out is not listed");
    QMenu menu;
    docs->itemMenu({list}, menu);
    QAction* renumber = menu.findChild<QAction*>("drawings.menu.renumber");
    check(renumber != nullptr, "the parts list's menu offers Renumber items");
    if (renumber) renumber->trigger();
    check(waitFor([&] { return numberOf(washer) == 3; }, 15000) && waitFor(settled, 30000), "renumbered: the washer is 3");
    canvas->fitSheet();
    waitFor(settled, 10000);
    QCoreApplication::processEvents();
    page->grab().save(prefix + ".tables.png");
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: tables: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
