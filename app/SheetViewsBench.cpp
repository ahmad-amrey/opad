#include "MainWindow.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>

#include <cmath>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "SheetViewTool.hpp"
#include "opad/drawing/sheet.hpp"

using opad::drawing::Vec2;

// OPAD_BENCH_SHEET_VIEWS=<prefix> (UI-82): section, detail and auxiliary views, crops and breaks with mouse and key events
// on the sheet canvas, on a 60 x 40 x 10 plate with a 10 mm hole drawn front and top at 2:1. Section view: the cutting
// line clicked on the front view (previewed, kept upright), Enter, the view measured on a worker and following the
// pointer lined up on the side it goes, a click places it (one step): hatched, labelled A-A, the front view drawing its
// cutting line; dragged, it moves only away from its parent; Esc steps back a point, then leaves. Detail view: centre,
// radius, place (5:1, the next standard scale from twice 2:1). Auxiliary view square to an edge of the top view, lined up.
// Crop by dragging a box on the top view, Break by two clicks on the front view (the top view broken with it), Remove
// crop from the view's menu, Ctrl+Z. <prefix>.views.png.
OPAD_BENCH(OPAD_BENCH_SHEET_VIEWS, sheetViews) {
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage();
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sheet-views: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: sheet-views: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  SheetPage* page = docs->sheetPage();
  SheetCanvas* canvas = page->canvas();
  SheetViewTool* tool = page->viewTool();
  AppDocument* doc = w.m_doc;
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !tool->measuring() && !doc->designBusy && !states.empty() &&
           std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return v.final || !v.error.isEmpty(); });
  };
  const auto mouse = [&](QEvent::Type type, const QPointF& scene) {
    QWidget* vp = canvas->viewport();
    const QPoint p = canvas->mapFromScene(scene);
    QMouseEvent e(type, QPointF(p), QPointF(vp->mapToGlobal(p)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease || type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &e);
  };
  const auto click = [&](const QPointF& s) {
    mouse(QEvent::MouseMove, s);
    mouse(QEvent::MouseButtonPress, s);
    mouse(QEvent::MouseButtonRelease, s);
  };
  const auto key = [&](int k) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
    QApplication::sendEvent(canvas, &press);
  };
  const auto views = [&](const std::string& kind) {
    std::vector<std::string> out;
    if (const opad::Sheet* s = doc->scene.sheet(page->sheet()))
      for (const auto& id : s->views)
        if (const opad::SheetView* v = doc->scene.sheet_view(id); v && v->kind == kind) out.push_back(id);
    return out;
  };
  const auto at = [&](const std::string& view, Vec2 v) {  // a point in a view's coordinates, on the canvas
    const opad::drawing::ViewFrame* f = canvas->frame(view);
    if (!f) return QPointF();
    const Vec2 l = f->local(v);
    return canvas->toScene({f->at[0] + l[0], f->at[1] + l[1]});
  };
  const auto frameOf = [&](const std::string& id) -> opad::drawing::ViewFrame {
    const opad::drawing::ViewFrame* f = canvas->frame(id);
    return f ? *f : opad::drawing::ViewFrame{};
  };
  const auto select = [&](const std::string& id) {
    canvas->selectViews({id});
    w.updateCommands();
  };
  const auto onLayer = [&](const opad::drawing::Display& d, const std::string& layer, const std::string& source) {
    return std::count_if(d.prims.begin(), d.prims.end(), [&](const opad::drawing::Prim& p) { return d.layers[static_cast<size_t>(p.layer)].name == layer && p.source == source; });
  };
  try {
    doc->newDocument();
    const opad::json made = doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}});
    const std::string body = made["body_ids"][0];
    doc->run("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 0}, {"y", 0}, {"diameter", 10}, {"height", 10}, {"operation", "cut"}, {"targets", {body}}}}});
    w.action("workspace.drawings")->trigger();
    std::string sheet;
    docs->createDrawing({{"size", "A3"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "2:1"}, {"views", {"front", "top"}}},
                        [&](const std::string& id) { sheet = id; });
    check(waitFor([&] { return !sheet.empty(); }, 20000) && waitFor(settled, 30000), "a drawing of the plate: front and top at 2:1");
    const opad::Sheet* sh = doc->scene.sheet(sheet);  // the scene is rebuilt by every step: what is needed of it, kept
    if (!sh || sh->views.size() != 2) throw opad::Error("no sheet");
    const std::string front = sh->views[0], top = sh->views[1];
    const double paperW = sh->width;
    canvas->fitSheet();
    QCoreApplication::processEvents();

    // Section view: the cutting line clicked on the front view, Enter, then placed on the side it goes.
    select(front);
    check(w.action("drawings.sectionView")->isEnabled(), "Section view is offered for the selected view");
    w.action("drawings.sectionView")->trigger();
    check(tool->tool() == SheetViewTool::Tool::Section && !tool->prompt().isEmpty(), "Section view starts its tool and asks for the cutting line");
    click(at(front, {0, -12}));
    click(at(front, {0.6, 22}));  // a little off upright: kept upright
    check(tool->points() == 2 && canvas->preview() && !canvas->preview()->prims.empty(), "two points clicked, the cutting line previewed");
    key(Qt::Key_Escape);
    check(tool->points() == 1 && tool->active(), "Esc takes back the last point");
    click(at(front, {0.6, 22}));
    key(Qt::Key_Return);
    check(waitFor([&] { return tool->placing(); }, 15000), "Enter ends the line; the view is measured on a worker and follows the pointer");
    const opad::drawing::ViewFrame fr = frameOf(front);
    const QPointF leftOf = canvas->toScene({fr.box[0] - 50, fr.at[1] + 30});
    mouse(QEvent::MouseMove, leftOf);
    const QRectF ghost = tool->ghost();
    check(ghost.isValid() && ghost.right() < canvas->toScene({fr.box[0], 0}).x() && std::fabs(ghost.center().y() - canvas->toScene(fr.at).y()) < 0.01,
          "its frame follows the pointer left of the front view, lined up with it");
    const size_t ops = doc->doc.ops.size();
    click(leftOf);
    check(waitFor([&] { return views("section").size() == 1; }, 15000) && doc->doc.ops.size() == ops + 1 && waitFor(settled, 30000), "a click places it: one step");
    const std::string sec = views("section").empty() ? std::string() : views("section")[0];
    const opad::SheetView* sv = doc->scene.sheet_view(sec);
    check(sv && sv->def.value("letter", "") == "A" && sv->def["cut"].size() == 2 && std::fabs(sv->def["cut"][1][0].get<double>() - sv->def["cut"][0][0].get<double>()) < 1e-9 &&
              !tool->active(),
          "section A with an upright cutting line; the tool ends");
    {
      const auto d = opad::drawing::sheet_display(doc->doc, doc->scene, *doc->scene.sheet(sheet));
      const auto texts = [&](const std::string& source, const std::string& t) {
        return std::any_of(d.prims.begin(), d.prims.end(), [&](const opad::drawing::Prim& p) { return p.source == source && p.kind == opad::drawing::Prim::Kind::Text && p.text == t; });
      };
      check(onLayer(d, "Hatch", sec) > 10 && texts(sec, "A-A") && onLayer(d, "Section line", front) == 1 && texts(front, "A"),
            "drawn hatched and labelled A-A; the front view draws the cutting line and its letters");
    }
    const auto st = [&](const std::string& id) {
      for (const auto& v : canvas->viewStates())
        if (v.id == id) return v;
      return SheetCanvas::ViewState{};
    };
    check(st(sec).final && st(sec).prims > 10 && st(sec).frame.right() < st(front).frame.left(), "on the canvas: final linework, left of the front view");
    // Dragged: only away from or towards its parent (its gap).
    const double gap = sv ? sv->def.value("gap", 20.0) : 0;
    canvas->benchDrag(sec, {-10, 7});
    check(waitFor([&] { const opad::SheetView* v = doc->scene.sheet_view(sec); return v && std::fabs(v->def.value("gap", 0.0) - (gap + 10)) < 0.05; }, 10000) &&
              waitFor(settled, 30000) && std::fabs(frameOf(sec).at[1] - frameOf(front).at[1]) < 1e-6,
          "dragged left and up: 10 mm further away, still lined up");
    // Bodies left uncut: from the section's menu, a click on the plate draws it whole; again, cut.
    {
      QMenu menu;
      docs->viewMenu({sec}, menu);
      QAction* uncut = menu.findChild<QAction*>("drawings.menu.uncut");
      check(uncut != nullptr, "a section view's menu offers Leave bodies uncut…");
      if (uncut) uncut->trigger();
      check(tool->tool() == SheetViewTool::Tool::Uncut, "it starts its tool");
      const auto whole = [&] { const opad::SheetView* v = doc->scene.sheet_view(sec); return v ? v->def.value("whole", opad::json::array()) : opad::json::array(); };
      const auto hatched = [&] {
        const auto d = opad::drawing::sheet_display(doc->doc, doc->scene, *doc->scene.sheet(sheet));
        return onLayer(d, "Hatch", sec);
      };
      click(at(sec, {-12, 5}));  // inside the plate's hatched cut face
      check(waitFor([&] { return whole().size() == 1 && settled(); }, 15000) && hatched() == 0 && tool->active(), "a click on the plate: drawn whole, not hatched; the tool stays");
      click(at(sec, {-20, 5}));  // its outline, drawn whole now
      check(waitFor([&] { return whole().empty() && settled(); }, 15000) && hatched() > 10, "a click again: cut and hatched");
      key(Qt::Key_Escape);
      check(!tool->active(), "Esc ends it");
    }
    // Esc with nothing clicked leaves.
    select(front);
    w.action("drawings.sectionView")->trigger();
    key(Qt::Key_Escape);
    check(!tool->active() && tool->prompt().isEmpty(), "Esc with no point leaves the tool");

    // Detail view: centre, radius, place.
    select(front);
    w.action("drawings.detailView")->trigger();
    click(at(front, {25, 5}));
    mouse(QEvent::MouseMove, at(front, {33, 5}));
    check(canvas->preview() && !canvas->preview()->prims.empty(), "the detail's circle follows the pointer");
    click(at(front, {33, 5}));
    check(tool->placing(), "then the detail view follows the pointer");
    const QPointF spot = canvas->toScene({paperW - 90, 110});
    click(spot);
    check(waitFor([&] { return views("detail").size() == 1; }, 15000) && waitFor(settled, 30000), "a click places the detail view");
    const std::string det = views("detail").empty() ? std::string() : views("detail")[0];
    const opad::drawing::ViewFrame df = frameOf(det);
    check(df.scale == 5 && std::fabs(df.radius - 8) < 0.3 && std::fabs(df.box[2] - df.box[0] - 10 * df.radius) < 1e-6 && doc->scene.sheet_view(det)->def.value("letter", "") == "B" &&
              st(det).final && st(det).prims > 0 && df.box[0] > 0 && df.box[2] < paperW,
          QString("detail B at 5:1 (twice 2:1, the next standard scale), about 8 mm around its centre, on the paper where clicked: scale %1, radius %2")
              .arg(df.scale)
              .arg(df.radius));

    // Auxiliary view square to an edge of the top view.
    select(top);
    w.action("drawings.auxiliaryView")->trigger();
    mouse(QEvent::MouseMove, at(top, {30, 5}));
    check(canvas->preview() && !canvas->preview()->prims.empty(), "the edge under the pointer is highlighted");
    click(at(top, {30, 5}));
    check(waitFor([&] { return tool->placing(); }, 15000), "an edge picked: the view is measured and follows the pointer");
    const opad::drawing::ViewFrame tf = frameOf(top);
    const QPointF right = canvas->toScene({tf.box[2] + 50, tf.at[1] - 20});
    click(right);
    check(waitFor([&] { return views("auxiliary").size() == 1; }, 15000) && waitFor(settled, 30000), "a click places the auxiliary view");
    const std::string aux = views("auxiliary").empty() ? std::string() : views("auxiliary")[0];
    const opad::drawing::ViewFrame af = frameOf(aux);
    check(std::fabs(af.at[1] - tf.at[1]) < 1e-6 && af.box[0] > tf.box[2] && std::fabs(doc->scene.sheet_view(aux)->def.value("angle", 99.0)) < 1e-6 && st(aux).final,
          "square to the plate's right edge: right of the top view, lined up with it");

    // Crop: a box dragged on the top view (the left half).
    select(top);
    w.action("drawings.cropView")->trigger();
    mouse(QEvent::MouseMove, at(top, {-35, -25}));
    mouse(QEvent::MouseButtonPress, at(top, {-35, -25}));
    mouse(QEvent::MouseMove, at(top, {0, 25}));
    check(canvas->preview() && !canvas->preview()->prims.empty(), "the box follows the drag");
    mouse(QEvent::MouseButtonRelease, at(top, {0, 25}));
    const auto width = [&](const std::string& id) { return frameOf(id).box[2] - frameOf(id).box[0]; };
    check(waitFor([&] { return doc->scene.sheet_view(top)->def.contains("crop") && settled() && std::fabs(width(top) - 60) < 0.5 && std::fabs(st(top).linework.width() - 60) < 0.5; }, 30000),
          QString("released: one edit, the top view keeps its left half (30 mm at 2:1): frame %1 mm, linework %2 mm").arg(width(top)).arg(st(top).linework.width()));
    // Break: two clicks on the front view; the top view, lined up with it, breaks with it.
    select(front);
    w.action("drawings.breakView")->trigger();
    click(at(front, {-20, 5}));
    click(at(front, {-10, 5}));
    check(waitFor([&] { return doc->scene.sheet_view(front)->def.contains("breaks") && settled() && std::fabs(width(front) - 106) < 0.5 && frameOf(top).breaks.size() == 1; }, 30000),
          QString("broken: 10 mm taken out of the front view, 6 mm between the halves (%1 mm wide); the top view broken too").arg(width(front)));
    // Remove crop from the view's menu; Ctrl+Z puts it back.
    QMenu menu;
    docs->viewMenu({top}, menu);
    QAction* uncrop = menu.findChild<QAction*>("drawings.menu.uncrop");
    check(uncrop != nullptr && !menu.findChild<QAction*>("drawings.menu.unbreak"), "the top view's menu offers Remove crop (and no Remove breaks: it has none of its own)");
    if (uncrop) uncrop->trigger();
    check(waitFor([&] { return !doc->scene.sheet_view(top)->def.contains("crop") && settled() && width(top) > 100; }, 30000), "Remove crop: the whole top view again");
    w.action("edit.undo")->trigger();
    check(waitFor([&] { return doc->scene.sheet_view(top)->def.contains("crop") && settled() && width(top) < 60; }, 30000), "Ctrl+Z: cropped again");
    canvas->fitSheet();
    waitFor(settled, 10000);
    QCoreApplication::processEvents();
    page->grab().save(prefix + ".views.png");
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: sheet-views: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
