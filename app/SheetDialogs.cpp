#include "SheetDialogs.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

#include "AppDocument.hpp"
#include "I18n.hpp"
#include "Theme.hpp"
#include "opad/drawing/paint.hpp"
#include "opad/drawing/sheet.hpp"

namespace {
struct Choice {
  const char* label;
  const char* standard;
  const char* paper;
};
const std::vector<Choice>& choices() {
  static const std::vector<Choice> list = {{"ISO A4", "iso", "A4"},       {"ISO A3", "iso", "A3"},       {"ISO A2", "iso", "A2"},
                                           {"ISO A1", "iso", "A1"},       {"ISO A0", "iso", "A0"},       {"ANSI A", "ansi", "ANSI-A"},
                                           {"ANSI B", "ansi", "ANSI-B"}, {"ANSI C", "ansi", "ANSI-C"}, {"ANSI D", "ansi", "ANSI-D"},
                                           {"ANSI E", "ansi", "ANSI-E"}};
  return list;
}

const QStringList& scales() {
  static const QStringList list = {"10:1", "5:1", "2:1", "1:1", "1:2", "1:5", "1:10", "1:20", "1:50", "1:100"};
  return list;
}

// A template's frame and title block as a small picture (no geometry: cheap enough here).
QImage thumbnail(const std::string& standard, double w, double h, int width) {
  static const opad::Document empty = opad::Document::create();
  static const opad::Scene none;
  opad::Sheet s;
  s.width = w, s.height = h;
  s.def = {{"size", {{"w", w}, {"h", h}}}, {"template", opad::drawing::make_template(standard, w, h)}};
  opad::drawing::Display d;
  opad::drawing::draw_paper(d, empty, none, s);
  d.prims.erase(std::remove_if(d.prims.begin(), d.prims.end(), [](const auto& p) { return p.kind == opad::drawing::Prim::Kind::Text; }), d.prims.end());  // too small to read: lines only, fast
  const int height = static_cast<int>(std::lround(width * h / w));
  QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::white);
  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  opad::drawing::paint(p, d, {0, 0, w, h}, QRectF(0, 0, width, height), 0.6);
  p.setPen(QColor(0, 0, 0, 60));
  p.drawRect(QRectF(0.5, 0.5, width - 1, height - 1));
  return img;
}

QString text(const opad::json& j, const char* key) { return QString::fromStdString(j.value(key, "")); }
}  // namespace

