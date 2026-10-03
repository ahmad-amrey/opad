#include "PlotDialog.hpp"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <Prs3d_LineAspect.hxx>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#ifdef OPAD_HAVE_PRINT
#include <QPrintDialog>
#include <QPrinter>
#endif

#include <cmath>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "GuidedTool.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"
#include "Viewport.hpp"
#include "opad/version.hpp"

// ---------------------------------------------------------------- painting
std::shared_ptr<const PlotPicture> PlotPicture::build(plot::Sheet sheet) {
  auto picture = std::make_shared<PlotPicture>();
  picture->strokes.resize(sheet.styles.size());
  picture->fills.resize(sheet.styles.size());
  picture->dots.resize(sheet.styles.size());
  for (const plot::Item& item : sheet.items) {
    if (item.dot) {
      picture->dots[item.style].push_back({item.rings[0][0][0], item.rings[0][0][1]});
      continue;
    }
    QPainterPath& path = item.fill ? picture->fills[item.style].emplace_back() : picture->strokes[item.style];
    if (item.fill) path.setFillRule(Qt::OddEvenFill);
    for (const auto& ring : item.rings) {
      path.moveTo(ring[0][0], ring[0][1]);
      for (size_t i = 1; i < ring.size(); ++i) path.lineTo(ring[i][0], ring[i][1]);
      if (item.fill) path.closeSubpath();
    }
  }
  // Images: decoded from their data and fitted to their frames as the view does (preserveAspectRatio: meet or slice,
  // centred), placed by their corners.
  for (const plot::Image& image : sheet.images) {
    const auto comma = image.href.find(',');
    if (image.href.rfind("data:image/", 0) != 0 || comma == std::string::npos || image.href.substr(0, comma).find(";base64") == std::string::npos) continue;
    QImage pixels = QImage::fromData(QByteArray::fromBase64(QByteArray::fromStdString(image.href.substr(comma + 1))));
    if (pixels.isNull()) continue;
    const QPointF o(image.origin[0], image.origin[1]), right = QPointF(image.right[0], image.right[1]) - o, down = QPointF(image.down[0], image.down[1]) - o;
    const double across = std::hypot(right.x(), right.y()), tall = std::hypot(down.x(), down.y());
    if (across <= 0 || tall <= 0) continue;
    if (image.aspect != "none") {
      const double ratio = across / tall;
      const int h = int(std::clamp(std::max(double(pixels.height()), pixels.width() / ratio), 1.0, 4096.0)), w = int(std::clamp(h * ratio, 1.0, 8192.0));
      QImage canvas(w, h, QImage::Format_ARGB32_Premultiplied);
      canvas.fill(Qt::transparent);
      const QImage scaled = pixels.scaled(w, h, image.aspect.find("slice") != std::string::npos ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio, Qt::SmoothTransformation);
      QPainter painter(&canvas);
      painter.drawImage((w - scaled.width()) / 2, (h - scaled.height()) / 2, scaled);
      painter.end();
      pixels = canvas;
    }
    const double w = pixels.width(), h = pixels.height();
    picture->images.push_back({pixels, QTransform(right.x() / w, right.y() / w, down.x() / h, down.y() / h, o.x(), o.y())});
  }
  picture->sheet = std::move(sheet);
  return picture;
}

namespace {
// A layer's dashes (pattern mm, < 0 gap, 0 dot) as a pen's dash pattern: on and off lengths in pen widths, on paper as
// the view draws them on screen.
QList<qreal> dashPattern(const std::vector<double>& dashes, double paperWidth) {
  QList<qreal> out;
  bool on = true;
  for (double d : dashes) {
    const bool dash = d >= 0;
    const qreal length = std::max(std::abs(d) * plot::kPatternPaper / paperWidth, 0.5);
    if (out.isEmpty() && !dash) continue;  // a pattern starts with ink
    if (!out.isEmpty() && dash == on) out.back() += length;
    else out << length, on = dash;
  }
  if (out.size() % 2) out << 0.5;
  return out;
}
}  // namespace

