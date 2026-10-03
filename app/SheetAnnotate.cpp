#include "SheetAnnotate.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>

#include <algorithm>
#include <cmath>
#include <set>

#include "AppDocument.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/paint.hpp"
#include "opad/drawing/symbols.hpp"

using opad::json;
using opad::drawing::Display;
using opad::drawing::Vec2;
using Tool = SheetAnnotator::Tool;

namespace {

Vec2 vec2(const json& j, Vec2 fallback = {0, 0}) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) return fallback;
  return {j[0].get<double>(), j[1].get<double>()};
}
json js(Vec2 v) { return json::array({std::round(v[0] * 100) / 100, std::round(v[1] * 100) / 100}); }

bool isSet(Tool t) { return t == Tool::Ordinate || t == Tool::Baseline || t == Tool::Chain; }

const char* kindOf(Tool t) {
  switch (t) {
    case Tool::Dimension: return "dimension";
    case Tool::HoleCallout: return "hole_callout";
    case Tool::CentreMark: return "centermark";
    case Tool::CentreLine: return "centerline";
    case Tool::Note: return "note";
    case Tool::Datum: return "datum";
    case Tool::Frame: return "fcf";
    case Tool::Surface: return "surface";
    case Tool::Ordinate:
    case Tool::Baseline:
    case Tool::Chain: return "dimension_set";
    case Tool::HoleTable: return "hole_table";
    default: return "";
  }
}

QString toolName(Tool t) {
  switch (t) {
    case Tool::Dimension: return SheetAnnotator::tr("Dimension");
    case Tool::HoleCallout: return SheetAnnotator::tr("Hole callout");
    case Tool::CentreMark: return SheetAnnotator::tr("Centre mark");
    case Tool::CentreLine: return SheetAnnotator::tr("Centre line");
    case Tool::Note: return SheetAnnotator::tr("Note");
    case Tool::Datum: return SheetAnnotator::tr("Datum");
    case Tool::Frame: return SheetAnnotator::tr("Feature control frame");
    case Tool::Surface: return SheetAnnotator::tr("Surface texture");
    case Tool::Ordinate: return SheetAnnotator::tr("Ordinate dimensions");
    case Tool::Baseline: return SheetAnnotator::tr("Baseline dimensions");
    case Tool::Chain: return SheetAnnotator::tr("Chain dimensions");
    case Tool::HoleTable: return SheetAnnotator::tr("Hole table");
    case Tool::Reattach: return SheetAnnotator::tr("Re-attach");
    default: return {};
  }
}

// A drafting glyph as a small picture for a list (the characteristics).
QIcon glyphIcon(const std::string& glyph) {
  Display d;
  opad::drawing::rich_text(d, d.layer({"G", opad::drawing::kInk, opad::drawing::LineType::Continuous, 0.15}), glyph, {0, 0}, 3.5);
  const auto b = d.bounds();
  QPixmap pm(40, 32);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  const double m = 0.6;
  opad::drawing::paint(p, d, {b[0] - m, b[1] - m, b[2] + m, b[3] + m}, QRectF(4, 2, 32, 28), 1.4);
  p.end();
  QImage img = pm.toImage();  // ink in the theme's text colour
  const QColor ink = theme::current().fg;
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x)
      if (const int a = qAlpha(img.pixel(x, y)); a) img.setPixel(x, y, qRgba(ink.red(), ink.green(), ink.blue(), a));
  return QIcon(QPixmap::fromImage(img));
}

}  // namespace

SheetAnnotator::SheetAnnotator(AppDocument* doc, SheetCanvas* canvas, QWidget* parent) : QObject(parent), m_doc(doc), m_canvas(canvas) {
  buildBar();
  m_debounce.setSingleShot(true);
  m_debounce.setInterval(150);
  connect(&m_debounce, &QTimer::timeout, this, [this] {
    if (m_tool != Tool::None && !m_picks.empty()) replan();
  });
  canvas->setInteraction(this);
  showFields();
}

SheetAnnotator::~SheetAnnotator() {
  *m_alive = false;
  if (m_canvas) m_canvas->setInteraction(nullptr);
}

std::string SheetAnnotator::sheetId() const { return m_canvas ? m_canvas->sheet() : std::string(); }

