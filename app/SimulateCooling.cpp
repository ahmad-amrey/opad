#include "SimulateCooling.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

#include "AppDocument.hpp"
#include "I18n.hpp"
#include "Theme.hpp"
#include "opad/materials.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/cfd.hpp"

using opad::json;
namespace air = opad::sim::air;

namespace {

QString num(double v, int decimals = 1) { return QString::number(v, 'f', decimals); }

QString nameOf(const opad::Scene& s, const std::string& id) {
  const opad::Node* n = s.node(id);
  return n ? QString::fromStdString(n->name) : QString::fromStdString(id);
}

// The ways a fan can blow, as the table offers them.
const std::vector<std::pair<QString, opad::Vec3>>& ways() {
  static const std::vector<std::pair<QString, opad::Vec3>> w = {{"+X", {1, 0, 0}},  {"-X", {-1, 0, 0}}, {"+Y", {0, 1, 0}},
                                                                {"-Y", {0, -1, 0}}, {"+Z", {0, 0, 1}},  {"-Z", {0, 0, -1}}};
  return w;
}

QLabel* text(const QString& s, QWidget* parent, const char* object = "secondary") {
  auto* l = new QLabel(s, parent);
  l->setWordWrap(true);
  l->setObjectName(object);
  return l;
}

}  // namespace

QString CoolingAssistant::studyName() { return tr("Cooling (air solved)"); }
QString CoolingAssistant::sweepName() { return tr("Cooling: vent sweep"); }

