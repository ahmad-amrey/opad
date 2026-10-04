// Exploded-view drawings (UI-85) in the Drawings workspace. Case in tools/bench_cases/docs.py.
#include "MainWindow.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "DrawingsFolder.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "Toast.hpp"
#include "opad/drawing/sheet.hpp"

// OPAD_BENCH_SHEET_EXPLODED=<prefix>: a plate, a post and a lid stacked, drawn front on an A3 sheet. Exploded view with
// none saved says how to save one and places nothing; with "Exploded 1" saved (explode along Z), it follows the pointer
// at the size the exploded parts take and a click places a view of it: one step, the view op naming the exploded view and
// seen from its camera, drawn apart with its trail lines on the Trail layer, the front view as it was. Auto-balloon with
// no view selected balloons the exploded view and adds the parts list; Exploded 1 updated with twice the spacing (the
// Explode panel's Update view), the drawing view draws its parts further apart, the balloons still measured. The front
// view's View state: Exploded 1 (one step, still from the front, with trail lines), then Assembled again. Then Publish PDF
// from Review (UI-104, Review > Share and the File menu): the drawing's sheet written as a PDF, the workspace kept.
// <prefix>.exploded.png (both views apart), <prefix>.publish.pdf.
OPAD_BENCH(OPAD_BENCH_SHEET_EXPLODED, sheetExploded) {
  using opad::drawing::Vec2;
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage();
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sheet-exploded: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: sheet-exploded: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  SheetPage* page = docs->sheetPage();
  SheetCanvas* canvas = page->canvas();
  AppDocument* doc = w.m_doc;
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !doc->designBusy && !states.empty() &&
           std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return v.final || !v.error.isEmpty(); });
  };
  try {
    doc->newDocument();
    doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "8 mm"}}}});
    doc->run("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"origin", {0, 0, 8}}, {"normal", {0, 0, 1}}}}, {"diameter", 16}, {"height", 24}, {"operation", "new"}}}});
    doc->run("feature", {{"kind", "box"}, {"inputs", {{"plane", {{"origin", {0, 0, 32}}, {"normal", {0, 0, 1}}}}, {"length", "60 mm"}, {"width", "40 mm"}, {"height", "4 mm"}, {"operation", "new"}}}});
    w.action("workspace.drawings")->trigger();
    std::string sheet;
    docs->createDrawing({{"size", "A3"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "1:1"}, {"views", {"front"}}},
                        [&](const std::string& id) { sheet = id; });
    check(waitFor([&] { return !sheet.empty(); }, 20000) && waitFor(settled, 30000), "a drawing of the stack: its front view drawn");
    const opad::Sheet* sh = doc->scene.sheet(sheet);
    if (!sh || sh->views.size() != 1) throw opad::Error("no sheet");
    const std::string front = sh->views[0];
    const double sheetWidth = sh->width, sheetHeight = sh->height;  // the scene is made again by every change: sh goes
    // None saved: a hint, nothing to place.
    const size_t ops0 = doc->doc.ops.size();
    w.action("drawings.explodedView")->trigger();
    const QList<Toast*> shown = w.m_toasts->toasts();  // over the view, or in the status bar while the sheet is in its place
    const QString said = shown.isEmpty() ? w.statusBar()->currentMessage() : shown.back()->text();
    check(!canvas->placing() && doc->doc.ops.size() == ops0 && said.startsWith("Save an exploded view first"),
          "with no exploded view saved it says how to save one and places nothing: " + said);
    // "Exploded 1" saved: placed with the pointer, one step.
    doc->run("explode", {{"mode", "axis"}, {"name", "Exploded 1"}});
    const std::string exploded = doc->scene.views.empty() ? std::string() : doc->scene.views.back().id;
    check(!exploded.empty() && doc->scene.views.back().explode.is_object(), "an exploded view saved: Exploded 1");
    waitFor(settled, 30000);
    w.action("drawings.explodedView")->trigger();
    const bool sized = waitFor([&] { return canvas->placing() && canvas->placementSize()[1] > 30; }, 20000);
    check(sized, QString("Exploded view follows the pointer at the size its parts take apart (%1 mm high)").arg(canvas->placementSize()[1]));
    const size_t ops1 = doc->doc.ops.size();
    const Vec2 target{sheetWidth * 0.74, sheetHeight * 0.55};  // clear of the front view, also once that is drawn apart
    canvas->placeAt(target);  // where a click on the sheet places it (SheetBench's way: no window under a hidden one's events)
    check(waitFor([&] { return doc->doc.ops.size() > ops1 && !doc->designBusy; }, 15000) && waitFor(settled, 60000) && doc->doc.ops.size() == ops1 + 1,
          "a click places it: one step, drawn");
    const opad::Sheet* after = doc->scene.sheet(sheet);
    const opad::SheetView* v = after && after->views.size() == 2 ? doc->scene.sheet_view(after->views[1]) : nullptr;
    const opad::json at = v ? v->def.value("at", opad::json::array()) : opad::json::array();
    check(at.size() == 2 && std::fabs(at[0].get<double>() - target[0]) <= 1 && std::fabs(at[1].get<double>() - target[1]) <= 1,
          "placed where it was put: " + QString::fromStdString(at.dump()));
    check(v && v->error.empty() && v->def["source"]["explode"]["view"] == exploded && v->def["orient"]["view"] == exploded,
          "its view op names the exploded view and is seen from its camera: " + QString::fromStdString(v ? v->def.dump() : std::string()).left(240));
    if (v) {
      const opad::drawing::Display d = opad::drawing::sheet_display(doc->doc, doc->scene, *after);
      int trail = -1;
      for (size_t i = 0; i < d.layers.size(); ++i)
        if (d.layers[i].name == "Trail") trail = static_cast<int>(i);
      const auto inView = [&](const std::string& id, int layer) {
        return std::count_if(d.prims.begin(), d.prims.end(), [&](const opad::drawing::Prim& p) { return p.source == id && p.layer == layer; });
      };
      // A trail line inside a part is hidden by it: the post's runs inside the post, the lid's shows between them.
      check(trail >= 0 && d.layers[size_t(trail)].line == opad::drawing::LineType::Phantom && inView(v->id, trail) >= 1 && inView(front, trail) == 0,
            QString("drawn apart with its trail lines on the Trail layer (%1), the front view without").arg(trail >= 0 ? inView(v->id, trail) : 0));
      opad::drawing::ViewSpec spec = opad::drawing::view_spec(doc->scene, *v);
      opad::drawing::resolve_explode(doc->doc, doc->scene, spec);
      check(spec.offsets.size() >= 2, QString("the lid and the post drawn moved (%1 parts)").arg(spec.offsets.size()));
    }
    const std::string drawn = v ? v->id : std::string();  // the scene is made again by every change: v goes
    const auto stateOf = [&](const std::string& id) {
      for (const auto& s : canvas->viewStates())
        if (s.id == id) return s;
      return SheetCanvas::ViewState{};
    };
    const auto trails = [&](const std::string& id) {  // the view's prims on the Trail layer, as the sheet is drawn and exported
      const opad::Sheet* s = doc->scene.sheet(sheet);
      if (!s) return 0L;
      const opad::drawing::Display d = opad::drawing::sheet_display(doc->doc, doc->scene, *s);
      long n = 0;
      for (const auto& p : d.prims) n += p.source == id && p.layer >= 0 && size_t(p.layer) < d.layers.size() && d.layers[size_t(p.layer)].name == "Trail";
      return n;
    };
    const auto count = [&](const char* kind, const std::string& view) {
      int n = 0;
      if (const opad::Sheet* s = doc->scene.sheet(sheet))
        for (const auto& id : s->items)
          if (const opad::SheetItem* t = doc->scene.sheet_item(id); t && t->kind == kind && (view.empty() || t->view == view)) ++n;
      return n;
    };
    {  // the browser's Drawings folder tells it apart: named after the exploded view, its icon
      std::function<const drawings::Row*(const std::vector<drawings::Row>&)> find = [&](const std::vector<drawings::Row>& rows) -> const drawings::Row* {
        for (const auto& r : rows) {
          if (r.id == drawn) return &r;
          if (const drawings::Row* in = find(r.children)) return in;
        }
        return nullptr;
      };
      const auto rows = drawings::rows(*doc);
      const drawings::Row* row = find(rows);
      check(row && row->name.contains("Exploded 1") && row->icon == "explodedView" && row->tooltip.contains("Exploded 1"),
            "the Drawings folder names the view after Exploded 1, with the exploded view's icon: " + (row ? row->name : QString()));
    }
    // Auto-balloon with no view selected: the exploded view's parts (an assembly drawing's balloons), a parts list with them.
    canvas->selectViews({});
    w.action("drawings.autoBalloon")->trigger();
    const bool ballooned = waitFor([&] { return count("balloon", drawn) >= 3 && !doc->designBusy; }, 30000) && waitFor(settled, 60000);
    check(ballooned && count("balloon", front) == 0 && count("parts_list", "") == 1 && canvas->dangling().empty(),
          QString("Auto-balloon with no view selected balloons the exploded view (%1 balloons) and adds the parts list").arg(count("balloon", drawn)));
    // The exploded view updated (Explode panel's Update view: one edit of its explode): the drawing view follows.
    const QRectF wasDrawn = stateOf(drawn).linework;
    doc->run("explode", {{"view", exploded}, {"spacing", 2}, {"update", true}});
    const bool followed = waitFor([&] { return settled() && stateOf(drawn).final && stateOf(drawn).linework.height() > wasDrawn.height() + 5; }, 60000);
    check(followed && canvas->dangling().empty(),
          QString("the exploded view updated with twice the spacing: the drawing view draws its parts further apart (%1 -> %2 mm high), its balloons still on them")
              .arg(wasDrawn.height(), 0, 'f', 1).arg(stateOf(drawn).linework.height(), 0, 'f', 1));
    // View state on the front view: Exploded 1 draws its parts apart from the front, one step.
    {
      QMenu menu;
      docs->viewMenu({front}, menu);
      QAction* assembled = menu.findChild<QAction*>("drawings.menu.state.assembled");
      QAction* apart = menu.findChild<QAction*>(QString::fromStdString("drawings.menu.state." + exploded));
      check(assembled && apart && assembled->isChecked() && !apart->isChecked(), "the front view's menu: View state with Assembled ticked and Exploded 1");
      const size_t before = doc->doc.ops.size();
      const QRectF was = stateOf(front).linework;
      if (apart) apart->trigger();
      const bool stepped = waitFor([&] { return doc->doc.ops.size() == before + 1 && !doc->designBusy; }, 15000) && waitFor(settled, 60000);
      check(stepped, "Exploded 1 chosen: one step, drawn");
      const opad::SheetView* f = doc->scene.sheet_view(front);
      check(f && f->def["source"]["explode"]["view"] == exploded && f->def["orient"].value("preset", "") == "front" && trails(front) >= 1 &&
                stateOf(front).linework.height() > was.height() + 5,
            QString("the front view draws the parts apart, still from the front, with trail lines (%1 -> %2 mm high)")
                .arg(was.height(), 0, 'f', 1).arg(stateOf(front).linework.height(), 0, 'f', 1));
    }
    page->grab().save(prefix + ".exploded.png");
    {
      QMenu menu;
      docs->viewMenu({front}, menu);
      QAction* assembled = menu.findChild<QAction*>("drawings.menu.state.assembled");
      QAction* apart = menu.findChild<QAction*>(QString::fromStdString("drawings.menu.state." + exploded));
      check(assembled && apart && apart->isChecked() && !assembled->isChecked(), "its menu ticks Exploded 1 now");
      const size_t before = doc->doc.ops.size();
      if (assembled) assembled->trigger();
      check(waitFor([&] { return doc->doc.ops.size() == before + 1 && !doc->designBusy; }, 15000) && waitFor(settled, 60000), "Assembled chosen: one step");
      const opad::SheetView* f = doc->scene.sheet_view(front);
      check(f && !f->def.value("source", opad::json::object()).contains("explode") && trails(front) == 0, "the front view is drawn assembled again, without trail lines");
    }
    // Publish PDF from Review: the drawing written as a PDF without going to Drawings (no file dialog in a bench).
    w.setWorkspace("review");
    const QString pdf = prefix + ".publish.pdf";
    QFile::remove(pdf);
    qputenv("OPAD_BENCH_EXPORT_OUT", pdf.toUtf8());
    const CommandInfo* publish = w.m_commands.find("drawings.publish");
    w.updateCommands();
    check(publish && publish->menuPath == "file" && publish->workspaces.contains("review") && w.action("drawings.publish")->isEnabled(),
          "Publish PDF in the File menu and on Review > Share, enabled with a drawing in the document");
    w.action("drawings.publish")->trigger();
    const auto written = [&] {
      QFile f(pdf);
      return f.open(QIODevice::ReadOnly) && f.read(5) == "%PDF-";
    };
    check(waitFor(written, 60000) && w.workspaceId() == "review", "Publish PDF from Review writes the drawing as a PDF and stays in Review");
    qunsetenv("OPAD_BENCH_EXPORT_OUT");
  } catch (const std::exception& e) {
    check(false, QString("bench stopped: %1").arg(e.what()));
  }
  trace::log(QString("bench: sheet-exploded: %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
