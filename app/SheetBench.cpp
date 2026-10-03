#include "MainWindow.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTabBar>
#include <QToolButton>

#include <cmath>
#include <map>
#include <filesystem>
#include <set>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "IssueRevision.hpp"
#include "SheetCanvas.hpp"
#include "SheetDialogs.hpp"
#include "SheetPage.hpp"
#include "TemplateFields.hpp"
#include "Toast.hpp"
#include "opad/cache.hpp"
#include "opad/drawing/sheet.hpp"

// OPAD_BENCH_SHEET=<prefix> (UI-78): the Drawings workspace on a 60 x 40 x 10 plate with a 10 mm hole, made through the UI
// code: Ctrl+3 shows the sheet page in the viewport's place (the "no drawing yet" card), New drawing… (the dialog's
// template thumbnails, ISO A3, front + top + side + iso) lays the views out at 2:1 in first angle, snaps on the views (an
// end, a middle, a centre, a point on an edge; the hover marker and readout; the Snap switch), the canvas shows each
// view's draft before its final linework once the projections are not cached, the base view dragged on the canvas
// takes its projected views along (alignment kept) and a projected view drags only along its axis (its gap), Ctrl+Z, a
// base and a projected view placed with the mouse path, hidden lines from the ribbon, the sheet's properties (A2: the
// template follows), Document properties (Approved by in the title block), a template from a DXF file (its placeholders
// filled in), Title block fields (a field added and one moved with the mouse), a new sheet in the drawing, the browser's
// row opening its sheet, PDF export, Del and Esc on the canvas, and back to Design. <prefix>.empty.png, .sheet.png,
// .snap.png, .final.png, .window.png, .fields.png, .template.png.
OPAD_BENCH(OPAD_BENCH_SHEET, sheet) {
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage();
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sheet: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: sheet: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  SheetPage* page = docs->sheetPage();
  SheetCanvas* canvas = page->canvas();
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !w.m_doc->designBusy && !states.empty() && canvas->paperPictured() &&
           std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return (v.final && (v.picture || v.prims == 0)) || !v.error.isEmpty(); });
  };
  const auto views = [&]() -> std::vector<std::string> {
    const opad::Sheet* s = w.m_doc->scene.sheet(page->sheet());
    return s ? s->views : std::vector<std::string>{};
  };
  const auto state = [&](const std::string& id) {
    for (const auto& v : canvas->viewStates())
      if (v.id == id) return v;
    return SheetCanvas::ViewState{};
  };
  try {
    w.m_doc->newDocument();
    const opad::json made = w.m_doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}});
    const std::string body = made["body_ids"][0];
    w.m_doc->run("feature", {{"kind", "cylinder"},
                             {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 0}, {"y", 0}, {"diameter", 10}, {"height", 10}, {"operation", "cut"}, {"targets", {body}}}}});
    w.m_doc->run("part_properties", {{"target", body}, {"set", {{"part_number", "OP-2001"}, {"material", "aluminium-6061"}}}});

    // Ctrl+3: the sheet page in the viewport's place, a card to start from.
    w.action("workspace.drawings")->trigger();
    QCoreApplication::processEvents();
    check(w.workspaceId() == "drawings" && w.m_stack->currentWidget() == page && page->empty(), "Ctrl+3 shows the Drawings workspace's page with its start card");
    check(w.m_ribbon->tabIds().contains("drawings.drawing") && w.action("workspace.drawings")->shortcut() == QKeySequence("Ctrl+3"), "its ribbon tab and Ctrl+3");
    page->grab().save(prefix + ".empty.png");
    {  // toasts show over the page in the viewport's place (the window's notices, the areas' results)
      QPointer<Toast> t = w.m_toasts->toast("Bench toast");
      QCoreApplication::processEvents();
      check(t && t->parentWidget() == page && t->isVisible() && std::abs(t->geometry().center().x() - page->rect().center().x()) <= 1 &&
                t->geometry().bottom() < page->rect().bottom(),
            "a toast shows over the Drawings page, bottom centre");
      if (t) t->dismiss();
    }

    // New drawing… through its dialog.
    w.action("drawings.new")->trigger();
    NewDrawingDialog* dialog = nullptr;
    waitFor([&] {
      for (QWidget* top : QApplication::topLevelWidgets())
        if (auto* d = qobject_cast<NewDrawingDialog*>(top); d && d->isVisible()) dialog = d;
      return dialog != nullptr;
    }, 3000);
    check(dialog, "New drawing… opens its dialog");
    if (!dialog) throw opad::Error("no dialog");
    QListWidget* list = dialog->templates();
    int thumbs = 0;
    for (int i = 0; i < list->count(); ++i) thumbs += !list->item(i)->icon().isNull();
    check(list->count() == 10 && thumbs == 10, QString("10 templates (ISO A4-A0, ANSI A-E) with thumbnails: %1").arg(thumbs));
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->text() == "ISO A3") list->setCurrentRow(i);
    for (auto* r : dialog->findChildren<QRadioButton*>())
      if (r->text() == QObject::tr("Landscape")) r->setChecked(true);
    const opad::json args = dialog->args();
    check(args["size"] == "A3" && args["projection"] == "first" && args["views"] == opad::json({"front", "top", "side", "iso"}) && args["scale"] == "auto" &&
              args.value("centermarks", false),
          "the dialog asks for ISO A3, first angle, front + top + side + iso at an automatic scale, with centre marks: " + QString::fromStdString(args.dump()));
    for (auto* b : dialog->findChildren<QPushButton*>())
      if (b->objectName() == "primary") b->click();
    check(waitFor([&] { return views().size() == 4; }, 20000), "Create drawing adds a sheet with 4 views");
    const opad::Sheet* sheet = w.m_doc->scene.sheet(page->sheet());
    std::vector<std::string> v = views();
    if (!sheet || v.size() != 4) throw opad::Error("no sheet");
    const std::string sheetId = sheet->id, front = v[0], top = v[1], side = v[2], iso = v[3];
    const auto orient = [&](const std::string& id) { return opad::drawing::view_orientation(w.m_doc->scene, *w.m_doc->scene.sheet_view(id)); };
    check(sheet->def["template"]["id"] == "iso" && opad::drawing::scale_text(sheet->scale) == "2:1" && orient(front) == "front" && orient(top) == "top" &&
              orient(side) == "left" && orient(iso) == "iso" && w.workspaceId() == "drawings" && !page->empty() && page->tabs()->count() == 1,
          "an ISO A3 sheet at 2:1: front, top (below), the view from the left (right of it), iso; shown in its tab");
    check(waitFor(settled, 30000), "every view drawn in its final linework");
    {
      double worst = 0;  // a frame's sides from its linework's (the isometric view's included)
      for (const auto& s : canvas->viewStates())
        worst = std::max({worst, std::fabs(s.frame.left() - s.linework.left()), std::fabs(s.frame.right() - s.linework.right()),
                          std::fabs(s.frame.top() - s.linework.top()), std::fabs(s.frame.bottom() - s.linework.bottom())});
      check(worst < 0.05, QString("every frame hugs its view's linework (at most %1 mm off)").arg(worst, 0, 'f', 3));
    }
    const QRectF ff = state(front).frame, ft = state(top).frame, fs = state(side).frame, fi = state(iso).frame;
    check(std::fabs(ff.center().x() - ft.center().x()) < 0.01 && ft.center().y() > ff.center().y() && std::fabs(ff.center().y() - fs.center().y()) < 0.01 &&
              fs.center().x() > ff.center().x() && fi.center().x() > ff.center().x() && fi.center().y() > ff.center().y() &&
              QRectF(0, 0, 420, 297).contains(ff | ft | fs | fi),
          "first angle on the canvas: top view under the front, side view right of it, iso bottom right, all on the paper");
    check(canvas->paperPrims() > 80 && state(front).prims >= 4, QString("the paper draws its frame, zones and title block (%1 primitives), the front view its edges (%2)")
                                                                    .arg(canvas->paperPrims()).arg(state(front).prims));
    page->grab().save(prefix + ".sheet.png");

    // Snaps on the projected geometry: a corner of the front view, the middle of its top edge, the hole's centre in the
    // top view; the pointer shows the one it takes and the readout gives its point; the Snap switch turns them off.
    {
      const auto paperOf = [&](const QPointF& scene) { return canvas->toPaper(scene); };
      const QRectF lf = state(front).linework, lt = state(top).linework;
      const auto snapped = [&](const QPointF& around, opad::drawing::SnapKind kind, const QPointF& want) {
        const auto s = canvas->snapAt(around);
        const opad::drawing::Vec2 p = paperOf(want);
        return s && s->kind == kind && std::hypot(s->at[0] - p[0], s->at[1] - p[1]) < 0.02;
      };
      check(snapped(lf.topLeft() + QPointF(0.4, 0.3), opad::drawing::SnapKind::End, lf.topLeft()) &&
                snapped(QPointF(lf.center().x() + 0.4, lf.top() + 0.3), opad::drawing::SnapKind::Mid, QPointF(lf.center().x(), lf.top())) &&
                snapped(lt.center() + QPointF(0.4, 0.4), opad::drawing::SnapKind::Centre, lt.center()) &&
                snapped(QPointF(lf.left() + 7.3, lf.top() + 0.2), opad::drawing::SnapKind::Nearest, QPointF(lf.left() + 7.3, lf.top())),
            "snaps: the front view's corner (end), its top edge's middle, the hole's centre in the top view, a point on an edge");
      QWidget* vp = canvas->viewport();
      const QPoint px = canvas->mapFromScene(lf.topLeft() + QPointF(0.4, 0.3));
      QMouseEvent move(QEvent::MouseMove, QPointF(px), QPointF(vp->mapToGlobal(px)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(vp, &move);
      QLabel* readout = page->cursorLabel();
      const opad::drawing::Vec2 corner = paperOf(lf.topLeft());
      check(canvas->hoverSnap() && canvas->hoverSnap()->kind == opad::drawing::SnapKind::End && readout &&
                readout->text().contains(SheetCanvas::snapName(opad::drawing::SnapKind::End)) &&
                readout->text().contains(QString::number(corner[0], 'f', 2)),
            "hovering near the corner shows the end snap; the readout gives the corner: " + (readout ? readout->text() : QString()));
      QCoreApplication::processEvents();  // the readout laid out for its text
      const QImage seen = page->grab().toImage();
      seen.save(prefix + ".snap.png");
      const QPoint inside = canvas->mapTo(page, canvas->mapFromScene(QPointF(lf.center().x(), lf.top() + 0.25 * lf.height())));
      check(QColor(seen.pixel(inside * seen.devicePixelRatio())).lightness() > 200, "the hovered view keeps its white paper (its dashed frame only)");
      page->snapButton()->setChecked(false);
      QApplication::sendEvent(vp, &move);
      check(!canvas->hoverSnap() && canvas->snapKinds() == 0 && !readout->text().contains(SheetCanvas::snapName(opad::drawing::SnapKind::End)),
            "the Snap switch turns them off");
      page->snapButton()->setChecked(true);
      check(canvas->snapKinds() == opad::drawing::kAllSnaps, "and on again, every kind");
    }

    // Never projected as it is now: the drafts show first, then the final linework.
    for (const auto& id : v) {
      const auto spec = opad::drawing::view_spec(w.m_doc->scene, *w.m_doc->scene.sheet_view(id));
      std::error_code e;
      std::filesystem::remove(opad::cache_dir() / "projection" / (opad::drawing::projection_fingerprint(w.m_doc->doc, w.m_doc->scene, spec, opad::drawing::Quality::Auto) + ".bin"), e);
    }
    opad::drawing::clear_projection_memory();
    canvas->partLog.clear();
    canvas->refresh();
    int updating = 0;  // views waiting for their linework (marked Updating… when it takes a moment)
    for (const auto& s : canvas->viewStates()) updating += !s.final;
    check(waitFor(settled, 30000), "drawn again");
    bool draftFirst = true;
    for (const auto& id : v) {
      int draftAt = -1, finalAt = -1;
      for (size_t i = 0; i < canvas->partLog.size(); ++i)
        if (canvas->partLog[i].first != id) continue;
        else if (canvas->partLog[i].second && draftAt < 0) draftAt = static_cast<int>(i);
        else if (!canvas->partLog[i].second && finalAt < 0) finalAt = static_cast<int>(i);
      draftFirst = draftFirst && draftAt >= 0 && finalAt > draftAt;
    }
    check(draftFirst && updating == 4, QString("each uncached view waited (%2 of 4) and showed its draft before its final linework (%1 parts)").arg(canvas->partLog.size()).arg(updating));

    // The base view dragged: its projected views come along, still aligned; one edit.
    const opad::json at0 = w.m_doc->scene.sheet_view(front)->def["at"];
    const size_t ops0 = w.m_doc->doc.ops.size();
    canvas->benchDrag(front, {15, -10});
    check(waitFor([&] { return w.m_doc->doc.ops.size() == ops0 + 1; }, 5000) && waitFor(settled, 10000), "dragging the front view is one edit");
    const opad::json at1 = w.m_doc->scene.sheet_view(front)->def["at"];
    const QRectF gf = state(front).frame, gt = state(top).frame, gs = state(side).frame;
    check(std::fabs(at1[0].get<double>() - at0[0].get<double>() - 15) < 0.02 && std::fabs(at1[1].get<double>() - at0[1].get<double>() + 10) < 0.02 &&
              std::fabs(gf.center().x() - gt.center().x()) < 0.01 && std::fabs(gf.center().y() - gs.center().y()) < 0.01 && std::fabs(gt.center().x() - ft.center().x() - 15) < 0.02,
          "the front view moved 15 right and 10 down, its top and side views with it, aligned");
    // A projected view drags along its axis only: its gap.
    const double gap0 = w.m_doc->scene.sheet_view(side)->def.value("gap", 20.0);
    canvas->benchDrag(side, {12, 30});
    waitFor([&] { return std::fabs(w.m_doc->scene.sheet_view(side)->def.value("gap", 20.0) - gap0 - 12) < 0.02; }, 5000);
    waitFor(settled, 10000);
    check(std::fabs(w.m_doc->scene.sheet_view(side)->def.value("gap", 20.0) - gap0 - 12) < 0.02 && std::fabs(state(side).frame.center().y() - state(front).frame.center().y()) < 0.01,
          "the side view drags only away from the front view (its gap grew by 12), level with it");
    w.action("edit.undo")->trigger();
    waitFor([&] { return std::fabs(w.m_doc->scene.sheet_view(side)->def.value("gap", 20.0) - gap0) < 0.02; }, 5000);
    check(std::fabs(w.m_doc->scene.sheet_view(side)->def.value("gap", 20.0) - gap0) < 0.02, "Ctrl+Z puts it back");

    // A base view placed with the mouse path (size from a worker, click), then a view projected from it.
    bool placed = false;
    canvas->placeBase("right", [&](bool added) { placed = added; });
    check(canvas->placing(), "Base view from the right waits for a click on the sheet");
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &esc);
    check(!canvas->placing() && !placed, "Esc cancels the placement");
    canvas->placeBase("right", [&](bool added) { placed = added; });
    waitFor([&] { return !w.m_doc->designBusy; }, 5000);
    QCoreApplication::processEvents();
    canvas->placeAt({70, 70});
    check(waitFor([&] { return views().size() == 5; }, 5000) && placed, "a click adds the base view");
    const std::string right = views().back();
    const opad::json rd = w.m_doc->scene.sheet_view(right)->def;
    check(rd["kind"] == "base" && rd["orient"]["preset"] == "right" && std::fabs(rd["at"][0].get<double>() - 70) < 1.01 && std::fabs(rd["at"][1].get<double>() - 70) < 1.01,
          "where it was clicked (snapped to whole millimetres): " + QString::fromStdString(rd["at"].dump()));
    waitFor(settled, 10000);
    w.m_browser->selectIds({right});  // as a click on its row: the canvas and the commands follow
    check(canvas->selectedViews() == std::vector<std::string>{right} && w.action("drawings.projectedView")->isEnabled(), "selecting its row selects the view on the canvas");
    w.action("drawings.projectedView")->trigger();
    check(canvas->placing(), "Projected view from the selected view waits for the side");
    waitFor([&] { return !w.m_doc->designBusy; }, 5000);
    QCoreApplication::processEvents();
    const QRectF rf = state(right).frame;
    canvas->placeAt(canvas->toPaper(QPointF(rf.right() + 80, rf.center().y())));
    check(waitFor([&] { return views().size() == 6; }, 5000), "a click right of it adds a projected view");
    const opad::json pd = w.m_doc->scene.sheet_view(views().back())->def;
    check(pd["kind"] == "projected" && pd["parent"] == right && pd["side"] == "right" && pd["gap"].get<double>() > 10,
          "projected to the right of the new view: " + QString::fromStdString(pd.dump()));

    // Hidden lines from the ribbon, for the selected front view: the hole shows dashed.
    waitFor(settled, 10000);
    canvas->selectViews({front});
    QAction* hidden = w.action("drawings.hiddenLines");
    hidden->setChecked(true);
    emit hidden->triggered(true);
    check(waitFor([&] { return w.m_doc->scene.sheet_view(front)->def.value("style", opad::json::object()).value("hidden", false); }, 5000) && waitFor(settled, 10000),
          "Hidden lines turns them on for the selected view");
    int dashed = 0;
    {
      // Count the front view's hidden primitives through a fresh display of the sheet (what the canvas was given).
      opad::json report;
      const auto d = opad::drawing::sheet_display(w.m_doc->doc, w.m_doc->scene, *w.m_doc->scene.sheet(sheetId), {}, &report);
      for (const auto& p : d.prims)
        if (p.source == front && d.layers[size_t(p.layer)].name == "Hidden") ++dashed;
    }
    check(dashed >= 2, QString("the front view draws the hole's two hidden lines (%1)").arg(dashed));
    {  // centre marks by default: a mark on the hole seen from above, its axis from the front; the views placed since have them too
      opad::json report;
      const auto d = opad::drawing::sheet_display(w.m_doc->doc, w.m_doc->scene, *w.m_doc->scene.sheet(sheetId), {}, &report);
      std::map<std::string, int> marks;
      for (const auto& p : d.prims)
        if (d.layers[size_t(p.layer)].name == "Center") ++marks[p.source];
      bool placed = true;
      for (size_t i = 4; i < views().size(); ++i) placed = placed && w.m_doc->scene.sheet_view(views()[i])->def.value("style", opad::json::object()).value("centermarks", false);
      check(marks[top] >= 2 && marks[front] >= 1 && marks[side] >= 1 && placed && views().size() == 6,
            QString("centre marks on new drawings: top %1, front %2, side %3 primitives; the views placed with the mouse take them").arg(marks[top]).arg(marks[front]).arg(marks[side]));
    }

    // Sheet properties: A2, the title block's own fields; the template follows the paper.
    w.action("drawings.sheetProperties")->trigger();
    SheetPropertiesDialog* props = nullptr;
    waitFor([&] {
      for (QWidget* t : QApplication::topLevelWidgets())
        if (auto* d = qobject_cast<SheetPropertiesDialog*>(t); d && d->isVisible()) props = d;
      return props != nullptr;
    }, 3000);
    check(props, "Sheet properties… opens its dialog");
    if (props) {
      for (auto* c : props->findChildren<QComboBox*>())
        if (c->findData("A2") >= 0) c->setCurrentIndex(c->findData("A2"));
      if (auto* owner = props->findChild<QLineEdit*>("field.owner")) owner->setText("OPAD Bench Works");
      check(props->findChild<QLineEdit*>("field.number") && props->findChild<QLineEdit*>("field.number")->placeholderText() == "OP-2001",
            "an empty field shows what is filled in for it (the part number)");
      props->accept();
    }
    check(waitFor([&] { return w.m_doc->scene.sheet(sheetId)->width == 594; }, 5000) && w.m_doc->scene.sheet(sheetId)->def["template"]["zones"]["x"] == 12 &&
              w.m_doc->scene.sheet(sheetId)->def["values"]["owner"] == "OPAD Bench Works",
          "A2 with its zones, the owner in the title block");
    // Document properties: what every drawing of the document says (the ISO block's Approved by), one step.
    w.action("file.documentProperties")->trigger();
    DocumentPropertiesDialog* docProps = nullptr;
    waitFor([&] {
      for (QWidget* t : QApplication::topLevelWidgets())
        if (auto* d = qobject_cast<DocumentPropertiesDialog*>(t); d && d->isVisible()) docProps = d;
      return docProps != nullptr;
    }, 3000);
    check(docProps && docProps->field("approved") && docProps->field("owner") && docProps->field("owner")->text().isEmpty(), "Document properties… opens its dialog");
    if (docProps) {
      docProps->field("approved")->setText("J. Doe");
      docProps->field("project")->setText("Bench pump");
      const size_t before = w.m_doc->doc.ops.size();
      docProps->accept();
      check(waitFor([&] { return w.m_doc->doc.ops.size() == before + 1; }, 5000) && w.m_doc->scene.properties.value("approved", "") == "J. Doe" &&
                w.m_doc->scene.properties.value("project", "") == "Bench pump",
            "Apply sets them in one step");
    }
    {
      const opad::json filled = opad::drawing::title_values(w.m_doc->doc, w.m_doc->scene, *w.m_doc->scene.sheet(sheetId), false);
      check(filled.value("approved", "") == "J. Doe" && filled.value("owner", "") == "OPAD Bench Works", "the title block's Approved by comes from the document, the sheet's own owner stays");
    }
    waitFor(settled, 10000);
    canvas->fitSheet();
    check(waitFor(settled, 10000), "fitted again, every part drawn for the new zoom");
    page->grab().save(prefix + ".final.png");
    w.grab().save(prefix + ".window.png");

    // A company frame from a DXF file: the sheet takes its geometry and paper.
    {
      opad::drawing::Display company;
      const int ink = company.layer({"Border", opad::drawing::kInk, opad::drawing::LineType::Continuous, 0.5});
      company.polyline(ink, {{10, 10}, {410, 10}, {410, 287}, {10, 287}}, true);
      company.polyline(ink, {{250, 10}, {410, 10}, {410, 50}, {250, 50}}, true);
      company.text(ink, "OPAD BENCH WORKS", {260, 30}, 6);
      company.text(ink, "{title}", {260, 40}, 5);  // placeholders: the title block's fields
      company.text(ink, "<DWG_NO>", {260, 14}, 3.5);
      opad::write_text_file(std::filesystem::path((prefix + ".company.dxf").toStdU16String()), opad::drawing::dxf_text(company));
      docs->templateFromFile(prefix + ".company.dxf");
      check(waitFor([&] { return w.m_doc->scene.sheet(sheetId)->def["template"].value("id", "") == "file"; }, 15000) && w.m_doc->scene.sheet(sheetId)->width == 420,
            "Template from DXF: the company frame on A3, its geometry in the body store");
      const opad::json fields = w.m_doc->scene.sheet(sheetId)->def["template"].value("fields", opad::json::array());
      opad::drawing::Display paper;
      opad::drawing::draw_paper(paper, w.m_doc->doc, w.m_doc->scene, *w.m_doc->scene.sheet(sheetId));
      std::map<std::string, opad::drawing::Vec2> texts;
      for (const auto& p : paper.prims)
        if (p.kind == opad::drawing::Prim::Kind::Text) texts[p.text] = p.at;
      check(fields.size() == 2 && fields[0]["key"] == "title" && fields[1]["key"] == "number" && texts.count("OP-2001") && std::fabs(texts["OP-2001"][0] - 260) < 0.01 &&
                std::fabs(texts["OP-2001"][1] - 14) < 0.01 && !texts.count("<DWG_NO>"),
            QString("its placeholders became fields, the part number written where <DWG_NO> stood (%1 fields)").arg(fields.size()));
    }
    // Title block fields…: the template behind the fields, a drag adds one, a drag moves one; one edit.
    {
      w.action("drawings.templateFields")->trigger();
      TemplateFieldsDialog* tf = nullptr;
      waitFor([&] {
        for (QWidget* t : QApplication::topLevelWidgets())
          if (auto* d = qobject_cast<TemplateFieldsDialog*>(t); d && d->isVisible()) tf = d;
        return tf != nullptr;
      }, 3000);
      check(tf && tf->count() == 2, "Title block fields… opens with the template's two fields");
      if (tf) {
        check(waitFor([&] { return tf->pictured(); }, 10000), "the template drawn on a worker behind them");
        QCoreApplication::processEvents();
        QWidget* vp = tf->view()->viewport();
        const auto mouse = [&](QEvent::Type type, opad::drawing::Vec2 at) {
          const QPoint p = tf->at(at);
          QMouseEvent e(type, QPointF(p), QPointF(vp->mapToGlobal(p)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
          QApplication::sendEvent(vp, &e);
        };
        const auto dragOn = [&](opad::drawing::Vec2 a, opad::drawing::Vec2 b) {
          mouse(QEvent::MouseButtonPress, a);
          mouse(QEvent::MouseMove, {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2});
          mouse(QEvent::MouseMove, b);
          mouse(QEvent::MouseButtonRelease, b);
        };
        tf->keyBox()->setCurrentIndex(tf->keyBox()->findData("project"));
        dragOn({30, 270}, {90, 262});
        const opad::json added = tf->count() == 3 ? tf->field(2) : opad::json();
        const double mm = 1.5 / std::max(tf->view()->transform().m11(), 1e-6);  // a pixel or so
        check(added.value("key", "") == "project" && std::fabs(added["rect"][0].get<double>() - 30) < mm && std::fabs(added["rect"][1].get<double>() - 262) < mm &&
                  std::fabs(added["rect"][2].get<double>() - 60) < 2 * mm && std::fabs(added["rect"][3].get<double>() - 8) < 2 * mm,
              "a drag on the paper adds a project field there: " + QString::fromStdString(added.dump()));
        dragOn({270, 42}, {280, 46});
        const opad::json moved = tf->field(0);
        check(moved["key"] == "title" && !moved.contains("at") && moved.contains("rect") && std::fabs(moved["rect"][0].get<double>() - 270) < mm + 0.01,
              "a drag on the title field moves it (its anchor becomes a box): " + QString::fromStdString(moved.dump()));
        tf->grab().save(prefix + ".fields.png");
        const size_t before = w.m_doc->doc.ops.size();
        tf->accept();
        check(waitFor([&] { return w.m_doc->doc.ops.size() == before + 1; }, 5000) &&
                  w.m_doc->scene.sheet(sheetId)->def["template"]["fields"].size() == 3 &&
                  opad::drawing::title_values(w.m_doc->doc, w.m_doc->scene, *w.m_doc->scene.sheet(sheetId), false).value("project", "") == "Bench pump",
              "Apply is one edit; the project field shows the document's project");
        waitFor(settled, 10000);
        page->grab().save(prefix + ".template.png");
      }
    }
    // A new sheet in the drawing, its tab, the browser's row opening the first again.
    w.action("drawings.newSheet")->trigger();
    check(waitFor([&] { return w.m_doc->scene.sheets.size() == 2 && page->sheet() != sheetId; }, 5000) && page->tabs()->count() == 2 &&
              w.m_doc->scene.sheets[1].drawing == w.m_doc->scene.sheets[0].drawing && w.m_doc->scene.sheets[1].def["template"].value("id", "") == "file",
          "New sheet: a second sheet of the drawing with its template, shown in its own tab");
    docs->openSheet(top);
    check(page->sheet() == sheetId && canvas->selectedViews() == std::vector<std::string>{top}, "opening a view's row shows its sheet with the view selected");
    // PDF export of the shown sheet.
    qputenv("OPAD_BENCH_EXPORT_OUT", (prefix + ".sheet.pdf").toUtf8());
    docs->lastExport = nullptr;
    w.action("drawings.exportSheet")->trigger();
    waitFor([&] { return !docs->lastExport.is_null(); }, 20000);
    QFile pdf(prefix + ".sheet.pdf");
    check(pdf.open(QIODevice::ReadOnly) && pdf.read(5) == "%PDF-" && !docs->lastExport.contains("error"), "Export sheet… writes the PDF");
    qunsetenv("OPAD_BENCH_EXPORT_OUT");
    // Del on the canvas deletes the selected view (and what hangs on it).
    waitFor(settled, 10000);
    canvas->setFocus();
    canvas->selectViews({iso});
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(canvas, &del);
    check(waitFor([&] { return !w.m_doc->scene.sheet_view(iso); }, 5000), "Del on the canvas deletes the selected view");
    // Back to Design: the viewport again.
    w.action("workspace.design")->trigger();
    QCoreApplication::processEvents();
    check(w.m_stack->currentWidget() == w.m_viewport && w.m_toasts->host() == w.m_viewport, "Design brings the viewport back (the toasts with it)");
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: sheet: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}

// OPAD_BENCH_SHEET_LOADED=<prefix> (UI-78, a big model: the Engine): the loaded file gets a drawing (A2, front + top + side +
// iso at an automatic scale) through the area's own path; times the frames, the first draft, every view final, a drag of the
// base view afterwards (cached projections) and Issue revision…'s measure of the frozen linework, with a 1 ms ticker whose
// worst gap is the longest the event loop was held. <prefix>.png is the sheet once drawn.
OPAD_BENCH(OPAD_BENCH_SHEET_LOADED, sheetLoaded) {
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage() && w.m_doc->hasDocument;
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sheet-loaded: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  if (!ok) {
    check(false, "a loaded document");
    QCoreApplication::exit(2);
    return true;
  }
  SheetCanvas* canvas = docs->sheetPage()->canvas();
  QElapsedTimer gap, clock;
  qint64 worst = 0;
  QTimer ticker;
  ticker.setTimerType(Qt::PreciseTimer);
  QObject::connect(&ticker, &QTimer::timeout, [&] { worst = std::max(worst, gap.restart()); });
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
  };
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !w.m_doc->designBusy && !states.empty() && canvas->paperPictured() &&
           std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return (v.final && (v.picture || v.prims == 0)) || !v.error.isEmpty(); });
  };
  w.action("workspace.drawings")->trigger();
  gap.start();
  clock.start();
  ticker.start(1);
  std::string sheet;
  docs->createDrawing({{"size", "A2"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "auto"}, {"views", {"front", "top", "side", "iso"}}},
                      [&](const std::string& id) { sheet = id; });
  const bool added_ = waitFor([&] { return !sheet.empty(); }, 600000);  // before the message: arguments have no order
  check(added_, QString("the drawing laid out on a worker and added in %1 ms (worst event-loop gap %2 ms)").arg(clock.elapsed()).arg(worst));
  const qint64 added = clock.elapsed();
  qint64 framed = -1, drafted = -1;
  QObject::connect(canvas, &SheetCanvas::partsArrived, canvas, [&] {
    if (framed < 0 && !canvas->viewStates().empty() && !canvas->viewStates()[0].frame.isEmpty()) framed = clock.elapsed();
    if (drafted < 0 && canvas->draftsShown() > 0) drafted = clock.elapsed();
  });
  worst = 0;
  gap.restart();
  const bool done = waitFor(settled, 900000);
  int curves = 0;
  for (const auto& s : canvas->viewStates()) curves += s.prims;
  check(done && worst < 250, QString("4 views drawn: frames at %1 ms, first draft at %2 ms, all final at %3 ms, %4 primitives; worst event-loop gap %5 ms")
                                 .arg(framed - added).arg(drafted < 0 ? -1 : drafted - added).arg(clock.elapsed() - added).arg(curves).arg(worst));
  for (const auto& s : canvas->viewStates())
    trace::log(QString("bench: sheet-loaded: view %1: frame %2 x %3 mm, linework %4 x %5 mm").arg(QString::fromStdString(s.id.substr(0, 8))).arg(s.frame.width(), 0, 'f', 1)
                   .arg(s.frame.height(), 0, 'f', 1).arg(s.linework.width(), 0, 'f', 1).arg(s.linework.height(), 0, 'f', 1));
  docs->sheetPage()->grab().save(prefix + ".png");
  if (const opad::Sheet* s = w.m_doc->scene.sheet(sheet); s && !s->views.empty()) {
    worst = 0;
    gap.restart();
    clock.restart();
    const size_t ops = w.m_doc->doc.ops.size();
    canvas->benchDrag(s->views[0], {10, 5});
    const bool moved = waitFor([&] { return w.m_doc->doc.ops.size() == ops + 1; }, 30000) && waitFor(settled, 120000);
    check(moved && worst < 250, QString("the base view dragged: drawn again in %1 ms from cached projections; worst event-loop gap %2 ms").arg(clock.elapsed()).arg(worst));
  }
  // Issue revision… measures what the frozen linework adds on a worker (UI-84); closed without issuing.
  worst = 0;
  gap.restart();
  clock.restart();
  w.action("drawings.issue")->trigger();
  IssueDialog* issue = nullptr;
  waitFor([&] { return (issue = w.findChild<IssueDialog*>()) != nullptr; }, 5000);
  const bool measured = issue && waitFor([&] { return issue->freezeBox()->text().contains("adds about"); }, 600000);
  check(measured && worst < 250, QString("Issue revision… measured the frozen linework in %1 ms: %2; worst event-loop gap %3 ms")
                                     .arg(clock.elapsed()).arg(issue ? issue->freezeBox()->text() : QString()).arg(worst));
  if (issue) issue->reject();
  waitFor([&] { return !w.m_doc->designBusy; }, 60000);
  ticker.stop();
  trace::log(QString("bench: sheet-loaded: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