// ---------------------------------------------------------------- new drawing
NewDrawingDialog::NewDrawingDialog(AppDocument* doc, const std::vector<std::string>& nodes, QWidget* parent) : QDialog(parent), m_doc(doc), m_nodes(nodes) {
  setObjectName("newDrawingDialog");
  setWindowTitle(tr("New drawing"));
  QSettings settings;
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(10);
  auto* heading = new QLabel(tr("Template"), this);
  heading->setFont(theme::ui(13, QFont::DemiBold));
  v->addWidget(heading);
  m_templates = new QListWidget(this);
  m_templates->setObjectName("drawing.templates");
  m_templates->setViewMode(QListView::IconMode);
  m_templates->setFlow(QListView::LeftToRight);
  m_templates->setWrapping(true);
  m_templates->setMovement(QListView::Static);
  m_templates->setResizeMode(QListView::Adjust);
  m_templates->setIconSize(QSize(120, 120));
  m_templates->setGridSize(QSize(140, 150));
  m_templates->setMinimumSize(5 * 140 + 24, 2 * 150 + 12);
  m_templates->setSpacing(4);
  for (const auto& c : choices()) {
    auto* item = new QListWidgetItem(QString::fromLatin1(c.label), m_templates);
    item->setData(Qt::UserRole, QString::fromLatin1(c.paper));
    item->setData(Qt::UserRole + 1, QString::fromLatin1(c.standard));
  }
  const QString last = settings.value("drawings/paper", "A3").toString();
  for (int i = 0; i < m_templates->count(); ++i)
    if (m_templates->item(i)->data(Qt::UserRole).toString() == last) m_templates->setCurrentRow(i);
  if (m_templates->currentRow() < 0) m_templates->setCurrentRow(1);
  v->addWidget(m_templates);

  auto* columns = new QHBoxLayout();
  columns->setSpacing(28);
  auto* left = new QFormLayout();
  m_name = new QLineEdit(this);
  std::set<std::string> drawings;
  for (const auto& s : doc->scene.sheets) drawings.insert(s.drawing);
  int n = static_cast<int>(drawings.size()) + 1;
  while (drawings.count(tr("Drawing %1").arg(n).toStdString())) ++n;
  m_name->setText(tr("Drawing %1").arg(n));
  left->addRow(tr("Drawing"), m_name);
  auto* orientation = new QHBoxLayout();
  m_landscape = new QRadioButton(tr("Landscape"), this);
  m_portrait = new QRadioButton(tr("Portrait"), this);
  (settings.value("drawings/orientation", "landscape").toString() == "portrait" ? m_portrait : m_landscape)->setChecked(true);
  orientation->addWidget(m_landscape);
  orientation->addWidget(m_portrait);
  orientation->addStretch();
  left->addRow(tr("Paper"), orientation);
  m_projection = new QComboBox(this);
  m_projection->setObjectName("drawing.projection");
  m_projection->addItem(tr("First angle (ISO)"), "first");
  m_projection->addItem(tr("Third angle (ASME)"), "third");
  left->addRow(tr("Projection"), m_projection);
  m_scale = new QComboBox(this);
  m_scale->setObjectName("drawing.scale");
  m_scale->addItem(tr("Auto: the largest that fits"), "auto");
  for (const auto& s : scales()) m_scale->addItem(s, s);
  left->addRow(tr("Drawing scale"), m_scale);
  auto* views = new QHBoxLayout();
  m_base = new QComboBox(this);
  m_base->setObjectName("drawing.base");
  for (const auto& [id, label] : std::initializer_list<std::pair<const char*, QString>>{
           {"front", tr("Front view")}, {"top", tr("Top view")}, {"right", tr("Right view")}, {"left", tr("Left view")}, {"back", tr("Back view")}, {"bottom", tr("Bottom view")}})
    m_base->addItem(label, id);
  m_base->setCurrentIndex(std::max(0, m_base->findData(settings.value("drawings/base", "front"))));
  m_top = new QCheckBox(tr("Top view"), this);
  m_side = new QCheckBox(tr("Side view"), this);
  m_iso = new QCheckBox(tr("Isometric view"), this);
  const QStringList shown = settings.value("drawings/views", QStringList{"top", "side", "iso"}).toStringList();
  m_top->setChecked(shown.contains("top"));
  m_side->setChecked(shown.contains("side"));
  m_iso->setChecked(shown.contains("iso"));
  views->addWidget(m_base);
  views->addWidget(m_top);
  views->addWidget(m_side);
  views->addWidget(m_iso);
  views->addStretch();
  left->addRow(tr("Views"), views);
  auto* style = new QHBoxLayout();
  m_hidden = new QCheckBox(tr("Hidden lines"), this);
  m_hidden->setChecked(settings.value("drawings/hidden", false).toBool());
  m_tangent = new QComboBox(this);
  m_tangent->addItem(tr("Tangent edges thin"), "thin");
  m_tangent->addItem(tr("Tangent edges as edges"), "show");
  m_tangent->addItem(tr("No tangent edges"), "hide");
  m_tangent->setCurrentIndex(std::max(0, m_tangent->findData(settings.value("drawings/tangent", "thin"))));
  style->addWidget(m_hidden);
  style->addWidget(m_tangent);
  style->addStretch();
  left->addRow(tr("Style"), style);
  auto* of = new QHBoxLayout();
  m_whole = new QRadioButton(tr("The whole model"), this);
  m_selection = new QRadioButton(nodes.size() == 1 ? tr("The selection: %1").arg(doc->nodeName(nodes[0])) : tr("The selection (%1)").arg(nodes.size()), this);
  m_selection->setEnabled(!nodes.empty());
  (nodes.empty() ? m_whole : m_selection)->setChecked(true);
  of->addWidget(m_whole);
  of->addWidget(m_selection);
  of->addStretch();
  left->addRow(tr("Of"), of);
  columns->addLayout(left, 3);

  auto* right = new QFormLayout();
  m_title = new QLineEdit(this);
  m_title->setPlaceholderText(tr("The part's or the file's name"));
  if (nodes.size() == 1) m_title->setText(doc->nodeName(nodes[0]));
  right->addRow(tr("Title"), m_title);
  m_number = new QLineEdit(this);
  m_number->setPlaceholderText(tr("The part number"));
  right->addRow(tr("Number"), m_number);
  m_owner = new QLineEdit(settings.value("drawings/owner").toString(), this);
  m_owner->setPlaceholderText(tr("Company or legal owner"));
  right->addRow(tr("Owner"), m_owner);
  m_author = new QLineEdit(settings.value("user/name", QString::fromStdString(opad::default_author())).toString(), this);
  right->addRow(tr("Drawn by"), m_author);
  m_revision = new QLineEdit(this);
  right->addRow(tr("Revision"), m_revision);
  columns->addLayout(right, 2);
  v->addLayout(columns);

  auto* footer = new QHBoxLayout();
  footer->addStretch();
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  auto* create = new QPushButton(tr("Create drawing"), this);
  create->setObjectName("primary");
  create->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(create);
  v->addLayout(footer);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(create, &QPushButton::clicked, this, [this] {
    QSettings s;
    s.setValue("drawings/paper", m_templates->currentItem()->data(Qt::UserRole));
    s.setValue("drawings/orientation", m_portrait->isChecked() ? "portrait" : "landscape");
    s.setValue("drawings/base", m_base->currentData());
    QStringList views;
    for (const auto& [box, id] : std::initializer_list<std::pair<QCheckBox*, const char*>>{{m_top, "top"}, {m_side, "side"}, {m_iso, "iso"}})
      if (box->isChecked()) views << id;
    s.setValue("drawings/views", views);
    s.setValue("drawings/hidden", m_hidden->isChecked());
    s.setValue("drawings/tangent", m_tangent->currentData());
    s.setValue("drawings/owner", m_owner->text().trimmed());
    accept();
  });
  const auto standardChanged = [this] {
    if (m_projectionTouched || !m_templates->currentItem()) return;
    m_projection->setCurrentIndex(m_templates->currentItem()->data(Qt::UserRole + 1).toString() == "ansi" ? 1 : 0);
  };
  connect(m_templates, &QListWidget::currentRowChanged, this, standardChanged);
  connect(m_projection, &QComboBox::activated, this, [this] { m_projectionTouched = true; });
  connect(m_landscape, &QRadioButton::toggled, this, &NewDrawingDialog::renderThumbnails);
  standardChanged();
  renderThumbnails();
}

