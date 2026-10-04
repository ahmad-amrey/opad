#include "SheetPrint.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPageLayout>
#include <QPainter>
#include <QPointer>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <map>

#include "AppDocument.hpp"
#include "DocsArea.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "Theme.hpp"
#include "opad/drawing/paint.hpp"
#include "opad/drawing/sheet.hpp"

namespace dr = opad::drawing;

namespace printing {
dr::Display inked(const dr::Display& d) {
  dr::Display out = d;
  for (auto& l : out.layers) l.rgb = dr::kInk;
  for (auto& p : out.prims)
    if (p.kind != dr::Prim::Kind::Image) p.rgb = dr::kByLayer;
  return out;
}

void print(JobRunner* jobs, Pages pages, Settings s, std::function<void(bool, const QString&)> done) {
  const QString phase = QObject::tr("Printing");
  jobs->async(
      s.pdf.isEmpty() ? QObject::tr("Printing %1").arg(s.title) : QObject::tr("Printing %1 to %2").arg(s.title, QFileInfo(s.pdf).fileName()),
      [pages, s, phase](Progress p) {
        QPrinter printer(QPrinter::HighResolution);
        if (!s.pdf.isEmpty()) {
          printer.setOutputFormat(QPrinter::PdfFormat);
          printer.setOutputFileName(s.pdf);
        } else if (!s.printer.isEmpty()) {
          printer.setPrinterName(s.printer);
        }
        if (!printer.isValid()) throw opad::Error("the printer is not available");
        printer.setDocName(s.title);
        printer.setCreator("OPAD");
        printer.setFullPage(true);  // device coordinates from the paper's corner: actual size is actual
        printer.setCopyCount(std::max(1, s.copies));
        printer.setColorMode(s.black ? QPrinter::GrayScale : QPrinter::Color);
        static const std::map<std::string, QPageSize::PageSizeId> ids = {
            {"A4", QPageSize::A4},         {"A3", QPageSize::A3},         {"A2", QPageSize::A2},         {"A1", QPageSize::A1},
            {"A0", QPageSize::A0},         {"ANSI-A", QPageSize::AnsiA}, {"ANSI-B", QPageSize::AnsiB}, {"ANSI-C", QPageSize::AnsiC},
            {"ANSI-D", QPageSize::AnsiD}, {"ANSI-E", QPageSize::AnsiE}};
        const QPageSize own = printer.pageLayout().pageSize();  // the printer's paper, for fitting
        std::vector<int> which = s.pages;
        if (which.empty())
          for (int i = 0; i < static_cast<int>(pages->size()); ++i) which.push_back(i);
        QPainter painter;
        for (size_t k = 0; k < which.size(); ++k) {
          if (p.cancelled()) break;
          p.setPhase(phase, static_cast<int>(100 * k / which.size()));
          const dr::Display& sheet = (*pages)[static_cast<size_t>(which[k])];
          const dr::Display black = s.black ? inked(sheet) : dr::Display();
          const dr::Display& d = s.black ? black : sheet;
          const dr::Page page = dr::page_for(d);
          const auto id = ids.find(page.paper);
          const QPageSize paper = s.fit ? own
                                  : id != ids.end() ? QPageSize(id->second)
                                                    : QPageSize(QSizeF(std::min(page.w, page.h), std::max(page.w, page.h)), QPageSize::Millimeter, QString(), QPageSize::ExactMatch);
          QPageLayout layout = printer.pageLayout();
          layout.setPageSize(paper);
          layout.setOrientation(page.w > page.h ? QPageLayout::Landscape : QPageLayout::Portrait);
          printer.setPageLayout(layout);  // before the page it is for; a printer without that paper keeps its own
          if (k == 0 ? !painter.begin(&printer) : !printer.newPage()) throw opad::Error("the printer refused the page");
          painter.setRenderHint(QPainter::Antialiasing);
          const double dots = printer.resolution() / 25.4;
          const QRectF full = printer.pageLayout().fullRectPixels(printer.resolution());
          QRectF target;
          if (s.fit) {  // inside what the printer can print
            target = printer.pageLayout().paintRectPixels(printer.resolution());
            if (target.isEmpty()) target = full;
          } else {  // 1:1, centred on the paper
            const QSizeF size(page.w * dots, page.h * dots);
            target = QRectF(full.center() - QPointF(size.width() / 2, size.height() / 2), size);
          }
          dr::paint(painter, d, page.window, target);
          if (s.pauseMs > 0) QThread::msleep(static_cast<unsigned long>(s.pauseMs));
        }
        if (p.cancelled()) {  // the pages painted so far go nowhere: the printer's job aborted (ended unsent), a PDF removed
          if (painter.isActive()) {
            printer.abort();
            painter.end();
          }
          if (!s.pdf.isEmpty()) QFile::remove(s.pdf);
          throw opad::Error("cancelled");
        }
        if (painter.isActive()) painter.end();
      },
      std::move(done));
}
}  // namespace printing