CoolingAssistant::CoolingAssistant(Hooks hooks, QWidget* parent) : QWidget(parent, Qt::Window), m_hooks(std::move(hooks)) {
  setObjectName("coolingAssistant");
  setWindowTitle(tr("Cooling assistant"));
  setAttribute(Qt::WA_StyledBackground);
  resize(980, 640);
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  m_steps = new QListWidget(this);
  m_steps->setObjectName("referenceList");
  m_steps->setFixedWidth(230);
  m_steps->setFrameShape(QFrame::NoFrame);
  for (const QString& s : {tr("1  The box"), tr("2  Heat"), tr("3  Air"), tr("4  Run"), tr("5  Best vents")}) m_steps->addItem(s);
  h->addWidget(m_steps);
  auto* rule = new QFrame(this);
  rule->setFixedWidth(1);
  rule->setObjectName("referenceRule");
  h->addWidget(rule);
  auto* right = new QWidget(this);
  auto* rv = new QVBoxLayout(right);
  rv->setContentsMargins(20, 16, 20, 16);
  m_pages = new QStackedWidget(right);
  rv->addWidget(m_pages, 1);
  auto* nav = new QHBoxLayout();
  auto* back = new QPushButton(tr("Back"), right);
  auto* next = new QPushButton(tr("Next"), right);
  next->setObjectName("coolingNext");
  nav->addStretch(1);
  nav->addWidget(back);
  nav->addWidget(next);
  rv->addLayout(nav);
  h->addWidget(right, 1);
  auto restyle = [rule] { rule->setStyleSheet(QString("background: %1;").arg(theme::css(theme::current().line))); };
  restyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, restyle);
  connect(m_steps, &QListWidget::currentRowChanged, this, &CoolingAssistant::showStep);
  connect(back, &QPushButton::clicked, this, [this] { m_steps->setCurrentRow(std::max(0, m_steps->currentRow() - 1)); });
  connect(next, &QPushButton::clicked, this, [this] { m_steps->setCurrentRow(std::min(steps() - 1, m_steps->currentRow() + 1)); });
  auto page = [this](const QString& title, const QString& intro) {
    auto* w = new QWidget(m_pages);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    auto* t = new QLabel(title, w);
    t->setObjectName("panelTitle");
    v->addWidget(t);
    v->addWidget(text(intro, w));
    m_pages->addWidget(w);
    return v;
  };

  // ---- 1. the box
  {
    QVBoxLayout* v = page(tr("The box"), tr("The air is solved inside the box and in a margin of the room around it: its holes are the vents. The box is "
                                            "the smallest body that holds the parts that make heat; pick another if it is not the one."));
    m_engine = text(QString(), this);
    v->addWidget(m_engine);
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Box:"), this));
    m_box = new QComboBox(this);
    m_box->setObjectName("coolingBox");
    row->addWidget(m_box, 1);
    v->addLayout(row);
    v->addWidget(text(tr("Inside it, taking part (the board, chips, heatsink, connectors):"), this, "sectionHeader"));
    m_inside = new QListWidget(this);
    v->addWidget(m_inside, 1);
    m_example = new QPushButton(tr("Build the example: a board in a vented box"), this);
    m_example->setObjectName("coolingExample");
    m_example->setToolTip(tr("A 110 x 80 x 40 mm ABS box with a 30 mm intake fan, three exhaust slots at the height vent_z, a board, a 4 W chip "
                             "under a finned heatsink and a 0.5 W power chip: everything set up to run and to sweep."));
    v->addWidget(m_example);
    connect(m_box, &QComboBox::currentIndexChanged, this, [this] { boxChanged(); });
    connect(m_example, &QPushButton::clicked, this, [this] { buildExample(); });
  }
  // ---- 2. heat
  {
    QVBoxLayout* v = page(tr("Heat"), tr("Tick each part that makes heat and type its power. Tick Board for a printed circuit board: its copper "
                                         "spreads heat along it far better than through it (give its copper layers). A chip with no material is "
                                         "taken as silicon."));
    m_heat = new QTableWidget(0, 4, this);
    m_heat->setObjectName("coolingHeat");
    m_heat->setHorizontalHeaderLabels({tr("Part"), tr("Power (W)"), tr("Board"), tr("Copper layers")});
    m_heat->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_heat->verticalHeader()->hide();
    v->addWidget(m_heat, 1);
  }
  // ---- 3. air
  {
    QVBoxLayout* v = page(tr("Air"), tr("A fan is a block where it sits, as big as it, or the heatsink it blows on. Choose the fan (or Custom, with "
                                        "its free flow and shut-off pressure from its datasheet) and the way it blows. Without fans the air moves "
                                        "only by rising where it is warm: put vents low and high."));
    m_withFans = new QCheckBox(tr("Fans move the air (untick: vents only, warm air rising)"), this);
    m_withFans->setChecked(true);
    v->addWidget(m_withFans);
    m_fans = new QTableWidget(0, 5, this);
    m_fans->setObjectName("coolingFans");
    m_fans->setHorizontalHeaderLabels({tr("On"), tr("Fan"), tr("Free flow (m³/h)"), tr("Shut-off (Pa)"), tr("Blows")});
    m_fans->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fans->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_fans->verticalHeader()->hide();
    v->addWidget(m_fans, 1);
    auto* row = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add a fan"), this);
    auto* remove = new QPushButton(tr("Remove the fan"), this);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    v->addLayout(row);
    connect(m_withFans, &QCheckBox::toggled, m_fans, &QWidget::setEnabled);
    connect(add, &QPushButton::clicked, this, [this] { addFanRow(m_parts.empty() ? std::string() : m_parts.front(), json("40x10"), {1, 0, 0}); });
    connect(remove, &QPushButton::clicked, this, [this] {
      if (m_fans->currentRow() >= 0) m_fans->removeRow(m_fans->currentRow());
    });
  }
  // ---- 4. run
  {
    QVBoxLayout* v = page(tr("Run"), tr("Quick takes a few minutes and is for comparing; Normal is the answer to trust; Fine checks it. The parts' "
                                        "radiation to each other and to the room is counted too."));
    auto* g = new QGridLayout();
    g->addWidget(new QLabel(tr("Room temperature:"), this), 0, 0);
    m_ambient = new QDoubleSpinBox(this);
    m_ambient->setRange(-40, 80);
    m_ambient->setValue(25);
    m_ambient->setSuffix(" °C");
    g->addWidget(m_ambient, 0, 1);
    g->addWidget(new QLabel(tr("Quality:"), this), 1, 0);
    m_quality = new QComboBox(this);
    m_quality->addItem(tr("Quick (coarse cells)"), "quick");
    m_quality->addItem(tr("Normal"), "normal");
    m_quality->addItem(tr("Fine (slow)"), "fine");
    m_quality->setCurrentIndex(1);
    g->addWidget(m_quality, 1, 1);
    m_radiation = new QCheckBox(tr("Radiation"), this);
    m_radiation->setChecked(true);
    g->addWidget(m_radiation, 2, 1);
    g->setColumnStretch(2, 1);
    v->addLayout(g);
    m_runButton = new QPushButton(tr("Run"), this);
    m_runButton->setObjectName("coolingRun");
    v->addWidget(m_runButton, 0, Qt::AlignLeft);
    m_status = text(QString(), this);
    v->addWidget(m_status);
    m_result = new QLabel(this);
    m_result->setObjectName("coolingResult");
    m_result->setTextFormat(Qt::RichText);
    m_result->setWordWrap(true);
    m_result->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_result);
    v->addWidget(scroll, 1);
    m_resultButtons = new QWidget(this);
    auto* row = new QHBoxLayout(m_resultButtons);
    row->setContentsMargins(0, 0, 0, 0);
    auto* temps = new QPushButton(tr("Temperatures"), m_resultButtons);
    auto* speed = new QPushButton(tr("Air speed"), m_resultButtons);
    auto* inside = new QPushButton(tr("See inside the box"), m_resultButtons);
    inside->setCheckable(true);
    row->addWidget(temps);
    row->addWidget(speed);
    row->addWidget(inside);
    row->addStretch(1);
    m_resultButtons->hide();
    v->addWidget(m_resultButtons);
    connect(m_runButton, &QPushButton::clicked, this, [this] { startRun(); });
    connect(temps, &QPushButton::clicked, this, [this] { m_hooks.showField("temperature"); });
    connect(speed, &QPushButton::clicked, this, [this] { m_hooks.showField("air_speed"); });
    connect(inside, &QPushButton::toggled, this, [this](bool on) {
      const std::string box = m_box->currentData().toString().toStdString();
      if (!box.empty()) m_hooks.setVisible(box, !on);
    });
  }
  // ---- 5. the best vents
  {
    QVBoxLayout* v = page(tr("The best place for the vents"),
                          tr("Make what you want to try a parameter: a vent's position or size, a fan's place. Edit the feature that makes "
                             "it (double-click it in the timeline) and type a parameter's name instead of its number, e.g. vent_z; "
                             "Parameters on the Design ribbon makes and lists them. Then choose it here with a range: each value is "
                             "run, the coolest design is found and you can use it."));
    auto* g = new QGridLayout();
    g->addWidget(new QLabel(tr("Parameter:"), this), 0, 0);
    m_param = new QComboBox(this);
    m_param->setObjectName("coolingParam");
    g->addWidget(m_param, 0, 1, 1, 3);
    g->addWidget(new QLabel(tr("From:"), this), 1, 0);
    m_from = new QDoubleSpinBox(this);
    m_from->setRange(-1e5, 1e5);
    g->addWidget(m_from, 1, 1);
    g->addWidget(new QLabel(tr("To:"), this), 1, 2);
    m_to = new QDoubleSpinBox(this);
    m_to->setRange(-1e5, 1e5);
    g->addWidget(m_to, 1, 3);
    g->addWidget(new QLabel(tr("Values:"), this), 2, 0);
    m_count = new QSpinBox(this);
    m_count->setRange(2, 30);
    m_count->setValue(5);
    g->addWidget(m_count, 2, 1);
    g->addWidget(new QLabel(tr("Then closer in:"), this), 2, 2);
    m_refine = new QSpinBox(this);
    m_refine->setRange(0, 10);
    m_refine->setValue(2);
    m_refine->setToolTip(tr("More runs between the best value's neighbours (golden section)"));
    g->addWidget(m_refine, 2, 3);
    g->addWidget(new QLabel(tr("Keep cool:"), this), 3, 0);
    m_objective = new QComboBox(this);
    g->addWidget(m_objective, 3, 1, 1, 3);
    m_screen = new QCheckBox(tr("Compare at Quick quality, then run the best at the Run page's quality"), this);
    m_screen->setChecked(true);
    g->addWidget(m_screen, 4, 1, 1, 3);
    g->setColumnStretch(1, 1);
    g->setColumnStretch(3, 1);
    v->addLayout(g);
    m_sweepButton = new QPushButton(tr("Find the best"), this);
    m_sweepButton->setObjectName("coolingSweep");
    v->addWidget(m_sweepButton, 0, Qt::AlignLeft);
    m_sweepStatus = text(QString(), this);
    v->addWidget(m_sweepStatus);
    m_points = new QTableWidget(0, 3, this);
    m_points->setObjectName("coolingPoints");
    m_points->setHorizontalHeaderLabels({tr("Value"), tr("Hottest (°C)"), tr("Note")});
    m_points->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_points->verticalHeader()->hide();
    m_points->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->addWidget(m_points, 1);
    m_best = new QPushButton(tr("Use the best"), this);
    m_best->setObjectName("coolingBest");
    m_best->setEnabled(false);
    v->addWidget(m_best, 0, Qt::AlignLeft);
    connect(m_param, &QComboBox::currentIndexChanged, this, [this] {
      // A range around the value it has now.
      const AppDocument* d = m_hooks.document();
      const opad::Param* p = d ? d->scene.param(m_param->currentText().toStdString()) : nullptr;
      if (!p) return;
      const double now = p->angle ? p->value * 180 / M_PI : p->value;
      const double span = std::max(std::fabs(now) * 0.5, 1.0);
      const QString unit = p->angle ? " deg" : p->len == 1 ? " mm" : "";
      for (QDoubleSpinBox* s : {m_from, m_to}) s->setSuffix(unit);
      m_from->setValue(now - span), m_to->setValue(now + span);
    });
    connect(m_sweepButton, &QPushButton::clicked, this, [this] { startSweep(); });
    connect(m_best, &QPushButton::clicked, this, [this] {
      for (const auto& [name, value] : m_bestParams.items()) {
        const AppDocument* d = m_hooks.document();
        const opad::Param* p = d ? d->scene.param(name) : nullptr;
        const QString unit = p && p->angle ? " deg" : p && p->len == 1 ? " mm" : "";
        m_hooks.write("param", {{"name", name}, {"expr", (num(value.get<double>(), 3) + unit).toStdString()}}, tr("Use the best design"));
      }
      m_sweepStatus->setText(tr("The best design is now the model's. Run it on the Run page to see it in full."));
    });
  }
  m_steps->setCurrentRow(0);
}