void NewDrawingDialog::renderThumbnails() {
  const bool landscape = m_landscape->isChecked();
  for (int i = 0; i < m_templates->count(); ++i) {
    QListWidgetItem* item = m_templates->item(i);
    const opad::json size = opad::drawing::paper_size(item->data(Qt::UserRole).toString().toStdString(), landscape);
    const QImage img = thumbnail(item->data(Qt::UserRole + 1).toString().toStdString(), size["w"].get<double>(), size["h"].get<double>(), landscape ? 120 : 85);
    QPixmap px(120, 120);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.drawImage(QPointF((120 - img.width()) / 2.0, (120 - img.height()) / 2.0), img);
    p.end();
    item->setIcon(QIcon(px));
  }
}

opad::json NewDrawingDialog::args() const {
  const QListWidgetItem* item = m_templates->currentItem();
  const std::string standard = item && item->data(Qt::UserRole + 1).toString() == "ansi" ? "asme" : "iso";
  opad::json a = {{"drawing", m_name->text().trimmed().isEmpty() ? tr("Drawing").toStdString() : m_name->text().trimmed().toStdString()},
                  {"size", item ? item->data(Qt::UserRole).toString().toStdString() : "A3"},
                  {"orientation", m_portrait->isChecked() ? "portrait" : "landscape"},
                  {"standard", standard},
                  {"projection", m_projection->currentData().toString().toStdString()},
                  {"scale", m_scale->currentData().toString().toStdString()},
                  {"hidden", m_hidden->isChecked()},
                  {"tangent", m_tangent->currentData().toString().toStdString()}};
  const std::string base = m_base->currentData().toString().toStdString();
  opad::json views = {base};
  if (m_top->isChecked()) views.push_back(base == "top" || base == "bottom" ? "front" : "top");
  if (m_side->isChecked()) views.push_back("side");
  if (m_iso->isChecked()) views.push_back("iso");
  a["views"] = views;
  if (m_selection->isChecked()) a["select"] = m_nodes;
  opad::json values = opad::json::object();
  for (const auto& [key, edit] : std::initializer_list<std::pair<const char*, QLineEdit*>>{
           {"title", m_title}, {"number", m_number}, {"owner", m_owner}, {"author", m_author}, {"revision", m_revision}})
    if (!edit->text().trimmed().isEmpty()) values[key] = edit->text().trimmed().toStdString();
  if (!values.empty()) a["values"] = values;
  return a;
}

