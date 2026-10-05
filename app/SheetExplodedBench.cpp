// Exploded-view drawings (UI-85) in the Drawings workspace. Case in tools/bench_cases/docs.py.
#include "MainWindow.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QMenu>
#include <QRegularExpression>
#include <QStatusBar>
#include <QtEndian>

#include <algorithm>
#include <cmath>

#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "DrawingsFolder.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "Toast.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/sheet.hpp"

// OPAD_BENCH_SHEET_EXPLODED=<prefix>: a plate, a post and a lid stacked, drawn front on an A3 sheet. Exploded view with
// none saved says how to save one and places nothing; with "Exploded 1" saved (explode along Z), it follows the pointer
// at the size the exploded parts take and a click places a view of it: one step, the view op naming the exploded view and
// seen from its camera, drawn apart with its trail lines on the Trail layer, the front view as it was. Auto-balloon with
// no view selected balloons the exploded view and adds the parts list, each leader ending on its part where the view
// draws it and crossing no other part's lines; Exploded 1 updated with twice the spacing (the
// Explode panel's Update view), the drawing view draws its parts further apart, the balloons still measured. The front
// view's View state: Exploded 1 (one step, still from the front, with trail lines), then Assembled again. Then Publish PDF
// from Review (UI-104, Review > Share and the File menu): the drawing's sheet written as a PDF whose page strokes the
// exploded view's trail lines with the phantom dash pattern (its content streams read back), the workspace kept, a PDF
// also for a name typed with .dxf and Export sheet's remembered format left alone.
// <prefix>.exploded.png (both views apart), <prefix>.publish.dxf.pdf.
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
    {  // each leader ends on its part's lines where the view draws it apart, over no other part (the post's crossed the plate)
      const opad::Sheet* s = doc->scene.sheet(sheet);
      const auto frames = opad::drawing::layout(doc->doc, doc->scene, *s);
      const auto f = std::find_if(frames.begin(), frames.end(), [&](const opad::drawing::ViewFrame& x) { return x.id == drawn; });
      int good = 0, balloons = 0;
      QStringList bad;
      if (f != frames.end()) {
        const auto g = opad::drawing::shape_linework(opad::drawing::project(doc->doc, doc->scene, opad::drawing::view_spec(doc->scene, *doc->scene.sheet_view(drawn))), *f);
        struct Piece {
          Vec2 a, b;
          std::string node;
        };
        std::vector<Piece> shown;
        const auto paper = [&](Vec2 p) { return Vec2{f->at[0] + f->scale * (p[0] - f->centre[0]), f->at[1] + f->scale * (p[1] - f->centre[1])}; };
        for (const auto& c : g->curves)
          if (!c.hidden && c.body >= 0) {
            const auto pts = c.sample(0.01);
            for (size_t k = 1; k < pts.size(); ++k) shown.push_back({paper(pts[k - 1]), paper(pts[k]), g->bodies[size_t(c.body)].node});
          }
        const auto side = [](Vec2 o, Vec2 p, Vec2 q) { return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0]); };
        const auto crosses = [&](Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
          const double d1 = side(c, d, a), d2 = side(c, d, b), d3 = side(a, b, c), d4 = side(a, b, d);
          return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
        };
        for (const auto& id : s->items) {
          const opad::SheetItem* t = doc->scene.sheet_item(id);
          if (!t || t->kind != "balloon" || t->view != drawn) continue;
          ++balloons;
          const opad::json m = opad::drawing::measure_item(doc->doc, doc->scene, *s, *t, &*f);
          const Vec2 at{f->at[0] + t->def["place"]["text"][0].get<double>(), f->at[1] + t->def["place"]["text"][1].get<double>()};
          const Vec2 tip{f->at[0] + m["tip"][0].get<double>(), f->at[1] + m["tip"][1].get<double>()};
          const double l = std::hypot(tip[0] - at[0], tip[1] - at[1]);
          const Vec2 from{at[0] + (tip[0] - at[0]) * 5 / l, at[1] + (tip[1] - at[1]) * 5 / l}, to{tip[0] - (tip[0] - at[0]) / l, tip[1] - (tip[1] - at[1]) / l};
          double nearest = 1e9;
          int across = 0;
          for (const Piece& p : shown) {
            if (p.node == t->refs[0].body) {
              const Vec2 d{p.b[0] - p.a[0], p.b[1] - p.a[1]};
              const double l2 = d[0] * d[0] + d[1] * d[1];
              const double u = l2 > 0 ? std::clamp(((tip[0] - p.a[0]) * d[0] + (tip[1] - p.a[1]) * d[1]) / l2, 0.0, 1.0) : 0;
              nearest = std::min(nearest, std::hypot(p.a[0] + u * d[0] - tip[0], p.a[1] + u * d[1] - tip[1]));
            } else {
              across += crosses(from, to, p.a, p.b);
            }
          }
          if (nearest < 0.05 && across == 0) ++good;
          else bad << QString("%1 (%2 mm off its part, across %3)").arg(QString::fromStdString(m.value("number", ""))).arg(nearest, 0, 'f', 2).arg(across);
        }
      }
      check(balloons >= 3 && good == balloons, QString("each balloon's leader ends on its part where it is drawn apart and crosses no other part (%1 of %2)%3")
                                                   .arg(good).arg(balloons).arg(bad.isEmpty() ? QString() : ": " + bad.join(", ")));
    }
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
    // Publish PDF from Review: the drawing written as a PDF without going to Drawings (no file dialog in a bench), whatever
    // name was typed (here one ending in .dxf), and Export sheet keeps the format it was last used with (DXF here).
    w.setWorkspace("review");
    const QString typed = prefix + ".publish.dxf", pdf = typed + ".pdf";
    QFile::remove(pdf);
    QFile::remove(typed);
    QSettings().setValue("export/sheetFormat", "dxf");
    qputenv("OPAD_BENCH_EXPORT_OUT", typed.toUtf8());
    const CommandInfo* publish = w.m_commands.find("drawings.publish");
    w.updateCommands();
    check(publish && publish->menuPath == "file" && publish->workspaces.contains("review") && w.action("drawings.publish")->isEnabled(),
          "Publish PDF in the File menu and on Review > Share, enabled with a drawing in the document");
    w.action("drawings.publish")->trigger();
    const auto pdfBytes = [&] {
      QFile f(pdf);
      return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const auto written = [&] {
      const QByteArray b = pdfBytes();
      return b.startsWith("%PDF-") && b.trimmed().endsWith("%%EOF");
    };
    check(waitFor(written, 60000) && w.workspaceId() == "review", "Publish PDF from Review writes the drawing as a PDF and stays in Review");
    qunsetenv("OPAD_BENCH_EXPORT_OUT");
    check(!QFile::exists(typed) && QSettings().value("export/sheetFormat").toString() == "dxf",
          "a name typed with .dxf still gets a PDF (.dxf.pdf), and Export sheet still offers DXF first");
    {  // its pages are vectors: the exploded view's trail lines stroked with the phantom dash pattern, one stroke each
      const QByteArray data = pdfBytes();
      QByteArray content;  // the pages' content streams, inflated
      for (qsizetype at = data.indexOf(" obj"); at >= 0; at = data.indexOf(" obj", at + 4)) {
        const qsizetype open = data.indexOf("stream", at), next = data.indexOf(" obj", at + 4);
        if (open < 0 || (next >= 0 && next < open)) continue;
        qsizetype from = open + 6;
        from += data.mid(from, 2) == "\r\n" ? 2 : data.mid(from, 1) == "\n" ? 1 : 0;
        const qsizetype end = data.indexOf("endstream", from);
        if (end < 0) break;
        const QByteArray head = data.mid(at, open - at);
        at = end;
        if (!head.contains("/FlateDecode") || head.contains("/Length1") || head.contains("/Subtype")) continue;
        QByteArray packed(4, '\0');
        qToBigEndian<quint32>(quint32(std::min<qsizetype>((end - from) * 8 + 65536, 1 << 26)), packed.data());  // qUncompress grows it as needed
        content += qUncompress(packed + data.mid(from, end - from));
      }
      static const QRegularExpression phantom(R"(\[12 3 1\.5\d* 3 1\.5\d* 3 \]0\s+d\s+((?:[-\d.]+ [-\d.]+ m\s+[-\d.]+ [-\d.]+ l\s+S\s+)+))");
      long strokes = 0;
      for (auto m = phantom.globalMatch(QString::fromLatin1(content)); m.hasNext();) strokes += m.next().captured(1).count(QRegularExpression(R"(\bS\b)"));
      const long want = trails(drawn);
      check(!content.isEmpty() && want >= 1 && strokes == want,
            QString("its PDF strokes the exploded view's trail lines as phantom lines, one each (%1 of %2)").arg(strokes).arg(want));
    }
  } catch (const std::exception& e) {
    check(false, QString("bench stopped: %1").arg(e.what()));
  }
  trace::log(QString("bench: sheet-exploded: %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
