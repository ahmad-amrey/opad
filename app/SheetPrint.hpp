#pragma once
// Printing drawings (TODO 11 UI-86): the sheets of the shown drawing as the PDF export draws them (drawing::sheet_display on a
// worker), a preview of each page rendered on a worker at the size shown, and a print job that paints the pages into the
// printer on a worker (QPainter may paint a QPrinter off the GUI thread) through the same QPainter backend as PDF and PNG
// (drawing::paint): what prints is the PDF page. Options: the printer, the drawing's sheets or the shown one, fitted to the
// printer's paper or at actual size (1:1 on the sheet's own paper, centred when the printer's is another), black ink or
// colour, copies. Ctrl+Alt+P (Ctrl+P is Properties).
#include <QDialog>
#include <QImage>
#include <functional>
#include <memory>
#include <vector>

#include "opad/drawing/display.hpp"

class JobRunner;
class QCheckBox;
class QComboBox;
class QLabel;
class QRadioButton;
class QSpinBox;
class QTimer;
class QToolButton;

namespace printing {
using Pages = std::shared_ptr<const std::vector<opad::drawing::Display>>;
struct Settings {
  QString printer;  // a printer's name; empty: the default one
  QString pdf;      // a PDF file instead of a printer (benches, "print to file")
  bool fit = true, black = true;
  int copies = 1;
  std::vector<int> pages;  // which; empty: all
  QString title;
  int pauseMs = 0;  // benches: a wait after each page, so that a cancel lands between pages
};
// The pages on a printer or into a PDF file, on a worker; done(ok, error) on the UI thread. Cancel stops between pages and
// sends nothing: the printer's job is aborted, a PDF removed.
void print(JobRunner* jobs, Pages pages, Settings settings, std::function<void(bool ok, const QString& error)> done);
opad::drawing::Display inked(const opad::drawing::Display& d);  // everything in black ink (images as they are)
}  // namespace printing

class SheetPrintDialog : public QDialog {
  Q_OBJECT
 public:
  SheetPrintDialog(JobRunner* jobs, printing::Pages pages, const QStringList& names, int current, QWidget* parent);
  ~SheetPrintDialog() override { *m_alive = false; }
  printing::Settings settings() const;
  // Benches: print into this PDF instead of a printer, waiting pauseMs after each page.
  void setOutputFile(const QString& pdf, int pauseMs = 0) { m_outputFile = pdf, m_pause = pauseMs; }
  // For benches.
  int page() const { return m_page; }
  void showPage(int index);
  bool previewReady() const;  // rendered for the size and screen it is shown on
  QLabel* previewLabel() const { return m_view; }
  QRadioButton* thisButton() const { return m_this; }
  const QImage& preview() const { return m_preview; }
  QComboBox* printerBox() const { return m_printer; }
  QRadioButton* fitButton() const { return m_fit; }
  QRadioButton* actualButton() const { return m_actual; }
  QRadioButton* allButton() const { return m_all; }
  QCheckBox* blackBox() const { return m_black; }
  QLabel* pageLabel() const { return m_pageLabel; }

 signals:
  void printRequested();

 private:
  void render();  // the shown page's preview on a worker, for the label's size and the screen's pixel ratio
  void resizeEvent(QResizeEvent* e) override;  // rendered again (debounced), also on another screen
  bool event(QEvent* e) override;
  JobRunner* m_jobs;
  printing::Pages m_pages;
  QStringList m_names;
  int m_page = 0, m_current = 0;
  bool m_rendering = false, m_again = false;
  QImage m_preview;
  QString m_outputFile;
  int m_pause = 0;
  QLabel *m_view, *m_pageLabel;
  QToolButton *m_prev, *m_next;
  QComboBox* m_printer;
  QRadioButton *m_all, *m_this, *m_fit, *m_actual;
  QCheckBox* m_black;
  QSpinBox* m_copies;
  QTimer* m_resized;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