// ---------------------------------------------------------------- document properties
namespace {
QString shown(const opad::json& v) { return v.is_string() ? QString::fromStdString(v.get<std::string>()) : v.is_number() ? QString::number(v.get<double>()) : QString(); }
}  // namespace

DocumentPropertiesDialog::DocumentPropertiesDialog(AppDocument* doc, QWidget* parent) : QDialog(parent), m_before(doc->scene.properties) {
  setObjectName("documentPropertiesDialog");
  setWindowTitle(tr("Document properties"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(10);
  auto* intro = new QLabel(tr("What every drawing of this document says in its title block, unless a sheet says otherwise."), this);
  intro->setObjectName("secondary");
  intro->setWordWrap(true);
  v->addWidget(intro);
  auto* form = new QFormLayout();
  for (const auto& [key, label] : std::initializer_list<std::pair<const char*, QString>>{
           {"title", tr("Title")}, {"number", tr("Number")}, {"revision", tr("Revision")}, {"status", tr("Status")}, {"owner", tr("Owner")},
           {"project", tr("Project")}, {"author", tr("Designed by")}, {"checked", tr("Checked by")}, {"approved", tr("Approved by")},
           {"description", tr("Description")}}) {
    auto* edit = new QLineEdit(shown(m_before.value(key, std::string(key) == "owner" ? m_before.value("company", opad::json()) : opad::json())), this);
    edit->setObjectName(QString("document.") + key);
    m_fields[key] = edit;
    form->addRow(label, edit);
  }
  v->addLayout(form);
  auto* more = new QLabel(tr("More properties (=doc:name in a title block field)"), this);
  more->setFont(theme::ui(12, QFont::DemiBold));
  v->addWidget(more);
  m_custom = new QTableWidget(0, 2, this);
  m_custom->setObjectName("document.custom");
  m_custom->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
  m_custom->horizontalHeader()->setStretchLastSection(true);
  m_custom->verticalHeader()->hide();
  m_custom->setMinimumHeight(120);
  for (const auto& [k, value] : m_before.items()) {
    if (m_fields.count(k) || (k == "company" && !m_before.contains("owner"))) continue;
    const int row = m_custom->rowCount();
    m_custom->insertRow(row);
    m_custom->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(k)));
    m_custom->setItem(row, 1, new QTableWidgetItem(shown(value)));
  }
  v->addWidget(m_custom);
  auto* footer = new QHBoxLayout();
  auto* add = new QPushButton(tr("Add property"), this);
  footer->addWidget(add);
  footer->addStretch();
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  auto* apply = new QPushButton(tr("Apply"), this);
  apply->setObjectName("primary");
  apply->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(apply);
  v->addLayout(footer);
  connect(add, &QPushButton::clicked, this, [this] {
    const int row = m_custom->rowCount();
    m_custom->insertRow(row);
    m_custom->setItem(row, 0, new QTableWidgetItem());
    m_custom->setItem(row, 1, new QTableWidgetItem());
    m_custom->editItem(m_custom->item(row, 0));
  });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(apply, &QPushButton::clicked, this, &QDialog::accept);
  resize(460, sizeHint().height());
}

opad::json DocumentPropertiesDialog::change() const {
  opad::json after = opad::json::object();
  for (const auto& [key, edit] : m_fields)
    if (!edit->text().trimmed().isEmpty()) after[key] = edit->text().trimmed().toStdString();
  for (int row = 0; row < m_custom->rowCount(); ++row) {
    const QTableWidgetItem *name = m_custom->item(row, 0), *value = m_custom->item(row, 1);
    const QString k = name ? name->text().trimmed() : QString();
    if (!k.isEmpty() && value && !value->text().trimmed().isEmpty() && !m_fields.count(k.toStdString())) after[k.toStdString()] = value->text().trimmed().toStdString();
  }
  if (m_before.contains("company") && !m_before.contains("owner") && after.contains("owner") && shown(m_before["company"]).toStdString() == after["owner"].get<std::string>()) {
    after["company"] = after["owner"];  // shown as the owner, kept as it was written
    after.erase("owner");
  }
  opad::json set = opad::json::object();
  for (const auto& [k, value] : after.items())
    if (!value.is_null() && (!m_before.contains(k) || shown(m_before[k]).toStdString() != value.get<std::string>())) set[k] = value;
  for (const auto& [k, value] : m_before.items())
    if (!after.contains(k) || after[k].is_null()) set[k] = nullptr;
  return set.empty() ? opad::json(nullptr) : set;
}