// ---------------------------------------------------------------- the options bar
void SheetAnnotator::buildBar() {
  m_bar = new QWidget(qobject_cast<QWidget*>(parent()));
  m_bar->setObjectName("annotateBar");
  m_bar->setAutoFillBackground(true);
  auto* h = new QHBoxLayout(m_bar);
  h->setContentsMargins(10, 3, 10, 3);
  h->setSpacing(6);
  m_title = new QLabel(m_bar);
  m_title->setFont(theme::ui(12, QFont::DemiBold));
  h->addWidget(m_title);
  h->addSpacing(8);
  const auto field = [&](QWidget* w, const QString& label, std::vector<Tool> tools, std::vector<std::string> kinds) {
    QWidget* box = w;
    if (!label.isEmpty()) {
      box = new QWidget(m_bar);
      auto* l = new QHBoxLayout(box);
      l->setContentsMargins(0, 0, 0, 0);
      l->setSpacing(4);
      auto* text = new QLabel(label, box);
      text->setObjectName("secondary");
      l->addWidget(text);
      w->setParent(box);
      l->addWidget(w);
    }
    h->addWidget(box);
    m_fields.push_back({box, std::move(tools)});
    m_itemFields.push_back({box, std::move(kinds)});
  };
  const auto combo = [&](std::initializer_list<std::pair<QString, QString>> items) {
    auto* c = new QComboBox(m_bar);
    for (const auto& [label, data] : items) c->addItem(label, data);
    return c;
  };
  const auto edit = [&](const QString& placeholder, int width) {
    auto* e = new QLineEdit(m_bar);
    e->setPlaceholderText(placeholder);
    if (width) e->setFixedWidth(width);
    return e;
  };
  const std::vector<Tool> sets{Tool::Ordinate, Tool::Baseline, Tool::Chain};
  m_typeBox = combo({{tr("Automatic"), "auto"}, {tr("Horizontal"), "horizontal"}, {tr("Vertical"), "vertical"}, {tr("Aligned"), "aligned"}});
  m_typeBox->setObjectName("annotate.type");
  field(m_typeBox, tr("Type"), {Tool::Dimension}, {});
  m_precision = combo({{"1", "0"}, {"0.1", "1"}, {"0.01", "2"}, {"0.001", "3"}, {"0.0001", "4"}});
  m_precision->setObjectName("annotate.precision");
  m_precision->setCurrentIndex(std::clamp(units::current().decimals, 0, 4));
  m_precision->setToolTip(tr("Decimals shown; trailing zeros are dropped in millimetres and kept for inches on ASME sheets"));
  {
    std::vector<Tool> t{Tool::Dimension, Tool::HoleCallout, Tool::HoleTable};
    t.insert(t.end(), sets.begin(), sets.end());
    field(m_precision, tr("Precision"), t, {"dimension", "hole_callout", "hole_table", "dimension_set"});
  }
  m_tolBox = combo({{tr("No tolerance"), "none"}, {QString::fromUtf8("± ") + tr("Symmetric"), "sym"}, {tr("+/− Deviation"), "dev"}, {tr("Limits"), "limits"}});
  m_tolBox->setObjectName("annotate.tolerance");
  field(m_tolBox, tr("Tolerance"), {Tool::Dimension}, {"dimension"});
  m_plus = edit("+0.1", 64);
  m_plus->setObjectName("annotate.plus");
  field(m_plus, QString(), {Tool::Dimension}, {"dimension"});
  m_minus = edit(QString::fromUtf8("−0.1"), 64);
  m_minus->setObjectName("annotate.minus");
  field(m_minus, QString(), {Tool::Dimension}, {"dimension"});
  m_text = edit(tr("Text; <> is the value"), 170);
  m_text->setObjectName("annotate.text");
  field(m_text, tr("Text"), {Tool::Dimension, Tool::Note}, {"dimension", "note"});
  m_letter = edit("A", 40);
  m_letter->setObjectName("annotate.letter");
  m_letter->setMaxLength(2);
  field(m_letter, tr("Letter"), {Tool::Datum}, {"datum"});
  m_characteristic = new QComboBox(m_bar);
  m_characteristic->setObjectName("annotate.characteristic");
  const std::vector<std::pair<std::string, QString>> names = {
      {"position", tr("Position")},         {"flatness", tr("Flatness")},           {"straightness", tr("Straightness")},
      {"circularity", tr("Circularity")},   {"cylindricity", tr("Cylindricity")},   {"perpendicularity", tr("Perpendicularity")},
      {"parallelism", tr("Parallelism")},   {"angularity", tr("Angularity")},       {"line_profile", tr("Profile of a line")},
      {"surface_profile", tr("Profile of a surface")}, {"concentricity", tr("Concentricity")}, {"symmetry", tr("Symmetry")},
      {"circular_runout", tr("Circular runout")}, {"total_runout", tr("Total runout")}};
  for (const auto& [id, label] : names) m_characteristic->addItem(glyphIcon(opad::drawing::characteristic_glyph(id)), label, QString::fromStdString(id));
  field(m_characteristic, QString(), {Tool::Frame}, {"fcf"});
  m_zone = new QCheckBox(QString::fromUtf8("⌀"), m_bar);
  m_zone->setObjectName("annotate.zone");
  m_zone->setToolTip(tr("A cylindrical tolerance zone"));
  field(m_zone, QString(), {Tool::Frame}, {"fcf"});
  m_value = edit("0.1", 90);
  m_value->setObjectName("annotate.value");
  field(m_value, tr("Value"), {Tool::Frame, Tool::Surface}, {"fcf", "surface"});
  m_material = combo({{tr("No modifier"), ""}, {QString::fromUtf8("Ⓜ ") + tr("Maximum material"), "M"}, {QString::fromUtf8("Ⓛ ") + tr("Least material"), "L"},
                      {QString::fromUtf8("Ⓢ ") + tr("Regardless of size"), "S"}});
  m_material->setObjectName("annotate.material");
  field(m_material, QString(), {Tool::Frame}, {"fcf"});
  {
    auto* box = new QWidget(m_bar);
    auto* l = new QHBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(3);
    auto* label = new QLabel(tr("Datums"), box);
    label->setObjectName("secondary");
    l->addWidget(label);
    for (int i = 0; i < 3; ++i) {
      m_datums[i] = new QLineEdit(box);
      m_datums[i]->setPlaceholderText(QString(QChar('A' + i)));
      m_datums[i]->setFixedWidth(46);
      m_datums[i]->setObjectName(QString("annotate.datum%1").arg(i + 1));
      m_datums[i]->setToolTip(tr("A datum letter, with (M) or (L) for a modifier"));
      l->addWidget(m_datums[i]);
    }
    h->addWidget(box);
    m_fields.push_back({box, {Tool::Frame}});
    m_itemFields.push_back({box, {"fcf"}});
  }
  m_process = combo({{tr("Material removal required"), "removal"}, {tr("Any process"), "any"}, {tr("No material removal"), "no_removal"}});
  m_process->setObjectName("annotate.process");
  field(m_process, QString(), {Tool::Surface}, {"surface"});
  m_axis = combo({{tr("Automatic"), "auto"}, {tr("Horizontal"), "horizontal"}, {tr("Vertical"), "vertical"}});
  m_axis->setObjectName("annotate.axis");
  field(m_axis, tr("Direction"), sets, {"dimension_set"});
  m_done = new QPushButton(tr("Done"), m_bar);
  m_done->setToolTip(tr("Ends picking features (Enter)"));
  connect(m_done, &QPushButton::clicked, this, [this] { finish(); });
  field(m_done, QString(), sets, {});
  h->addStretch(1);
  const auto paint = [this] {
    QPalette pal = m_bar->palette();
    pal.setColor(QPalette::Window, theme::current().bg2);
    m_bar->setPalette(pal);
  };
  paint();
  connect(theme::notifier(), &theme::Notifier::changed, m_bar, paint);
  // Changes: a tool plans again; a selected annotation is edited.
  connect(m_typeBox, &QComboBox::currentIndexChanged, this, [this] { fieldChanged("type"); });
  connect(m_precision, &QComboBox::currentIndexChanged, this, [this] { fieldChanged("precision"); });
  connect(m_tolBox, &QComboBox::currentIndexChanged, this, [this] { fieldChanged("tol"); });
  connect(m_characteristic, &QComboBox::currentIndexChanged, this, [this] { fieldChanged("characteristic"); });
  connect(m_material, &QComboBox::currentIndexChanged, this, [this] { fieldChanged("material"); });
  connect(m_process, &QComboBox::currentIndexChanged, this, [this] { fieldChanged("process"); });
  connect(m_axis, &QComboBox::currentIndexChanged, this, [this] { fieldChanged("axis"); });
  connect(m_zone, &QCheckBox::toggled, this, [this] { fieldChanged("zone"); });
  for (auto [e, key] : std::initializer_list<std::pair<QLineEdit*, const char*>>{
           {m_plus, "tol"}, {m_minus, "tol"}, {m_text, "text"}, {m_letter, "letter"}, {m_value, "value"}, {m_datums[0], "datums"}, {m_datums[1], "datums"}, {m_datums[2], "datums"}})
    connect(e, &QLineEdit::editingFinished, this, [this, key] { fieldChanged(key); });
  for (QLineEdit* e : {m_plus, m_minus, m_text, m_letter, m_value, m_datums[0], m_datums[1], m_datums[2]})  // Enter in a field places the item
    connect(e, &QLineEdit::returnPressed, this, [this] {
      if (m_tool != Tool::None && m_canvas) m_canvas->setFocus();
    });
}

