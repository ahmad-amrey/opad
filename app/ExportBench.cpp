#include "MainWindow.hpp"

#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTimer>

#include <functional>
#include <map>

#include "AppDocument.hpp"
#include "DrawingsFolder.hpp"
#include "Panels.hpp"
#include "Jobs.hpp"
#include "Viewport.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/render.hpp"

// OPAD_BENCH_EXPORT=<prefix>: a 60 x 40 x 10 plate with a 10 mm hole through it. The Export dialog offers DXF, SVG and
// DWG for the solid (they were refused before), shows its 2D view row only while one of those is chosen, and exports the
// front view with hidden lines as DXF on a worker (the call returns while the job runs); the file read back has the
// plate's outline on Visible (4 lines) and the hole's sides on Hidden (2). Then SVG of the current camera (iso): the
// hole's rims come out as arcs, no hidden layer. The dialog opens again with those choices. <prefix>.dialog.png is the
// dialog, <prefix>.front.dxf and <prefix>.iso.svg the files. Then PDF (UI-87: one vector page on A4, the hidden lines
// in it) and PNG (300 dpi, the outline where the drawing puts it) of the front view: <prefix>.front.pdf/.png, and the
// dialog with PDF chosen, <prefix>.pdf-dialog.png. Last a drawing sheet (two views, a dimension, a note) exported from
// its row's Export sheet… as a PDF of its A4 paper, and with a second (A3) sheet the drawing's Export drawing… as a
// PDF of two pages: <prefix>.sheet.pdf, <prefix>.drawing.pdf.
// OPAD_BENCH_EXPORT_OPEN=<prefix>: the loaded file's roots (hidden or not) as a front view with hidden lines through the
// dialog, as DXF or as OPAD_BENCH_EXPORT_FORMAT says (pdf, png, ...), timed (the Engine: the stall watchdog stays quiet
// while the worker projects and writes); <prefix>.<format>.
bool MainWindow::benchExport() {
  QString prefix = qEnvironmentVariable("OPAD_BENCH_EXPORT");
  const QString open = qEnvironmentVariable("OPAD_BENCH_EXPORT_OPEN");
  if (prefix.isEmpty() && open.isEmpty()) return false;
  bool ok = true;
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: export: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto settle = [&](const std::function<bool()>& done, int ms) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  // Runs `fn` on the Export dialog from inside its own event loop (it is modal), then answers it.
  const auto withDialog = [&](const std::function<bool(QDialog*)>& fn, std::vector<std::string> ids = {}) {
    QTimer::singleShot(100, this, [this, fn] {
      auto* d = findChild<QDialog*>("exportDialog");
      if (!d) return;
      if (fn(d)) d->accept();
      else d->reject();
    });
    exportDialog(std::move(ids));
  };
  const auto radio = [](QDialog* d, const char* format) {
    for (auto* r : d->findChildren<QRadioButton*>())
      if (r->property("format").toString() == format) return r;
    return static_cast<QRadioButton*>(nullptr);
  };
  // Edges per layer of a 2D file read back.
  const auto layers = [](const QString& file) {
    std::map<std::string, int> edges;
    opad::Document doc = opad::Document::create();
    opad::import_file(doc, std::filesystem::path(file.toStdU16String()));
    const opad::Scene scene = opad::resolve(doc);
    for (const auto& id : scene.all_bodies())
      for (TopExp_Explorer e(opad::node_world_shape(doc, scene, id), TopAbs_EDGE); e.More(); e.Next()) ++edges[scene.node(id)->name];
    return edges;
  };
  if (!open.isEmpty()) {  // the loaded file (the Engine): timed, the watchdog's stalls in the trace
    const QString format = qEnvironmentVariable("OPAD_BENCH_EXPORT_FORMAT", "dxf"), out = open + "." + format;
    QFile::remove(out);
    qputenv("OPAD_BENCH_EXPORT_OUT", out.toUtf8());
    m_lastExport = opad::json();
    QElapsedTimer clock;
    try {
      withDialog([&](QDialog* d) {
        radio(d, format.toUtf8().constData())->click();
        auto* view = d->findChild<QComboBox*>("export.view");
        view->setCurrentIndex(view->findData("front"));
        d->findChild<QCheckBox*>("export.hidden")->setChecked(true);
        return true;
      }, m_doc->scene.roots);
      clock.start();
      trace::log("bench: export-open: started");
      settle([&] { return !m_lastExport.is_null(); }, 600000);
    } catch (const std::exception& e) {
      m_lastExport = {{"error", e.what()}};
    }
    const bool done = m_lastExport.value("total", 0) > 0;
    check(done, QString("the loaded file's front view with hidden lines in %1 ms: %2").arg(clock.elapsed()).arg(QString::fromStdString(m_lastExport.dump()).left(400)));
    qunsetenv("OPAD_BENCH_EXPORT_OUT");
    QCoreApplication::exit(ok ? 0 : 2);
    return true;
  }
  try {
    m_doc->newDocument();
    const std::string plate = m_doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}})["body_ids"][0];
    double x0, y0, z0, x1, y1, z1;
    opad::node_world_bbox(m_doc->doc, m_doc->scene, plate).Get(x0, y0, z0, x1, y1, z1);
    const auto mm = [](double v) { return QString::number(v, 'f', 3).toStdString() + " mm"; };
    m_doc->run("feature", {{"kind", "cylinder"},
                           {"inputs", {{"plane", {{"origin", {0, 0, z0 - 5}}, {"normal", {0, 0, 1}}}}, {"x", mm((x0 + x1) / 2)}, {"y", mm((y0 + y1) / 2)},
                                       {"diameter", "10 mm"}, {"height", "30 mm"}, {"operation", "cut"}}}});
    settle([&] { return !m_jobs->busy(); }, 20000);
    check(m_doc->scene.all_bodies().size() == 1, "a plate with a hole");

    // DXF of the front view with hidden lines.
    const QString dxf = prefix + ".front.dxf";
    QFile::remove(dxf);
    qputenv("OPAD_BENCH_EXPORT_OUT", dxf.toUtf8());
    m_lastExport = opad::json();
    withDialog([&](QDialog* d) {
      auto* view = d->findChild<QComboBox*>("export.view");
      auto* hidden = d->findChild<QCheckBox*>("export.hidden");
      check(view && hidden && radio(d, "dxf") && radio(d, "dxf")->isEnabled() && radio(d, "svg")->isEnabled() && radio(d, "dwg")->isEnabled(),
            "DXF, SVG and DWG are offered for a solid");
      if (!view || !hidden) return false;
      radio(d, "step")->click();
      check(!view->isVisibleTo(d), "STEP shows no 2D view row");
      radio(d, "dxf")->click();
      check(view->isVisibleTo(d) && view->count() == 8 && hidden->isVisibleTo(d), "DXF shows the view row (8 views, hidden lines)");
      view->setCurrentIndex(view->findData("front"));
      hidden->setChecked(true);
      QString summary;
      for (auto* l : d->findChildren<QLabel*>())
        if (l->objectName() == "secondary" && l->text().contains("2D view")) summary = l->text();
      check(!summary.isEmpty(), "the summary names the view: " + summary);
      d->grab().save(prefix + ".dialog.png");
      return true;
    });
    check(m_jobs->busy() && m_lastExport.is_null(), "the export runs as a job, the dialog's call has returned");
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    check(m_lastExport.value("layers", opad::json::object()).value("Visible", 0) == 4 && m_lastExport["layers"].value("Hidden", 0) == 2,
          "front view: 4 visible and 2 hidden lines " + QString::fromStdString(m_lastExport.dump()));
    const auto front = layers(dxf);
    check(front.count("Visible") && front.at("Visible") == 4 && front.count("Hidden") && front.at("Hidden") == 2, "the DXF reads back with them on their layers");

    // SVG of the current camera, no hidden lines.
    m_viewport->setCameraJson(opad::Camera::preset("iso").to_json());
    const QString svg = prefix + ".iso.svg";
    QFile::remove(svg);
    qputenv("OPAD_BENCH_EXPORT_OUT", svg.toUtf8());
    m_lastExport = opad::json();
    withDialog([&](QDialog* d) {
      radio(d, "svg")->click();
      auto* view = d->findChild<QComboBox*>("export.view");
      view->setCurrentIndex(view->findData("camera"));
      d->findChild<QCheckBox*>("export.hidden")->setChecked(false);
      return true;
    });
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    const auto dir = m_lastExport.value("view", opad::json::object()).value("dir", opad::json::array());
    const bool iso = dir.size() == 3 && std::abs(dir[0].get<double>() + dir[1].get<double>()) < 1e-6 * std::abs(dir[0].get<double>()) &&
                     std::abs(dir[0].get<double>() - dir[2].get<double>()) < 1e-6 * std::abs(dir[0].get<double>());
    check(iso && m_lastExport.value("entities", opad::json::object()).value("ELLIPSE", 0) >= 2 && !m_lastExport["layers"].contains("Hidden"),
          "the current (iso) view as SVG: the hole's rims as arcs, no hidden layer");
    const auto round = layers(svg);
    check(round.count("Visible") && !round.count("Hidden"), "the SVG reads back");

    // The choices are kept.
    withDialog([&](QDialog* d) {
      radio(d, "dxf")->click();
      check(d->findChild<QComboBox*>("export.view")->currentData().toString() == "camera" && !d->findChild<QCheckBox*>("export.hidden")->isChecked(),
            "the dialog opens with the last view and hidden-line choice");
      return false;
    });

    // PDF of the front view with hidden lines: one vector page on an A4 sheet, written on a worker.
    const QString pdf = prefix + ".front.pdf";
    QFile::remove(pdf);
    qputenv("OPAD_BENCH_EXPORT_OUT", pdf.toUtf8());
    m_lastExport = opad::json();
    withDialog([&](QDialog* d) {
      auto* view = d->findChild<QComboBox*>("export.view");
      check(radio(d, "pdf") && radio(d, "pdf")->isEnabled() && radio(d, "png") && radio(d, "png")->isEnabled(), "PDF and PNG are offered for a solid");
      if (!radio(d, "pdf")) return false;
      radio(d, "pdf")->click();
      bool note = false;
      for (auto* l : d->findChildren<QLabel*>()) note = note || (l->isVisibleTo(d) && l->text().contains("ISO sheet"));
      check(view->isVisibleTo(d) && note, "PDF shows the view row and says what page it makes");
      view->setCurrentIndex(view->findData("front"));
      d->findChild<QCheckBox*>("export.hidden")->setChecked(true);
      d->grab().save(prefix + ".pdf-dialog.png");
      return true;
    });
    check(m_jobs->busy() && m_lastExport.is_null(), "the PDF is written by a job, the dialog's call has returned");
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    QFile pdfFile(pdf);
    const bool pdfOpen = pdfFile.open(QIODevice::ReadOnly);
    check(m_lastExport.value("paper", "") == "A4" && m_lastExport.value("layers", opad::json::object()).value("Hidden", 0) == 2 && pdfOpen &&
              pdfFile.read(5) == "%PDF-",
          "the front view as a PDF page on A4 with its hidden lines " + QString::fromStdString(m_lastExport.dump()).left(300));

    // PNG of the front view without hidden lines: the plate's outline 2 mm in from the picture's edges, at 300 dpi.
    const QString png = prefix + ".front.png";
    QFile::remove(png);
    qputenv("OPAD_BENCH_EXPORT_OUT", png.toUtf8());
    m_lastExport = opad::json();
    withDialog([&](QDialog* d) {
      radio(d, "png")->click();
      d->findChild<QCheckBox*>("export.hidden")->setChecked(false);
      return true;
    });
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    const QImage picture(png);
    const auto pixels = m_lastExport.value("pixels", opad::json::array());
    check(pixels.size() == 2 && pixels[0] == 756 && pixels[1] == 165 && picture.width() == 756 && qGray(picture.pixel(24, 83)) < 110 &&
              qGray(picture.pixel(378, 83)) > 240,
          "the front view as a 300 dpi picture: 64 x 14 mm, its left side dark, no hidden lines " + QString::fromStdString(m_lastExport.dump()).left(300));

    // A drawing sheet from the Drawings folder's Export sheet… (UI-86): A4 with a front view, the top view below it, the
    // plate's width dimensioned and a note; a PDF page of the sheet's own size written by a job.
    std::string width;  // the top front edge along x
    const TopoDS_Shape shape = opad::node_world_shape(m_doc->doc, m_doc->scene, plate);
    for (int i = 0; width.empty() && i < opad::subshape_count(shape, opad::Ref::Kind::Edge); ++i) {
      const opad::Ref r{plate, opad::Ref::Kind::Edge, i};
      const opad::json e = opad::inspect_ref(m_doc->doc, m_doc->scene, r);
      if (e.contains("direction") && std::abs(std::abs(e["direction"][0].get<double>()) - 1) < 1e-9 && std::abs(e["bbox"]["center"][1].get<double>() - y0) < 1e-6 &&
          std::abs(e["bbox"]["center"][2].get<double>() - z1) < 1e-6)
        width = r.str();
    }
    const std::string sheet = m_doc->run("sheet", {{"size", "A4"}, {"name", "Plate"}, {"drawing", "Plate drawing"}})["id"];
    const std::string base = m_doc->run("sheet_view", {{"sheet", sheet}, {"orient", "front"}, {"at", {90, 160}}})["id"];
    m_doc->run("sheet_view", {{"sheet", sheet}, {"parent", base}, {"side", "bottom"}});
    m_doc->run("sheet_item", {{"sheet", sheet}, {"view", base}, {"type", "horizontal"}, {"refs", {width}}});
    m_doc->run("sheet_item", {{"sheet", sheet}, {"text", "ALL EDGES 0.5 x 45°"}, {"at", {30, 30}}});
    QMenu menu;
    drawings::contextMenu(m_doc, sheet, menu, [] {}, [this, sheet] { emit m_browser->sheetExportRequested(sheet); });
    QAction* exportSheet = menu.findChild<QAction*>("drawings.export");
    check(exportSheet != nullptr, "a sheet row offers Export sheet…");
    const QString sheetPdf = prefix + ".sheet.pdf";
    QFile::remove(sheetPdf);
    qputenv("OPAD_BENCH_EXPORT_OUT", sheetPdf.toUtf8());
    m_lastExport = opad::json();
    if (exportSheet) exportSheet->trigger();
    check(m_jobs->busy() && m_lastExport.is_null(), "the sheet is projected and written by a job");
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    QFile sheetFile(sheetPdf);
    const bool sheetOpen = sheetFile.open(QIODevice::ReadOnly);
    const auto drawn = m_lastExport.value("sheet", opad::json::object());
    check(drawn.value("views", 0) == 2 && drawn.value("items", 0) == 2 && m_lastExport.value("paper", "") == "A4" &&
              m_lastExport.value("layers", opad::json::object()).value("Dimensions", 0) > 0 && sheetOpen && sheetFile.read(5) == "%PDF-",
          "the sheet as an A4 PDF page: 2 views, the dimension and the note " + QString::fromStdString(m_lastExport.dump()).left(400));

    // The whole drawing (its row's Export drawing…): a second sheet, A3 with an isometric view, as the second page.
    const std::string second = m_doc->run("sheet", {{"size", "A3"}, {"name", "Plate iso"}, {"drawing", "Plate drawing"}})["id"];
    m_doc->run("sheet_view", {{"sheet", second}, {"orient", "iso"}, {"scale", "2:1"}});
    QMenu drawingMenu;
    drawings::contextMenu(m_doc, "drawing:Plate drawing", drawingMenu, [] {}, [this] { emit m_browser->sheetExportRequested("drawing:Plate drawing"); });
    QAction* exportDrawing = drawingMenu.findChild<QAction*>("drawings.export");
    check(exportDrawing && exportDrawing->text().contains("drawing"), "a drawing row offers Export drawing…");
    const QString drawingPdf = prefix + ".drawing.pdf";
    QFile::remove(drawingPdf);
    qputenv("OPAD_BENCH_EXPORT_OUT", drawingPdf.toUtf8());
    m_lastExport = opad::json();
    if (exportDrawing) exportDrawing->trigger();
    check(m_jobs->busy() && m_lastExport.is_null(), "the drawing is projected and written by a job");
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    QFile drawingFile(drawingPdf);
    const QByteArray pdfData = drawingFile.open(QIODevice::ReadOnly) ? drawingFile.readAll() : QByteArray();
    const auto pages = m_lastExport.value("pages", opad::json::array());
    check(pages.size() == 2 && pages[0].value("paper", "") == "A4" && pages[1].value("paper", "") == "A3" && m_lastExport.value("sheets", opad::json::array()).size() == 2 &&
              pdfData.startsWith("%PDF-") && QString::fromLatin1(pdfData).count(QRegularExpression("/Type\\s*/Page(?!s)")) == 2,
          "the drawing as a PDF of two pages, A4 and A3 " + QString::fromStdString(m_lastExport.dump()).left(400));
  } catch (const std::exception& e) {
    check(false, QString("error: ") + e.what());
  }
  qunsetenv("OPAD_BENCH_EXPORT_OUT");
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