int CoolingAssistant::step() const { return m_steps->currentRow(); }
int CoolingAssistant::steps() const { return m_steps->count(); }

void CoolingAssistant::open(int at) {
  reload();
  if (at >= 0) m_steps->setCurrentRow(std::clamp(at, 0, steps() - 1));
  QWidget::show();
  raise();
  activateWindow();
}

void CoolingAssistant::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

void CoolingAssistant::showStep(int i) {
  if (i >= 0 && i < m_pages->count()) m_pages->setCurrentIndex(i);
}

// ---------------------------------------------------------------- the pages from the document
void CoolingAssistant::reload() {
  AppDocument* d = m_hooks.document();
  const bool foam = opad::sim::openfoam().found();
  m_engine->setText(foam ? QString()
                         : tr("OpenFOAM is not installed: the air cannot be solved. Install it (Ubuntu: apt install openfoam; Windows: "
                              "OpenCFD's build) or set OPAD_OPENFOAM to its directory, then open this again."));
  m_engine->setVisible(!foam);
  m_runButton->setEnabled(foam);
  m_sweepButton->setEnabled(foam);
  const QSignalBlocker quiet(m_box);
  m_box->clear();
  m_candidates.clear();
  if (!d || !d->hasDocument) return;
  const opad::Scene& s = d->scene;
  // The box: the cooling study's, else the one the heat sources (or every other body) are found in.
  std::string box;
  if (const opad::Study* st = s.study(studyId())) box = st->def.value("settings", json::object()).value("cfd", json::object()).value("enclosure", std::string());
  std::vector<std::string> heated;
  for (const auto& l : s.loads)
    if (l.kind == "heat")
      for (const auto& r : l.refs) heated.push_back(r.body);
  std::vector<std::string> solids;
  for (const auto& id : s.all_bodies())
    if (const opad::Node* n = s.node(id); n && !n->body_missing && n->representation == "solid" && s.effectively_visible(id)) solids.push_back(id);
  try {
    if (box.empty() && !heated.empty()) box = opad::sim::find_enclosure(d->doc, s, heated).enclosure;
    if (box.empty()) {  // the body that holds the most others
      size_t most = 0;
      for (const auto& id : solids)
        if (const size_t n = opad::sim::find_enclosure(d->doc, s, {}, id).inside.size(); n > most) most = n, box = id;
    }
  } catch (const std::exception&) {
  }
  for (const auto& id : solids) {
    m_candidates.push_back(id);
    m_box->addItem(nameOf(s, id), QString::fromStdString(id));
  }
  m_box->setCurrentIndex(std::max(0, int(std::find(m_candidates.begin(), m_candidates.end(), box) - m_candidates.begin())));
  if (box.empty()) m_box->setCurrentIndex(-1);
  m_example->setVisible(solids.empty());
  boxChanged();
  // The sweep's parameters and what it keeps cool.
  {
    const QSignalBlocker q(m_param);
    m_param->clear();
    for (const auto& p : s.params) m_param->addItem(QString::fromStdString(p.name));
    if (const opad::Study* sw = s.study(sweepId())) {
      const json ps = sw->def.value("settings", json::object()).value("params", json::array());
      if (!ps.empty()) m_param->setCurrentText(QString::fromStdString(ps[0].value("name", "")));
    }
  }
  emit m_param->currentIndexChanged(m_param->currentIndex());
  if (const opad::Study* sw = s.study(sweepId()); sw && sw->result.is_object() && !sw->result.empty()) showSweep(sw->result);
}