// ---------------------------------------------------------------- sheet properties
SheetPropertiesDialog::SheetPropertiesDialog(AppDocument* doc, const std::string& sheet, QWidget* parent) : QDialog(parent), m_doc(doc), m_sheet(sheet) {
  setObjectName("sheetPropertiesDialog");
  setWindowTitle(tr("Sheet properties"));
  const opad::Sheet* s = doc->scene.sheet(sheet);
  if (!s) throw opad::Error("That sheet is gone.");
  m_def = s->def;
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(10);
  auto* columns = new QHBoxLayout();
  columns->setSpacing(28);
  auto* left = new QFormLayout();
  m_name = new QLineEdit(QString::fromStdString(s->name), this);
  left->addRow(tr("Name"), m_name);
  m_drawing = new QLineEdit(QString::fromStdString(s->drawing), this);
  left->addRow(tr("Drawing"), m_drawing);
  m_size = new QComboBox(this);
  const opad::json size = m_def.value("size", opad::json::object());
  for (const auto& p : opad::drawing::paper_sizes()) m_size->addItem(QString::fromLatin1(p.name).replace("ANSI-", "ANSI "), QString::fromLatin1(p.name));
  if (size.contains("preset")) m_size->setCurrentIndex(std::max(0, m_size->findData(text(size, "preset"))));
  else {
    m_size->insertItem(0, tr("Own size: %1 × %2 mm").arg(s->width).arg(s->height), QString());
    m_size->setCurrentIndex(0);
  }
  left->addRow(tr("Paper"), m_size);
  auto* orientation = new QHBoxLayout();
  m_landscape = new QRadioButton(tr("Landscape"), this);
  m_portrait = new QRadioButton(tr("Portrait"), this);
  (s->width >= s->height ? m_landscape : m_portrait)->setChecked(true);
  orientation->addWidget(m_landscape);
  orientation->addWidget(m_portrait);
  orientation->addStretch();
  left->addRow(QString(), orientation);
  m_template = new QComboBox(this);
  const opad::json t = m_def.value("template", opad::json());
  const std::string tid = t.is_object() ? t.value("id", "") : "none";
  m_template->addItem(tr("ISO 5457 frame, ISO 7200 title block"), "iso");
  m_template->addItem(tr("ASME frame and title block"), "ansi");
  if (tid != "iso" && tid != "ansi" && tid != "none") m_template->addItem(tr("From %1").arg(text(t, "name")), "keep");
  m_template->addItem(tr("Frame only"), "none");
  m_template->setCurrentIndex(std::max(0, m_template->findData(QString::fromStdString(tid == "iso" || tid == "ansi" || tid == "none" ? tid : "keep"))));
  left->addRow(tr("Template"), m_template);
  m_standard = new QComboBox(this);
  m_standard->addItem("ISO", "iso");
  m_standard->addItem("ASME", "asme");
  m_standard->setCurrentIndex(s->standard == "asme" ? 1 : 0);
  left->addRow(tr("Standard"), m_standard);
  m_projection = new QComboBox(this);
  m_projection->addItem(tr("First angle"), "first");
  m_projection->addItem(tr("Third angle"), "third");
  m_projection->setCurrentIndex(s->projection == "third" ? 1 : 0);
  left->addRow(tr("Projection"), m_projection);
  m_scale = new QComboBox(this);
  m_scale->setEditable(true);
  m_scale->addItems(scales());
  m_scale->setCurrentText(QString::fromStdString(opad::drawing::scale_text(s->scale)));
  left->addRow(tr("Drawing scale"), m_scale);
  m_units = new QComboBox(this);
  m_units->addItem(tr("Millimetres"), "mm");
  m_units->addItem(tr("Inches"), "in");
  m_units->setCurrentIndex(m_def.value("units", "mm") == "in" ? 1 : 0);
  left->addRow(tr("Dimension units"), m_units);
  columns->addLayout(left, 1);

  // Title block: each field of the template, empty ones showing what is filled in for them.
  auto* right = new QFormLayout();
  opad::Sheet bare = *s;
  bare.def.erase("values");
  const opad::json filled = opad::drawing::title_values(doc->doc, doc->scene, bare, false);
  const opad::json values = m_def.value("values", opad::json::object());
  std::vector<std::pair<std::string, std::string>> fields;
  if (t.is_object() && t.contains("title_block"))
    for (const auto& f : t["title_block"].value("fields", opad::json::array()))
      if (f.value("key", "") != "projection") fields.push_back({f.value("key", ""), f.value("label", "")});
  if (fields.empty())
    for (const char* k : {"title", "number", "owner", "author", "revision"}) fields.push_back({k, k});
  for (const auto& [key, label] : fields) {
    if (m_fields.count(key)) continue;
    auto* edit = new QLineEdit(this);
    edit->setObjectName(QString::fromStdString("field." + key));
    const opad::json value = values.value(key, opad::json());
    if (value.is_string()) edit->setText(QString::fromStdString(value.get<std::string>()));
    QString hint = QString::fromStdString(filled.value(key, ""));
    edit->setPlaceholderText(hint.replace('\n', ' '));
    m_fields[key] = edit;
    right->addRow(i18n::t(QString::fromStdString(label)), edit);
  }
  auto* hint = new QLabel(tr("Empty fields are filled in from the sheet, the part and the document properties; =key looks a value up (=scale, =prop:finish, =doc:project)."), this);
  hint->setObjectName("tertiary");
  hint->setWordWrap(true);
  right->addRow(hint);
  columns->addLayout(right, 1);
  v->addLayout(columns);
  auto* footer = new QHBoxLayout();
  footer->addStretch();
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  auto* apply = new QPushButton(tr("Apply"), this);
  apply->setObjectName("primary");
  apply->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(apply);
  v->addLayout(footer);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(apply, &QPushButton::clicked, this, &QDialog::accept);
  connect(m_standard, &QComboBox::activated, this, [this](int i) { m_projection->setCurrentIndex(i); });
}