void SheetAnnotator::showFields() {
  const opad::SheetItem* item = m_tool == Tool::None && !m_item.empty() ? m_doc->scene.sheet_item(m_item) : nullptr;
  bool any = false;
  m_filling = true;
  for (size_t i = 0; i < m_fields.size(); ++i) {
    const bool shown = m_tool != Tool::None ? std::find(m_fields[i].second.begin(), m_fields[i].second.end(), m_tool) != m_fields[i].second.end()
                                            : item && std::find(m_itemFields[i].second.begin(), m_itemFields[i].second.end(), item->kind) != m_itemFields[i].second.end();
    m_fields[i].first->setVisible(shown);
    any = any || shown;
  }
  if (item) {  // its own values
    const json& d = item->def;
    m_title->setText(tr("Selected annotation"));
    m_precision->setCurrentIndex(std::clamp(d.value("precision", 2), 0, 4));
    const json tol = d.value("tol", json());
    m_tolBox->setCurrentIndex(std::max(0, m_tolBox->findData(QString::fromStdString(tol.is_object() ? tol.value("type", "sym") : "none"))));
    m_plus->setText(tol.is_object() && tol.contains("plus") ? QString::number(tol["plus"].get<double>()) : QString());
    m_minus->setText(tol.is_object() && tol.contains("minus") ? QString::number(tol["minus"].get<double>()) : QString());
    m_text->setText(QString::fromStdString(d.value("text", "")));
    m_letter->setText(QString::fromStdString(d.value("letter", "")));
    m_characteristic->setCurrentIndex(std::max(0, m_characteristic->findData(QString::fromStdString(d.value("characteristic", "position")))));
    const json v = d.value("value", json());
    m_value->setText(v.is_string() ? QString::fromStdString(v.get<std::string>()) : v.is_number() ? QString::number(v.get<double>()) : QString());
    m_zone->setChecked(d.value("zone", "") == "diameter");
    m_material->setCurrentIndex(std::max(0, m_material->findData(QString::fromStdString(d.value("material", "")))));
    const json ds = d.value("datums", json::array());
    for (int i = 0; i < 3; ++i) m_datums[i]->setText(i < static_cast<int>(ds.size()) && ds[size_t(i)].is_string() ? QString::fromStdString(ds[size_t(i)].get<std::string>()) : QString());
    m_process->setCurrentIndex(std::max(0, m_process->findData(QString::fromStdString(d.value("process", "removal")))));
    m_axis->setCurrentIndex(std::max(0, m_axis->findData(QString::fromStdString(d.value("axis", "horizontal")))));
  } else {
    m_title->setText(toolName(m_tool));
  }
  const bool tol = m_tolBox->currentData().toString() != "none";
  m_plus->setEnabled(tol);
  m_minus->setEnabled(tol && m_tolBox->currentData().toString() != "sym");
  m_filling = false;
  m_bar->setVisible(any);
}

