#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QRadioButton>
#include <QRegularExpression>

#include <algorithm>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "Jobs.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "SheetPrint.hpp"

// OPAD_BENCH_SHEET_PRINT=<prefix> (UI-86): a two-sheet drawing of a plate. Print… (Ctrl+Alt+P) draws the drawing's sheets on
// a worker and opens the print dialog on the shown sheet: its preview rendered on a worker (white paper, black ink, the
// sheet's proportions, shown whole in its label; a bigger dialog renders it again, bigger), "sheet 1 of 2 · A3", the next
// sheet's preview; printed at actual size to a PDF through the printer path on a worker: two A3 landscape pages; This sheet is
// the sheet previewed, alone fitted to the printer's paper: one landscape page; a print cancelled after its first page leaves
// no PDF. Export sheet… offers DWG only while a converter is found. <prefix>.print.png, <prefix>.actual.pdf, <prefix>.fit.pdf.
OPAD_BENCH(OPAD_BENCH_SHEET_PRINT, sheetPrint) {
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage();
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: print: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: print: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  SheetPage* page = docs->sheetPage();
  SheetCanvas* canvas = page->canvas();
  AppDocument* doc = w.m_doc;
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !doc->designBusy && std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return v.final || !v.error.isEmpty(); });
  };
  // Pages of a PDF and the first one's size in points.
  const auto pdfPages = [](const QString& file, QSizeF* size) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QString text = QString::fromLatin1(f.readAll());
    const auto m = QRegularExpression("/MediaBox \\[\\s*0\\s+0\\s+([\\d.]+)\\s+([\\d.]+)\\s*\\]").match(text);
    if (size && m.hasMatch()) *size = QSizeF(m.captured(1).toDouble(), m.captured(2).toDouble());
    return static_cast<int>(text.count(QRegularExpression("/Type\\s*/Page\\b(?!s)")));
  };
  try {
    doc->newDocument();
    doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "80 mm"}, {"width", "50 mm"}, {"height", "10 mm"}}}});
    w.action("workspace.drawings")->trigger();
    std::string sheet;
    docs->createDrawing({{"size", "A3"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "1:1"}, {"views", {"front", "top"}}},
                        [&](const std::string& id) { sheet = id; });
    check(waitFor([&] { return !sheet.empty(); }, 20000) && waitFor(settled, 30000), "a drawing of the plate");
    docs->newSheet();
    check(waitFor([&] { return doc->scene.sheets.size() == 2 && !doc->designBusy; }, 10000), "a second sheet in the drawing");
    page->showSheet(sheet);
    waitFor(settled, 30000);
    QAction* print = w.action("drawings.print");
    check(print && print->shortcut() == QKeySequence("Ctrl+Alt+P") && print->isEnabled(), "Print… is a command on Ctrl+Alt+P, enabled with a sheet shown");
    SheetPrintDialog* dialog = nullptr;
    docs->printSheets([&](SheetPrintDialog* d) { dialog = d; });
    check(waitFor([&] { return dialog != nullptr; }, 30000), "the sheets are drawn on a worker, then the dialog opens");
    if (!dialog) throw opad::Error("no dialog");
    check(waitFor([&] { return dialog->previewReady(); }, 30000), "its preview is rendered on a worker");
    {
      const QImage& img = dialog->preview();
      int ink = 0, coloured = 0;
      const int step = img.width() >= 800 ? 2 : 1;  // every pixel of a small preview (a window at display scale 1)
      for (int y = 0; y < img.height(); y += step)
        for (int x = 0; x < img.width(); x += step) {
          const QRgb c = img.pixel(x, y);
          ink += qGray(c) < 100;
          coloured += std::max({qRed(c), qGreen(c), qBlue(c)}) - std::min({qRed(c), qGreen(c), qBlue(c)}) > 24;
        }
      check(std::abs(static_cast<double>(img.width()) / img.height() - 420.0 / 297) < 0.02 && qGray(img.pixel(img.width() / 2, 3)) > 240 && ink > 200 && coloured == 0,
            QString("white paper of the sheet's proportions with black ink, its text grey on white (%1 x %2, %3 dark samples, %4 coloured)")
                .arg(img.width()).arg(img.height()).arg(ink).arg(coloured));
    }
    check(dialog->pageLabel()->text().contains("sheet 1 of 2") && dialog->pageLabel()->text().contains("A3") && dialog->allButton()->isChecked(),
          "on the shown sheet: " + dialog->pageLabel()->text() + "; all sheets chosen");
    const auto shown = [&] {  // the preview as the label shows it: whole (never cropped), filling most of it
      const QSizeF pm = dialog->previewLabel()->pixmap().deviceIndependentSize();
      const QSize room = dialog->previewLabel()->size();
      return std::pair<bool, QString>{pm.width() <= room.width() + 1 && pm.height() <= room.height() + 1 &&
                                          std::max(pm.width() / room.width(), pm.height() / room.height()) > 0.8,
                                      QString("%1 x %2 in %3 x %4").arg(pm.width()).arg(pm.height()).arg(room.width()).arg(room.height())};
    };
    check(shown().first, "the preview shown whole in its label, for its size: " + shown().second);
    dialog->grab().save(prefix + ".print.png");
    const int narrow = dialog->preview().width();
    dialog->resize(dialog->width() + 300, dialog->height() + 200);
    const bool again = waitFor([&] { return !dialog->previewReady(); }, 2000) && waitFor([&] { return dialog->previewReady(); }, 30000);  // before the message
    check(again && dialog->preview().width() > narrow && shown().first,
          QString("a bigger dialog renders it again, bigger (%1 -> %2 px): ").arg(narrow).arg(dialog->preview().width()) + shown().second);
    dialog->showPage(1);
    check(waitFor([&] { return dialog->previewReady(); }, 30000) && dialog->pageLabel()->text().contains("sheet 2 of 2"), "the next sheet's preview");
    // Actual size, both sheets, into a PDF through the printer.
    const QString actual = prefix + ".actual.pdf";
    QFile::remove(actual);
    dialog->actualButton()->setChecked(true);
    dialog->setOutputFile(actual);
    docs->lastPrint = nullptr;
    emit dialog->printRequested();
    dialog->accept();
    check(waitFor([&] { return docs->lastPrint.is_object(); }, 60000) && docs->lastPrint.value("pages", 0) == 2, "printed on a worker: " + QString::fromStdString(docs->lastPrint.dump()));
    QSizeF size;
    const int pages = pdfPages(actual, &size);
    check(pages == 2 && std::abs(size.width() - 1190.55) < 1 && std::abs(size.height() - 841.89) < 1,
          QString("actual size: %1 pages of %2 x %3 pt (A3 landscape)").arg(pages).arg(size.width()).arg(size.height()));
    // This sheet alone, fitted to the printer's own paper.
    dialog = nullptr;
    docs->printSheets([&](SheetPrintDialog* d) { dialog = d; });
    check(waitFor([&] { return dialog != nullptr && dialog->previewReady(); }, 30000), "Print… again");
    if (!dialog) throw opad::Error("no dialog");
    const QString fit = prefix + ".fit.pdf";
    QFile::remove(fit);
    dialog->thisButton()->setChecked(true);
    dialog->showPage(1);
    check(waitFor([&] { return dialog->previewReady(); }, 30000) && dialog->settings().pages == std::vector<int>{1} &&
              dialog->thisButton()->text().contains(dialog->pageLabel()->text().section(QString::fromUtf8(" · "), 0, 0)),
          "This sheet is the sheet previewed: " + dialog->thisButton()->text());
    dialog->fitButton()->setChecked(true);
    dialog->setOutputFile(fit);
    docs->lastPrint = nullptr;
    emit dialog->printRequested();
    dialog->accept();
    check(waitFor([&] { return docs->lastPrint.is_object(); }, 60000) && docs->lastPrint.value("pages", 0) == 1, "this sheet alone");
    size = {};
    const int one = pdfPages(fit, &size);
    check(one == 1 && size.width() > size.height(), QString("fitted: one landscape page of %1 x %2 pt").arg(size.width()).arg(size.height()));
    // Cancelled after its first page: nothing goes out (here the PDF is removed; a printer's job is aborted).
    dialog = nullptr;
    docs->printSheets([&](SheetPrintDialog* d) { dialog = d; });
    check(waitFor([&] { return dialog != nullptr && dialog->previewReady(); }, 30000), "Print… a third time");
    if (!dialog) throw opad::Error("no dialog");
    const QString dropped = prefix + ".cancelled.pdf";
    QFile::remove(dropped);
    dialog->allButton()->setChecked(true);
    dialog->setOutputFile(dropped, 1500);
    docs->lastPrint = nullptr;
    emit dialog->printRequested();
    dialog->accept();
    Job* job = docs->services().jobs()->newest();  // the printing just begun
    const bool begun = job && job->title().startsWith("Printing") && waitFor([&] { return QFile::exists(dropped); }, 20000);
    if (job && begun) job->cancel();
    check(begun && waitFor([&] { return docs->lastPrint.is_object(); }, 5000) && docs->lastPrint.value("error", "") == "cancelled" &&
              waitFor([&] { return !QFile::exists(dropped); }, 10000),
          "cancelled after its first page: the print reports it and its PDF is gone");
    // Export sheet… offers DWG only while a converter is found (here: OPAD_DXF2DWG naming a file, then none); a drawing only PDF.
    const auto has = [](const std::vector<std::pair<QString, QString>>& types, const char* f) {
      return std::any_of(types.begin(), types.end(), [&](const auto& t) { return t.first == f; });
    };
    const QByteArray was = qgetenv("OPAD_DXF2DWG");
    qputenv("OPAD_DXF2DWG", QDir(QDir::tempPath()).filePath("no-such-dxf2dwg.exe").toUtf8());
    const auto without = DocsArea::sheetExportTypes(false);
    qputenv("OPAD_DXF2DWG", QCoreApplication::applicationFilePath().toUtf8());
    const auto with = DocsArea::sheetExportTypes(false);
    if (was.isEmpty()) qunsetenv("OPAD_DXF2DWG");
    else qputenv("OPAD_DXF2DWG", was);
    check(!has(without, "dwg") && has(without, "pdf") && has(without, "svg") && has(without, "dxf") && has(without, "png") && has(with, "dwg") &&
              DocsArea::sheetExportTypes(true).size() == 1,
          "Export sheet offers DWG only with a converter, a drawing only PDF");
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: print: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