void CoolingAssistant::boxChanged() {
  AppDocument* d = m_hooks.document();
  m_parts.clear();
  m_inside->clear();
  m_heat->setRowCount(0);
  m_fans->setRowCount(0);
  if (!d || !d->hasDocument || m_box->currentIndex() < 0) return;
  const opad::Scene& s = d->scene;
  const std::string box = m_box->currentData().toString().toStdString();
  try {
    m_parts = opad::sim::find_enclosure(d->doc, s, {}, box).inside;
  } catch (const std::exception&) {
  }
  // What the cooling study set, to show it again: heat per body, boards, fans.
  std::map<std::string, double> watts;
  std::map<std::string, int> boards;
  json fans = json::array();
  const opad::Study* st = s.study(studyId());
  const json settings = st ? st->def.value("settings", json::object()) : json::object();
  const json mats = settings.value("materials", json::object());
  for (const auto& [id, m] : mats.items())
    if (m.is_object() && m.contains("pcb")) boards[id] = m["pcb"].value("layers", 4);
  for (const auto& l : s.loads) {
    if (l.load_case != kCase) continue;
    if (l.kind == "heat")
      for (const auto& r : l.refs) watts[r.body] += l.def.value("value", 0.0) / double(l.refs.size());
    if (l.kind == "fan" && !l.refs.empty()) fans.push_back({{"body", l.refs.front().body}, {"fan", l.def.value("fan", json("80x25"))}, {"vector", l.def.value("vector", opad::Vec3{1, 0, 0})}});
  }
  m_heat->setRowCount(int(m_parts.size()));
  for (size_t i = 0; i < m_parts.size(); ++i) {
    const std::string& id = m_parts[i];
    const QString name = nameOf(s, id);
    m_inside->addItem(name);
    auto* part = new QTableWidgetItem(name);
    part->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
    part->setCheckState(watts.count(id) ? Qt::Checked : Qt::Unchecked);
    part->setData(Qt::UserRole, QString::fromStdString(id));
    m_heat->setItem(int(i), 0, part);
    auto* w = new QDoubleSpinBox(m_heat);
    w->setRange(0, 1000);
    w->setDecimals(2);
    w->setValue(watts.count(id) ? watts[id] : 1.0);
    m_heat->setCellWidget(int(i), 1, w);
    auto* board = new QCheckBox(m_heat);
    board->setChecked(boards.count(id) > 0);
    m_heat->setCellWidget(int(i), 2, board);
    auto* layers = new QSpinBox(m_heat);
    layers->setRange(1, 32);
    layers->setValue(boards.count(id) ? boards[id] : 4);
    m_heat->setCellWidget(int(i), 3, layers);
  }
  m_withFans->setChecked(!st || !settings.value("cfd", json::object()).value("buoyancy", false) || !fans.empty());
  for (const auto& f : fans) addFanRow(f["body"].get<std::string>(), f["fan"], f["vector"].get<opad::Vec3>());
  if (fans.empty() && m_withFans->isChecked()) {
    // A first guess: a body called fan, else none yet.
    for (const auto& id : m_parts)
      if (nameOf(s, id).contains("fan", Qt::CaseInsensitive)) addFanRow(id, json("40x10"), {1, 0, 0});
  }
  // What to keep cool: the hottest part, or one of them.
  const QSignalBlocker q(m_objective);
  m_objective->clear();
  m_objective->addItem(tr("The hottest part"), QString());
  for (const auto& id : m_parts) m_objective->addItem(nameOf(s, id), QString::fromStdString(id));
}