SheetPrintDialog::SheetPrintDialog(JobRunner* jobs, printing::Pages pages, const QStringList& names, int current, QWidget* parent)
    : QDialog(parent), m_jobs(jobs), m_pages(std::move(pages)), m_names(names), m_page(current), m_current(current) {
  setObjectName("printDialog");
  setWindowTitle(tr("Print"));
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(16, 16, 16, 16);
  h->setSpacing(16);
  // The preview, page by page.
  auto* left = new QVBoxLayout();
  m_view = new QLabel(this);
  m_view->setObjectName("printPreview");
  m_view->setMinimumSize(560, 400);
  m_view->setAlignment(Qt::AlignCenter);
  m_view->setAutoFillBackground(true);
  QPalette pal = m_view->palette();
  pal.setColor(QPalette::Window, theme::current().bg3);
  m_view->setPalette(pal);
  left->addWidget(m_view, 1);
  auto* nav = new QHBoxLayout();
  m_prev = new QToolButton(this);
  m_prev->setArrowType(Qt::LeftArrow);
  m_prev->setAutoRaise(true);
  m_prev->setToolTip(tr("Previous sheet"));
  m_next = new QToolButton(this);
  m_next->setArrowType(Qt::RightArrow);
  m_next->setAutoRaise(true);
  m_next->setToolTip(tr("Next sheet"));
  m_pageLabel = new QLabel(this);
  m_pageLabel->setObjectName("secondary");
  nav->addStretch();
  nav->addWidget(m_prev);
  nav->addWidget(m_pageLabel);
  nav->addWidget(m_next);
  nav->addStretch();
  left->addLayout(nav);
  h->addLayout(left, 1);
  // The options.
  auto* right = new QVBoxLayout();
  auto* form = new QFormLayout();
  m_printer = new QComboBox(this);
  m_printer->setObjectName("print.printer");
  const QString preferred = QSettings().value("print/printer", QPrinterInfo::defaultPrinterName()).toString();
  for (const QString& name : QPrinterInfo::availablePrinterNames()) m_printer->addItem(name, name);
  if (m_printer->count() == 0) m_printer->addItem(tr("No printer found"), QString());
  m_printer->setCurrentIndex(std::max(0, m_printer->findData(preferred)));
  form->addRow(tr("Printer"), m_printer);
  const int n = static_cast<int>(m_pages->size());
  auto* sheets = new QVBoxLayout();
  m_all = new QRadioButton(tr("All %n sheets of the drawing", nullptr, n), this);
  m_all->setObjectName("print.all");
  m_this = new QRadioButton(tr("This sheet"), this);
  m_this->setObjectName("print.this");
  m_all->setEnabled(n > 1);
  sheets->addWidget(m_all);
  sheets->addWidget(m_this);
  form->addRow(tr("Sheets"), sheets);
  const dr::Page first = dr::page_for((*m_pages)[static_cast<size_t>(std::clamp(current, 0, n - 1))]);
  auto* scale = new QVBoxLayout();
  m_fit = new QRadioButton(tr("Fit to the printer's paper"), this);
  m_fit->setObjectName("print.fit");
  m_actual = new QRadioButton(first.paper.empty() ? tr("Actual size (1:1)") : tr("Actual size (1:1 on %1)").arg(QString::fromStdString(first.paper)), this);
  m_actual->setObjectName("print.actual");
  m_actual->setToolTip(tr("On the sheet's own paper when the printer has it, else centred on the printer's: scales measured on the print are true"));
  (QSettings().value("print/fit", true).toBool() ? m_fit : m_actual)->setChecked(true);
  for (const auto& [a, b] : {std::pair<QRadioButton*, QRadioButton*>{m_all, m_this}, {m_fit, m_actual}}) {  // two choices, not one
    auto* group = new QButtonGroup(this);
    group->addButton(a);
    group->addButton(b);
  }
  (n > 1 ? m_all : m_this)->setChecked(true);  // checked again: siblings were exclusive until grouped
  scale->addWidget(m_fit);
  scale->addWidget(m_actual);
  form->addRow(tr("Scale"), scale);
  m_black = new QCheckBox(tr("Black ink only"), this);
  m_black->setObjectName("print.black");
  m_black->setChecked(QSettings().value("print/black", true).toBool());
  m_black->setToolTip(tr("Coloured layers and annotations print black, as drawings are read"));
  form->addRow(tr("Ink"), m_black);
  m_copies = new QSpinBox(this);
  m_copies->setRange(1, 99);
  form->addRow(tr("Copies"), m_copies);
  right->addLayout(form);
  right->addStretch();
  auto* footer = new QHBoxLayout();
  footer->addStretch();
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  auto* print = new QPushButton(tr("Print"), this);
  print->setObjectName("primary");
  print->setDefault(true);
  print->setEnabled(m_printer->currentData().isValid() && !m_printer->currentData().toString().isEmpty());
  footer->addWidget(cancel);
  footer->addWidget(print);
  right->addLayout(footer);
  h->addLayout(right);
  m_resized = new QTimer(this);
  m_resized->setSingleShot(true);
  m_resized->setInterval(150);
  connect(m_resized, &QTimer::timeout, this, [this] { render(); });
  connect(m_prev, &QToolButton::clicked, this, [this] { showPage(m_page - 1); });
  connect(m_next, &QToolButton::clicked, this, [this] { showPage(m_page + 1); });
  connect(m_black, &QCheckBox::toggled, this, [this] { render(); });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(print, &QPushButton::clicked, this, [this] {
    QSettings st;
    st.setValue("print/printer", m_printer->currentData().toString());
    st.setValue("print/fit", m_fit->isChecked());
    st.setValue("print/black", m_black->isChecked());
    emit printRequested();
    accept();
  });
  resize(900, 560);
  showPage(current);
}