void SheetAnnotator::itemsSelected(const std::vector<std::string>& items) {
  if (m_tool != Tool::None) return;
  m_item = items.size() == 1 ? items[0] : std::string();
  showFields();
}

json SheetAnnotator::tolerance() const {
  const QString type = m_tolBox->currentData().toString();
  if (type == "none") return nullptr;
  const opad::Sheet* sheet = m_doc->scene.sheet(sheetId());
  units::Display d = units::current();
  d.length = sheet && sheet->def.value("units", "mm") == "in" ? "in" : "mm";  // bare numbers in the sheet's unit
  const double per = units::mmPer(d.length);
  const auto value = [&](QLineEdit* e) -> std::optional<double> {
    if (e->text().trimmed().isEmpty()) return std::nullopt;
    const auto v = units::parse(units::Kind::Length, e->text().replace(QChar(0x2212), '-'), d);
    if (!v) return std::nullopt;
    return std::round(*v / per * 1e6) / 1e6;
  };
  const double plus = value(m_plus).value_or(0.1);
  json t = {{"type", type.toStdString()}, {"plus", plus}};
  if (type != "sym") t["minus"] = value(m_minus).value_or(-plus);
  return t;
}

void SheetAnnotator::fieldChanged(const char* key) {
  if (m_filling) return;
  const std::string k = key;
  if (k == "tol") {
    const bool tol = m_tolBox->currentData().toString() != "none";
    m_plus->setEnabled(tol);
    m_minus->setEnabled(tol && m_tolBox->currentData().toString() != "sym");
  }
  if (m_tool != Tool::None) {
    if (k == "type" || (k == "axis" && m_plan.contains("choices"))) return updatePreview();
    if (k == "text" && m_tool == Tool::Note) return updatePreview();
    m_debounce.start();
    return;
  }
  const opad::SheetItem* item = m_item.empty() ? nullptr : m_doc->scene.sheet_item(m_item);
  if (!item || !m_runner) return;
  json set;
  if (k == "precision") set["precision"] = m_precision->currentData().toInt();
  else if (k == "tol") set["tol"] = tolerance();
  else if (k == "text") set["text"] = m_text->text().isEmpty() && item->kind != "note" ? json(nullptr) : json(m_text->text().toStdString());
  else if (k == "letter") set["letter"] = m_letter->text().toUpper().toStdString();
  else if (k == "characteristic") set["characteristic"] = m_characteristic->currentData().toString().toStdString();
  else if (k == "material") set["material"] = m_material->currentData().toString().isEmpty() ? json(nullptr) : json(m_material->currentData().toString().toStdString());
  else if (k == "process") set["process"] = m_process->currentData().toString().toStdString();
  else if (k == "axis") set["axis"] = m_axis->currentData().toString() == "auto" ? "horizontal" : m_axis->currentData().toString().toStdString();
  else if (k == "zone") set["zone"] = m_zone->isChecked() ? json("diameter") : json(nullptr);
  else if (k == "value") {
    bool number = false;
    const double v = m_value->text().toDouble(&number);
    set["value"] = number && item->kind == "fcf" ? json(v) : json(m_value->text().toStdString());
  } else if (k == "datums") {
    json ds = json::array();
    for (QLineEdit* e : m_datums)
      if (!e->text().trimmed().isEmpty()) ds.push_back(e->text().trimmed().toUpper().toStdString());
    set["datums"] = ds.empty() ? json(nullptr) : ds;
  }
  if (set.empty()) return;
  if (k == "text" && item->kind == "note" && m_text->text().isEmpty()) return;  // a note keeps some text
  m_runner("sheet_edit", {{"target", m_item}, {"set", set}}, {});
}

QString SheetAnnotator::nextLetter() const {
  std::set<std::string> used;
  if (const opad::Sheet* s = m_doc->scene.sheet(sheetId()))
    for (const auto& id : s->items)
      if (const opad::SheetItem* t = m_doc->scene.sheet_item(id); t && t->kind == "datum") used.insert(t->def.value("letter", ""));
  for (char c = 'A'; c <= 'Z'; ++c)
    if (c != 'I' && c != 'O' && c != 'Q' && !used.count(std::string(1, c))) return QString(QChar(c));
  return "AA";
}

// ---------------------------------------------------------------- tools
void SheetAnnotator::start(Tool tool) {
  cancel();
  if (tool == Tool::None || tool == Tool::Reattach || !m_canvas || sheetId().empty()) return;
  m_tool = tool;
  m_item.clear();
  if (tool == Tool::Datum) m_letter->setText(nextLetter());
  m_filling = true;  // a note's text is no dimension's
  m_text->clear();
  m_filling = false;
  showFields();
  promptForStep();
  m_canvas->setFocus();
  emit toolChanged();
}

void SheetAnnotator::reattach(const std::string& item) {
  cancel();
  const opad::SheetItem* t = m_doc->scene.sheet_item(item);
  if (!t || !m_canvas) return;
  m_needed = std::max<int>(1, static_cast<int>(t->def.value("refs", json::array()).size()));
  m_tool = Tool::Reattach;
  m_item = item;
  m_view = t->view;
  showFields();
  promptForStep();
  m_canvas->setFocus();
  emit toolChanged();
}

void SheetAnnotator::cancel() {
  const bool was = m_tool != Tool::None;
  m_tool = Tool::None;
  m_picks.clear();
  m_plan = nullptr;
  m_view.clear();
  m_ending = false;
  m_again = false;
  m_type.clear();
  m_item.clear();
  m_debounce.stop();
  if (m_canvas) m_canvas->setPreview(nullptr);
  setPrompt(QString());
  showFields();
  if (was) emit toolChanged();
}