void CoolingAssistant::addFanRow(const std::string& body, const json& fan, const opad::Vec3& way) {
  AppDocument* d = m_hooks.document();
  const int r = m_fans->rowCount();
  m_fans->insertRow(r);
  auto* on = new QComboBox(m_fans);
  for (const auto& id : m_parts) on->addItem(d ? nameOf(d->scene, id) : QString(), QString::fromStdString(id));
  on->setCurrentIndex(std::max(0, int(std::find(m_parts.begin(), m_parts.end(), body) - m_parts.begin())));
  m_fans->setCellWidget(r, 0, on);
  auto* model = new QComboBox(m_fans);
  for (const auto& f : air::fans()) model->addItem(QString::fromStdString(f.name), QString::fromStdString(f.id));
  model->addItem(tr("Custom"), QString());
  auto* flow = new QDoubleSpinBox(m_fans);
  flow->setRange(0.1, 2000);
  auto* pressure = new QDoubleSpinBox(m_fans);
  pressure->setRange(0.1, 5000);
  if (fan.is_string()) {
    model->setCurrentIndex(std::max(0, model->findData(QString::fromStdString(fan.get<std::string>()))));
  } else {
    model->setCurrentIndex(model->count() - 1);
  }
  const air::Fan spec = [&] {
    try {
      return air::fan_from(fan);
    } catch (const std::exception&) {
      return air::fan_from(json("40x10"));
    }
  }();
  flow->setValue(spec.Qmax * 3600), pressure->setValue(spec.Pmax);
  auto custom = [model, flow, pressure] {
    const bool c = model->currentData().toString().isEmpty();
    flow->setEnabled(c), pressure->setEnabled(c);
    if (!c) {
      const air::Fan f = air::fan_from(json(model->currentData().toString().toStdString()));
      flow->setValue(f.Qmax * 3600), pressure->setValue(f.Pmax);
    }
  };
  connect(model, &QComboBox::currentIndexChanged, this, custom);
  custom();
  m_fans->setCellWidget(r, 1, model);
  m_fans->setCellWidget(r, 2, flow);
  m_fans->setCellWidget(r, 3, pressure);
  auto* blows = new QComboBox(m_fans);
  int at = 0;
  for (size_t i = 0; i < ways().size(); ++i) {
    blows->addItem(ways()[i].first);
    const opad::Vec3& w = ways()[i].second;
    if (w[0] * way[0] + w[1] * way[1] + w[2] * way[2] > 0.9) at = int(i);
  }
  blows->setCurrentIndex(at);
  m_fans->setCellWidget(r, 4, blows);
}

std::string CoolingAssistant::studyId() const {
  const AppDocument* d = m_hooks.document();
  if (!d) return {};
  for (const auto& st : d->scene.studies)
    if (st.kind == "thermal" && QString::fromStdString(st.name) == studyName()) return st.id;
  return {};
}

std::string CoolingAssistant::sweepId() const {
  const AppDocument* d = m_hooks.document();
  if (!d) return {};
  for (const auto& st : d->scene.studies)
    if (st.kind == "sweep" && QString::fromStdString(st.name) == sweepName()) return st.id;
  return {};
}

