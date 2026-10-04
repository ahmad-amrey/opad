#include "TemplateFields.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "AppDocument.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "opad/drawing/paint.hpp"
#include "opad/drawing/sheet.hpp"

namespace {
double r2(double v) { return std::round(v * 100) / 100; }

// The fields a title block can have filled in, by key, as the user reads them.
const std::vector<std::pair<const char*, QString>>& keys() {
  static const std::vector<std::pair<const char*, QString>> list = {
      {"title", TemplateFieldsDialog::tr("Title")},          {"number", TemplateFieldsDialog::tr("Number")},
      {"revision", TemplateFieldsDialog::tr("Revision")},    {"date", TemplateFieldsDialog::tr("Date")},
      {"scale", TemplateFieldsDialog::tr("Scale")},          {"sheet", TemplateFieldsDialog::tr("Sheet")},
      {"size", TemplateFieldsDialog::tr("Size")},            {"material", TemplateFieldsDialog::tr("Material")},
      {"mass", TemplateFieldsDialog::tr("Mass")},            {"author", TemplateFieldsDialog::tr("Drawn by")},
      {"checked", TemplateFieldsDialog::tr("Checked by")},   {"approved", TemplateFieldsDialog::tr("Approved by")},
      {"owner", TemplateFieldsDialog::tr("Owner")},          {"project", TemplateFieldsDialog::tr("Project")},
      {"description", TemplateFieldsDialog::tr("Description")}, {"status", TemplateFieldsDialog::tr("Status")},
      {"doctype", TemplateFieldsDialog::tr("Document type")}, {"projection", TemplateFieldsDialog::tr("Projection")},
      {"tolerance", TemplateFieldsDialog::tr("General tolerances")}};
  return list;
}

QString shownKey(const std::string& key) {
  for (const auto& [k, label] : keys())
    if (key == k) return label;
  return QString::fromStdString(key);
}
}  // namespace

// ---------------------------------------------------------------- view
TemplateFieldsView::TemplateFieldsView(QWidget* parent) : QGraphicsView(parent) {
  setObjectName("templateFieldsView");
  setLayoutDirection(Qt::LeftToRight);  // paper is never mirrored
  setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFrameShape(QFrame::NoFrame);
  setMouseTracking(true);
}

QRectF TemplateFieldsView::grip(const QRectF& box) const {
  const double k = 7 / std::max(transform().m11(), 1e-6);  // 7 px
  return QRectF(box.bottomRight() - QPointF(k, k), QSizeF(2 * k, 2 * k));
}

void TemplateFieldsView::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  emit pressed(mapToScene(e->position().toPoint()), false);
}

void TemplateFieldsView::mouseMoveEvent(QMouseEvent* e) { emit dragged(mapToScene(e->position().toPoint())); }

void TemplateFieldsView::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) emit released(mapToScene(e->position().toPoint()));
}

void TemplateFieldsView::resizeEvent(QResizeEvent* e) {
  QGraphicsView::resizeEvent(e);
  if (scene()) fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
}

void TemplateFieldsView::drawBackground(QPainter* p, const QRectF& rect) { p->fillRect(rect, theme::current().vp); }