void SheetAnnotator::setPrompt(const QString& text) {
  m_prompt = text;
  if (m_canvas) m_canvas->setPrompt(text);
}

bool SheetAnnotator::placing() const {
  if (m_pending || m_plan.is_null() || m_plan.contains("error")) return false;
  if (!m_plan.contains("op") && !m_plan.contains("choices")) return false;
  switch (m_tool) {
    case Tool::Dimension:
    case Tool::HoleCallout:
    case Tool::Datum:
    case Tool::Surface:
    case Tool::HoleTable: return true;
    case Tool::Note:
    case Tool::Frame: return !m_picks.empty();
    case Tool::Ordinate:
    case Tool::Baseline:
    case Tool::Chain: return m_ending;
    default: return false;
  }
}

bool SheetAnnotator::wantsPick() const {
  const size_t n = m_picks.size();
  switch (m_tool) {
    case Tool::Dimension: return n < 2;
    case Tool::CentreLine: return n < 2;
    case Tool::HoleCallout:
    case Tool::CentreMark:
    case Tool::Note:
    case Tool::Datum:
    case Tool::Frame:
    case Tool::Surface: return n < 1;
    case Tool::Ordinate:
    case Tool::Baseline:
    case Tool::Chain: return !m_ending;
    case Tool::Reattach: return static_cast<int>(n) < m_needed;
    default: return false;
  }
}

void SheetAnnotator::promptForStep() {
  if (m_tool == Tool::None) return setPrompt(QString());
  if (m_pending) return setPrompt(tr("Measuring…"));
  const size_t n = m_picks.size();
  const QString esc = tr(" · Esc takes back a pick or leaves");
  QString text;
  switch (m_tool) {
    case Tool::Dimension:
      text = n == 0 ? tr("Pick an edge, a circle or a point to dimension") : placing() && n < 2 ? tr("Click to place the dimension, or pick a second edge or point")
                                                                                                : tr("Click to place the dimension");
      break;
    case Tool::HoleCallout: text = n == 0 ? tr("Pick a hole: its circle or its wall") : tr("Click where the callout goes"); break;
    case Tool::CentreMark: text = tr("Pick a circle"); break;
    case Tool::CentreLine: text = n == 0 ? tr("Pick a circle, a cylinder seen from the side or a straight edge") : tr("Pick the second circle or edge"); break;
    case Tool::Note: text = n == 0 ? tr("Click where the note goes, or first pick what its leader points at") : tr("Click where the note goes"); break;
    case Tool::Datum: text = n == 0 ? tr("Pick the edge the datum stands on") : tr("Click where the datum's frame goes"); break;
    case Tool::Frame: text = n == 0 ? tr("Pick the feature the frame controls, or click where a frame without a leader goes") : tr("Click where the frame goes"); break;
    case Tool::Surface: text = n == 0 ? tr("Pick the edge of the surface") : tr("Click on the side the symbol stands, along the edge"); break;
    case Tool::Ordinate:
    case Tool::Baseline:
    case Tool::Chain:
      text = n == 0 ? tr("Pick the origin: a datum edge, a vertex or a centre") : !m_ending ? tr("Pick the features to dimension from it · Enter when done")
                                                                                            : tr("Click where the dimensions go");
      break;
    case Tool::HoleTable: text = m_view.empty() ? tr("Click the view whose holes go into the table") : tr("Click where the table's top left corner goes"); break;
    case Tool::Reattach: text = tr("Pick reference %1 of %2 for the annotation").arg(n + 1).arg(m_needed); break;
    default: break;
  }
  setPrompt(text + esc);
}

json SheetAnnotator::args() const {
  json a = {{"sheet", sheetId()}, {"kind", kindOf(m_tool)}};
  if (!m_view.empty()) a["view"] = m_view;
  const int precision = m_precision->currentData().toInt();
  switch (m_tool) {
    case Tool::Dimension:
      a["precision"] = precision;
      if (const json t = tolerance(); !t.is_null()) a["tolerance"] = t;
      if (!m_text->text().isEmpty()) a["text"] = m_text->text().toStdString();
      break;
    case Tool::HoleCallout:
    case Tool::HoleTable: a["precision"] = precision; break;
    case Tool::Note: a["text"] = m_text->text().isEmpty() ? tr("NOTE").toStdString() : m_text->text().toStdString(); break;
    case Tool::Datum: a["letter"] = (m_letter->text().isEmpty() ? nextLetter() : m_letter->text()).toUpper().toStdString(); break;
    case Tool::Frame: {
      a["characteristic"] = m_characteristic->currentData().toString().toStdString();
      bool number = false;
      const double v = m_value->text().toDouble(&number);
      a["value"] = m_value->text().isEmpty() ? json(0.1) : number ? json(v) : json(m_value->text().toStdString());
      if (m_zone->isChecked()) a["zone"] = "diameter";
      if (!m_material->currentData().toString().isEmpty()) a["material"] = m_material->currentData().toString().toStdString();
      json ds = json::array();
      for (QLineEdit* e : m_datums)
        if (!e->text().trimmed().isEmpty()) ds.push_back(e->text().trimmed().toUpper().toStdString());
      if (!ds.empty()) a["datums"] = ds;
      break;
    }
    case Tool::Surface:
      a["process"] = m_process->currentData().toString().toStdString();
      if (!m_value->text().isEmpty()) a["value"] = m_value->text().toStdString();
      break;
    case Tool::Ordinate:
    case Tool::Baseline:
    case Tool::Chain:
      a["type"] = m_tool == Tool::Ordinate ? "ordinate" : m_tool == Tool::Baseline ? "baseline" : "chain";
      a["precision"] = precision;
      a["axis"] = m_axis->currentData().toString().toStdString();
      break;
    default: break;
  }
  return a;
}