// ---------------------------------------------------------------- writing it
bool CoolingAssistant::apply() {
  AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return false;
  const std::string box = m_box->currentIndex() >= 0 ? m_box->currentData().toString().toStdString() : std::string();
  if (box.empty()) {
    m_status->setText(tr("Choose the box on the first page."));
    return false;
  }
  json materials = json::object();
  std::vector<std::pair<std::string, double>> heat;
  for (int r = 0; r < m_heat->rowCount(); ++r) {
    const std::string id = m_heat->item(r, 0)->data(Qt::UserRole).toString().toStdString();
    if (m_heat->item(r, 0)->checkState() == Qt::Checked) {
      heat.push_back({id, static_cast<QDoubleSpinBox*>(m_heat->cellWidget(r, 1))->value()});
      if (opad::material_of(d->doc, d->scene, id).id.empty()) materials[id] = {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon (assumed)"}};
    }
    if (static_cast<QCheckBox*>(m_heat->cellWidget(r, 2))->isChecked())
      materials[id] = {{"pcb", {{"layers", static_cast<QSpinBox*>(m_heat->cellWidget(r, 3))->value()}}}};
  }
  if (heat.empty()) {
    m_status->setText(tr("Nothing makes heat: tick at least one part on the Heat page."));
    m_steps->setCurrentRow(1);
    return false;
  }
  const bool fans = m_withFans->isChecked() && m_fans->rowCount() > 0;
  // The case's loads made again: heat sources, fans.
  std::vector<std::string> old;  // each write rebuilds the scene: its loads are gathered first
  for (const auto& l : d->scene.loads)
    if (l.load_case == kCase) old.push_back(l.id);
  for (const auto& id : old) m_hooks.write("delete", {{"target", id}}, tr("Cooling assistant"));
  for (const auto& [id, w] : heat)
    m_hooks.write("load", {{"kind", "heat"}, {"case", kCase}, {"on", {id}}, {"value", w}, {"name", tr("Heat").toStdString()}}, tr("Cooling assistant"));
  if (fans)
    for (int r = 0; r < m_fans->rowCount(); ++r) {
      const std::string on = static_cast<QComboBox*>(m_fans->cellWidget(r, 0))->currentData().toString().toStdString();
      const QString model = static_cast<QComboBox*>(m_fans->cellWidget(r, 1))->currentData().toString();
      const json fan = model.isEmpty() ? json{{"flow", static_cast<QDoubleSpinBox*>(m_fans->cellWidget(r, 2))->value()},
                                              {"pressure", static_cast<QDoubleSpinBox*>(m_fans->cellWidget(r, 3))->value()}}
                                       : json(model.toStdString());
      const opad::Vec3 way = ways()[size_t(static_cast<QComboBox*>(m_fans->cellWidget(r, 4))->currentIndex())].second;
      if (on.empty()) continue;
      m_hooks.write("load", {{"kind", "fan"}, {"case", kCase}, {"on", {on}}, {"fan", fan}, {"vector", way}, {"name", tr("Fan").toStdString()}},
                    tr("Cooling assistant"));
    }
  json settings = {{"case", kCase},
                   {"air", "cfd"},
                   {"ambient", m_ambient->value()},
                   {"cfd", {{"enclosure", box}, {"quality", m_quality->currentData().toString().toStdString()}, {"radiation", m_radiation->isChecked()},
                            {"buoyancy", !fans}}}};
  if (!materials.empty()) settings["materials"] = materials;
  const std::string id = studyId();
  if (id.empty()) m_hooks.write("study", {{"kind", "thermal"}, {"name", studyName().toStdString()}, {"settings", settings}, {"run", false}}, tr("Cooling assistant"));
  else m_hooks.write("study", {{"id", id}, {"settings", settings}, {"run", false}}, tr("Cooling assistant"));
  return true;
}

bool CoolingAssistant::startRun() {
  if (m_hooks.busy()) {
    m_status->setText(tr("A study is running: wait for it, or cancel it in the status bar."));
    return false;
  }
  if (!apply()) return false;
  m_status->setText(tr("Running: the air's mesh, its flow, then the heat back and forth between the parts and the air. The status bar "
                       "shows how far it is; you can keep working."));
  m_result->clear();
  m_resultButtons->hide();
  m_hooks.run(studyId());
  return true;
}

bool CoolingAssistant::startSweep() {
  AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return false;
  if (m_hooks.busy()) {
    m_sweepStatus->setText(tr("A study is running: wait for it, or cancel it in the status bar."));
    return false;
  }
  if (m_param->currentIndex() < 0) {
    m_sweepStatus->setText(tr("The model has no parameters yet: make the vent's position one (see above)."));
    return false;
  }
  if (!apply()) {
    m_sweepStatus->setText(tr("Set up the box, the heat and the air first (pages 1 to 3)."));
    return false;
  }
  json objective = {{"of", "max_temperature_C"}, {"goal", "min"}};
  if (!m_objective->currentData().toString().isEmpty()) objective["bodies"] = {m_objective->currentData().toString().toStdString()};
  json settings = {{"study", studyId()},
                   {"params", {{{"name", m_param->currentText().toStdString()}, {"from", m_from->value()}, {"to", m_to->value()}, {"steps", m_count->value()}}}},
                   {"refine", m_refine->value()},
                   {"objective", objective}};
  if (m_screen->isChecked() && m_quality->currentData().toString() != "quick") settings["screening"] = {{"cfd", {{"quality", "quick"}}}}, settings["confirm"] = true;
  const std::string id = sweepId();
  if (id.empty()) m_hooks.write("study", {{"kind", "sweep"}, {"name", sweepName().toStdString()}, {"settings", settings}, {"run", false}}, tr("Cooling assistant"));
  else m_hooks.write("study", {{"id", id}, {"settings", settings}, {"run", false}}, tr("Cooling assistant"));
  const int runs = m_count->value() + m_refine->value() + (settings.contains("screening") ? 1 : 0);
  m_sweepStatus->setText(tr("Running %1 designs one after another. The status bar shows which; it can take a while, and you can keep working.").arg(runs));
  m_points->setRowCount(0);
  m_best->setEnabled(false);
  m_hooks.run(sweepId());
  return true;
}

// ---------------------------------------------------------------- results
void CoolingAssistant::runFinished(const std::string& study, std::shared_ptr<const opad::sim::StudyRun> run, const QString& error) {
  AppDocument* d = m_hooks.document();
  if (!d) return;
  const bool sweep = study == sweepId();
  if (!sweep && study != studyId()) return;
  QLabel* status = sweep ? m_sweepStatus : m_status;
  if (!run) {
    status->setText(tr("It did not run: %1").arg(error));
    return;
  }
  if (sweep) {
    showSweep(run->summary);
    return;
  }
  const json& s = run->summary;
  QString html = "<p><b>" + tr("Hottest: %1 °C").arg(num(s.value("max_temperature_C", 0.0))) + "</b></p><table cellspacing=\"6\">";
  std::vector<std::pair<double, std::string>> bodies;
  const json per_body = s.value("bodies", json::object());
  for (const auto& [name, v] : per_body.items())
    if (v.is_object() && v.contains("max_temperature_C")) bodies.push_back({v["max_temperature_C"].get<double>(), name});
  std::sort(bodies.rbegin(), bodies.rend());
  for (const auto& [t, name] : bodies)
    html += "<tr><td>" + QString::fromStdString(name).toHtmlEscaped() + "</td><td align=\"right\">" + num(t) + " °C</td></tr>";
  html += "</table>";
  for (const auto& f : s.value("fans", json::array()))
    html += "<p>" + tr("%1: %2 m³/h at %3 Pa").arg(QString::fromStdString(f.value("name", "")).toHtmlEscaped(), num(f.value("flow_m3h", 0.0), 2),
                                                 num(f.value("pressure_Pa", 0.0))) +
            "</p>";
  if (const json v = s.value("vents", json()); v.is_object())
    html += "<p>" + tr("Air through the box: %1 m³/h, leaving at %2 °C; the air takes %3 W, radiation %4 W.")
                        .arg(num(v.value("air_out_m3h", 0.0), 2), num(v.value("outlet_air_C", 0.0)), num(v.value("heat_to_air_W", 0.0), 2),
                             num(s.value("radiated_W", 0.0), 2)) +
            "</p>";
  QStringList warnings;
  for (const auto& w : s.value("warnings", json::array())) warnings << i18n::t(QString::fromStdString(w.get<std::string>())).toHtmlEscaped();
  if (!warnings.isEmpty()) html += "<p>" + tr("Notes:") + "<br>" + warnings.join("<br>") + "</p>";
  m_result->setText(html);
  m_resultButtons->show();
  m_status->setText(tr("Done. Temperatures and Air speed show the map in the view; See inside the box hides it."));
}

void CoolingAssistant::showSweep(const json& summary) {
  m_points->setRowCount(0);
  const json params = summary.value("params", json::array());
  const std::string name = params.empty() ? std::string() : params[0].value("name", "");
  const QString unit = params.empty() ? QString() : QString::fromStdString(params[0].value("unit", ""));
  std::vector<json> points;
  for (const auto& p : summary.value("points", json::array())) points.push_back(p);
  std::sort(points.begin(), points.end(), [&](const json& a, const json& b) {
    const json va = a["params"].value(name, json()), vb = b["params"].value(name, json());
    return va.is_number() && vb.is_number() ? va.get<double>() < vb.get<double>() : false;
  });
  const json best = summary.value("best", json::object());
  for (const auto& p : points) {
    const int r = m_points->rowCount();
    m_points->insertRow(r);
    const json v = p["params"].value(name, json());
    m_points->setItem(r, 0, new QTableWidgetItem(v.is_number() ? num(v.get<double>(), 2) + " " + unit : QString::fromStdString(v.dump())));
    m_points->setItem(r, 1, new QTableWidgetItem(p.contains("objective") ? num(p["objective"].get<double>()) : QString("—")));
    QString note = p.contains("error") ? i18n::t(QString::fromStdString(p["error"].get<std::string>())) : QString();
    if (p["params"] == best.value("params", json())) note = tr("the best") + (note.isEmpty() ? QString() : "; " + note);
    m_points->setItem(r, 2, new QTableWidgetItem(note));
  }
  m_bestParams = best.value("params", json::object());
  m_best->setEnabled(!m_bestParams.empty());
  if (!m_bestParams.empty()) {
    const json v = m_bestParams.value(name, json());
    const double t = best.contains("confirmed") ? best["confirmed"].get<double>() : best.value("objective", 0.0);
    m_sweepStatus->setText(tr("The best: %1 = %2 %3, hottest %4 °C%5.")
                               .arg(QString::fromStdString(name), v.is_number() ? num(v.get<double>(), 2) : QString(), unit, num(t),
                                    best.contains("confirmed") ? tr(" (run again at full quality)") : QString()));
  }
}

// ---------------------------------------------------------------- the example
void CoolingAssistant::buildExample() {
  AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return;
  const QString label = tr("Cooling example");
  auto box = [&](const char* name, opad::Vec3 at, double l, double w, double h, json extra = json::object()) {
    json inputs = {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"length", l}, {"width", w}, {"height", h}, {"centered", false}};
    for (const auto& [k, v] : extra.items()) inputs[k] = v;
    json args = {{"kind", "box"}, {"inputs", inputs}};
    if (name) args["name"] = name;
    const json r = m_hooks.write("feature", args, label);
    return r.value("body_ids", json::array()).empty() ? std::string() : r["body_ids"][0].get<std::string>();
  };
  m_hooks.write("param", {{"name", "vent_z"}, {"expr", "21 mm"}, {"comment", "the exhaust slots' middle height"}}, label);
  const std::string encl = box("Enclosure", {0, 0, 0}, 110, 80, 40);
  box(nullptr, {2.5, 2.5, 2.5}, 105, 75, 35, {{"operation", "cut"}, {"targets", {encl}}});
  box(nullptr, {-1, 26, 6}, 5, 28, 28, {{"operation", "cut"}, {"targets", {encl}}});  // the fan's opening
  for (const char* dz : {"vent_z - 6 mm", "vent_z", "vent_z + 6 mm"})  // three slots in the +X wall, around vent_z
    m_hooks.write("feature",
                  {{"kind", "box"},
                   {"inputs", {{"plane", {{"origin", {106, 0, 0}}, {"normal", {1, 0, 0}}}}, {"x", 40}, {"y", dz}, {"length", 50}, {"width", 3}, {"height", 5},
                               {"centered", true}, {"operation", "cut"}, {"targets", {encl}}}}},
                  label);
  const std::string fan = box("Fan", {2.5, 25, 5}, 10, 30, 30);
  const std::string board = box("Board", {15, 12, 10}, 85, 56, 1.6);
  const std::string soc = box("SoC", {50, 33, 11.6}, 14, 14, 1.2);
  const std::string pmic = box("PMIC", {25, 20, 11.6}, 6, 6, 1);
  const std::string hs = box("Heatsink", {47, 30, 12.8}, 20, 20, 2);
  const double gap = (20 - 5 * 1.5) / 4;
  for (int i = 0; i < 5; ++i) box(nullptr, {47, 30 + i * (1.5 + gap), 14.8}, 20, 1.5, 10, {{"operation", "join"}, {"targets", {hs}}});
  m_hooks.write("part_properties", {{"target", encl}, {"set", {{"material", "abs"}}}}, label);
  m_hooks.write("part_properties", {{"target", hs}, {"set", {{"material", "aluminium-6061"}}}}, label);
  for (const auto& [id, w] : std::vector<std::pair<std::string, double>>{{soc, 4.0}, {pmic, 0.5}})
    m_hooks.write("load", {{"kind", "heat"}, {"case", kCase}, {"on", {id}}, {"value", w}, {"name", tr("Heat").toStdString()}}, label);
  m_hooks.write("load", {{"kind", "fan"}, {"case", kCase}, {"on", {fan}}, {"fan", {{"flow", 8}, {"pressure", 25}}}, {"vector", {1, 0, 0}}, {"name", tr("Fan").toStdString()}},
                label);
  json settings = {{"case", kCase},
                   {"air", "cfd"},
                   {"ambient", 25},
                   {"cfd", {{"enclosure", encl}, {"quality", "normal"}, {"radiation", true}, {"buoyancy", false}}},
                   {"materials", {{board, {{"pcb", {{"layers", 4}}}}}, {soc, {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon"}}},
                                  {pmic, {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon"}}}}}};
  m_hooks.write("study", {{"kind", "thermal"}, {"name", studyName().toStdString()}, {"settings", settings}, {"run", false}}, label);
  reload();
  if (m_param->findText("vent_z") >= 0) {
    m_param->setCurrentText("vent_z");
    m_from->setValue(11), m_to->setValue(29);
  }
  m_status->setText(tr("The example is ready: look through the pages, then Run."));
}
