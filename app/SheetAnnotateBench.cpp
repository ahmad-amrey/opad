#include "MainWindow.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>

#include <cmath>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "SheetAnnotate.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "opad/design/sketch.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/sheet.hpp"

// OPAD_BENCH_SHEET_ANNOTATE=<prefix> (UI-79, UI-80, UI-81): a drawing of an 80 x 50 x 12 plate with two 6 mm holes and a
// counterbored one (hole features) and a pin beside it, annotated through the sheet's tools with mouse and key events on
// the canvas: the smart dimension (an edge read horizontally by where the pointer is, a hole's diameter with a tolerance
// from the options bar, a corner to a hole's centre), hole callouts from the hole features (one in a side view, its leader
// on the opening's end), a centre mark, a centre line,
// a note with a leader, datums A and B, a feature control frame, surface texture, a chain set, dimensions from the datums,
// a hole table; Esc steps back; a dimension selected, edited in the bar, dragged and deleted (Ctrl+Z); a dimension of the
// pin dangles once the pin is gone and is re-attached from the sheet bar's menu. <prefix>.annotate.png, .bar.png.
OPAD_BENCH(OPAD_BENCH_SHEET_ANNOTATE, sheetAnnotate) {
  using opad::drawing::Vec2;
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage();
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: annotate: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: annotate: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  SheetPage* page = docs->sheetPage();
  SheetCanvas* canvas = page->canvas();
  SheetAnnotator* tools = page->annotator();
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !tools->busy() && !w.m_doc->designBusy && !states.empty() && canvas->paperPictured() &&
           std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return (v.final && (v.picture || v.prims == 0)) || !v.error.isEmpty(); });
  };
  const auto mouse = [&](QEvent::Type type, const QPointF& scene, Qt::MouseButton button = Qt::LeftButton) {
    QWidget* vp = canvas->viewport();
    const QPoint p = canvas->mapFromScene(scene);
    QMouseEvent e(type, QPointF(p), QPointF(vp->mapToGlobal(p)), type == QEvent::MouseMove ? Qt::NoButton : button,
                  type == QEvent::MouseButtonRelease || type == QEvent::MouseMove ? Qt::NoButton : button, Qt::NoModifier);
    QApplication::sendEvent(vp, &e);
  };
  const auto moveTo = [&](Vec2 paper) { mouse(QEvent::MouseMove, canvas->toScene(paper)); };
  const auto plus = [](Vec2 a, Vec2 b) { return Vec2{a[0] + b[0], a[1] + b[1]}; };
  const auto click = [&](Vec2 paper) {
    const QPointF s = canvas->toScene(paper);
    mouse(QEvent::MouseMove, s);
    mouse(QEvent::MouseButtonPress, s);
    mouse(QEvent::MouseButtonRelease, s);
  };
  const auto key = [&](int k, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, k, m);
    QApplication::sendEvent(canvas, &press);
  };
  const auto items = [&](const std::string& kind) {
    std::vector<const opad::SheetItem*> out;
    if (const opad::Sheet* s = w.m_doc->scene.sheet(page->sheet()))
      for (const auto& id : s->items)
        if (const opad::SheetItem* t = w.m_doc->scene.sheet_item(id); t && t->kind == kind) out.push_back(t);
    return out;
  };
  // Waits for the tool's worker, then for the n-th item of a kind.
  const auto added = [&](const std::string& kind, size_t n) {
    return waitFor([&] { return items(kind).size() >= n && !w.m_doc->designBusy; }, 15000) && waitFor(settled, 30000);
  };
  const auto planned = [&] { return waitFor([&] { return !tools->busy(); }, 15000); };
  try {
    w.m_doc->newDocument();
    const opad::json made = w.m_doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "80 mm"}, {"width", "50 mm"}, {"height", "-12 mm"}}}});
    const std::string body = made["body_ids"][0];
    opad::design::Sketch pts;
    for (const auto& [x, y] : std::initializer_list<std::pair<double, double>>{{-25, 10}, {25, 10}, {0, -8}}) {
      opad::design::SkEntity e;
      e.type = opad::design::SkEntity::Type::Point;
      e.p = {pts.add_point(x, y)};
      e.id = pts.next_id();
      pts.entities.push_back(e);
    }
    const opad::json sk = w.m_doc->run("sketch", {{"plane", {{"base", "xy"}}}, {"geometry", pts.to_json()}});
    const std::string sketch = sk.contains("sketch_id") ? sk["sketch_id"].get<std::string>() : sk["ids"][0].get<std::string>();
    opad::json at = opad::json::array();
    for (const auto& p : pts.points) at.push_back({{"sketch", sketch}, {"point", p.id}});
    const opad::json simple = w.m_doc->run("feature", {{"kind", "hole"}, {"inputs", {{"points", {at[0], at[1]}}, {"diameter", "6 mm"}, {"extent", "all"}}}});
    w.m_doc->run("feature", {{"kind", "hole"}, {"inputs", {{"points", {at[2]}}, {"type", "counterbore"}, {"diameter", "5 mm"}, {"depth", "8 mm"}, {"cb_diameter", "9 mm"}, {"cb_depth", "3 mm"}}}});
    const opad::json pin = w.m_doc->run("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 60}, {"y", 0}, {"diameter", 8}, {"height", 10}}}});
    const std::string holeFeature = simple.value("feature_id", "");
    w.action("workspace.drawings")->trigger();
    std::string sheet;
    docs->createDrawing({{"size", "A3"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "auto"}, {"views", {"front", "top"}}},
                        [&](const std::string& id) { sheet = id; });
    check(waitFor([&] { return !sheet.empty(); }, 20000) && waitFor(settled, 30000), "a drawing of the plate: front and top views drawn");
    const opad::Sheet* sh = w.m_doc->scene.sheet(sheet);
    if (!sh || sh->views.size() != 2) throw opad::Error("no sheet");
    const std::string front = sh->views[0], top = sh->views[1];
    const auto paper = [&](const std::string& view, opad::Vec3 p) {
      const opad::drawing::ViewFrame* f = canvas->frame(view);
      return f ? f->paper(p) : Vec2{0, 0};
    };
    check(w.m_ribbon->tabIds().contains("drawings.annotate"), "the Drawings workspace has its Annotate tab");

    // The smart dimension: the front view's top edge, read horizontally with the pointer above it.
    w.action("drawings.dimension")->trigger();
    check(tools->tool() == SheetAnnotator::Tool::Dimension && tools->bar()->isVisible() && !tools->prompt().isEmpty() && tools->typeBox()->isVisible(),
          "Dimension starts the tool: its prompt and the options bar (type, precision, tolerance)");
    click(paper(front, {10, -25, 0}));
    check(planned() && tools->pickCount() == 1 && tools->plan().value("choices", opad::json::array()).size() == 3,
          "a click on the top edge picks it; measured on a worker: horizontal, vertical and aligned readings");
    moveTo(plus(paper(front, {0, -25, 0}), {0, 12}));
    check(tools->chosenType() == "horizontal" && canvas->preview() && canvas->preview()->prims.size() > 4, "the pointer above the edge reads it horizontally; the preview follows");
    const size_t ops0 = w.m_doc->doc.ops.size();
    click(plus(paper(front, {0, -25, 0}), {0, 12}));
    check(added("dimension", 1) && w.m_doc->doc.ops.size() == ops0 + 1, "a click on the paper places it: one step");
    const opad::SheetItem* width = items("dimension")[0];
    const std::string widthId = width->id;
    check(width->type == "horizontal" && std::fabs(width->def["result"]["value"].get<double>() - 80) < 1e-9, "80 wide, horizontal: " + QString::fromStdString(width->def["result"].dump()));
    check(tools->tool() == SheetAnnotator::Tool::Dimension && tools->pickCount() == 0, "the tool stays for the next dimension");
    // A hole's diameter with a tolerance typed in the bar.
    tools->toleranceBox()->setCurrentIndex(tools->toleranceBox()->findData("sym"));
    tools->plusEdit()->setText("0.05");
    emit tools->plusEdit()->editingFinished();
    const Vec2 hole1 = paper(top, {-25, 10, 0}), hole2 = paper(top, {25, 10, 0});
    const double r6 = 3 * canvas->frame(top)->scale;
    click(plus(hole1, {r6 * 0.7071, r6 * 0.7071}));
    check(planned() && tools->plan().value("choices", opad::json::array()).size() == 1 && tools->plan()["choices"][0]["type"] == "diameter", "a circle reads as its diameter");
    click(plus(hole1, {-14, 14}));
    check(added("dimension", 2) && items("dimension")[1]->def["result"]["shown"] == "⌀6 ±0.05", "placed with the bar's tolerance: " +
          QString::fromStdString(items("dimension").size() > 1 ? items("dimension")[1]->def["result"].dump() : ""));
    tools->toleranceBox()->setCurrentIndex(0);
    // A corner to a hole's centre: two picks, the pointer below reads it horizontally.
    const Vec2 corner = paper(top, {-40, -25, 0});
    click(corner);
    planned();
    click(hole1);
    check(planned() && tools->pickCount() == 2 && tools->plan()["picks"][0]["what"] == "vertex" && tools->plan()["picks"][1]["what"] == "center",
          "the corner snaps to its vertex, the hole to its centre");
    click(plus(corner, {6, -10}));
    check(added("dimension", 3) && std::fabs(items("dimension")[2]->def["result"]["value"].get<double>() - 15) < 1e-9 && items("dimension")[2]->type == "horizontal",
          "15 from the corner to the hole's centre");
    // Esc takes back a pick, then leaves.
    click(paper(front, {10, -25, 0}));
    planned();
    key(Qt::Key_Escape);
    check(tools->pickCount() == 0 && tools->tool() == SheetAnnotator::Tool::Dimension, "Esc takes back the pick");
    key(Qt::Key_Escape);
    check(tools->tool() == SheetAnnotator::Tool::None && !tools->bar()->isVisible() && !canvas->preview(), "Esc again leaves the tool");

    // Hole callouts from the hole features.
    w.action("drawings.holeCallout")->trigger();
    click(plus(hole2, {r6 * 0.7071, -r6 * 0.7071}));
    planned();
    click(plus(hole2, {12, 10}));
    check(added("hole_callout", 1) && items("hole_callout")[0]->def["result"]["shown"] == "2× ⌀6 THRU", "the 6 mm holes' callout: " +
          QString::fromStdString(items("hole_callout").empty() ? "" : items("hole_callout")[0]->def["result"].dump()));
    const Vec2 bore = paper(top, {0, -8, 0});
    click(plus(bore, {4.5 * canvas->frame(top)->scale, 0}));
    planned();
    click(plus(bore, {16, -12}));
    check(added("hole_callout", 2) && items("hole_callout")[1]->def["result"]["shown"] == "⌀5 ↧8\n⌴ ⌀9 ↧3", "the counterbored hole's callout from its feature");
    key(Qt::Key_Escape);
    // Seen from the side (the front view with hidden lines): the leader ends where the hole's wall meets the surface.
    w.m_doc->run("sheet_edit", {{"target", front}, {"set", {{"style", {{"hidden", true}}}}}});
    waitFor(settled, 30000);
    w.action("drawings.holeCallout")->trigger();
    click(paper(front, {28, 10, -6}));
    check(planned() && tools->plan().contains("measured") && tools->plan()["measured"].contains("ends"), "a hole's hidden wall in the front view picks the hole");
    {
      const double z = tools->plan().value("measured", opad::json::object()).value("hole", opad::json::object()).value("entry", opad::json::array({0, 0, 0}))[2].get<double>();
      const Vec2 opening = paper(front, {28, 10, z});
      moveTo(plus(opening, {10, z < -6 ? -12 : 12}));
      bool tip = false;
      if (canvas->preview())
        for (const auto& p : canvas->preview()->prims)
          if (p.kind == opad::drawing::Prim::Kind::Curve && p.curve.type == opad::drawing::Curve::Type::Line && !p.curve.pts.empty())
            for (const Vec2 q : {p.curve.pts.front(), p.curve.pts.back()}) tip = tip || std::hypot(q[0] - opening[0], q[1] - opening[1]) < 0.05;
      check(tip, "the preview's leader ends at the opening's end nearer the pointer, not at its centre");
      click(plus(opening, {10, z < -6 ? -12 : 12}));
      check(added("hole_callout", 3) && items("hole_callout")[2]->def["result"]["shown"] == "2× ⌀6 THRU", "placed: the same callout as from above");
    }
    key(Qt::Key_Escape);
    w.m_doc->run("sheet_edit", {{"target", front}, {"set", {{"style", {{"hidden", false}}}}}});
    waitFor(settled, 30000);
    // A centre mark, a centre line through both holes, a note with a leader.
    w.action("drawings.centerMark")->trigger();
    click(plus(hole1, {r6 * 0.7071, -r6 * 0.7071}));
    check(added("centermark", 1), "a click on a circle adds its centre mark");
    key(Qt::Key_Escape);
    w.action("drawings.centerLine")->trigger();
    click(plus(hole1, {-r6 * 0.7071, -r6 * 0.7071}));
    planned();
    click(plus(hole2, {-r6 * 0.7071, -r6 * 0.7071}));
    check(added("centerline", 1) && items("centerline")[0]->def["refs"].size() == 2, "two circles make a centre line through their centres");
    key(Qt::Key_Escape);
    w.action("drawings.note")->trigger();
    tools->textEdit()->setText("DEBURR");
    click(plus(hole2, {-r6 * 0.7071, r6 * 0.7071}));
    planned();
    click(plus(hole2, {-20, 18}));
    check(added("note", 1) && items("note")[0]->def.contains("refs") && items("note")[0]->def["text"] == "DEBURR", "a note with a leader to the hole");
    key(Qt::Key_Escape);

    // Datums A and B, a feature control frame referring to them, surface texture.
    w.action("drawings.datum")->trigger();
    check(tools->letterEdit()->text() == "A", "the datum tool offers the first free letter");
    const Vec2 leftEdge = paper(top, {-40, 4, 0}), bottomEdge = paper(top, {10, -25, 0});
    click(leftEdge);
    planned();
    click(plus(leftEdge, {-14, 0}));
    check(added("datum", 1) && items("datum")[0]->def["letter"] == "A", "datum A on the left edge");
    check(tools->letterEdit()->text() == "B", "then B");
    click(bottomEdge);
    planned();
    click(plus(bottomEdge, {0, -14}));
    check(added("datum", 2) && items("datum")[1]->def["letter"] == "B", "datum B on the bottom edge");
    key(Qt::Key_Escape);
    w.action("drawings.fcf")->trigger();
    tools->characteristicBox()->setCurrentIndex(tools->characteristicBox()->findData("position"));
    tools->valueEdit()->setText("0.1");
    for (QWidget* field : tools->bar()->findChildren<QWidget*>()) {
      if (field->objectName() == "annotate.zone") static_cast<QAbstractButton*>(field)->setChecked(true);
      if (field->objectName() == "annotate.datum1") static_cast<QLineEdit*>(field)->setText("A");
      if (field->objectName() == "annotate.datum2") static_cast<QLineEdit*>(field)->setText("B");
    }
    click(plus(hole2, {r6 * 0.7071, r6 * 0.7071}));
    planned();
    click(plus(hole2, {14, 24}));
    check(added("fcf", 1) && items("fcf")[0]->def["characteristic"] == "position" && items("fcf")[0]->def["datums"] == opad::json({"A", "B"}) &&
              items("fcf")[0]->def["zone"] == "diameter",
          "a position frame ⌀0.1 to A and B on the hole");
    key(Qt::Key_Escape);
    w.action("drawings.surface")->trigger();
    tools->valueEdit()->setText("Ra 1.6");
    click(paper(front, {-20, -25, 0}));
    planned();
    click(plus(paper(front, {-20, -25, 0}), {0, 6}));
    check(added("surface", 1) && items("surface")[0]->def["value"] == "Ra 1.6", "surface texture on the top face");
    key(Qt::Key_Escape);

    // A chain set from the left edge through both holes (Enter ends the features), then sets from the datums.
    w.action("drawings.chain")->trigger();
    click(plus(leftEdge, {0, 4}));
    planned();
    click(hole1);
    planned();
    click(hole2);
    planned();
    tools->axisBox()->setCurrentIndex(tools->axisBox()->findData("horizontal"));
    key(Qt::Key_Return);
    check(planned() && tools->plan().contains("op"), "Enter ends the features; the chain is measured");
    click(plus(paper(top, {0, -25, 0}), {0, -30}));
    check(added("dimension_set", 1) && items("dimension_set")[0]->def["result"]["values"] == opad::json({15, 50}), "a chain from the left edge: 15 and 50");
    key(Qt::Key_Escape);
    w.action("drawings.fromDatums")->trigger();
    check(added("dimension_set", 3), "Dimension from datums adds a set from A and one from B");
    {
      int fromA = 0, fromB = 0;
      for (const auto* t : items("dimension_set")) fromA += t->def.value("datum", "") == "A", fromB += t->def.value("datum", "") == "B";
      check(fromA == 1 && fromB == 1, "one set from each datum");
    }
    // A hole table of the top view.
    w.action("drawings.holeTable")->trigger();
    click(paper(top, {0, 0, -6}));
    planned();
    const opad::drawing::ViewFrame* tf = canvas->frame(top);
    click({tf->box[2] + 40, tf->box[3]});
    check(added("hole_table", 1) && items("hole_table")[0]->def["result"]["rows"].size() == 3, "a hole table of the plate's three holes: " +
          QString::fromStdString(items("hole_table").empty() ? "" : items("hole_table")[0]->def["result"].dump()));
    key(Qt::Key_Escape);
    page->grab().save(prefix + ".annotate.png");
    {
      opad::json report;
      opad::drawing::sheet_display(w.m_doc->doc, w.m_doc->scene, *w.m_doc->scene.sheet(sheet), {}, &report);
      check(report["skipped"].empty() && canvas->dangling().empty() && !page->danglingButton()->isVisible(), "every annotation measured: none dangling");
    }

    // Selected with a click, edited in the bar, dragged, deleted, back with Ctrl+Z.
    const auto box = canvas->itemBox(widthId);
    check(box.has_value(), "the width's box is known");
    if (box) {
      // On its dimension line (it runs through its place), a little off the middle where the text stands.
      const opad::json placed = w.m_doc->scene.sheet_item(widthId)->def["place"]["text"];
      const opad::drawing::ViewFrame* ff = canvas->frame(front);
      click({ff->at[0] + placed[0].get<double>() + 25, ff->at[1] + placed[1].get<double>()});
      check(canvas->selectedItems() == std::vector<std::string>{widthId} && tools->bar()->isVisible() && tools->precisionBox()->isVisible() &&
                !tools->typeBox()->isVisible(),
            "a click on the dimension selects it; the bar shows its own options");
      const size_t before = w.m_doc->doc.ops.size();
      tools->precisionBox()->setCurrentIndex(1);
      check(waitFor([&] { return w.m_doc->doc.ops.size() == before + 1; }, 5000) && w.m_doc->scene.sheet_item(widthId)->def.value("precision", 2) == 1,
            "a precision from the bar is one edit");
      const opad::json place = w.m_doc->scene.sheet_item(widthId)->def["place"]["text"];
      canvas->benchDragItem(widthId, {5, 6});
      check(waitFor([&] { return w.m_doc->doc.ops.size() == before + 2; }, 5000) &&
                std::fabs(w.m_doc->scene.sheet_item(widthId)->def["place"]["text"][1].get<double>() - place[1].get<double>() - 6) < 1e-6,
            "dragging it moves its text: one edit");
      waitFor(settled, 15000);
      canvas->setFocus();
      key(Qt::Key_Delete);
      check(waitFor([&] { return !w.m_doc->scene.sheet_item(widthId); }, 5000), "Del deletes the selected dimension");
      waitFor(settled, 15000);
      w.action("edit.undo")->trigger();
      check(waitFor([&] { return w.m_doc->scene.sheet_item(widthId) != nullptr; }, 5000), "Ctrl+Z brings it back");
    }

    // The pin's diameter dangles once the pin is gone; re-attached to a hole from the sheet bar's menu.
    waitFor(settled, 15000);
    w.action("drawings.dimension")->trigger();
    const Vec2 pinAt = paper(top, {60, 0, 10});
    const double r8 = 4 * canvas->frame(top)->scale;
    click(plus(pinAt, {r8 * 0.7071, r8 * 0.7071}));
    planned();
    click(plus(pinAt, {10, 16}));
    check(added("dimension", 4), "the pin's diameter");
    key(Qt::Key_Escape);
    const std::string pinDim = items("dimension").back()->id;
    w.m_doc->run("delete", {{"target", pin.value("feature_id", "")}});
    check(waitFor([&] { return canvas->dangling().count(pinDim) > 0; }, 30000) && page->danglingButton()->isVisible(),
          "the pin gone, its dimension dangles: the sheet bar says so (" + page->danglingButton()->text() + ")");
    QMenu* menu = page->danglingButton()->menu();
    QStringList why;
    for (const auto& [id, error] : canvas->dangling()) why << QString::fromStdString(id.substr(0, 8)) + ": " + error;
    // The sets from the datums measured the plate's holes only (the datums stand on the plate): they stay.
    QAction* pinAction = nullptr;
    if (menu)
      for (QAction* a : menu->actions())
        if (a->text().contains(QString::fromUtf8("⌀8"))) pinAction = a;
    check(menu && menu->actions().size() == 1 && pinAction, "its menu offers to re-attach it: " + why.join("; "));
    if (pinAction) pinAction->trigger();
    check(tools->tool() == SheetAnnotator::Tool::Reattach, "Re-attach asks for its circle again");
    waitFor(settled, 15000);
    tools->bar()->grab().save(prefix + ".bar.png");
    const Vec2 hole2now = paper(top, {25, 10, 0});  // the views moved when the pin went
    click(plus(hole2now, {r6 * 0.7071, -r6 * 0.7071}));
    check(waitFor([&] { return !canvas->dangling().count(pinDim) && w.m_doc->scene.sheet_item(pinDim) &&
                               w.m_doc->scene.sheet_item(pinDim)->def["result"]["shown"] == "⌀6"; }, 30000) && waitFor(settled, 30000),
          "re-attached to the hole: measured again (⌀6), no longer dangling");
    check(!page->danglingButton()->isVisible() && canvas->dangling().empty() && tools->tool() == SheetAnnotator::Tool::None, "the sheet bar's warning goes");
    // Centre marks drawn by the views themselves.
    canvas->selectViews({});
    w.action("drawings.centerMarks")->setChecked(true);
    emit w.action("drawings.centerMarks")->triggered(true);
    check(waitFor([&] { return w.m_doc->scene.sheet_view(top)->def.value("style", opad::json::object()).value("centermarks", false); }, 5000) &&
              waitFor(settled, 30000),
          "Centre marks on views turns them on for every view");
    page->grab().save(prefix + ".marks.png");
    (void)holeFeature;
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: annotate: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