printing::Settings SheetPrintDialog::settings() const {
  printing::Settings s;
  s.printer = m_printer->currentData().toString();
  s.pdf = m_outputFile;
  s.fit = m_fit->isChecked();
  s.black = m_black->isChecked();
  s.copies = m_copies->value();
  s.pauseMs = m_pause;
  if (m_this->isChecked()) s.pages = {m_page};  // the sheet previewed
  s.title = m_names.value(m_this->isChecked() ? m_page : m_current);
  return s;
}

bool SheetPrintDialog::previewReady() const { return !m_rendering && !m_again && !m_preview.isNull() && !m_resized->isActive(); }

void SheetPrintDialog::resizeEvent(QResizeEvent* e) {
  QDialog::resizeEvent(e);
  m_resized->start();
}

bool SheetPrintDialog::event(QEvent* e) {
  if (e->type() == QEvent::DevicePixelRatioChange) m_resized->start();  // moved to a screen of another scale
  return QDialog::event(e);
}

void SheetPrintDialog::showPage(int index) {
  const int n = static_cast<int>(m_pages->size());
  m_page = std::clamp(index, 0, std::max(0, n - 1));
  m_prev->setEnabled(m_page > 0);
  m_next->setEnabled(m_page + 1 < n);
  const dr::Page page = dr::page_for((*m_pages)[static_cast<size_t>(m_page)]);
  m_pageLabel->setText(tr("%1 · sheet %2 of %3 · %4").arg(m_names.value(m_page)).arg(m_page + 1).arg(n).arg(
      page.paper.empty() ? QString("%1 × %2 mm").arg(page.w).arg(page.h) : QString::fromStdString(page.paper)));
  m_this->setText(n > 1 ? tr("This sheet: %1").arg(m_names.value(m_page)) : tr("This sheet"));
  m_actual->setText(page.paper.empty() ? tr("Actual size (1:1)") : tr("Actual size (1:1 on %1)").arg(QString::fromStdString(page.paper)));
  render();
}