void paintPlot(QPainter& p, const PlotPicture& picture, const plot::Settings& s, const plot::Placement& placed, double unitsPerMm) {
  if (!placed.valid()) return;
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(unitsPerMm, unitsPerMm);  // paper millimetres from here
  const QRectF printable(s.margin, s.margin, s.paperWidth - 2 * s.margin, s.paperHeight - 2 * s.margin);
  // The plot area, wide enough for the lines on its edge (the extents' outermost lines were cut down the middle).
  double reach = 0;
  for (const auto& style : picture.sheet.styles) reach = std::max(reach, plot::paperWeight(style, s));
  const QRectF area(placed.x, placed.y, placed.area.width() * placed.scale, placed.area.height() * placed.scale);
  p.setClipRect(printable.intersected(area.adjusted(-reach, -reach, reach, reach)));
  QTransform t;
  t.translate(placed.x, placed.y);
  t.scale(placed.scale, -placed.scale);  // the plot plane's y up, the paper's down
  t.translate(-placed.area.x0, -placed.area.y1);
  p.setTransform(t, true);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  for (const auto& image : picture.images) {  // under the lines; grey in monochrome
    p.save();
    p.setTransform(image.place, true);
    p.drawImage(QPointF(0, 0), s.monochrome ? image.image.convertToFormat(QImage::Format_Grayscale8) : image.image);
    p.restore();
  }
  const auto& styles = picture.sheet.styles;
  auto colour = [&](size_t i) {
    const drawing2d::Rgb c = plot::paperColor(styles[i], s);
    return QColor::fromRgbF(float(c[0]), float(c[1]), float(c[2]));
  };
  for (size_t i = 0; i < styles.size(); ++i)
    for (const QPainterPath& fill : picture.fills[i]) p.fillPath(fill, colour(i));
  for (size_t i = 0; i < styles.size(); ++i) {
    const double weight = plot::paperWeight(styles[i], s);
    if (!picture.strokes[i].isEmpty()) {
      QPen pen(colour(i), weight / placed.scale, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);
      if (!styles[i].dashes.empty()) pen.setDashPattern(dashPattern(styles[i].dashes, weight));
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      p.drawPath(picture.strokes[i]);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(colour(i));
    for (const QPointF& dot : picture.dots[i]) p.drawEllipse(dot, weight / placed.scale, weight / placed.scale);
  }
  p.restore();
  if (!s.stamp.empty()) {  // 2 mm text along the bottom margin, from the left margin
    p.save();
    QFont font = p.font();
    font.setPixelSize(std::max(1, int(std::lround(2 * unitsPerMm))));
    p.setFont(font);
    p.setPen(QColor(64, 64, 64));
    p.drawText(QPointF(std::max(s.margin, 3.0) * unitsPerMm, (s.paperHeight - std::clamp(s.margin * 0.35, 1.5, 4.0)) * unitsPerMm), QString::fromStdString(s.stamp));
    p.restore();
  }
}

// ---------------------------------------------------------------- preview
PlotPreview::PlotPreview(QWidget* parent) : QWidget(parent) {
  setObjectName("plotPreview");
  setMinimumSize(420, 320);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PlotPreview::setImage(const QImage& image) {
  m_image = image;
  update();
}

QSize PlotPreview::pageSize(double paperWidth, double paperHeight) const {
  const double room = 24, w = std::max(40.0, width() - 2 * room), h = std::max(40.0, height() - 2 * room);
  const double k = std::min(w / paperWidth, h / paperHeight) * devicePixelRatioF();
  return QSize(std::max(1, int(paperWidth * k)), std::max(1, int(paperHeight * k)));
}

void PlotPreview::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.fillRect(rect(), t.bg3);
  if (m_image.isNull()) return;
  const QSizeF size = QSizeF(m_image.size()) / devicePixelRatioF();
  const QRectF page(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
  p.fillRect(page.translated(3, 3), QColor(0, 0, 0, 60));  // the paper's shadow
  p.drawImage(page, m_image);
  p.setPen(QPen(t.line, 1));
  p.drawRect(page);
}

// ---------------------------------------------------------------- the dialog
namespace {
struct Paper {
  const char* name;
  QPageSize::PageSizeId id;
};
const std::vector<Paper>& papers() {
  static const std::vector<Paper> list = {{"ISO A4", QPageSize::A4},    {"ISO A3", QPageSize::A3},          {"ISO A2", QPageSize::A2},
                                          {"ISO A1", QPageSize::A1},    {"ISO A0", QPageSize::A0},          {"ANSI A (Letter)", QPageSize::Letter},
                                          {"Legal", QPageSize::Legal},  {"ANSI B (Tabloid)", QPageSize::Tabloid}, {"ANSI C", QPageSize::AnsiC},
                                          {"ANSI D", QPageSize::AnsiD}, {"ANSI E", QPageSize::AnsiE}};
  return list;
}
QLabel* header(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setObjectName("sectionHeader");
  return l;
}
// A one-page layout of the paper as it lies, without margins (the plot keeps its own).
QPageLayout pageLayout(const plot::Settings& s) {
  const QSizeF portrait(std::min(s.paperWidth, s.paperHeight), std::max(s.paperWidth, s.paperHeight));
  return QPageLayout(QPageSize(portrait, QPageSize::Millimeter), s.paperWidth > s.paperHeight ? QPageLayout::Landscape : QPageLayout::Portrait, QMarginsF(),
                     QPageLayout::Millimeter);
}
}  // namespace

PlotDialog::PlotDialog(AreaServices& services, QWidget* parent) : QDialog(parent), m_services(services) {
  setObjectName("plotDialog");
  setWindowTitle(tr("Plot"));
  setWindowModality(Qt::WindowModal);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto* body = new QHBoxLayout();
  body->setContentsMargins(16, 16, 16, 12);
  body->setSpacing(16);
  outer->addLayout(body, 1);
  auto* options = new QVBoxLayout();
  options->setSpacing(8);
  body->addLayout(options);

  options->addWidget(header(tr("PAPER"), this));
  auto* paperForm = new QFormLayout();
  m_paper = new QComboBox(this);
  m_paper->setObjectName("plotPaper");
  for (const Paper& p : papers()) m_paper->addItem(p.name, int(p.id));
  m_orientation = new QComboBox(this);
  m_orientation->setObjectName("plotOrientation");
  m_orientation->addItem(tr("Landscape"));
  m_orientation->addItem(tr("Portrait"));
  m_margin = new QDoubleSpinBox(this);
  m_margin->setObjectName("plotMargin");
  m_margin->setRange(0, 50);
  m_margin->setDecimals(1);
  m_margin->setSuffix(" mm");
  m_margin->setValue(10);
  paperForm->addRow(tr("Size"), m_paper);
  paperForm->addRow(tr("Orientation"), m_orientation);
  paperForm->addRow(tr("Margins"), m_margin);
  options->addLayout(paperForm);

  options->addWidget(header(tr("PLOT AREA"), this));
  auto* regions = new QButtonGroup(this);
  m_extents = new QRadioButton(tr("Extents: everything plotted"), this);
  m_display = new QRadioButton(tr("Display: what the view shows"), this);
  m_windowRegion = new QRadioButton(tr("Window"), this);
  m_extents->setObjectName("plotExtents");
  m_display->setObjectName("plotDisplay");
  m_windowRegion->setObjectName("plotWindow");
  m_extents->setChecked(true);
  m_pick = new QPushButton(tr("Pick…"), this);
  m_pick->setObjectName("plotPick");
  m_pick->setToolTip(tr("Pick two corners in the view (object snap applies)"));
  m_pick->setAutoDefault(false);  // Enter plots (the footer's primary), never starts a pick
  auto* windowRow = new QHBoxLayout();
  windowRow->addWidget(m_windowRegion);
  windowRow->addWidget(m_pick);
  windowRow->addStretch(1);
  for (auto* r : {m_extents, m_display, m_windowRegion}) regions->addButton(r);
  options->addWidget(m_extents);
  options->addWidget(m_display);
  options->addLayout(windowRow);

  options->addWidget(header(tr("SCALE"), this));
  m_fit = new QCheckBox(tr("Fit to paper"), this);
  m_fit->setObjectName("plotFit");
  m_fit->setChecked(true);
  auto* scaleRow = new QHBoxLayout();
  scaleRow->addWidget(new QLabel("1 :", this));
  m_scale = new QDoubleSpinBox(this);
  m_scale->setObjectName("plotScale");
  m_scale->setRange(0.001, 1e6);
  m_scale->setDecimals(3);
  m_scale->setValue(1);
  m_scale->setToolTip(tr("Drawing millimetres per paper millimetre: 1 : 50 draws 50 mm as 1 mm"));
  scaleRow->addWidget(m_scale, 1);
  m_scaleShown = new QLabel(this);
  m_scaleShown->setObjectName("secondary");
  options->addWidget(m_fit);
  options->addLayout(scaleRow);
  options->addWidget(m_scaleShown);

  options->addWidget(header(tr("STYLE"), this));
  m_monochrome = new QCheckBox(tr("Monochrome: every colour black"), this);
  m_monochrome->setObjectName("plotMonochrome");
  m_lineweights = new QCheckBox(tr("Plot lineweights"), this);
  m_lineweights->setObjectName("plotLineweights");
  m_lineweights->setChecked(true);
  m_lineweights->setToolTip(tr("Each layer's lineweight on paper (0.25 mm when it has none); off: every line 0.13 mm"));
  m_stampBox = new QCheckBox(tr("Plot stamp"), this);
  m_stampBox->setObjectName("plotStamp");
  m_stampBox->setToolTip(tr("The file, the date and time, the paper and the scale along the bottom margin"));
  options->addWidget(m_monochrome);
  options->addWidget(m_lineweights);
  options->addWidget(m_stampBox);

  options->addWidget(header(tr("OUTPUT"), this));
  auto* outputs = new QButtonGroup(this);
  m_pdf = new QRadioButton(tr("PDF file"), this);
  m_printer = new QRadioButton(tr("Printer"), this);
  m_pdf->setObjectName("plotPdf");
  m_printer->setObjectName("plotPrinter");
  outputs->addButton(m_pdf);
  outputs->addButton(m_printer);
  m_pdf->setChecked(true);
#ifndef OPAD_HAVE_PRINT
  m_printer->setEnabled(false);
  m_printer->setToolTip(tr("This build has no printing support"));
#endif
  auto* outputRow = new QHBoxLayout();
  outputRow->addWidget(m_pdf);
  outputRow->addWidget(m_printer);
  outputRow->addStretch(1);
  options->addLayout(outputRow);
  options->addStretch(1);

  auto* right = new QVBoxLayout();
  right->setSpacing(6);
  m_preview = new PlotPreview(this);
  right->addWidget(m_preview, 1);
  m_info = new QLabel(this);
  m_info->setObjectName("plotInfo");
  m_info->setFont(theme::mono(11));
  m_warning = new QLabel(this);
  m_warning->setObjectName("plotWarning");
  m_warning->setWordWrap(true);
  m_warning->hide();
  right->addWidget(m_info);
  right->addWidget(m_warning);
  body->addLayout(right, 1);

  m_footer = new PanelFooter(this);
  m_footer->setPrimary(tr("Plot…"));
  m_footer->primary()->setDefault(true);  // Enter plots
  outer->addWidget(m_footer);
  connect(m_footer, &PanelFooter::cancelled, this, &QDialog::reject);
  connect(m_footer, &PanelFooter::accepted, this, [this] { plot(); });

  // The last plot's settings.
  QSettings settings;
  m_paper->setCurrentIndex(std::max(0, m_paper->findData(settings.value("plot/paper", int(QPageSize::A4)).toInt())));
  m_margin->setValue(settings.value("plot/margin", 10.0).toDouble());
  m_fit->setChecked(settings.value("plot/fit", true).toBool());
  m_scale->setValue(settings.value("plot/scale", 1.0).toDouble());
  m_monochrome->setChecked(settings.value("plot/monochrome", false).toBool());
  m_lineweights->setChecked(settings.value("plot/lineweights", true).toBool());
  m_stampBox->setChecked(settings.value("plot/stamp", false).toBool());
  (settings.value("plot/output", "pdf").toString() == "printer" && m_printer->isEnabled() ? m_printer : m_pdf)->setChecked(true);
  if (settings.contains("plot/orientation")) m_orientation->setCurrentIndex(settings.value("plot/orientation").toInt());
  m_scale->setEnabled(!m_fit->isChecked());

  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(60);
  connect(&m_refreshTimer, &QTimer::timeout, this, [this] { render(); });
  for (QComboBox* c : {m_paper, m_orientation}) connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
  for (QDoubleSpinBox* b : {m_margin, m_scale}) connect(b, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { refresh(); });
  for (QCheckBox* c : {m_fit, m_monochrome, m_lineweights, m_stampBox}) connect(c, &QCheckBox::toggled, this, [this] { refresh(); });
  for (QRadioButton* r : {m_extents, m_display, m_windowRegion}) connect(r, &QRadioButton::toggled, this, [this] { refresh(); });
  connect(m_windowRegion, &QRadioButton::clicked, this, [this] {
    if (!m_hasWindow) pickWindow();
  });
  connect(m_pick, &QPushButton::clicked, this, [this] { pickWindow(); });
  connect(this, &QDialog::finished, this, [this] { saveSettings(); });
  resize(940, 600);
}

PlotDialog::~PlotDialog() {
  if (m_picking) endPick(false);
  for (Job* job : {m_collectJob, m_previewJob, m_outputJob, m_printerJob})
    if (job) job->cancel();
}

void PlotDialog::start() {
  if (const auto generation = m_services.document()->generation; generation != m_generation) {  // another document: its window is not this one's
    m_generation = generation;
    m_hasWindow = false;
    m_window = {};
    if (m_windowRegion->isChecked()) m_extents->setChecked(true);
  }
  m_picture.reset();
  m_preview->setImage({});
  ++m_stamp;
  open();
  collect();
}

void PlotDialog::collect() {
  if (m_collectJob) m_collectJob->cancel();
  AppDocument* doc = m_services.document();
  auto document = std::make_shared<opad::Document>(doc->doc);
  auto scene = std::make_shared<opad::Scene>(doc->scene);
  const opad::Frame plane = plot::plane(*document, *scene, m_services.viewport()->cameraPlane());
  auto result = std::make_shared<std::shared_ptr<const PlotPicture>>();
  m_info->setText(tr("Gathering the drawing…"));
  m_collectJob = m_services.jobs()->async(tr("Preparing the plot"), [document, scene, plane, result](Progress p) {
    *result = PlotPicture::build(plot::collect(*document, *scene, plane, [p] { return p.cancelled(); }));
  }, [this, result](bool ok, const QString& error) {
    m_collectJob = nullptr;
    if (!ok) return m_info->setText(error == "cancelled" ? QString() : i18n::t(error));
    m_picture = *result;
    // The paper turned as the drawing is, the first time.
    if (!QSettings().contains("plot/orientation")) m_orientation->setCurrentIndex(m_picture->sheet.y1 - m_picture->sheet.y0 > m_picture->sheet.x1 - m_picture->sheet.x0 ? 1 : 0);
    refresh();
  });
}

plot::Area PlotDialog::displayArea() {
  Viewport* v = m_services.viewport();
  plot::Area a{1e300, 1e300, -1e300, -1e300};
  for (const QPointF& corner : {QPointF(0, 0), QPointF(v->width(), 0), QPointF(0, v->height()), QPointF(v->width(), v->height())}) {
    double u, w;
    if (!planeAt(corner, u, w)) continue;
    a.x0 = std::min(a.x0, u), a.y0 = std::min(a.y0, w), a.x1 = std::max(a.x1, u), a.y1 = std::max(a.y1, w);
  }
  return a.x0 > a.x1 ? plot::Area{} : a;
}

bool PlotDialog::planeAt(const QPointF& widgetPos, double& u, double& v) {
  const opad::Frame plane = m_picture ? m_picture->sheet.plane : m_services.viewport()->cameraPlane();
  return m_services.viewport()->planePoint(widgetPos, plane, u, v);
}

plot::Settings PlotDialog::settings() const {
  plot::Settings s;
  s.region = m_display->isChecked() ? plot::Region::Display : m_windowRegion->isChecked() && m_hasWindow ? plot::Region::Window : plot::Region::Extents;
  s.display = const_cast<PlotDialog*>(this)->displayArea();
  s.window = m_window;
  const QSizeF size = QPageSize::size(QPageSize::PageSizeId(m_paper->currentData().toInt()), QPageSize::Millimeter);
  const bool landscape = m_orientation->currentIndex() == 0;
  s.paperWidth = landscape ? std::max(size.width(), size.height()) : std::min(size.width(), size.height());
  s.paperHeight = landscape ? std::min(size.width(), size.height()) : std::max(size.width(), size.height());
  s.margin = m_margin->value();
  s.fit = m_fit->isChecked();
  s.scale = 1 / m_scale->value();
  s.monochrome = m_monochrome->isChecked();
  s.lineweights = m_lineweights->isChecked();
  if (m_stampBox->isChecked()) {  // what was plotted, when, on what and at what scale
    const AppDocument* doc = m_services.document();
    QString name = QFileInfo(doc->browse ? doc->viewing : doc->path()).fileName();
    if (name.isEmpty()) name = doc->nodeName(doc->scene.roots.empty() ? std::string() : doc->scene.roots.front());
    const plot::Placement p = m_picture ? plot::place(m_picture->sheet, s) : plot::Placement{};
    s.stamp = QString("%1  ·  %2  ·  %3 %4  ·  %5")
                  .arg(name, QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat), m_paper->currentText(), m_orientation->currentText(),
                       QString::fromStdString(plot::scaleText(p.scale)))
                  .toStdString();
  }
  return s;
}

plot::Placement PlotDialog::placement() const { return m_picture ? plot::place(m_picture->sheet, settings()) : plot::Placement{}; }

QImage PlotDialog::previewImage() const { return m_preview->image(); }

void PlotDialog::refresh() {
  ++m_stamp;
  m_scale->setEnabled(!m_fit->isChecked());
  const plot::Settings s = settings();
  const plot::Placement p = placement();
  m_scaleShown->setText(p.valid() ? tr("Scale %1").arg(QString::fromStdString(plot::scaleText(p.scale))) : QString());
  if (m_picture)
    m_info->setText(m_picture->sheet.empty() ? tr("Nothing to plot: no drawing is shown on a plotted layer.")
                                             : tr("%1 × %2 mm · scale %3 · %4 objects").arg(s.paperWidth, 0, 'f', 0).arg(s.paperHeight, 0, 'f', 0)
                                                   .arg(QString::fromStdString(plot::scaleText(p.scale))).arg(m_picture->sheet.items.size()));
  m_warning->setText(tr("At this scale the plot area does not fit the paper: it is cut at the margins."));
  m_warning->setStyleSheet(QString("color: %1;").arg(theme::current().warning.name()));
  m_warning->setVisible(p.valid() && p.clipped);
  m_footer->setPrimaryEnabled(m_picture && !m_picture->sheet.empty() && p.valid());
  m_refreshTimer.start();
}

// The preview: the page at the widget's size, painted on a worker; a newer one supersedes it.
void PlotDialog::render() {
  if (!m_picture) return;
  if (m_previewJob) m_previewJob->cancel();
  const plot::Settings s = settings();
  const plot::Placement p = plot::place(m_picture->sheet, s);
  const QSize size = m_preview->pageSize(s.paperWidth, s.paperHeight);
  const qreal dpr = m_preview->devicePixelRatioF();
  auto picture = m_picture;
  auto image = std::make_shared<QImage>();
  const int stamp = m_stamp;
  m_previewJob = m_services.jobs()->async(tr("Plot preview"), [picture, s, p, size, dpr, image](Progress) {
    QImage page(size, QImage::Format_ARGB32_Premultiplied);
    page.fill(Qt::white);
    QPainter painter(&page);
    paintPlot(painter, *picture, s, p, size.width() / s.paperWidth);
    painter.end();
    page.setDevicePixelRatio(dpr);
    *image = page;
  }, [this, image, stamp](bool ok, const QString&) {
    m_previewJob = nullptr;
    if (!ok) return;
    m_preview->setImage(*image);
    m_previewStamp = stamp;
    emit previewUpdated();
  });
}

void PlotDialog::resizeEvent(QResizeEvent* event) {
  QDialog::resizeEvent(event);
  if (m_picture) m_refreshTimer.start();
}

void PlotDialog::saveSettings() const {
  QSettings settings;
  settings.setValue("plot/paper", m_paper->currentData().toInt());
  settings.setValue("plot/orientation", m_orientation->currentIndex());
  settings.setValue("plot/margin", m_margin->value());
  settings.setValue("plot/fit", m_fit->isChecked());
  settings.setValue("plot/scale", m_scale->value());
  settings.setValue("plot/monochrome", m_monochrome->isChecked());
  settings.setValue("plot/lineweights", m_lineweights->isChecked());
  settings.setValue("plot/stamp", m_stampBox->isChecked());
  settings.setValue("plot/output", m_printer->isChecked() ? "printer" : "pdf");
}

// ---------------------------------------------------------------- output
void PlotDialog::plot() {
  if (!m_picture || !placement().valid()) return;
  saveSettings();
  if (m_pdf->isChecked()) {
    const QString name = m_services.document()->nodeName(m_services.document()->scene.roots.empty() ? std::string() : m_services.document()->scene.roots.front());
    const QString dir = QSettings().value("plot/folder", QDir::homePath()).toString();
    const QString path = QFileDialog::getSaveFileName(this, tr("Plot to PDF"), QDir(dir).filePath((name.isEmpty() ? QString("plot") : name) + ".pdf"), tr("PDF (*.pdf)"));
    if (path.isEmpty()) return;
    QSettings().setValue("plot/folder", QFileInfo(path).absolutePath());
    plotToPdf(path);
    return;
  }
#ifdef OPAD_HAVE_PRINT
  withPrinter([this](std::shared_ptr<QPrinter> printer) {
    if (!printer || !isVisible()) return;
    QPrintDialog dialog(printer.get(), this);
    dialog.setWindowTitle(tr("Plot to a printer"));
    if (dialog.exec() == QDialog::Accepted) plotToPrinter(printer);
  });
#endif
}

void PlotDialog::withPrinter(std::function<void(std::shared_ptr<QPrinter>)> then) {
#ifdef OPAD_HAVE_PRINT
  const QPageLayout layout = pageLayout(settings());
  auto printer = std::make_shared<std::shared_ptr<QPrinter>>();
  m_footer->setPrimaryEnabled(false);
  if (m_printerJob) return;
  m_printerJob = m_services.jobs()->async(tr("Finding the printer"), [layout, printer](Progress) {
    *printer = std::make_shared<QPrinter>(QPrinter::HighResolution);
    (*printer)->setPageLayout(layout);
    (*printer)->setFullPage(true);
  }, [this, printer, then](bool ok, const QString& error) {
    m_printerJob = nullptr;
    if (error == "cancelled") return;  // the dialog went
    refresh();
    then(ok ? *printer : nullptr);
  });
#else
  then(nullptr);
#endif
}

void PlotDialog::plotToPdf(const QString& path) {
  if (!m_picture || m_outputJob) return;
  const plot::Settings s = settings();
  const plot::Placement p = plot::place(m_picture->sheet, s);
  auto picture = m_picture;
  const QString title = m_services.document()->nodeName(m_services.document()->scene.roots.empty() ? std::string() : m_services.document()->scene.roots.front());
  m_outputJob = m_services.jobs()->async(tr("Plotting to PDF"), [picture, s, p, path, title](Progress) {
    QPdfWriter writer(path);
    writer.setCreator(QString("OPAD %1").arg(OPAD_VERSION));
    writer.setTitle(title);
    writer.setResolution(1200);
    writer.setPageLayout(pageLayout(s));
    QPainter painter;
    if (!painter.begin(&writer)) throw opad::Error("cannot write " + path.toStdString());
    paintPlot(painter, *picture, s, p, writer.resolution() / 25.4);
    painter.end();
  }, [this, path](bool ok, const QString& error) {
    m_outputJob = nullptr;
    if (error == "cancelled") return;  // the dialog went
    emit plotted(ok, error);
    if (!ok) return m_services.toast(tr("The plot could not be written: %1").arg(i18n::t(error)), QString(), {}, 6000);
    m_services.toast(tr("Plotted to %1").arg(QFileInfo(path).fileName()), tr("Show"), [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
    if (isVisible()) accept();
  });
}

void PlotDialog::plotToPrinter(std::shared_ptr<QPrinter> printer) {
#ifdef OPAD_HAVE_PRINT
  if (!m_picture || m_outputJob) return;
  plot::Settings s = settings();
  // The paper the print dialog ended on (the user may have chosen another): the plot is placed on that one.
  printer->setFullPage(true);
  const QRectF paper = printer->pageLayout().fullRect(QPageLayout::Millimeter);
  if (paper.width() > 0 && paper.height() > 0) s.paperWidth = paper.width(), s.paperHeight = paper.height();
  const plot::Placement p = plot::place(m_picture->sheet, s);
  auto picture = m_picture;
  m_outputJob = m_services.jobs()->async(tr("Plotting"), [picture, s, p, printer](Progress) {
    QPainter painter;
    if (!painter.begin(printer.get())) throw opad::Error("the printer did not start the job");
    paintPlot(painter, *picture, s, p, printer->resolution() / 25.4);
    painter.end();
  }, [this, printer](bool ok, const QString& error) {
    m_outputJob = nullptr;
    if (error == "cancelled") return;  // the dialog went
    emit plotted(ok, error);
    if (!ok) return m_services.toast(tr("The plot could not be printed: %1").arg(i18n::t(error)), QString(), {}, 6000);
    m_services.toast(printer->outputFileName().isEmpty() ? tr("Plot sent to %1").arg(printer->printerName()) : tr("Plotted to %1").arg(QFileInfo(printer->outputFileName()).fileName()));
    if (isVisible()) accept();
  });
#else
  emit plotted(false, tr("This build has no printing support"));
#endif
}

// ---------------------------------------------------------------- the window, picked in the view
void PlotDialog::pickWindow() {
  if (m_picking || !m_picture) return;
  m_picking = true;
  m_corners = 0;
  hide();  // the view takes the clicks; the dialog comes back with the window
  Viewport* v = m_services.viewport();
  m_prompt = new PromptBar(v);
  m_prompt->setObjectName("plotPrompt");
  m_prompt->setAttribute(Qt::WA_NativeWindow);
  m_prompt->set("plot", tr("Plot window"), {{tr("Pick the first corner"), {}}, {tr("Pick the opposite corner"), {}}}, tr("Esc back to Plot"));
  m_prompt->move(std::max(8, (v->width() - m_prompt->width()) / 2), 44);
  m_prompt->show();
  m_snapBefore = int(v->snapPicks());
  v->setSnapPicks(Viewport::SnapPicks::Always);  // the snap's marker shows where a corner would land
  qApp->installEventFilter(this);
}

void PlotDialog::endPick(bool done) {
  if (!m_picking) return;
  m_picking = false;
  qApp->removeEventFilter(this);
  m_services.viewport()->setSnapPicks(Viewport::SnapPicks(m_snapBefore));
  if (!m_rubber.IsNull()) {
    m_services.viewport()->removeOverlay(m_rubber);
    m_rubber.Nullify();
  }
  delete m_prompt.data();
  if (done) {
    m_hasWindow = true;
    m_windowRegion->setChecked(true);
  } else if (!m_hasWindow) {
    m_extents->setChecked(true);
  }
  open();
  refresh();
}

bool PlotDialog::cornerAt(const QPointF& widgetPos, double& u, double& v) {
  Viewport* view = m_services.viewport();
  opad::Vec3 snapped;
  if (!view->objectSnap() || !view->snapAt(widgetPos, snapped)) return planeAt(widgetPos, u, v);
  m_picture->sheet.plane.to_local(snapped, u, v);
  return true;
}

void PlotDialog::pickCorner(const QPointF& widgetPos) {
  double u, w;
  if (!cornerAt(widgetPos, u, w)) return;
  if (m_corners++ == 0) {
    m_cornerU = u, m_cornerV = w;
    m_prompt->set("plot", tr("Plot window"), {{tr("Pick the first corner"), tr("First corner")}, {tr("Pick the opposite corner"), {}}}, tr("Esc back to Plot"));
    return;
  }
  m_window = {std::min(m_cornerU, u), std::min(m_cornerV, w), std::max(m_cornerU, u), std::max(m_cornerV, w)};
  if (m_window.width() <= 0 || m_window.height() <= 0) {  // the same point twice: pick the second again
    m_corners = 1;
    return;
  }
  endPick(true);
}

void PlotDialog::showRubber(const QPointF& widgetPos) {
  double u, w;
  if (m_corners != 1 || !cornerAt(widgetPos, u, w)) return;  // from the snapped point, where the click will put it
  const opad::Frame& f = m_picture->sheet.plane;
  auto at = [&f](double x, double y) {
    const opad::Vec3 p = f.to_world(x, y);
    return gp_Pnt(p[0], p[1], p[2]);
  };
  if (std::abs(u - m_cornerU) < 1e-12 || std::abs(w - m_cornerV) < 1e-12) return;
  m_rubberCorner = {u, w};
  BRepBuilderAPI_MakePolygon outline(at(m_cornerU, m_cornerV), at(u, m_cornerV), at(u, w), at(m_cornerU, w), true);
  Viewport* v = m_services.viewport();
  if (m_rubber.IsNull()) {
    m_rubber = new AIS_Shape(outline.Shape());
    const QColor c = theme::current().sel;
    m_rubber->Attributes()->SetWireAspect(new Prs3d_LineAspect(Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB), Aspect_TOL_DASH, v->lineWidth(1.5)));
    v->showOverlay(m_rubber);
  } else {
    m_rubber->SetShape(outline.Shape());
    v->updateOverlay(m_rubber);
  }
}

bool PlotDialog::eventFilter(QObject* watched, QEvent* event) {
  if (!m_picking) return QDialog::eventFilter(watched, event);
  if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
    event->accept();
    if (event->type() == QEvent::KeyPress) endPick(false);
    return true;
  }
  Viewport* v = m_services.viewport();
  auto* widget = qobject_cast<QWidget*>(watched);
  if (!widget || (widget != v && !v->isAncestorOf(widget))) return false;
  auto* mouse = static_cast<QMouseEvent*>(event);
  if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) && mouse->button() == Qt::LeftButton) {
    if (widget == v) pickCorner(mouse->position());
    return true;
  }
  if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) return true;
  if (event->type() == QEvent::MouseMove && widget == v) showRubber(mouse->position());
  return false;
}