void SheetAnnotator::replan() {
  if (!m_canvas || m_tool == Tool::None) return;
  if (m_pending) {
    m_again = true;
    return;
  }
  json a = args();
  json picks = json::array();
  for (const auto& p : m_picks) picks.push_back(p.pick);
  if (!picks.empty()) a["picks"] = picks;
  if (m_tool == Tool::HoleTable && m_view.empty()) return;
  if (m_tool == Tool::Frame && m_picks.empty()) {  // a frame without a leader: where it was clicked
    const auto* f = m_canvas->frame(m_view);
    const Vec2 p = m_canvas->toPaper(m_mouse);
    a["place"] = js(f ? Vec2{p[0] - f->at[0], p[1] - f->at[1]} : p);
  }
  const Tool tool = m_tool;
  const bool ending = m_ending;
  const int needed = m_needed;
  m_pending = true;
  promptForStep();
  auto out = std::make_shared<json>();
  auto alive = m_alive;
  m_canvas->read(
      tr("Measuring the annotation"),
      [a, tool, ending, needed, out](const opad::Document& doc, const opad::Scene& scene, Progress) {
        try {
          namespace dr = opad::drawing;
          const auto resolved = [&]() {  // the picks made references
            json list = json::array();
            if (!a.contains("picks")) return list;
            const opad::Sheet* sheet = scene.sheet(a["sheet"].get<std::string>());
            if (!sheet) throw opad::Error("the sheet is gone");
            const auto frames = dr::layout(doc, scene, *sheet);
            const auto f = std::find_if(frames.begin(), frames.end(), [&](const dr::ViewFrame& x) { return x.id == a.value("view", ""); });
            if (f == frames.end()) throw opad::Error("the view is gone");
            for (const auto& p : a["picks"]) list.push_back(dr::pick_reference(doc, scene, *f, p));
            return list;
          };
          if (tool == Tool::Dimension) {
            try {
              *out = dr::plan_dimension(doc, scene, a);
            } catch (const std::exception&) {
              if (a["picks"].size() != 1) throw;
              *out = {{"picks", resolved()}};  // a point alone: the second pick tells what to measure
            }
            return;
          }
          const json picks = resolved();
          const size_t n = picks.size();
          const bool sets = tool == Tool::Ordinate || tool == Tool::Baseline || tool == Tool::Chain;
          const bool partial = tool == Tool::Reattach ? static_cast<int>(n) < needed
                                                      : (sets && (!ending || n < 2)) || (tool == Tool::CentreLine && n == 1 && picks[0].value("what", "") != "cylinder");
          if (tool == Tool::Reattach || partial) {
            *out = {{"picks", picks}};
            return;
          }
          json b = a;
          b.erase("picks");
          json refs = json::array();
          for (const auto& p : picks) refs.push_back(p["ref"]);
          if (!refs.empty()) b["refs"] = refs;
          if (sets && b.value("axis", "auto") == "auto") {  // both directions; the pointer chooses
            json choices = json::array();
            std::string first;
            for (const char* axis : {"horizontal", "vertical"}) {
              b["axis"] = axis;
              try {
                json m;
                const json op = dr::plan_item(doc, scene, b, &m);
                choices.push_back({{"type", axis}, {"op", op}, {"measured", m}});
              } catch (const std::exception& e) {
                if (first.empty()) first = e.what();
              }
            }
            if (choices.empty()) throw opad::Error(first);
            *out = {{"picks", picks}, {"choices", choices}};
            return;
          }
          json m;
          const json op = dr::plan_item(doc, scene, b, &m);
          *out = {{"picks", picks}, {"op", op}, {"measured", m}};
        } catch (const std::exception& e) {
          *out = {{"error", e.what()}};
        }
      },
      [this, alive, out](bool ok, const QString& error) {
        if (!*alive) return;
        m_pending = false;
        if (m_again) {
          m_again = false;
          replan();
          return;
        }
        if (m_tool == Tool::None) return;
        if (!ok) {
          if (error != "cancelled") emit message(error);
          promptForStep();
          return;
        }
        planned(*out);
      });
}

void SheetAnnotator::planned(const json& plan) {
  if (plan.contains("error")) {  // the last pick makes nothing: taken back
    emit message(QString::fromStdString(plan["error"].get<std::string>()));
    if (m_tool == Tool::HoleTable) m_view.clear();
    if (isSet(m_tool) && m_ending) m_ending = false;
    else if (!m_picks.empty()) m_picks.pop_back();
    if (m_picks.empty() && m_tool != Tool::Reattach && m_tool != Tool::HoleTable) m_view.clear();
    m_plan = nullptr;
    updatePreview();
    promptForStep();
    if (!m_picks.empty() && m_tool != Tool::Reattach) replan();
    return;
  }
  m_plan = plan;
  if ((m_tool == Tool::CentreMark || m_tool == Tool::CentreLine) && plan.contains("op")) return commit();
  if (m_tool == Tool::Frame && m_picks.empty() && plan.contains("op")) return commit();
  if (m_tool == Tool::Reattach && static_cast<int>(m_picks.size()) >= m_needed) return finishReattach();
  updatePreview();
  promptForStep();
}