// ---------------------------------------------------------------- dialog
TemplateFieldsDialog::TemplateFieldsDialog(AppDocument* doc, JobRunner* jobs, const std::string& sheet, QWidget* parent)
    : QDialog(parent), m_doc(doc), m_sheet(sheet) {
  setObjectName("templateFieldsDialog");
  setWindowTitle(tr("Title block fields"));
  const opad::Sheet* s = doc->scene.sheet(sheet);
  if (!s) throw opad::Error("That sheet is gone.");
  m_template = s->def.value("template", opad::json());
  if (!m_template.is_object()) throw opad::Error("This sheet has no template: choose one in Sheet properties first.");
  m_fields = m_template.value("fields", opad::json::array());
  if (!m_fields.is_array()) m_fields = opad::json::array();
  m_w = s->width, m_h = s->height;

  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(16, 16, 16, 16);
  h->setSpacing(14);
  m_view = new TemplateFieldsView(this);
  m_view->paperH = m_h;
  auto* scene = new QGraphicsScene(this);
  scene->setSceneRect(-0.02 * m_w, -0.02 * m_h, 1.04 * m_w, 1.04 * m_h);
  m_view->setScene(scene);
  scene->addRect(QRectF(0, 0, m_w, m_h), Qt::NoPen, Qt::white);
  m_view->setMinimumSize(640, static_cast<int>(640 * m_h / m_w));
  h->addWidget(m_view, 1);
  // The built-in block's cells, faint: where its own fields are.
  if (m_template.contains("title_block") && m_template["title_block"].is_object() && m_template.contains("frame")) {
    const opad::json tb = m_template["title_block"], f = m_template["frame"];
    const double ox = m_w - f.value("right", 10.0) - tb.value("w", 0.0), oy = f.value("bottom", 10.0);
    for (const auto& c : tb.value("fields", opad::json::array())) {
      const opad::json r = c.value("rect", opad::json());
      if (!r.is_array() || r.size() != 4) continue;
      auto* cell = scene->addRect(QRectF(ox + r[0].get<double>(), m_h - oy - r[1].get<double>() - r[3].get<double>(), r[2].get<double>(), r[3].get<double>()),
                                  QPen(QColor(0, 0, 0, 0)), QColor(120, 120, 120, 28));
      cell->setZValue(1);
    }
  }

  auto* side = new QVBoxLayout();
  side->setSpacing(8);
  auto* hint = new QLabel(tr("Drag on the paper to add a field, drag a field to move it, its corner to resize it. Del removes the chosen one."), this);
  hint->setObjectName("secondary");
  hint->setWordWrap(true);
  hint->setMaximumWidth(260);
  side->addWidget(hint);
  m_list = new QListWidget(this);
  m_list->setObjectName("template.fields");
  m_list->setMaximumWidth(260);
  side->addWidget(m_list, 1);
  auto* form = new QFormLayout();
  m_key = new QComboBox(this);
  m_key->setObjectName("template.key");
  m_key->setEditable(true);
  for (const auto& [k, label] : keys()) m_key->addItem(label, QString::fromLatin1(k));
  m_key->setCurrentIndex(m_key->findData("title"));
  form->addRow(tr("Field"), m_key);
  m_height = new QDoubleSpinBox(this);
  m_height->setRange(1, 20);
  m_height->setDecimals(1);
  m_height->setSingleStep(0.5);
  m_height->setSuffix(" mm");
  m_height->setValue(3.5);
  form->addRow(tr("Text height"), m_height);
  m_align = new QComboBox(this);
  m_align->addItem(tr("Left"), "left");
  m_align->addItem(tr("Centred"), "center");
  m_align->addItem(tr("Right"), "right");
  form->addRow(tr("Alignment"), m_align);
  side->addLayout(form);
  auto* remove = new QPushButton(tr("Remove field"), this);
  side->addWidget(remove);
  side->addStretch();
  auto* buttons = new QHBoxLayout();
  buttons->addStretch();
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  auto* apply = new QPushButton(tr("Apply"), this);
  apply->setObjectName("primary");
  apply->setDefault(true);
  buttons->addWidget(cancel);
  buttons->addWidget(apply);
  side->addLayout(buttons);
  h->addLayout(side);

  connect(m_view, &TemplateFieldsView::pressed, this, &TemplateFieldsDialog::press);
  connect(m_view, &TemplateFieldsView::dragged, this, &TemplateFieldsDialog::drag);
  connect(m_view, &TemplateFieldsView::released, this, &TemplateFieldsDialog::release);
  connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
    if (!m_filling) select(row);
  });
  connect(m_key, &QComboBox::currentTextChanged, this, [this] { edited(); });
  connect(m_height, &QDoubleSpinBox::valueChanged, this, [this] { edited(); });
  connect(m_align, &QComboBox::currentIndexChanged, this, [this] { edited(); });
  connect(remove, &QPushButton::clicked, this, [this] {
    if (m_selected < 0) return;
    m_fields.erase(static_cast<size_t>(m_selected));
    m_selected = -1;
    rebuild();
  });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(apply, &QPushButton::clicked, this, &QDialog::accept);
  rebuild();

  m_bare = *s;  // the template as the sheet draws it, without these fields' values
  m_bare.def["template"].erase("fields");
  m_jobs = jobs;
  drawTemplate();
  resize(1040, 620);
}

