#include "SimulatePrint.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

#include "I18n.hpp"
#include "opad/sim/printing.hpp"

using opad::json;

namespace {
struct Direction {
  const char* label;
  opad::Vec3 up;
};
const std::vector<Direction>& directions() {
  static const std::vector<Direction> list = {{QT_TRANSLATE_NOOP("PrintDialog", "Along +Z (as modelled)"), {0, 0, 1}}, {QT_TRANSLATE_NOOP("PrintDialog", "Along −Z (upside down)"), {0, 0, -1}},
                                              {QT_TRANSLATE_NOOP("PrintDialog", "Along +Y"), {0, 1, 0}},               {QT_TRANSLATE_NOOP("PrintDialog", "Along −Y"), {0, -1, 0}},
                                              {QT_TRANSLATE_NOOP("PrintDialog", "Along +X"), {1, 0, 0}},               {QT_TRANSLATE_NOOP("PrintDialog", "Along −X"), {-1, 0, 0}}};
  return list;
}
struct Pattern {
  const char* id;
  const char* label;
};
const std::vector<Pattern>& patterns() {
  static const std::vector<Pattern> list = {{"grid", QT_TRANSLATE_NOOP("PrintDialog", "Grid")},           {"rectilinear", QT_TRANSLATE_NOOP("PrintDialog", "Rectilinear (lines)")},
                                            {"triangles", QT_TRANSLATE_NOOP("PrintDialog", "Triangles")}, {"honeycomb", QT_TRANSLATE_NOOP("PrintDialog", "Honeycomb")},
                                            {"cubic", QT_TRANSLATE_NOOP("PrintDialog", "Cubic")},         {"gyroid", QT_TRANSLATE_NOOP("PrintDialog", "Gyroid")},
                                            {"lightning", QT_TRANSLATE_NOOP("PrintDialog", "Lightning")}, {"concentric", QT_TRANSLATE_NOOP("PrintDialog", "Concentric")}};
  return list;
}
QString familyLabel(const std::string& family) {
  if (family == "solid") return PrintDialog::tr("solid (as the skins)");
  if (family == "lines") return PrintDialog::tr("Rectilinear (lines)");
  for (const auto& p : patterns())
    if (family == p.id) return PrintDialog::tr(p.label);
  return QString::fromStdString(family);
}
QDoubleSpinBox* spin(double lo, double hi, double step, int decimals, const QString& suffix, const char* name) {
  auto* s = new QDoubleSpinBox;
  s->setRange(lo, hi), s->setSingleStep(step), s->setDecimals(decimals), s->setSuffix(suffix);
  s->setObjectName(name);
  return s;
}
QSpinBox* count(int hi, const char* name) {
  auto* s = new QSpinBox;
  s->setRange(0, hi);
  s->setObjectName(name);
  return s;
}
}  // namespace