std::pair<json, json> SheetAnnotator::current() {
  if (!placing() || !m_canvas) return {nullptr, nullptr};
  const opad::drawing::ViewFrame* f = m_canvas->frame(m_view);
  const Vec2 origin = f ? f->at : Vec2{0, 0};
  const Vec2 paper = m_canvas->toPaper(m_mouse), rel{paper[0] - origin[0], paper[1] - origin[1]};
  json def, measured;
  if (m_plan.contains("choices")) {
    const json& choices = m_plan["choices"];
    const auto find = [&](const std::string& type) -> const json* {
      for (const auto& c : choices)
        if (c.value("type", "") == type) return &c;
      return nullptr;
    };
    const json* chosen = nullptr;
    const std::string forced = (isSet(m_tool) ? m_axis : m_typeBox)->currentData().toString().toStdString();
    if (forced != "auto") chosen = find(forced);
    if (!chosen && isSet(m_tool)) {  // values along the side of the view the pointer is on
      const bool beside = f && (paper[0] < f->box[0] || paper[0] > f->box[2]) && paper[1] >= f->box[1] && paper[1] <= f->box[3];
      chosen = find(beside ? "vertical" : "horizontal");
    }
    if (!chosen && (find("angle") || find("diameter") || find("radius"))) chosen = &choices[0];
    if (!chosen) {  // the pointer beside the measured span reads it square to that side
      const json* aligned = find("aligned");
      const json g = aligned ? (*aligned)["measured"]["geometry"] : choices[0]["measured"].value("geometry", json::object());
      const Vec2 a = vec2(g.value("from", json())), b = vec2(g.value("to", json()));
      const bool xin = rel[0] >= std::min(a[0], b[0]) && rel[0] <= std::max(a[0], b[0]);
      const bool yin = rel[1] >= std::min(a[1], b[1]) && rel[1] <= std::max(a[1], b[1]);
      chosen = xin && !yin ? find("horizontal") : yin && !xin ? find("vertical") : aligned;
      if (!chosen) chosen = &choices[0];
    }
    m_type = chosen->value("type", "");
    def = (*chosen)["op"];
    measured = (*chosen)["measured"];
  } else {
    def = m_plan["op"];
    measured = m_plan["measured"];
    m_type = def.value("type", "");
  }
  const std::string kind = def.value("kind", "");
  if (kind == "note") def["at"] = js(rel);
  else if (kind == "hole_table") def["at"] = js(paper);
  else def["place"] = {{"text", js(rel)}};
  if ((kind == "datum" || kind == "surface") && measured.contains("line")) {  // the foot follows the pointer along the edge
    const Vec2 a = vec2(measured["line"][0]), b = vec2(measured["line"][1]);
    const double l = std::hypot(b[0] - a[0], b[1] - a[1]);
    if (l > 1e-9) {
      const Vec2 u{(b[0] - a[0]) / l, (b[1] - a[1]) / l};
      const double t = std::clamp((rel[0] - a[0]) * u[0] + (rel[1] - a[1]) * u[1], 0.1 * l, 0.9 * l);
      const Vec2 foot{a[0] + u[0] * t, a[1] + u[1] * t};
      Vec2 out{-u[1], u[0]};
      if ((rel[0] - foot[0]) * out[0] + (rel[1] - foot[1]) * out[1] < 0) out = {-out[0], -out[1]};
      measured["foot"] = {foot[0], foot[1]};
      measured["out"] = {out[0], out[1]};
    }
  }
  if (kind == "note" && measured.contains("tip")) def.erase("to");
  return {def, measured};
}

void SheetAnnotator::updatePreview() {
  if (!m_canvas) return;
  auto d = std::make_shared<Display>();
  const QColor sel = theme::current().sel;
  const uint32_t rgb = (static_cast<uint32_t>(sel.red()) << 16) | (static_cast<uint32_t>(sel.green()) << 8) | static_cast<uint32_t>(sel.blue());
  const int picked = d->layer({"Picked", opad::drawing::kInk, opad::drawing::LineType::Continuous, 0.6});
  for (const auto& p : m_picks) d->curve(picked, p.curve, rgb);
  const auto [def, measured] = current();
  const opad::Sheet* sheet = m_doc->scene.sheet(sheetId());
  if (!def.is_null() && sheet) {
    const opad::drawing::ViewFrame* f = m_canvas->frame(m_view);
    const size_t from = d->prims.size();
    try {
      opad::drawing::draw_item(*d, *sheet, def, measured, f ? f->at : Vec2{0, 0});
    } catch (const std::exception&) {
      d->prims.resize(from);
    }
    for (size_t i = from; i < d->prims.size(); ++i) d->prims[i].rgb = rgb;
  }
  m_canvas->setPreview(d->prims.empty() ? nullptr : std::shared_ptr<const Display>(d));
}

void SheetAnnotator::commit() {
  const auto [def, measured] = current();
  json op = def;
  if (op.is_null() && m_plan.contains("op")) op = m_plan["op"];  // placed where the plan put it (centre marks and lines)
  if (op.is_null() || !m_runner) return;
  const Tool tool = m_tool;
  auto alive = m_alive;
  m_picks.clear();
  m_plan = nullptr;
  m_ending = false;
  m_view.clear();
  updatePreview();
  promptForStep();
  m_runner("sheet_item", {{"op", op}}, [this, alive, tool](const json& out) {
    if (!*alive || out.is_null()) return;
    if (tool == Tool::Datum && m_tool == Tool::Datum) m_letter->setText(nextLetter());
    emit added(out.value("id", ""));
  });
}

