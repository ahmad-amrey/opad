// Exploded-view drawings (UI-85) in the Drawings workspace. Case in tools/bench_cases/docs.py.
#include "MainWindow.hpp"

#include <QElapsedTimer>
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "Toast.hpp"
#include "opad/drawing/sheet.hpp"

// OPAD_BENCH_SHEET_EXPLODED=<prefix>: a plate, a post and a lid stacked, drawn front on an A3 sheet. Exploded view with
// none saved says how to save one and places nothing; with "Exploded 1" saved (explode along Z), it follows the pointer
// at the size the exploded parts take and a click places a view of it: one step, the view op naming the exploded view and
// seen from its camera, drawn apart with its trail lines on the Trail layer, the front view as it was.
// <prefix>.exploded.png.
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
    const Vec2 target{sh->width * 0.62, sh->height * 0.55};
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
      check(trail >= 0 && d.layers[size_t(trail)].line == opad::drawing::LineType::Phantom && inView(v->id, trail) >= 2 && inView(front, trail) == 0,
            QString("drawn apart with its trail lines on the Trail layer (%1), the front view without").arg(trail >= 0 ? inView(v->id, trail) : 0));
      opad::drawing::ViewSpec spec = opad::drawing::view_spec(doc->scene, *v);
      opad::drawing::resolve_explode(doc->doc, doc->scene, spec);
      check(spec.offsets.size() >= 2, QString("the lid and the post drawn moved (%1 parts)").arg(spec.offsets.size()));
    }
    page->grab().save(prefix + ".exploded.png");
  } catch (const std::exception& e) {
    check(false, QString("bench stopped: %1").arg(e.what()));
  }
  trace::log(QString("bench: sheet-exploded: %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
