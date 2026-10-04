#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>

#include <cmath>
#include <set>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "SheetAnnotate.hpp"
#include "SheetCanvas.hpp"
#include "SheetDialogs.hpp"
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
// Dimensioned on its outline (two cut lines pick the plate's sides: 40). Hatching… (the dialog, a typed angle and
// spacing, automatic again). An aligned section on the top view (three points,
// the last segment at 30 degrees: both sides hatched). A broken-out section on the front view (outline previewed, Esc
// back from the depth, the depth clicked on the hole's centre in the top view; floor hatched, break lines; removed from
// the menu, Ctrl+Z). Crop by dragging a box on the top view, Break by two clicks on the front view (the top view broken
// with it), freehand break lines from the menu, Remove crop from the view's menu, Ctrl+Z. A detail's scale (4:1) and letter
// from its menu. Typed values (UI-122) on each tool's card: a detail's radius and scale, an auxiliary view's gap, a crop's
// width and height (Esc clears them first), a break's length, a broken-out section's depth; digits never the window's
// shortcuts. <prefix>.views.png, <prefix>.hatch.png.
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
    // Dimensioned on its outline: the cut's edges pick the faces they lie on (the plate's sides), read horizontally.
    {
      SheetAnnotator* notes = page->annotator();
      const auto planned = [&] { return waitFor([&] { return !notes->busy(); }, 15000); };
      const auto onSec = [&](opad::Vec3 p) {
        const opad::drawing::ViewFrame f = frameOf(sec);
        const Vec2 q = f.paper(p);
        return canvas->toScene(q);
      };
      w.action("drawings.dimension")->trigger();
      click(onSec({0, 20, 5}));
      planned();
      click(onSec({0, -20, 5}));
      const bool measured = planned();
      const opad::json picks = notes->plan().is_object() ? notes->plan().value("picks", opad::json::array()) : opad::json::array();
      check(measured && notes->pickCount() == 2 && picks.size() == 2 && picks[0].value("what", "") == "plane" && picks[1].value("what", "") == "plane",
            "two clicks on the section's outer cut lines pick the plate's sides: " + QString::fromStdString(picks.dump()));
      const QPointF below = onSec({0, 0, 0}) + QPointF(0, canvas->toScene({0, 0}).y() - canvas->toScene({0, 12}).y());
      click(below);
      const auto dims = [&] {
        std::vector<const opad::SheetItem*> out;
        for (const auto& id : doc->scene.sheet(sheet)->items)
          if (const opad::SheetItem* t = doc->scene.sheet_item(id); t && t->kind == "dimension" && t->view == sec) out.push_back(t);
        return out;
      };
      check(waitFor([&] { return dims().size() == 1 && settled(); }, 15000) && std::fabs(dims()[0]->def["result"]["value"].get<double>() - 40) < 1e-6,
            "placed below: 40, the plate's depth across the section");
      key(Qt::Key_Escape);
      key(Qt::Key_Escape);
      check(notes->tool() == SheetAnnotator::Tool::None, "Esc leaves the dimension tool");
    }
    // Dragged: only away from or towards its parent (its gap).
    const opad::SheetView* placed = doc->scene.sheet_view(sec);  // the scene was rebuilt by the dimension
    const double gap = placed ? placed->def.value("gap", 20.0) : 0;
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
    // Hatching… from the section's menu: the dialog shows automatic, a typed angle and spacing are one edit and drawn so;
    // automatic again removes them.
    {
      const auto openDialog = [&]() -> HatchDialog* {
        QMenu menu;
        docs->viewMenu({sec}, menu);
        QAction* a = menu.findChild<QAction*>("drawings.menu.hatching");
        if (!a) return nullptr;
        a->trigger();
        HatchDialog* dialog = nullptr;
        waitFor([&] {
          for (QWidget* t : QApplication::topLevelWidgets())
            if (auto* d = qobject_cast<HatchDialog*>(t); d && d->isVisible()) dialog = d;
          return dialog != nullptr;
        }, 3000);
        return dialog;
      };
      const auto angles = [&] {  // the section's hatch lines' angles (degrees, whole)
        std::set<long> out;
        const auto d = opad::drawing::sheet_display(doc->doc, doc->scene, *doc->scene.sheet(sheet));
        for (const auto& p : d.prims)
          if (p.source == sec && d.layers[static_cast<size_t>(p.layer)].name == "Hatch" && p.curve.pts.size() == 2)
            out.insert((std::lround(std::atan2(p.curve.pts[1][1] - p.curve.pts[0][1], p.curve.pts[1][0] - p.curve.pts[0][0]) * 180 / M_PI) + 360) % 180);
        return out;
      };
      check(angles() == std::set<long>{45}, "the plate's cut faces hatched at 45 degrees (ISO 128-50)");
      HatchDialog* dialog = openDialog();
      check(dialog && dialog->autoAngle()->isChecked() && dialog->autoSpacing()->isChecked() && dialog->fillThin()->isChecked() &&
                dialog->pattern()->currentData().toString() == "general",
            "Hatching… opens its dialog: general lining, angle and spacing automatic, narrow faces filled");
      if (dialog) {
        dialog->autoAngle()->setChecked(false);
        dialog->angle()->setValue(30);
        dialog->autoSpacing()->setChecked(false);
        dialog->spacing()->setValue(3);
        dialog->grab().save(prefix + ".hatch.png");
        const size_t before = doc->doc.ops.size();
        dialog->accept();
        check(waitFor([&] { const opad::SheetView* v = doc->scene.sheet_view(sec); return doc->doc.ops.size() == before + 1 && v && v->def.value("hatch", opad::json()) == opad::json{{"angle", 30.0}, {"spacing", 3.0}} && settled(); }, 15000) &&
                  angles() == std::set<long>{30},
              "30 degrees, 3 mm: one edit, drawn at 30 degrees");
      }
      dialog = openDialog();
      check(dialog && !dialog->autoAngle()->isChecked() && std::fabs(dialog->angle()->value() - 30) < 1e-9 && std::fabs(dialog->spacing()->value() - 3) < 1e-9,
            "opened again: it shows them");
      if (dialog) {
        dialog->autoAngle()->setChecked(true);
        dialog->autoSpacing()->setChecked(true);
        dialog->accept();
        check(waitFor([&] { const opad::SheetView* v = doc->scene.sheet_view(sec); return v && !v->def.contains("hatch") && settled(); }, 15000) && angles() == std::set<long>{45},
              "automatic again: the setting removed, 45 degrees");
      }
    }
    // Esc with nothing clicked leaves.
    select(front);
    w.action("drawings.sectionView")->trigger();
    key(Qt::Key_Escape);
    check(!tool->active() && tool->prompt().isEmpty(), "Esc with no point leaves the tool");

    // An aligned section on the top view: down to the hole's middle, then on at 30 degrees below level (not snapped): the
    // inclined side revolved onto the first one's line and hatched with it, one step.
    {
      select(top);
      w.action("drawings.sectionView")->trigger();
      click(at(top, {0, 26}));
      click(at(top, {0, 0}));
      click(at(top, {30 * std::cos(M_PI / 6), -30 * std::sin(M_PI / 6)}));
      key(Qt::Key_Return);
      check(waitFor([&] { return tool->placing(); }, 15000), "three points, Enter: measured and following the pointer");
      const opad::drawing::ViewFrame tf = frameOf(top);
      const size_t before = views("section").size();
      click(canvas->toScene({tf.box[2] + 40, tf.at[1]}));
      check(waitFor([&] { return views("section").size() == before + 1; }, 15000) && waitFor(settled, 30000), "a click places it");
      const std::string al = views("section").empty() ? std::string() : views("section").back();
      const opad::SheetView* v = doc->scene.sheet_view(al);
      double area = 0;
      if (v) {
        const auto g = opad::drawing::project(doc->doc, doc->scene, opad::drawing::view_spec(doc->scene, *v));
        for (const auto& r : g->sections)
          for (const auto& l : r.loops) {
            double a = 0;
            for (size_t k = 0; k < l.size(); ++k) a += l[k][0] * l[(k + 1) % l.size()][1] - l[(k + 1) % l.size()][0] * l[k][1];
            area += std::fabs(a / 2);
          }
      }
      // The plate is 10 thick: 15 mm of it from the hole to the edge (20) up the first side, and from the hole to the right
      // edge (30 / cos 30) along the inclined one.
      const double want = 10 * (15 + 30 / std::cos(M_PI / 6) - 5);
      check(v && v->def.value("aligned", false) && v->def["cut"].size() == 3 && std::fabs(area - want) < 1.5 && st(al).final,
            QString("aligned section B: its faces %1 mm² (both sides, the inclined one revolved; %2 expected)").arg(area).arg(want));
      w.action("edit.undo")->trigger();
      check(waitFor([&] { return views("section").size() == before && settled(); }, 15000), "Ctrl+Z takes it away");
    }

    // A broken-out section on the front view: four points round the hole (the closed curve previewed), Enter, Esc back to
    // the points, Enter, then the depth clicked on the hole's centre in the top view: one edit, the floor hatched, thin
    // break lines where the cut ends over the plate. Removed from the menu, Ctrl+Z.
    {
      select(front);
      check(w.action("drawings.breakoutView")->isEnabled(), "Broken-out section is offered for the front view");
      w.action("drawings.breakoutView")->trigger();
      for (const Vec2 q : {Vec2{-9, -1}, Vec2{9, -1}, Vec2{9, 11}}) click(at(front, q));
      mouse(QEvent::MouseMove, at(front, {-9, 11}));
      check(tool->tool() == SheetViewTool::Tool::Breakout && canvas->preview() && canvas->preview()->prims.size() >= 4, "three points: the closed curve through them and the pointer previewed");
      click(at(front, {-9, 11}));
      key(Qt::Key_Return);
      check(tool->askingDepth() && tool->prompt().contains("depth"), "Enter closes the outline and asks for the depth");
      key(Qt::Key_Escape);
      check(!tool->askingDepth() && tool->points() == 4 && tool->active(), "Esc goes back to the outline's points");
      key(Qt::Key_Return);
      const size_t before = doc->doc.ops.size();
      click(at(top, {0, 0}));  // the hole's centre: the cut goes through the hole's axis
      const auto opened = [&] { const opad::SheetView* v = doc->scene.sheet_view(front); return v ? v->def.value("breakouts", opad::json::array()) : opad::json::array(); };
      check(waitFor([&] { return opened().size() == 1 && settled(); }, 30000) && doc->doc.ops.size() == before + 1 && std::fabs(opened()[0].value("depth", 99.0)) < 1e-6 &&
                opened()[0]["outline"].size() == 4 && !tool->active(),
            "the depth clicked in the top view: one edit through the hole's middle; the tool ends");
      const auto drawnOn = [&](const std::string& layer) {
        const auto d = opad::drawing::sheet_display(doc->doc, doc->scene, *doc->scene.sheet(sheet));
        return onLayer(d, layer, front);
      };
      check(drawnOn("Hatch") > 4 && drawnOn("Break") >= 2 && st(front).final, "its floor hatched, thin break lines where it ends over the plate");
      QMenu menu;
      docs->viewMenu({front}, menu);
      QAction* remove = menu.findChild<QAction*>("drawings.menu.unbreakout");
      check(remove && menu.findChild<QAction*>("drawings.menu.hatching"), "the front view's menu offers Remove broken-out sections and Hatching…");
      if (remove) remove->trigger();
      check(waitFor([&] { return opened().empty() && settled(); }, 15000) && drawnOn("Hatch") == 0, "removed: the plain front view again");
      w.action("edit.undo")->trigger();
      check(waitFor([&] { return opened().size() == 1 && settled(); }, 15000) && drawnOn("Break") >= 2, "Ctrl+Z: opened again");
    }

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
    {  // its own scale and its letter, from its menu
      QMenu menu;
      docs->viewMenu({det}, menu);
      QAction* four = menu.findChild<QAction*>("drawings.menu.scale.4:1");
      QAction* five = menu.findChild<QAction*>("drawings.menu.scale.5:1");
      QAction* twice = menu.findChild<QAction*>("drawings.menu.scale.sheet");
      QAction* letter = menu.findChild<QAction*>("drawings.menu.letter");
      check(four && five && five->isChecked() && twice && !twice->isChecked() && letter, "the detail's menu offers its scale (5:1 checked, twice its parent's, 4:1, ...) and its letter");
      if (four) four->trigger();
      check(waitFor([&] { return frameOf(det).scale == 4 && settled(); }, 15000), "4:1 from its menu: drawn at 4:1");
      if (letter) letter->trigger();
      auto* dialog = w.findChild<QInputDialog*>("viewLetterDialog");
      check(dialog && dialog->textValue() == "B", "Letter… asks for its letter (B)");
      if (dialog) {
        dialog->setTextValue("k");
        dialog->accept();
      }
      const auto labelled = [&] {
        const auto d = opad::drawing::sheet_display(doc->doc, doc->scene, *doc->scene.sheet(sheet));
        return std::any_of(d.prims.begin(), d.prims.end(), [&](const opad::drawing::Prim& p) { return p.kind == opad::drawing::Prim::Kind::Text && p.text == "K (4:1)"; });
      };
      check(waitFor([&] { return doc->scene.sheet_view(det)->def.value("letter", "") == "K" && settled(); }, 15000) && labelled(), "lettered K: labelled K (4:1)");
    }

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
    // Its break lines freehand, from the view's menu.
    {
      const auto longest = [&] {
        size_t most = 0;
        const auto d = opad::drawing::sheet_display(doc->doc, doc->scene, *doc->scene.sheet(sheet));
        for (const auto& p : d.prims)
          if (p.source == front && d.layers[static_cast<size_t>(p.layer)].name == "Break" && p.curve.type == opad::drawing::Curve::Type::Polyline) most = std::max(most, p.curve.pts.size());
        return most;
      };
      const size_t zigzag = longest();
      QMenu menu;
      docs->viewMenu({front}, menu);
      QAction* freehand = menu.findChild<QAction*>("drawings.menu.break.freehand");
      QAction* ruled = menu.findChild<QAction*>("drawings.menu.break.zigzag");
      check(freehand && ruled && ruled->isChecked() && zigzag == 6, "the broken view's menu offers its break lines, ruled with a zigzag now");
      if (freehand) freehand->trigger();
      check(waitFor([&] { const opad::SheetView* v = doc->scene.sheet_view(front); return v && v->def.value("style", opad::json::object()).value("break", "") == "freehand" && settled(); }, 15000) &&
                longest() > 20,
            QString("Freehand: one edit, the break lines drawn as waves (%1 points)").arg(longest()));
    }
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

    // Typed values (UI-122): each tool's value card takes the stage's numbers, Tab to the next, Enter; digits never the window's.
    const auto shortcutTaken = [&](int k) {
      QKeyEvent so(QEvent::ShortcutOverride, k, Qt::NoModifier, QString(QChar(k)));
      so.ignore();
      QApplication::sendEvent(canvas, &so);
      return so.isAccepted();
    };
    const auto type = [&](const QString& text) {
      for (const QChar c : text) {
        const int k = c == ':' ? Qt::Key_Colon : c == '.' ? Qt::Key_Period : Qt::Key_0 + (c.unicode() - '0');
        QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier, QString(c));
        QApplication::sendEvent(canvas, &e);
      }
    };
    const auto shows = [&](const std::string& key, const QString& text) { return tool->cardShown() && tool->inputText(key) == text; };
    canvas->setFocus();
    // A detail: its centre clicked, radius 6 and scale 4 typed, Enter, placed where clicked.
    select(top);
    w.action("drawings.detailView")->trigger();
    check(shortcutTaken(Qt::Key_5) && shortcutTaken(Qt::Key_Period), "with a view tool, digits are the canvas's, never the window's shortcuts");
    click(at(top, {-20, 0}));
    mouse(QEvent::MouseMove, at(top, {-17, 0}));
    check(tool->inputs() == std::vector<std::string>{"radius", "scale"} && shows("scale", "5:1"), "the detail asks for its radius and scale (5:1 from the pointer)");
    type("6");
    key(Qt::Key_Tab);
    type("4");
    check(shows("radius", "6") && shows("scale", "4"), "6 typed for the radius, Tab, 4 for the scale");
    key(Qt::Key_Return);
    check(tool->placing() && tool->inputs() == std::vector<std::string>{"scale"} && shows("scale", "4"), "Enter takes the radius: placing at the typed scale");
    const size_t details = views("detail").size();
    click(canvas->toScene({paperW - 60, 60}));
    check(waitFor([&] { return views("detail").size() == details + 1 && settled(); }, 15000), "a click places it");
    if (views("detail").size() == details + 1) {
      const opad::SheetView* d = doc->scene.sheet_view(views("detail").back());
      check(std::fabs(d->def.value("radius", 0.0) - 6) < 1e-9 && d->def.value("scale", "") == "4:1", "the detail has the typed radius (6) and scale (4:1)");
    }
    // An auxiliary view 15 mm from its parent.
    select(top);
    w.action("drawings.auxiliaryView")->trigger();
    click(at(top, {-10, 20}));
    check(waitFor([&] { return tool->placing(); }, 15000), "an edge picked: the auxiliary view follows the pointer");
    mouse(QEvent::MouseMove, canvas->toScene({frameOf(top).at[0] + 10, frameOf(top).box[3] + 40}));
    check(tool->inputs() == std::vector<std::string>{"gap"}, "it asks for its gap");
    const size_t auxs = views("auxiliary").size();
    type("15");
    key(Qt::Key_Return);
    check(waitFor([&] { return views("auxiliary").size() == auxs + 1 && settled(); }, 15000) &&
              std::fabs(doc->scene.sheet_view(views("auxiliary").back())->def.value("gap", 0.0) - 15) < 1e-9,
          "15 typed and Enter: placed 15 mm from its parent");
    // A crop of 20 x 12 from its first corner, and a break of 8 along the front view.
    {  // the top view's crop removed: cropped again by typed sizes
      QMenu m;
      docs->viewMenu({top}, m);
      if (QAction* a = m.findChild<QAction*>("drawings.menu.uncrop")) a->trigger();
      waitFor([&] { return !doc->scene.sheet_view(top)->def.contains("crop") && settled(); }, 15000);
    }
    select(top);
    w.action("drawings.cropView")->trigger();
    click(at(top, {-25, -15}));
    mouse(QEvent::MouseMove, at(top, {-5, 5}));
    check(tool->inputs() == std::vector<std::string>{"width", "height"} && tool->cardShown() && !tool->inputText("width").isEmpty(),
          "the crop asks for its width and height (" + tool->inputText("width") + " from the pointer)");
    type("20");
    key(Qt::Key_Tab);
    type("12");
    key(Qt::Key_Escape);
    check(tool->active() && tool->inputText("width") != "20" && tool->inputText("height") != "12", "Esc clears what was typed first, the tool stays");
    type("12");
    key(Qt::Key_Tab);
    type("15");
    key(Qt::Key_Return);
    const auto crop = [&] { return doc->scene.sheet_view(top)->def.value("crop", opad::json()); };
    check(waitFor([&] { return crop().is_array() && settled(); }, 15000) && std::fabs(crop()[2].get<double>() - crop()[0].get<double>() - 12) < 1e-6 &&
              std::fabs(crop()[3].get<double>() - crop()[1].get<double>() - 15) < 1e-6,
          "12 Tab 15 Enter: a 12 x 15 crop box from its first corner (" + QString::fromStdString(crop().dump()) + ")");
    select(front);
    w.action("drawings.breakView")->trigger();
    click(at(front, {15, 5}));
    mouse(QEvent::MouseMove, at(front, {20, 5}));
    check(tool->inputs() == std::vector<std::string>{"length"}, "the break asks for its length");
    type("8");
    key(Qt::Key_Return);
    const auto lastBreak = [&] {
      const opad::json b = doc->scene.sheet_view(front)->def.value("breaks", opad::json::array());
      return b.empty() ? 0.0 : b.back()["to"].get<double>() - b.back()["from"].get<double>();
    };
    check(waitFor([&] { return doc->scene.sheet_view(front)->def.value("breaks", opad::json::array()).size() == 2 && settled(); }, 15000) && std::fabs(lastBreak() - 8) < 1e-6,
          QString("8 and Enter: an 8 mm band taken out (%1)").arg(lastBreak()));
    // A broken-out section 4 mm deep into the top view's plate.
    select(top);
    w.action("drawings.breakoutView")->trigger();
    for (const Vec2 p : {Vec2{-23, -13}, Vec2{-15, -13}, Vec2{-15, -3}, Vec2{-23, -3}}) click(at(top, p));
    key(Qt::Key_Return);
    const bool asks = tool->askingDepth() && waitFor([&] { return tool->inputs() == std::vector<std::string>{"depth"} && !tool->inputText("depth").isEmpty(); }, 15000);
    check(asks, "the outline closed: it asks for the depth below the part's front (" + tool->inputText("depth") + ")");
    type("4");
    key(Qt::Key_Return);
    const auto breakout = [&] { return doc->scene.sheet_view(top)->def.value("breakouts", opad::json::array()); };
    check(waitFor([&] { return breakout().size() == 1 && settled(); }, 15000) && std::fabs(breakout()[0].value("depth", 0.0) - 6) < 1e-6,
          "4 and Enter: cut 4 mm below the plate's front (depth 6 of 10): " + QString::fromStdString(breakout().dump()).left(80));
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: sheet-views: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