PrintDialog::PrintDialog(const json& print, QWidget* parent) : QDialog(parent) {
  setObjectName("printDialog");
  setWindowTitle(tr("Printed part"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  m_on = new QCheckBox(tr("The bodies are 3D printed (FFF / FDM)"));
  m_on->setObjectName("printEnabled");
  v->addWidget(m_on);
  auto* intro = new QLabel(tr("Walls, top and bottom skins and infill get the stiffness and strength of printed roads: along a road, across it and "
                              "between layers. The study shows where it fails first and how."));
  intro->setWordWrap(true);
  intro->setObjectName("hint");
  v->addWidget(intro);
  m_form = new QWidget;
  auto* f = new QFormLayout(m_form);
  f->setContentsMargins(0, 8, 0, 0);
  auto* import = new QPushButton(tr("Import slicer profile…"));
  import->setObjectName("printImport");
  import->setToolTip(tr("PrusaSlicer .ini, OrcaSlicer or Bambu Studio .json, Cura .cfg, a .3mf project or a G-code file"));
  f->addRow(import);
  m_material = new QComboBox;
  m_material->setObjectName("printMaterial");
  for (const auto& fil : opad::sim::filaments()) m_material->addItem(QString::fromStdString(fil.name), QString::fromStdString(fil.id));
  f->addRow(tr("Material"), m_material);
  m_up = new QComboBox;
  m_up->setObjectName("printDirection");
  for (const auto& d : directions()) m_up->addItem(tr(d.label));
  f->addRow(tr("Build direction"), m_up);
  m_layer = spin(0.02, 1.0, 0.04, 2, " mm", "printLayer");
  f->addRow(tr("Layer height"), m_layer);
  m_line = spin(0.1, 2.0, 0.05, 2, " mm", "printLine");
  f->addRow(tr("Line width"), m_line);
  m_walls = count(50, "printWalls");
  f->addRow(tr("Walls"), m_walls);
  m_top = count(200, "printTop");
  f->addRow(tr("Top layers"), m_top);
  m_bottom = count(200, "printBottom");
  f->addRow(tr("Bottom layers"), m_bottom);
  m_infill = spin(0, 100, 5, 0, " %", "printInfill");
  f->addRow(tr("Infill"), m_infill);
  m_pattern = new QComboBox;
  m_pattern->setObjectName("printPattern");
  for (const auto& p : patterns()) m_pattern->addItem(tr(p.label), QString(p.id));
  f->addRow(tr("Infill pattern"), m_pattern);
  m_angle = spin(-180, 180, 15, 0, "°", "printAngle");
  f->addRow(tr("Infill angle"), m_angle);
  m_flow = spin(0.5, 1.5, 0.01, 2, "", "printFlow");
  f->addRow(tr("Flow"), m_flow);
  v->addWidget(m_form);
  m_note = new QLabel;
  m_note->setObjectName("printNote");
  m_note->setWordWrap(true);
  v->addWidget(m_note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  v->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(import, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import slicer profile"), {}, tr("Slicer profiles (*.ini *.json *.cfg *.3mf *.gcode);;All files (*)"));
    if (path.isEmpty()) return;
    try {
      importProfile(path);
    } catch (const std::exception& e) {
      m_note->setText(i18n::t(QString::fromStdString(e.what())));
    }
  });
  load(print);
  for (QComboBox* c : {m_material, m_up, m_pattern}) connect(c, &QComboBox::currentIndexChanged, this, &PrintDialog::update);
  for (QDoubleSpinBox* s : {m_layer, m_line, m_infill, m_angle, m_flow}) connect(s, &QDoubleSpinBox::valueChanged, this, &PrintDialog::update);
  for (QSpinBox* s : {m_walls, m_top, m_bottom}) connect(s, &QSpinBox::valueChanged, this, &PrintDialog::update);
  connect(m_on, &QCheckBox::toggled, this, &PrintDialog::update);
  update();
}

void PrintDialog::load(const json& print) {
  const bool on = print.is_object();
  m_on->setChecked(on);
  json p = on ? print : json::object();
  // Defaults, then what the study has (a profile it names read first, as the study would).
  opad::sim::PrintSettings s;
  try {
    s = opad::sim::print_settings(p);
  } catch (const std::exception&) {
  }
  m_kept = json::object();
  for (const auto& [k, val] : p.items())
    if (k == "bodies" || (k == "material" && val.is_object())) m_kept[k] = val;
  m_source = QString::fromStdString(p.value("source", std::string()));
  const int mi = m_material->findData(QString::fromStdString(s.material.id));
  m_material->setCurrentIndex(std::max(0, mi));
  int di = 0;
  for (size_t i = 0; i < directions().size(); ++i)
    if (directions()[i].up[0] * s.up[0] + directions()[i].up[1] * s.up[1] + directions()[i].up[2] * s.up[2] > 0.999) di = int(i);
  m_up->setCurrentIndex(di);
  m_layer->setValue(s.layer), m_line->setValue(s.line), m_flow->setValue(s.flow);
  m_walls->setValue(s.walls), m_top->setValue(s.top), m_bottom->setValue(s.bottom);
  m_infill->setValue(s.density * 100), m_angle->setValue(s.angle);
  int pi = m_pattern->findData(QString::fromStdString(s.pattern));
  if (pi < 0) {  // a slicer's own name for one of the models
    m_pattern->addItem(QString::fromStdString(s.pattern), QString::fromStdString(s.pattern));
    pi = m_pattern->count() - 1;
  }
  m_pattern->setCurrentIndex(pi);
}

void PrintDialog::importProfile(const QString& path) {
  json p = opad::sim::read_profile(opad::path_from_utf8(path.toStdString()));
  if (m_kept.contains("bodies")) p["bodies"] = m_kept["bodies"];
  p["source"] = QFileInfo(path).fileName().toStdString();
  load(p);
  m_on->setChecked(true);
  update();
}

json PrintDialog::print() const {
  if (!m_on->isChecked()) return nullptr;
  json p = m_kept;
  if (!p.contains("material") || !p["material"].is_object()) p["material"] = m_material->currentData().toString().toStdString();
  else p["material"]["base"] = m_material->currentData().toString().toStdString();
  p["build_direction"] = directions()[size_t(std::max(0, m_up->currentIndex()))].up;
  p["layer_height"] = m_layer->value(), p["line_width"] = m_line->value(), p["flow"] = m_flow->value();
  p["walls"] = m_walls->value(), p["top_layers"] = m_top->value(), p["bottom_layers"] = m_bottom->value();
  p["infill"] = m_infill->value(), p["pattern"] = m_pattern->currentData().toString().toStdString(), p["infill_angle"] = m_angle->value();
  if (!m_source.isEmpty()) p["source"] = m_source.toStdString();
  return p;
}

void PrintDialog::update() {
  m_form->setEnabled(m_on->isChecked());
  if (!m_on->isChecked()) {
    m_note->setText(tr("Not printed: each body is solid, with its material's properties."));
    return;
  }
  try {
    const opad::sim::PrintSettings s = opad::sim::print_settings(print());
    QString text = tr("Walls %1 mm · top %2 mm · bottom %3 mm · infill model: %4 · roads fill %5 % of their box")
                       .arg(s.wall_thickness(), 0, 'g', 3)
                       .arg(s.top_thickness(), 0, 'g', 3)
                       .arg(s.bottom_thickness(), 0, 'g', 3)
                       .arg(familyLabel(s.family))
                       .arg(std::lround(100 * s.fill()));
    if (!m_source.isEmpty()) text = tr("From %1").arg(m_source) + "\n" + text;
    m_note->setText(text);
  } catch (const std::exception& e) {
    m_note->setText(i18n::t(QString::fromStdString(e.what())));
  }
}
