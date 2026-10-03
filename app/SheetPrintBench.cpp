#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QRadioButton>
#include <QRegularExpression>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "SheetPrint.hpp"

// OPAD_BENCH_SHEET_PRINT=<prefix> (UI-86): a two-sheet drawing of a plate. Print… (Ctrl+Alt+P) draws the drawing's sheets on
// a worker and opens the print dialog on the shown sheet: its preview rendered on a worker (white paper, black ink, the
// sheet's proportions), "sheet 1 of 2 · A3", the next sheet's preview; printed at actual size to a PDF through the printer
// path on a worker: two A3 landscape pages; this sheet alone fitted to the printer's paper: one landscape page.
// <prefix>.print.png, <prefix>.actual.pdf, <prefix>.fit.pdf.
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
      int ink = 0;
      for (int y = 0; y < img.height(); y += 2)
        for (int x = 0; x < img.width(); x += 2) ink += qGray(img.pixel(x, y)) < 100;
      check(std::abs(static_cast<double>(img.width()) / img.height() - 420.0 / 297) < 0.02 && qGray(img.pixel(img.width() / 2, 3)) > 240 && ink > 200,
            QString("white paper of the sheet's proportions with black ink (%1 x %2, %3 dark samples)").arg(img.width()).arg(img.height()).arg(ink));
    }
    check(dialog->pageLabel()->text().contains("sheet 1 of 2") && dialog->pageLabel()->text().contains("A3") && dialog->allButton()->isChecked(),
          "on the shown sheet: " + dialog->pageLabel()->text() + "; all sheets chosen");
    dialog->grab().save(prefix + ".print.png");
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
    dialog->findChild<QRadioButton*>("print.this")->setChecked(true);
    dialog->fitButton()->setChecked(true);
    dialog->setOutputFile(fit);
    docs->lastPrint = nullptr;
    emit dialog->printRequested();
    dialog->accept();
    check(waitFor([&] { return docs->lastPrint.is_object(); }, 60000) && docs->lastPrint.value("pages", 0) == 1, "this sheet alone");
    size = {};
    const int one = pdfPages(fit, &size);
    check(one == 1 && size.width() > size.height(), QString("fitted: one landscape page of %1 x %2 pt").arg(size.width()).arg(size.height()));
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: print: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