void SheetAnnotator::finishReattach() {
  json refs = json::array();
  for (const auto& p : m_plan.value("picks", json::array())) refs.push_back(p["ref"]);
  const std::string item = m_item;
  auto alive = m_alive;
  cancel();
  if (!m_runner || refs.empty()) return;
  m_runner("sheet_edit", {{"target", item}, {"set", {{"refs", refs}}}}, [this, alive, item](const json& out) {
    if (*alive && !out.is_null()) emit added(item);
  });
}

void SheetAnnotator::finish() {
  if (isSet(m_tool) && !m_ending) {
    if (m_picks.size() < 2) return emit message(tr("Pick the origin and at least one feature first"));
    m_ending = true;
    replan();
    return;
  }
  if (placing()) commit();
}

void SheetAnnotator::clickAt(const QPointF& scene) {
  if (m_tool == Tool::None || !m_canvas) return;
  m_mouse = scene;
  if (m_pending) return;  // the last pick is still being measured
  if (m_tool == Tool::HoleTable && m_view.empty()) {
    m_view = m_canvas->viewUnder(scene);
    if (m_view.empty()) return emit message(tr("Click inside a view"));
    replan();
    return;
  }
  const auto pick = m_canvas->pickAt(scene);
  if (pick && wantsPick()) {
    const std::string view = m_tool == Tool::Reattach ? m_view : m_view.empty() ? pick->view : m_view;
    if (pick->view != view) return emit message(m_tool == Tool::Reattach ? tr("Pick on the annotation's view") : tr("Pick on the same view"));
    m_view = view;
    m_picks.push_back(*pick);
    if (m_tool == Tool::Dimension || m_tool == Tool::HoleCallout || m_tool == Tool::Datum || m_tool == Tool::Surface || m_tool == Tool::Note || m_tool == Tool::Frame)
      m_plan = nullptr;
    updatePreview();
    replan();
    return;
  }
  if (placing()) return commit();
  if (m_tool == Tool::Note && m_picks.empty()) {  // a note where it was clicked, on the view there or on the sheet
    if (m_text->text().trimmed().isEmpty()) {
      m_text->setFocus();
      return emit message(tr("Type the note's text in the bar first"));
    }
    const std::string view = m_canvas->viewUnder(scene);
    const auto* f = view.empty() ? nullptr : m_canvas->frame(view);
    const Vec2 p = m_canvas->toPaper(scene);
    json a = {{"sheet", sheetId()}, {"kind", "note"}, {"text", m_text->text().toStdString()}, {"at", js(f ? Vec2{p[0] - f->at[0], p[1] - f->at[1]} : p)}};
    if (f) a["view"] = view;
    if (m_runner) {
      auto alive = m_alive;
      m_runner("sheet_item", a, [this, alive](const json& out) {
        if (*alive && !out.is_null()) emit added(out.value("id", ""));
      });
    }
    return;
  }
  if (m_tool == Tool::Frame && m_picks.empty()) {  // a frame without a leader
    m_view = m_canvas->viewUnder(scene);
    if (m_view.empty()) return emit message(tr("Click inside a view"));
    replan();
  }
}

void SheetAnnotator::moveTo(const QPointF& scene) {
  m_mouse = scene;
  if (placing()) updatePreview();
}

// ---------------------------------------------------------------- input
bool SheetAnnotator::mousePress(QMouseEvent* e, const QPointF& scene) {
  if (m_tool == Tool::None) return false;
  if (e->button() == Qt::LeftButton) clickAt(scene);
  else if (e->button() == Qt::RightButton) {
    if (isSet(m_tool) && !m_ending) finish();
    else {
      QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      keyPress(&esc);
    }
  }
  return true;
}

bool SheetAnnotator::mouseMove(QMouseEvent*, const QPointF& scene) {
  if (m_tool == Tool::None) return false;
  moveTo(scene);
  return false;  // the canvas still shows the snap under the pointer
}

bool SheetAnnotator::mouseRelease(QMouseEvent*, const QPointF&) { return m_tool != Tool::None; }

bool SheetAnnotator::wantsKey(QKeyEvent* e) {
  const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
  if (m_tool != Tool::None)
    return e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || (e->key() == Qt::Key_Tab && plain);
  return plain && !(e->modifiers() & Qt::ShiftModifier) && (e->key() == Qt::Key_D || e->key() == Qt::Key_T);
}

bool SheetAnnotator::keyPress(QKeyEvent* e) {
  if (m_tool == Tool::None) {
    if (!wantsKey(e)) return false;
    start(e->key() == Qt::Key_D ? Tool::Dimension : Tool::Note);
    return true;
  }
  switch (e->key()) {
    case Qt::Key_Escape:
      if (m_ending) {
        m_ending = false;
        m_plan = nullptr;
      } else if (!m_picks.empty() && !(m_tool == Tool::HoleTable)) {
        m_picks.pop_back();
        m_plan = nullptr;
        if (m_picks.empty() && m_tool != Tool::Reattach) m_view.clear();
        else if (m_tool != Tool::Reattach) replan();
      } else if (m_tool == Tool::HoleTable && !m_view.empty()) {
        m_view.clear();
        m_plan = nullptr;
      } else {
        cancel();
        return true;
      }
      updatePreview();
      promptForStep();
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter: finish(); return true;
    case Qt::Key_Tab:
      for (const auto& [w, tools] : m_fields)
        if (w->isVisible()) {
          w->setFocus();
          if (auto* edit = w->findChild<QLineEdit*>()) edit->setFocus();
          else if (auto* own = qobject_cast<QLineEdit*>(w)) own->setFocus();
          return true;
        }
      return true;
    default: return false;
  }
}