void SheetPrintDialog::render() {
  if (m_rendering) {
    m_again = true;
    return;
  }
  m_rendering = true;
  m_view->setText(tr("Drawing the preview…"));
  const QSize room = m_view->size() * devicePixelRatioF();
  const dr::Page page = dr::page_for((*m_pages)[static_cast<size_t>(m_page)]);
  const double dpi = 25.4 * std::min((room.width() - 16) / std::max(page.w, 1.0), (room.height() - 16) / std::max(page.h, 1.0));
  auto image = std::make_shared<QImage>();
  const auto pages = m_pages;
  const int index = m_page;
  const bool black = m_black->isChecked();
  auto alive = m_alive;
  m_jobs->async(
      tr("Drawing the print preview"),
      [pages, index, black, dpi, image](Progress) {
        const dr::Display& d = (*pages)[static_cast<size_t>(index)];
        *image = dr::paint_image(black ? printing::inked(d) : d, std::max(dpi, 10.0));
      },
      [this, alive, image](bool ok, const QString& error) {
        if (!*alive) return;
        m_rendering = false;
        if (m_again) {
          m_again = false;
          return render();
        }
        if (!ok) return m_view->setText(error);
        m_preview = *image;
        const double dpr = devicePixelRatioF();
        QPixmap pm = QPixmap::fromImage(m_preview);
        const QSize room = m_view->size() * dpr;  // the label shrank meanwhile: never cropped (a new one is on its way)
        if (pm.width() > room.width() || pm.height() > room.height()) pm = pm.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pm.setDevicePixelRatio(dpr);
        m_view->setPixmap(pm);
      });
}

// ---------------------------------------------------------------- the area's part
void DocsArea::printSheets(std::function<void(SheetPrintDialog*)> opened) {
  const std::string shown = m_page ? m_page->sheet() : std::string();
  const opad::Sheet* sheet = services().document()->scene.sheet(shown);
  if (!sheet) throw opad::Error("Open a sheet first.");
  std::vector<std::string> ids;
  QStringList names;
  int current = 0;
  for (const auto& s : services().document()->scene.sheets)
    if (s.id == sheet->id || (!sheet->drawing.empty() && s.drawing == sheet->drawing)) {
      if (s.id == sheet->id) current = static_cast<int>(ids.size());
      ids.push_back(s.id);
      names << QString::fromStdString(s.name);
    }
  auto pages = std::make_shared<std::vector<dr::Display>>(ids.size());
  const QString phase = tr("Drawing the sheets");
  const QString title = QString::fromStdString(sheet->drawing.empty() ? sheet->name : sheet->drawing);
  QPointer<DocsArea> self(this);
  m_page->canvas()->read(
      tr("Preparing to print"),
      [ids, pages, phase](const opad::Document& doc, const opad::Scene& scene, Progress p) {
        for (size_t i = 0; i < ids.size(); ++i)
          if (const opad::Sheet* s = scene.sheet(ids[i]))
            (*pages)[i] = dr::sheet_display(doc, scene, *s, [&](double f, const std::string&) {
              p.setPhase(phase, f < 0 ? -1 : static_cast<int>(100 * (static_cast<double>(i) + f) / static_cast<double>(ids.size())));
              return !p.cancelled();
            });
      },
      [self, pages, names, current, title, opened](bool ok, const QString& error) {
        if (!self) return;
        if (!ok) {
          if (error != "cancelled") self->services().guarded([&] { throw opad::Error(error.toStdString()); });
          return;
        }
        auto* dialog = new SheetPrintDialog(self->services().jobs(), pages, names, current, self->services().window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &SheetPrintDialog::printRequested, self, [self, dialog, pages, title] {
          printing::Settings s = dialog->settings();
          if (s.title.isEmpty()) s.title = title;
          const int count = s.pages.empty() ? static_cast<int>(pages->size()) : static_cast<int>(s.pages.size());
          self->lastPrint = nullptr;
          printing::print(self->services().jobs(), pages, s, [self, s, count](bool ok, const QString& error) {
            if (!self) return;
            self->lastPrint = ok ? opad::json{{"pages", count}} : opad::json{{"error", error.toStdString()}};
            if (ok) self->services().toast(s.pdf.isEmpty() ? tr("%n sheets sent to %1", nullptr, count).arg(s.printer) : tr("%n sheets printed to %1", nullptr, count).arg(s.pdf));
            else if (error != "cancelled") self->services().guarded([&] { throw opad::Error(("Printing failed: " + error).toStdString()); });
          });
        });
        dialog->open();
        if (opened) opened(dialog);
      });
}