// Painted on a worker; while something else reads the document (the sheet canvas), again a moment later.
void TemplateFieldsDialog::drawTemplate() {
  auto image = std::make_shared<QImage>();
  const opad::Sheet bare = m_bare;
  const double w = m_w, hgt = m_h;
  QPointer<TemplateFieldsDialog> self(this);
  Job* job = m_doc->readAsync(
      m_jobs, tr("Drawing the template"),
      [bare, image, w, hgt](const opad::Document& d, const opad::Scene& sc, Progress) {
        opad::drawing::Display display;
        opad::drawing::draw_paper(display, d, sc, bare);
        const double k = 2400 / std::max(w, hgt);
        QImage img(static_cast<int>(std::ceil(w * k)), static_cast<int>(std::ceil(hgt * k)), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        opad::drawing::paint(p, display, {0, 0, w, hgt}, QRectF(0, 0, img.width(), img.height()), 1.0);
        p.end();
        *image = std::move(img);
      },
      [self, image, w](bool ok, const QString&) {
        if (!self || !ok || image->isNull()) return;
        self->m_picture = self->m_view->scene()->addPixmap(QPixmap::fromImage(*image));
        self->m_picture->setTransformationMode(Qt::SmoothTransformation);
        self->m_picture->setScale(w / image->width());
        self->m_picture->setZValue(0.5);
        self->m_pictured = true;
      });
  if (!job) QTimer::singleShot(100, this, &TemplateFieldsDialog::drawTemplate);
}

QPoint TemplateFieldsDialog::at(opad::drawing::Vec2 paper) const { return m_view->mapFromScene(QPointF(paper[0], m_h - paper[1])); }

QRectF TemplateFieldsDialog::box(const opad::json& f) const {
  const opad::json rect = f.value("rect", opad::json());
  if (rect.is_array() && rect.size() == 4)
    return QRectF(rect[0].get<double>(), m_h - rect[1].get<double>() - rect[3].get<double>(), rect[2].get<double>(), rect[3].get<double>());
  // A text anchor (a template file's attribute or placeholder): about the box its value takes.
  const opad::json p = f.value("at", opad::json::array({0, 0}));
  const double th = f.value("height", 2.5), x = p[0].get<double>(), y = p[1].get<double>();
  const double w = f.value("w", 0.0) > 0 ? f["w"].get<double>() : std::max(20.0, 0.62 * th * 12);
  const std::string align = f.value("align", "left"), valign = f.value("valign", "baseline");
  const double x0 = align == "center" ? x - w / 2 : align == "right" ? x - w : x;
  const double y0 = valign == "top" ? y - 1.4 * th : valign == "middle" ? y - 0.7 * th : valign == "bottom" ? y : y - 0.3 * th;
  return QRectF(x0, m_h - y0 - 1.4 * th, w, 1.4 * th);
}

void TemplateFieldsDialog::rebuild() {
  for (QGraphicsRectItem* b : m_boxes) delete b;
  m_boxes.clear();
  const Tokens& t = theme::current();
  for (size_t i = 0; i < m_fields.size(); ++i) {
    const bool chosen = static_cast<int>(i) == m_selected;
    QPen pen(chosen ? t.sel : QColor(40, 110, 200), chosen ? 2 : 1.2, chosen ? Qt::SolidLine : Qt::DashLine);
    pen.setCosmetic(true);
    auto* item = m_view->scene()->addRect(box(m_fields[i]), pen, QColor(40, 110, 200, chosen ? 50 : 22));
    item->setZValue(2);
    auto* label = new QGraphicsSimpleTextItem(shownKey(m_fields[i].value("key", "")), item);
    label->setFlag(QGraphicsItem::ItemIgnoresTransformations);
    label->setBrush(QColor(20, 70, 150));
    label->setFont(theme::ui(9));
    label->setPos(item->rect().topLeft());
    m_boxes.push_back(item);
  }
  m_filling = true;
  m_list->clear();
  for (const auto& f : m_fields) m_list->addItem(shownKey(f.value("key", "")));
  m_list->setCurrentRow(m_selected);
  m_filling = false;
}

void TemplateFieldsDialog::select(int i) {
  m_selected = i >= 0 && i < count() ? i : -1;
  if (m_selected >= 0) {
    const opad::json& f = m_fields[static_cast<size_t>(m_selected)];
    m_filling = true;
    const QString key = QString::fromStdString(f.value("key", ""));
    const int at = m_key->findData(key);
    if (at >= 0) m_key->setCurrentIndex(at);
    else m_key->setEditText(key);
    m_height->setValue(f.value("height", 3.5));
    m_align->setCurrentIndex(std::max(0, m_align->findData(QString::fromStdString(f.value("align", "left")))));
    m_filling = false;
  }
  rebuild();
}

void TemplateFieldsDialog::edited() {
  if (m_filling || m_selected < 0) return;  // nothing chosen: what the next box gets
  opad::json& f = m_fields[static_cast<size_t>(m_selected)];
  const int at = m_key->findText(m_key->currentText());
  std::string key = at >= 0 ? m_key->itemData(at).toString().toStdString() : m_key->currentText().trimmed().toStdString();
  if (key.empty()) return;
  f["key"] = key;
  f["height"] = r2(m_height->value());
  f["align"] = m_align->currentData().toString().toStdString();
  rebuild();
}

void TemplateFieldsDialog::press(const QPointF& at, bool) {
  m_start = at;
  m_mode = Mode::Create;
  for (int i = count() - 1; i >= 0; --i) {
    const QRectF b = m_boxes[static_cast<size_t>(i)]->rect();
    if (m_view->grip(b).contains(at)) m_mode = Mode::Resize;
    else if (b.contains(at)) m_mode = Mode::Move;
    else continue;
    m_origin = b;
    select(i);
    return;
  }
  select(-1);
}

void TemplateFieldsDialog::drag(const QPointF& at) {
  if (m_mode == Mode::None) {
    bool grip = false;
    for (QGraphicsRectItem* b : m_boxes) grip = grip || m_view->grip(b->rect()).contains(at);
    m_view->viewport()->setCursor(grip ? Qt::SizeFDiagCursor : Qt::CrossCursor);
    return;
  }
  const QPointF d = at - m_start;
  if (m_mode == Mode::Create) {
    if (!m_rubber) {
      QPen pen(theme::current().sel, 1.5, Qt::DashLine);
      pen.setCosmetic(true);
      m_rubber = m_view->scene()->addRect(QRectF(), pen, QColor(40, 110, 200, 30));
      m_rubber->setZValue(3);
    }
    m_rubber->setRect(QRectF(m_start, at).normalized());
  } else if (m_selected >= 0) {
    QRectF r = m_origin;
    if (m_mode == Mode::Move) r.translate(d);
    else r.setBottomRight(QPointF(std::max(m_origin.left() + 2, m_origin.right() + d.x()), std::max(m_origin.top() + 2, m_origin.bottom() + d.y())));
    m_boxes[static_cast<size_t>(m_selected)]->setRect(r);
  }
}

void TemplateFieldsDialog::release(const QPointF& at) {
  const Mode mode = std::exchange(m_mode, Mode::None);
  const auto paper = [&](const QRectF& r) {  // scene -> paper mm [x, y, w, h], to 0.01 mm
    return opad::json::array({r2(r.left()), r2(m_h - r.bottom()), r2(r.width()), r2(r.height())});
  };
  if (mode == Mode::Create) {
    if (m_rubber) {
      delete m_rubber;
      m_rubber = nullptr;
    }
    const QRectF r = QRectF(m_start, at).normalized();
    if (r.width() < 2 || r.height() < 2) return;  // a click
    const int i = m_key->findText(m_key->currentText());
    std::string key = i >= 0 ? m_key->itemData(i).toString().toStdString() : m_key->currentText().trimmed().toStdString();
    if (key.empty()) key = "title";
    m_fields.push_back({{"key", key}, {"rect", paper(r)}, {"height", r2(m_height->value())}, {"align", m_align->currentData().toString().toStdString()}, {"valign", "middle"}});
    select(count() - 1);
  } else if ((mode == Mode::Move || mode == Mode::Resize) && m_selected >= 0 && at != m_start) {
    opad::json& f = m_fields[static_cast<size_t>(m_selected)];
    f["rect"] = paper(m_boxes[static_cast<size_t>(m_selected)]->rect());
    if (f.contains("at")) {  // a text anchor becomes a box: its value fitted in, centred upright
      for (const char* k : {"at", "angle", "w"}) f.erase(k);
      f["valign"] = "middle";
    }
    rebuild();
  }
}

void TemplateFieldsDialog::keyPressEvent(QKeyEvent* e) {
  if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && m_selected >= 0 && !m_key->hasFocus()) {
    m_fields.erase(static_cast<size_t>(m_selected));
    m_selected = -1;
    rebuild();
    return;
  }
  QDialog::keyPressEvent(e);
}

opad::json TemplateFieldsDialog::change() const {
  if (m_fields == m_template.value("fields", opad::json::array())) return nullptr;
  opad::json t = m_template;
  if (m_fields.empty()) t.erase("fields");
  else t["fields"] = m_fields;
  return {{"template", t}};
}