opad::json SheetPropertiesDialog::change() const {
  opad::json set = opad::json::object();
  const opad::Sheet* s = m_doc->scene.sheet(m_sheet);
  if (!s) return nullptr;
  if (m_name->text().trimmed().toStdString() != s->name && !m_name->text().trimmed().isEmpty()) set["name"] = m_name->text().trimmed().toStdString();
  if (m_drawing->text().trimmed().toStdString() != s->drawing) set["drawing"] = m_drawing->text().trimmed().toStdString();
  const bool landscape = m_landscape->isChecked();
  const QString preset = m_size->currentData().toString();
  const opad::json size = m_def.value("size", opad::json::object());
  if (!preset.isEmpty() && (preset.toStdString() != size.value("preset", "") || landscape != (s->width >= s->height))) {
    set["size"] = preset.toStdString();
    set["orientation"] = landscape ? "landscape" : "portrait";
  }
  const opad::json t = m_def.value("template", opad::json());
  const std::string tid = t.is_object() ? t.value("id", "") : "none", want = m_template->currentData().toString().toStdString();
  if (want != "keep" && want != tid) set["template"] = want;
  if (m_standard->currentData().toString().toStdString() != s->standard) set["standard"] = m_standard->currentData().toString().toStdString();
  if (m_projection->currentData().toString().toStdString() != s->projection) set["projection"] = m_projection->currentData().toString().toStdString();
  try {
    const double scale = opad::drawing::parse_scale(m_scale->currentText().trimmed().toStdString());
    if (std::fabs(scale - s->scale) > 1e-12) set["scale"] = opad::drawing::scale_text(scale);
  } catch (const std::exception&) {
    throw opad::Error("The scale is a ratio such as 1:2 or 2:1.");
  }
  if (m_units->currentData().toString().toStdString() != m_def.value("units", "mm")) set["units"] = m_units->currentData().toString().toStdString();
  opad::json values = m_def.value("values", opad::json::object()), before = values;
  for (const auto& [key, edit] : m_fields) {
    const std::string now = edit->text().trimmed().toStdString();
    if (now.empty()) values.erase(key);
    else values[key] = now;
  }
  if (values != before) set["values"] = values.empty() ? opad::json(nullptr) : values;
  return set.empty() ? opad::json(nullptr) : set;
}
