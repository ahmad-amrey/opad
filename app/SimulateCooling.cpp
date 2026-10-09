#include "SimulateCooling.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
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
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <Bnd_Box.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <set>

#include "AppDocument.hpp"
#include "I18n.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "SearchCombo.hpp"
#include "Theme.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"
#include "opad/materials.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/cfd.hpp"

using opad::json;
namespace air = opad::sim::air;

namespace {

QString num(double v, int decimals = 1) { return QString::number(v, 'f', decimals); }

constexpr int kFaceRole = Qt::UserRole + 1;  // a heat row of a face picked: its Ref::str

bool sameRef(const opad::Ref& a, const opad::Ref& b) { return a.body == b.body && a.kind == b.kind && a.index == b.index; }

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

// The way the air goes, as the page says it: an axis by its name, else its components.
QString wayText(const opad::Vec3& v) {
  for (const auto& [name, w] : ways())
    if (std::fabs(v[0] - w[0]) + std::fabs(v[1] - w[1]) + std::fabs(v[2] - w[2]) < 1e-6) return name;
  return QString("(%1, %2, %3)").arg(num(v[0], 2), num(v[1], 2), num(v[2], 2));
}

// A number cell's editor, made only while it is typed in: watts (0-1000) or copper layers (1-32).
class RangeDelegate : public QStyledItemDelegate {
 public:
  RangeDelegate(double low, double high, int decimals, QObject* parent) : QStyledItemDelegate(parent), m_low(low), m_high(high), m_decimals(decimals) {}
  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override {
    if (m_decimals == 0) {
      auto* e = new QSpinBox(parent);
      e->setRange(int(m_low), int(m_high));
      return e;
    }
    auto* e = new QDoubleSpinBox(parent);
    e->setRange(m_low, m_high);
    e->setDecimals(m_decimals);
    return e;
  }

 private:
  double m_low, m_high;
  int m_decimals;
};

QLabel* text(const QString& s, QWidget* parent, const char* object = "secondary") {
  auto* l = new QLabel(s, parent);
  l->setWordWrap(true);
  l->setObjectName(object);
  return l;
}

}  // namespace

QString CoolingAssistant::studyName() { return tr("Cooling (air solved)"); }

CoolingAssistant::CoolingAssistant(Hooks hooks, QWidget* parent) : QWidget(parent, Qt::Window), m_hooks(std::move(hooks)) {
  setObjectName("coolingAssistant");
  setWindowTitle(tr("Thermal setup"));
  setAttribute(Qt::WA_StyledBackground);
  resize(980, 640);
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  m_steps = new QListWidget(this);
  m_steps->setObjectName("referenceList");
  m_steps->setFixedWidth(230);
  m_steps->setFrameShape(QFrame::NoFrame);
  for (const QString& s : {tr("1  The box"), tr("2  Heat"), tr("3  Air"), tr("4  Run")}) m_steps->addItem(s);
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
  auto* back = m_back = new QPushButton(tr("Back"), right);
  auto* next = m_next = new QPushButton(tr("Next"), right);
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
  if (m_hooks.onFilterApplied) m_hooks.onFilterApplied(this, [this] { filterApplied(); });
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
    QVBoxLayout* v = page(tr("The box"), tr("The air is solved inside the box and in a margin of the room around it: its holes are the vents. Pick "
                                            "every body the box is made of (a base and its lid, a frame and its panels) in the view or the browser: a "
                                            "click adds a body or takes it out. The ones found around the parts that make heat are picked already."));
    m_engine = text(QString(), this);
    v->addWidget(m_engine);
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("The box's bodies:"), this));
    m_pick = new PickBox(this);
    m_pick->setObjectName("coolingBoxPick");
    row->addWidget(m_pick, 1);
    v->addLayout(row);
    m_walls = new QListWidget(this);  // what is picked, by name
    m_walls->setObjectName("coolingWalls");
    m_walls->setSelectionMode(QAbstractItemView::NoSelection);
    m_walls->setMaximumHeight(120);
    v->addWidget(m_walls);
    connect(m_pick, &QPushButton::clicked, this, [this] { startPicking(Pick::Box); });
    connect(m_pick, &PickBox::cleared, this, [this] {
      m_picked.clear();
      if (m_picking == Pick::Box && m_hooks.select) m_hooks.select({});
      showPicked();
      wallsChanged();
    });
    v->addWidget(text(tr("Inside it, taking part (the board, chips, heatsink, connectors):"), this, "sectionHeader"));
    m_inside = new QListWidget(this);
    v->addWidget(m_inside, 1);
    m_example = new QPushButton(tr("Build the example: a board in a vented box"), this);
    m_example->setObjectName("coolingExample");
    m_example->setToolTip(tr("A 110 x 80 x 40 mm ABS box with a 30 mm intake fan, three exhaust slots at the height vent_z, a board, a 4 W chip "
                             "under a finned heatsink and a 0.5 W power chip: everything set up to run."));
    v->addWidget(m_example);
    connect(m_example, &QPushButton::clicked, this, [this] { buildExample(); });
  }
  // ---- 2. heat
  {
    QVBoxLayout* v = page(tr("Heat"), tr("Tick each part that makes heat and type its power. A chip joined into its board has no body of its own: "
                                         "click its face in the view (the Faces filter is on here) and type that face's power. Tick Board for a "
                                         "printed circuit board: its copper spreads heat along it far better than through it (give its copper "
                                         "layers). A chip with no material is taken as silicon."));
    auto* picks = new QFormLayout();
    m_partsPick = new PickBox(this);
    m_partsPick->setObjectName("coolingHeatParts");
    picks->addRow(tr("Parts that make heat:"), m_partsPick);
    m_facePick = new PickBox(this);
    m_facePick->setObjectName("coolingHeatFaces");
    picks->addRow(tr("Faces that make heat:"), m_facePick);
    v->addLayout(picks);
    connect(m_partsPick, &QPushButton::clicked, this, [this] { startPicking(Pick::Parts); });
    connect(m_partsPick, &PickBox::cleared, this, [this] {
      {
        const QSignalBlocker quiet(m_heat);
        for (int r = 0; r < int(m_parts.size()); ++r) m_heat->item(r, 0)->setCheckState(Qt::Unchecked);
      }
      if (m_picking == Pick::Parts) reselect();
      showParts();
    });
    connect(m_facePick, &QPushButton::clicked, this, [this] { startPicking(Pick::Faces); });
    connect(m_facePick, &PickBox::cleared, this, [this] {
      keepFacePowers();
      m_faces.clear();
      if (m_picking == Pick::Faces && m_hooks.select) m_hooks.select({});
      showFaces();
    });
    m_heat = new QTableWidget(0, 4, this);
    m_heat->setObjectName("coolingHeat");
    m_heat->setHorizontalHeaderLabels({tr("Part"), tr("Power, watts"), tr("Board"), tr("Copper layers")});
    m_heat->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_heat->verticalHeader()->hide();
    m_heat->setItemDelegateForColumn(1, new RangeDelegate(0, 1000, 2, m_heat));
    m_heat->setItemDelegateForColumn(3, new RangeDelegate(1, 32, 0, m_heat));
    v->addWidget(m_heat, 1);
    connect(m_heat, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
      if (item->column() != 0 || item->row() >= int(m_parts.size())) return;
      if (m_picking == Pick::Parts) reselect();  // a part ticked in the table: shown selected as a pick would be
      showParts();
    });
  }
  // ---- 3. air
  {
    QVBoxLayout* v = page(tr("Air"), tr("Pick each fan in the view or the browser: its own model (a component: frame, hub and blades as one) or a "
                                        "block where it sits, as big as it. Pick the flat face its air goes through (Flip turns the way round), "
                                        "choose the fan (Custom: its free flow and shut-off pressure from its datasheet, typed below it), and the "
                                        "heatsink it blows through with its material. Without fans the air moves only by rising where it is warm: "
                                        "put vents low and high."));
    m_withFans = new QCheckBox(tr("Fans move the air (untick: vents only, warm air rising)"), this);
    m_withFans->setChecked(true);
    v->addWidget(m_withFans);
    auto* columns = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    m_fanList = new QListWidget(this);
    m_fanList->setObjectName("coolingFanList");
    m_fanList->setFixedWidth(220);
    left->addWidget(m_fanList, 1);
    auto* buttons = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add a fan"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    left->addLayout(buttons);
    columns->addLayout(left);
    m_fanForm = new QWidget(this);
    m_fanRows = new QFormLayout(m_fanForm);
    m_fanRows->setContentsMargins(12, 0, 0, 0);
    m_fanPick = new PickBox(m_fanForm);
    m_fanPick->setObjectName("coolingFanPick");
    m_fanRows->addRow(tr("Fan:"), m_fanPick);
    auto* way = new QHBoxLayout();
    m_wayPick = new PickBox(m_fanForm);
    m_wayPick->setObjectName("coolingWayPick");
    m_flip = new QPushButton(tr("Flip"), m_fanForm);
    m_flip->setObjectName("coolingWayFlip");
    way->addWidget(m_wayPick, 1);
    way->addWidget(m_flip);
    m_fanRows->addRow(tr("Air goes through:"), way);
    m_axis = new QComboBox(m_fanForm);
    for (const auto& w : ways()) m_axis->addItem(QString(QChar(0x202A)) + w.first + QChar(0x202C));  // kept left to right in Arabic
    m_fanRows->addRow(tr("Or along:"), m_axis);
    m_wayText = text(QString(), m_fanForm);
    m_fanRows->addRow(QString(), m_wayText);
    m_model = new QComboBox(m_fanForm);
    m_model->setObjectName("coolingFanModel");
    for (const auto& f : air::fans()) m_model->addItem(QString::fromStdString(f.name), QString::fromStdString(f.id));
    m_model->addItem(tr("Custom (from its datasheet)"), QString());
    search_combo::enable(m_model);
    m_fanRows->addRow(tr("Fan model:"), m_model);
    m_flow = new QDoubleSpinBox(m_fanForm);
    m_flow->setObjectName("coolingFanFlow");
    m_flow->setRange(0.1, 2000);
    m_flow->setSuffix(" m³/h");
    m_fanRows->addRow(tr("Free flow:"), m_flow);
    m_pressure = new QDoubleSpinBox(m_fanForm);
    m_pressure->setObjectName("coolingFanPressure");
    m_pressure->setRange(0.1, 5000);
    m_pressure->setSuffix(" Pa");
    m_fanRows->addRow(tr("Shut-off pressure:"), m_pressure);
    m_sinkPick = new PickBox(m_fanForm);
    m_sinkPick->setObjectName("coolingSinkPick");
    m_fanRows->addRow(tr("Heatsink:"), m_sinkPick);
    m_sinkMaterial = new QComboBox(m_fanForm);
    m_sinkMaterial->setObjectName("coolingSinkMaterial");
    m_sinkMaterial->addItem(tr("As modelled"), QString());
    for (const auto& m : opad::materials())
      if (opad::thermal(m.id)) m_sinkMaterial->addItem(i18n::t(QString::fromStdString(m.name)), QString::fromStdString(m.id));
    search_combo::enable(m_sinkMaterial);
    m_fanRows->addRow(tr("Heatsink material:"), m_sinkMaterial);
    columns->addWidget(m_fanForm, 1);
    v->addLayout(columns, 1);
    auto edit = [this](auto change) {
      return [this, change](auto&&...) {
        if (m_fanLoading || m_fan < 0 || m_fan >= int(m_fanSpecs.size())) return;
        change(m_fanSpecs[size_t(m_fan)]);
        showFans();
      };
    };
    connect(m_withFans, &QCheckBox::toggled, this, [this](bool on) {
      m_fanList->setEnabled(on);
      m_fanForm->setEnabled(on);
      if (!on && (m_picking == Pick::Fan || m_picking == Pick::Way || m_picking == Pick::Sink)) stopPicking();
      if (on && step() == 2 && isVisible()) {
        if (m_fanSpecs.empty()) m_fanSpecs.push_back({}), m_fan = 0;
        showFans();
        startPicking(Pick::Fan);
      }
    });
    connect(add, &QPushButton::clicked, this, [this] {
      m_fanSpecs.push_back({});
      m_fan = int(m_fanSpecs.size()) - 1;
      showFans();
      startPicking(Pick::Fan);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
      if (m_fan < 0 || m_fan >= int(m_fanSpecs.size())) return;
      m_fanSpecs.erase(m_fanSpecs.begin() + m_fan);
      m_fan = std::min(m_fan, int(m_fanSpecs.size()) - 1);
      showFans();
      if (m_picking == Pick::Fan || m_picking == Pick::Way || m_picking == Pick::Sink) reselect();
    });
    connect(m_fanList, &QListWidget::currentRowChanged, this, [this](int row) {
      if (m_fanLoading || row < 0 || row == m_fan) return;
      m_fan = row;
      showFan();
      if (m_picking == Pick::Fan || m_picking == Pick::Way || m_picking == Pick::Sink) reselect();  // this fan's picks shown
    });
    connect(m_fanPick, &QPushButton::clicked, this, [this] { startPicking(Pick::Fan); });
    connect(m_wayPick, &QPushButton::clicked, this, [this] { startPicking(Pick::Way); });
    connect(m_sinkPick, &QPushButton::clicked, this, [this] { startPicking(Pick::Sink); });
    connect(m_fanPick, &PickBox::cleared, this, edit([this](FanSpec& f) {
      f.on.clear();
      if (m_picking == Pick::Fan) m_hooks.select({});
    }));
    connect(m_wayPick, &PickBox::cleared, this, edit([this](FanSpec& f) {
      f.across = {}, f.flip = false;
      if (m_picking == Pick::Way) m_hooks.select({});
    }));
    connect(m_sinkPick, &PickBox::cleared, this, edit([this](FanSpec& f) {
      f.sink.clear();
      if (m_picking == Pick::Sink) m_hooks.select({});
    }));
    connect(m_flip, &QPushButton::clicked, this, edit([](FanSpec& f) { f.flip = !f.flip; }));
    connect(m_axis, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) { f.axis = std::max(0, m_axis->currentIndex()); }));
    connect(m_model, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) {
      f.model = m_model->currentData().toString();
      if (!f.model.isEmpty()) {
        const air::Fan spec = air::fan_from(json(f.model.toStdString()));
        f.flow = spec.Qmax * 3600, f.pressure = spec.Pmax;
      }
    }));
    connect(m_flow, &QDoubleSpinBox::valueChanged, this, edit([this](FanSpec& f) { f.flow = m_flow->value(); }));
    connect(m_pressure, &QDoubleSpinBox::valueChanged, this, edit([this](FanSpec& f) { f.pressure = m_pressure->value(); }));
    connect(m_sinkMaterial, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) { f.material = m_sinkMaterial->currentData().toString(); }));
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
    m_runNote = text(QString(), this);
    m_runNote->setObjectName("coolingRunNote");
    v->addWidget(m_runNote);
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
      for (const auto& wall : walls()) m_hooks.setVisible(wall, !on);  // every wall of it: the lid too
    });
  }
  m_steps->setCurrentRow(0);
}

int CoolingAssistant::step() const { return m_steps->currentRow(); }
int CoolingAssistant::steps() const { return m_steps->count(); }

void CoolingAssistant::open(int at) {
  // Found again when it was closed or the document changed since: else the pages stay as they are (a page switch).
  const AppDocument* d = m_hooks.document();
  const std::pair<unsigned long long, unsigned long long> state = d ? std::make_pair(d->generation, d->revision) : std::make_pair(0ull, 0ull);
  if (!isVisible() || state != m_loaded) reload();
  if (at >= 0) m_steps->setCurrentRow(std::clamp(at, 0, steps() - 1));
  QWidget::show();
  raise();
  activateWindow();
}

void CoolingAssistant::hideEvent(QHideEvent* e) {
  stopPicking();  // the view's clicks select again
  QWidget::hideEvent(e);
}

void CoolingAssistant::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

void CoolingAssistant::showStep(int i) {
  if (i >= 0 && i < m_pages->count()) m_pages->setCurrentIndex(i);
  m_back->setEnabled(i > 0);
  m_next->setVisible(i < steps() - 1);  // the last page runs (its Run button)
  // Each page picks in the view and the browser, as a feature's pick box does: the box's bodies, the parts (or the faces)
  // that make heat, the fan.
  if (!isVisible() || !m_boxes) return stopPicking();
  if (i == 0) startPicking(Pick::Box);
  else if (i == 1) startPicking(m_faces.empty() ? Pick::Parts : Pick::Faces);
  else if (i == 2 && m_withFans->isChecked()) {
    if (m_fanSpecs.empty()) m_fanSpecs.push_back({}), m_fan = 0;
    showFans();
    startPicking(Pick::Fan);
  } else
    stopPicking();
}

// ---------------------------------------------------------------- the pages from the document
void CoolingAssistant::reload() {
  AppDocument* d = m_hooks.document();
  m_loaded = d ? std::make_pair(d->generation, d->revision) : std::make_pair(0ull, 0ull);
  const bool foam = opad::sim::openfoam().found();
  m_engine->setText(foam ? QString()
                         : tr("OpenFOAM is not installed: the air cannot be solved. Install it (Ubuntu: apt install openfoam; Windows: "
                              "OpenCFD's build) or set OPAD_OPENFOAM to its directory, then open this again."));
  m_engine->setVisible(!foam);
  m_runButton->setEnabled(foam);
  m_runNote->setText(m_engine->text());  // Run greyed: said where it is
  m_runNote->setVisible(!foam);
  stopPicking();
  m_picked.clear();
  m_faces.clear();
  m_facePower.clear();
  m_fanSpecs.clear();
  m_fan = -1;
  showPicked();
  m_boxes.reset();
  ++m_serial;
  if (Job* j = std::exchange(m_job, nullptr)) j->cancel();
  wallsChanged();
  if (!d || !d->hasDocument) return;
  const opad::Scene& s = d->scene;
  // The box's bodies: the cooling study's (a body, or a list), else the enclosure the heat sources are found in (with what
  // closes it: a lid, a cover, the other half), else the body that holds the most others and what closes it. Every shown
  // solid's box measured once on a worker, the box found from them there: measured again per candidate on the UI thread, a
  // board with its parts held the app for a minute.
  auto given = std::make_shared<std::vector<std::string>>();
  if (const opad::Study* st = s.study(studyId())) {
    const json e = st->def.value("settings", json::object()).value("cfd", json::object()).value("enclosure", json());
    if (e.is_string()) given->push_back(e.get<std::string>());
    for (const auto& w : e.is_array() ? e : json::array())
      if (w.is_string()) given->push_back(w.get<std::string>());
  }
  auto heated = std::make_shared<std::vector<std::string>>();
  for (const auto& l : s.loads)
    if (l.kind == "heat")
      for (const auto& r : l.refs) heated->push_back(r.body);
  auto doc = std::make_shared<const opad::Document>(d->doc);
  auto scene = std::make_shared<const opad::Scene>(s);
  auto boxes = std::make_shared<opad::sim::BodyBoxes>();
  auto box = std::make_shared<std::vector<std::string>>();
  m_status->clear();
  m_pick->setNote(tr("Finding the box among the bodies…"));
  JobRunner* jobs = m_hooks.jobs ? m_hooks.jobs() : nullptr;
  if (!jobs) return;
  const unsigned serial = m_serial;
  m_job = jobs->async(tr("Finding the box"), [doc, scene, boxes, box, given, heated](Progress p) {
    *boxes = opad::sim::body_boxes(*doc, *scene);
    if (p.cancelled()) return;
    for (const auto& id : *given)
      if (boxes->at.count(id)) box->push_back(id);
    try {
      if (box->empty() && !heated->empty()) *box = opad::sim::find_enclosure(*boxes, *heated, {}).walls;
      if (box->empty()) *box = opad::sim::likely_enclosure(*boxes);
    } catch (const std::exception&) {
    }
  }, [this, self = QPointer<CoolingAssistant>(this), serial, boxes, box](bool ok, const QString&) {
    if (!self || serial != m_serial) return;
    m_job = nullptr;
    AppDocument* d = m_hooks.document();
    if (!ok || !d || !d->hasDocument) return;
    m_boxes = boxes;
    m_pick->setNote(QString());
    m_picked = *box;
    showPicked();
    // The faces the study's heat sources are on (a chip joined into its board), with their powers.
    for (const auto& l : d->scene.loads)
      if (l.load_case == kCase && l.kind == "heat")
        for (const auto& r : l.refs)
          if (r.kind == opad::Ref::Kind::Face && std::none_of(m_faces.begin(), m_faces.end(), [&](const opad::Ref& f) { return sameRef(f, r); })) {
            m_faces.push_back(r);
            m_facePower[r.str()] = l.def.value("value", 0.0) / double(l.refs.size());
          }
    m_example->setVisible(boxes->ids.empty());
    wallsChanged();
    loadFans(d->scene);
    showStep(step());  // the page's picking, now that the bodies are known
  });
}

std::vector<std::string> CoolingAssistant::walls() const { return m_picked; }

// ---------------------------------------------------------------- picking the box's bodies, the faces that make heat
void CoolingAssistant::startPicking(Pick what) {
  if (!m_boxes || !isVisible()) return;
  const Pick was = m_picking;
  m_picking = Pick::None;  // switching the filter clears the selection: no pick taken out
  const QString filter = what == Pick::Faces || what == Pick::Way ? "select.faces" : "select.bodies";  // pickFilter() once picking
  QString before = filter;
  if (m_hooks.filter) {
    before = m_hooks.filter(filter);
    if (was == Pick::None) m_filterBefore = before;
  }
  if (was == Pick::None && m_hooks.accumulate) m_hooks.accumulate(true);  // a click adds or takes out, as a feature's picks
  m_picking = what;
  m_awaiting = before != filter && m_hooks.onFilterApplied;  // the view's picks taken once its targets are built (filterApplied)
  if (!m_awaiting) reselect();
  showPicked();
  showParts();
  showFaces();
  showFan();
}

QString CoolingAssistant::pickFilter() const { return m_picking == Pick::Faces || m_picking == Pick::Way ? "select.faces" : "select.bodies"; }

// Faces can be selected only once the Faces filter has built their pick targets (a sliced job), and a filter switched (here,
// by hand, by a key) clears the selection, not the picks: once the page's filter is applied the picks are selected again.
void CoolingAssistant::filterApplied() {
  if (m_picking == Pick::None || !m_hooks.filter || m_hooks.filter(QString()) != pickFilter()) return;
  m_awaiting = false;
  reselect();
}

void CoolingAssistant::stopPicking() {
  if (m_picking == Pick::None) return;
  m_picking = Pick::None;
  m_awaiting = false;
  if (m_hooks.accumulate) m_hooks.accumulate(false);
  if (m_hooks.filter && !m_filterBefore.isEmpty()) m_hooks.filter(m_filterBefore);  // the filter the view had
  m_filterBefore.clear();
  showPicked();
  showParts();
  showFaces();
  showFan();
}

void CoolingAssistant::reselect() {
  if (!m_hooks.select || m_picking == Pick::None) return;
  if (m_picking == Pick::Faces) return m_hooks.select(m_faces);
  const FanSpec* fan = m_fan >= 0 && m_fan < int(m_fanSpecs.size()) ? &m_fanSpecs[size_t(m_fan)] : nullptr;
  if (m_picking == Pick::Way) return m_hooks.select(fan && !fan->across.body.empty() ? std::vector<opad::Ref>{fan->across} : std::vector<opad::Ref>{});
  const std::vector<std::string> ids = m_picking == Pick::Box     ? m_picked
                                       : m_picking == Pick::Parts ? heatedParts()
                                       : !fan                     ? std::vector<std::string>{}
                                       : m_picking == Pick::Fan   ? fan->on
                                                                  : fan->sink;
  std::vector<opad::Ref> refs;
  for (const auto& id : ids) {
    opad::Ref r;
    r.body = id;  // a body, or a component (one part): its row in the browser, its bodies in the view
    refs.push_back(r);
  }
  m_hooks.select(refs);  // what is picked, shown selected in the view and the browser
}

void CoolingAssistant::selectionChanged(const std::vector<std::string>& ids, const std::vector<opad::Ref>& refs) {
  // A filter switched (by the page, by hand) clears the selection, not the picks: shown again once applied (filterApplied).
  if (m_picking == Pick::None || !m_boxes || m_awaiting || (m_hooks.filterSwitching && m_hooks.filterSwitching())) return;
  const AppDocument* d = m_hooks.document();
  if (!d) return;
  // Another filter switched to while picking (the ribbon, a key): its picks are something else, the page's are kept.
  if (m_hooks.filter && m_hooks.filter(QString()) != pickFilter()) return;
  // Those kept stay in their order, the new ones after them.
  auto merged = [](const auto& was, const auto& now, auto same) {
    std::decay_t<decltype(was)> next;
    for (const auto& x : was)
      if (std::any_of(now.begin(), now.end(), [&](const auto& y) { return same(x, y); })) next.push_back(x);
    for (const auto& x : now)
      if (std::none_of(next.begin(), next.end(), [&](const auto& y) { return same(x, y); })) next.push_back(x);
    return next;
  };
  auto sameId = [](const std::string& a, const std::string& b) { return a == b; };
  if (m_picking == Pick::Faces) {
    // The faces picked, on shown solids.
    std::vector<opad::Ref> now;
    for (const auto& r : refs)
      if (r.kind == opad::Ref::Kind::Face && m_boxes->at.count(r.body) &&
          std::none_of(now.begin(), now.end(), [&](const opad::Ref& f) { return sameRef(f, r); }))
        now.push_back(r);
    std::vector<opad::Ref> next = merged(m_faces, now, sameRef);
    if (next.size() == m_faces.size() && std::equal(next.begin(), next.end(), m_faces.begin(), sameRef)) return;
    keepFacePowers();
    m_faces = std::move(next);
    showFaces();
    return;
  }
  FanSpec* fan = m_fan >= 0 && m_fan < int(m_fanSpecs.size()) ? &m_fanSpecs[size_t(m_fan)] : nullptr;
  if (m_picking == Pick::Way) {
    // One face: the one clicked last (a click on another replaces it).
    if (!fan) return;
    opad::Ref next;
    for (const auto& r : refs)
      if (r.kind == opad::Ref::Kind::Face && (fan->across.body.empty() || !sameRef(r, fan->across))) next = r;
    if (next.body.empty())
      for (const auto& r : refs)
        if (r.kind == opad::Ref::Kind::Face) next = r;  // the same face still picked
    if (sameRef(next, fan->across) && !next.body.empty()) return;
    fan->across = next;
    fan->flip = false;
    if (refs.size() > 1) QTimer::singleShot(0, this, [this] { reselect(); });  // the one face left selected
    showFans();
    if (!next.body.empty()) measureWay(m_fan);
    return;
  }
  if (m_picking == Pick::Fan || m_picking == Pick::Sink) {
    // Bodies or components, each one part: a component picked in the browser stands for all its bodies.
    if (!fan) return;
    std::vector<std::string> now;
    for (const auto& id : ids)
      if (const opad::Node* n = d->scene.node(id); n && std::find(m_picked.begin(), m_picked.end(), id) == m_picked.end() &&
                                                  std::find(now.begin(), now.end(), id) == now.end())
        now.push_back(id);  // not the box's walls
    std::vector<std::string>& field = m_picking == Pick::Fan ? fan->on : fan->sink;
    std::vector<std::string> next = merged(field, now, sameId);
    if (next == field) return;
    field = std::move(next);
    showFans();
    return;
  }
  // The bodies picked (a component stands for its bodies), shown solids only.
  std::vector<std::string> now;
  auto take = [&](const std::string& id) {
    if (m_boxes->at.count(id) && std::find(now.begin(), now.end(), id) == now.end()) now.push_back(id);
  };
  for (const auto& id : ids)
    if (const opad::Node* n = d->scene.node(id)) {
      if (n->kind == opad::Node::Kind::Body) take(id);
      else
        for (const auto& b : d->scene.bodies_under(id)) take(b);
    }
  if (m_picking == Pick::Parts) {
    // The parts inside the box that make heat: their ticks in the table.
    std::set<std::string> on(now.begin(), now.end());
    const QSignalBlocker quiet(m_heat);
    for (int r = 0; r < int(m_parts.size()); ++r)
      m_heat->item(r, 0)->setCheckState(on.count(m_parts[size_t(r)]) ? Qt::Checked : Qt::Unchecked);
    showParts();
    return;
  }
  std::vector<std::string> next = merged(m_picked, now, sameId);
  if (next == m_picked) return;
  m_picked = std::move(next);
  showPicked();
  wallsChanged();
}

void CoolingAssistant::showPicked() {
  const AppDocument* d = m_hooks.document();
  QStringList names;
  m_walls->clear();
  for (const auto& id : m_picked) {
    const QString name = d ? nameOf(d->scene, id) : QString::fromStdString(id);
    names << name;
    auto* item = new QListWidgetItem(name, m_walls);
    item->setData(Qt::UserRole, QString::fromStdString(id));
  }
  m_pick->set(int(m_picked.size()), names.join(", "), m_picking == Pick::Box, !m_picked.empty());
}

void CoolingAssistant::keepFacePowers() {
  for (int r = int(m_parts.size()); r < m_heat->rowCount(); ++r)
    if (const QTableWidgetItem* part = m_heat->item(r, 0); part && m_heat->item(r, 1))
      m_facePower[part->data(kFaceRole).toString().toStdString()] = m_heat->item(r, 1)->data(Qt::EditRole).toDouble();
}

void CoolingAssistant::showFaces() {
  keepFacePowers();  // the powers typed in the rows made again
  const AppDocument* d = m_hooks.document();
  const int first = int(m_parts.size());
  m_heat->setRowCount(first);
  m_heat->setRowCount(first + int(m_faces.size()));
  QStringList names;
  for (size_t i = 0; i < m_faces.size(); ++i) {
    const opad::Ref& f = m_faces[i];
    const int r = first + int(i);
    const QString name = tr("%1, face %2").arg(d ? nameOf(d->scene, f.body) : QString::fromStdString(f.body)).arg(f.index + 1);
    names << name;
    auto* part = new QTableWidgetItem(name);
    part->setFlags(Qt::ItemIsEnabled);
    part->setData(kFaceRole, QString::fromStdString(f.str()));
    part->setToolTip(tr("A face picked in the view: click it again there to take it out."));
    m_heat->setItem(r, 0, part);
    auto* w = new QTableWidgetItem();
    const auto kept = m_facePower.find(f.str());
    w->setData(Qt::EditRole, kept != m_facePower.end() ? kept->second : 1.0);
    m_heat->setItem(r, 1, w);
    m_heat->setItem(r, 2, new QTableWidgetItem());  // a face is no board
    m_heat->item(r, 2)->setFlags(Qt::NoItemFlags);
    m_heat->setItem(r, 3, new QTableWidgetItem());
    m_heat->item(r, 3)->setFlags(Qt::NoItemFlags);
  }
  m_facePick->set(int(m_faces.size()), names.join(", "), m_picking == Pick::Faces, true);
}

void CoolingAssistant::wallsChanged() {
  AppDocument* d = m_hooks.document();
  keepFacePowers();
  m_parts.clear();
  m_inside->clear();
  const QSignalBlocker quiet(m_heat);
  m_heat->setRowCount(0);
  if (!d || !d->hasDocument || !m_boxes || walls().empty()) {
    showParts();
    return showFaces();
  }
  const opad::Scene& s = d->scene;
  try {
    m_parts = opad::sim::find_enclosure(*m_boxes, {}, walls()).inside;  // from the boxes: nothing measured here
  } catch (const std::exception&) {
  }
  // What the cooling study set, to show it again: heat per body, boards.
  std::map<std::string, double> watts;
  std::map<std::string, int> boards;
  const opad::Study* st = s.study(studyId());
  const json settings = st ? st->def.value("settings", json::object()) : json::object();
  const json mats = settings.value("materials", json::object());
  for (const auto& [id, m] : mats.items())
    if (m.is_object() && m.contains("pcb")) boards[id] = m["pcb"].value("layers", 4);
  for (const auto& l : s.loads) {
    if (l.load_case != kCase) continue;
    if (l.kind == "heat")
      for (const auto& r : l.refs)
        if (r.kind == opad::Ref::Kind::Body) watts[r.body] += l.def.value("value", 0.0) / double(l.refs.size());  // faces: showFaces
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
    // Plain cells (an editor only while one is typed in): a widget per cell took seconds for a box holding a thousand parts.
    auto* w = new QTableWidgetItem();
    w->setData(Qt::EditRole, watts.count(id) ? watts[id] : 1.0);
    m_heat->setItem(int(i), 1, w);
    auto* board = new QTableWidgetItem();
    board->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
    board->setCheckState(boards.count(id) ? Qt::Checked : Qt::Unchecked);
    m_heat->setItem(int(i), 2, board);
    auto* layers = new QTableWidgetItem();
    layers->setData(Qt::EditRole, boards.count(id) ? boards[id] : 4);
    m_heat->setItem(int(i), 3, layers);
  }
  showParts();
  showFaces();
}

std::vector<std::string> CoolingAssistant::heatedParts() const {
  std::vector<std::string> out;
  for (int r = 0; r < int(m_parts.size()) && r < m_heat->rowCount(); ++r)
    if (m_heat->item(r, 0) && m_heat->item(r, 0)->checkState() == Qt::Checked) out.push_back(m_parts[size_t(r)]);
  return out;
}

void CoolingAssistant::showParts() {
  const AppDocument* d = m_hooks.document();
  QStringList names;
  for (const auto& id : heatedParts()) names << (d ? nameOf(d->scene, id) : QString::fromStdString(id));
  m_partsPick->set(int(names.size()), names.join(", "), m_picking == Pick::Parts, true);
}

// ---------------------------------------------------------------- the fans
opad::Vec3 CoolingAssistant::FanSpec::vector() const {
  const opad::Vec3 v = across.body.empty() || std::hypot(normal[0], normal[1], normal[2]) < 0.5 ? ways()[size_t(std::clamp(axis, 0, 5))].second : normal;
  return flip ? opad::Vec3{-v[0], -v[1], -v[2]} : v;
}

void CoolingAssistant::loadFans(const opad::Scene& s) {
  m_fanSpecs.clear();
  const opad::Study* st = s.study(studyId());
  const json settings = st ? st->def.value("settings", json::object()) : json::object();
  const json mats = settings.value("materials", json::object());
  for (const auto& l : s.loads) {
    if (l.load_case != kCase || l.kind != "fan" || l.refs.empty()) continue;
    FanSpec f;
    for (const auto& r : l.refs) f.on.push_back(r.body);
    const json fan = l.def.value("fan", json("80x25"));
    f.model = fan.is_string() ? QString::fromStdString(fan.get<std::string>()) : QString();
    try {
      const air::Fan spec = air::fan_from(fan);
      f.flow = spec.Qmax * 3600, f.pressure = spec.Pmax;
    } catch (const std::exception&) {
    }
    const opad::Vec3 v = l.def.value("vector", opad::Vec3{1, 0, 0});
    if (const json a = l.def.value("across", json()); a.is_object()) {
      try {
        f.across = opad::Ref::from_json(a), f.normal = v;  // the way as written: the face's normal, flipped or not
      } catch (const std::exception&) {
      }
    }
    if (f.across.body.empty())
      for (size_t i = 0; i < ways().size(); ++i)
        if (v[0] * ways()[i].second[0] + v[1] * ways()[i].second[1] + v[2] * ways()[i].second[2] > 0.9) f.axis = int(i);
    for (const auto& h : l.def.value("heatsink", json::array()))
      if (h.is_string()) f.sink.push_back(h.get<std::string>());
    // The heatsink's material, when the study set one for it (each of its bodies has it).
    for (const auto& id : f.sink)
      for (const auto& b : s.node(id) && s.node(id)->kind != opad::Node::Kind::Body ? s.bodies_under(id) : std::vector<std::string>{id})
        if (mats.contains(b) && mats[b].is_object() && mats[b].contains("material")) f.material = QString::fromStdString(mats[b].value("material", ""));
    m_fanSpecs.push_back(f);
  }
  m_withFans->setChecked(!st || !settings.value("cfd", json::object()).value("buoyancy", false) || !m_fanSpecs.empty());
  if (m_fanSpecs.empty() && m_withFans->isChecked())
    for (const auto& id : m_parts)  // a first guess: a body called fan
      if (nameOf(s, id).contains("fan", Qt::CaseInsensitive)) {
        FanSpec f;
        f.on = {id};
        m_fanSpecs.push_back(f);
      }
  m_fan = m_fanSpecs.empty() ? -1 : 0;
  showFans();
}

void CoolingAssistant::showFans() {
  const AppDocument* d = m_hooks.document();
  m_fanLoading = true;
  m_fanList->clear();
  for (size_t i = 0; i < m_fanSpecs.size(); ++i) {
    QStringList names;
    for (const auto& id : m_fanSpecs[i].on) names << (d ? nameOf(d->scene, id) : QString::fromStdString(id));
    m_fanList->addItem(tr("Fan %1: %2").arg(i + 1).arg(names.isEmpty() ? tr("pick it") : names.join(", ")));
  }
  m_fanList->setCurrentRow(m_fan);
  m_fanLoading = false;
  showFan();
}

void CoolingAssistant::showFan() {
  const AppDocument* d = m_hooks.document();
  const bool any = m_fan >= 0 && m_fan < int(m_fanSpecs.size());
  m_fanForm->setVisible(any);
  if (!any) return;
  const FanSpec& f = m_fanSpecs[size_t(m_fan)];
  auto names = [&](const std::vector<std::string>& ids) {
    QStringList out;
    for (const auto& id : ids) {
      const opad::Node* n = d ? d->scene.node(id) : nullptr;
      // A component is one part: said so, with its bodies' count.
      out << (n && n->kind != opad::Node::Kind::Body ? tr("%1 (component, %2 bodies)").arg(nameOf(d->scene, id)).arg(d->scene.bodies_under(id).size())
                                                     : d ? nameOf(d->scene, id) : QString::fromStdString(id));
    }
    return out;
  };
  m_fanLoading = true;
  m_fanPick->set(int(f.on.size()), names(f.on).join(", "), m_picking == Pick::Fan, !f.on.empty());
  const bool face = !f.across.body.empty();
  m_wayPick->set(face ? 1 : 0, face ? tr("%1, face %2").arg(d ? nameOf(d->scene, f.across.body) : QString()).arg(f.across.index + 1) : QString(),
                 m_picking == Pick::Way, true);
  m_axis->setCurrentIndex(std::clamp(f.axis, 0, 5));
  m_fanRows->setRowVisible(m_axis, !face);  // the face sets the way: no axis to choose
  const bool measured = face && std::hypot(f.normal[0], f.normal[1], f.normal[2]) > 0.5;
  m_wayText->setText(face && !measured ? tr("Reading the face…") : tr("The air goes along %1.").arg(QString(QChar(0x202A)) + wayText(f.vector()) + QChar(0x202C)));
  m_model->setCurrentIndex(std::max(0, m_model->findData(f.model)));
  const bool custom = f.model.isEmpty();
  m_flow->setValue(f.flow);
  m_pressure->setValue(f.pressure);
  m_flow->setEnabled(custom);
  m_pressure->setEnabled(custom);
  m_fanRows->setRowVisible(m_flow, custom);  // a custom fan's numbers, under its choice
  m_fanRows->setRowVisible(m_pressure, custom);
  m_sinkPick->set(int(f.sink.size()), names(f.sink).join(", "), m_picking == Pick::Sink, true);
  m_sinkMaterial->setCurrentIndex(std::max(0, m_sinkMaterial->findData(f.material)));
  m_sinkMaterial->setEnabled(!f.sink.empty());
  m_fanLoading = false;
}

void CoolingAssistant::measureWay(int fan) {
  AppDocument* d = m_hooks.document();
  JobRunner* jobs = m_hooks.jobs ? m_hooks.jobs() : nullptr;
  if (!d || !jobs || fan < 0 || fan >= int(m_fanSpecs.size())) return;
  const opad::Ref face = m_fanSpecs[size_t(fan)].across;
  m_fanSpecs[size_t(fan)].normal = {0, 0, 0};
  showFan();
  auto doc = std::make_shared<const opad::Document>(d->doc);
  auto scene = std::make_shared<const opad::Scene>(d->scene);
  auto normal = std::make_shared<opad::Vec3>();
  jobs->async(tr("Reading the face"), [doc, scene, face, normal](Progress) {
    const TopoDS_Shape f = opad::subshape(opad::node_world_shape(*doc, *scene, face.body), opad::Ref::Kind::Face, face.index);
    if (f.IsNull() || f.ShapeType() != TopAbs_FACE) throw opad::Error("no such face");
    const opad::Frame fr = opad::design::face_frame(TopoDS::Face(f));  // throws for a face that is not flat
    const opad::Vec3 n{fr.x[1] * fr.y[2] - fr.x[2] * fr.y[1], fr.x[2] * fr.y[0] - fr.x[0] * fr.y[2], fr.x[0] * fr.y[1] - fr.x[1] * fr.y[0]};
    const double l = std::hypot(n[0], n[1], n[2]);
    *normal = {n[0] / l, n[1] / l, n[2] / l};
  }, [this, self = QPointer<CoolingAssistant>(this), fan, face, normal](bool ok, const QString&) {
    if (!self || fan >= int(m_fanSpecs.size()) || !sameRef(m_fanSpecs[size_t(fan)].across, face)) return;
    FanSpec& f = m_fanSpecs[size_t(fan)];
    if (ok) f.normal = *normal;
    else {
      f.across = {};  // a curved face gives no one way
      m_status->setText(tr("That face is not flat: pick a flat face the air goes through, or choose an axis."));
    }
    showFan();
  });
}

std::string CoolingAssistant::studyId() const {
  const AppDocument* d = m_hooks.document();
  if (!d) return {};
  for (const auto& st : d->scene.studies)
    if (st.kind == "thermal" && QString::fromStdString(st.name) == studyName()) return st.id;
  return {};
}

// ---------------------------------------------------------------- writing it
bool CoolingAssistant::apply() {
  AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument) return false;
  const std::vector<std::string> box = walls();
  if (box.empty()) {
    m_status->setText(tr("Tick the box's bodies on the first page."));
    m_steps->setCurrentRow(0);
    return false;
  }
  json materials = json::object();
  std::vector<std::pair<json, double>> heat;  // on a body (its id) or a face (its reference), and the power
  for (int r = 0; r < m_heat->rowCount(); ++r) {
    const double watts = std::clamp(m_heat->item(r, 1)->data(Qt::EditRole).toDouble(), 0.0, 1000.0);
    if (const QString face = m_heat->item(r, 0)->data(kFaceRole).toString(); !face.isEmpty()) {
      heat.push_back({opad::Ref::parse(face.toStdString()).to_json(), watts});
      continue;
    }
    const std::string id = m_heat->item(r, 0)->data(Qt::UserRole).toString().toStdString();
    if (m_heat->item(r, 0)->checkState() == Qt::Checked) {
      heat.push_back({json(id), watts});
      if (opad::material_of(d->doc, d->scene, id).id.empty()) materials[id] = {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon (assumed)"}};
    }
    if (m_heat->item(r, 2)->checkState() == Qt::Checked)
      materials[id] = {{"pcb", {{"layers", std::clamp(m_heat->item(r, 3)->data(Qt::EditRole).toInt(), 1, 32)}}}};
  }
  if (heat.empty()) {
    m_status->setText(tr("Nothing makes heat: tick at least one part on the Heat page, or pick a face."));
    m_steps->setCurrentRow(1);
    return false;
  }
  const bool fans = m_withFans->isChecked() && std::any_of(m_fanSpecs.begin(), m_fanSpecs.end(), [](const FanSpec& f) { return !f.on.empty(); });
  // The case's loads made again: heat sources, fans.
  std::vector<std::string> old;  // each write rebuilds the scene: its loads are gathered first
  for (const auto& l : d->scene.loads)
    if (l.load_case == kCase) old.push_back(l.id);
  for (const auto& id : old) m_hooks.write("delete", {{"target", id}}, tr("Thermal setup"));
  for (const auto& [on, w] : heat) {
    json list = json::array();
    list.push_back(on);
    m_hooks.write("load", {{"kind", "heat"}, {"case", kCase}, {"on", list}, {"value", w}, {"name", tr("Heat").toStdString()}}, tr("Thermal setup"));
  }
  if (fans)
    for (const FanSpec& f : m_fanSpecs) {
      if (f.on.empty()) continue;
      json args = {{"kind", "fan"}, {"case", kCase}, {"on", f.on}, {"vector", f.vector()}, {"name", tr("Fan").toStdString()},
                   {"fan", f.model.isEmpty() ? json{{"flow", f.flow}, {"pressure", f.pressure}} : json(f.model.toStdString())}};
      if (!f.across.body.empty()) args["across"] = f.across.to_json();
      if (!f.sink.empty()) args["heatsink"] = f.sink;
      m_hooks.write("load", args, tr("Thermal setup"));
      if (!f.material.isEmpty())  // the heatsink one part of one material: each of its bodies
        for (const auto& id : f.sink)
          for (const auto& b : d->scene.node(id) && d->scene.node(id)->kind != opad::Node::Kind::Body ? d->scene.bodies_under(id) : std::vector<std::string>{id})
            materials[b] = {{"material", f.material.toStdString()}};
    }
  json settings = {{"case", kCase},
                   {"air", "cfd"},
                   {"ambient", m_ambient->value()},
                   {"cfd", {{"enclosure", box.size() == 1 ? json(box.front()) : json(box)}, {"quality", m_quality->currentData().toString().toStdString()}, {"radiation", m_radiation->isChecked()},
                            {"buoyancy", !fans}}}};
  if (!materials.empty()) settings["materials"] = materials;
  const std::string id = studyId();
  if (id.empty()) m_hooks.write("study", {{"kind", "thermal"}, {"name", studyName().toStdString()}, {"settings", settings}, {"run", false}}, tr("Thermal setup"));
  else m_hooks.write("study", {{"id", id}, {"settings", settings}, {"run", false}}, tr("Thermal setup"));
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

// ---------------------------------------------------------------- results
void CoolingAssistant::runFinished(const std::string& study, std::shared_ptr<const opad::sim::StudyRun> run, const QString& error) {
  AppDocument* d = m_hooks.document();
  if (!d) return;
  if (study != studyId()) return;
  if (!run) {
    m_status->setText(tr("It did not run: %1").arg(error));
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
  m_hooks.write("load", {{"kind", "fan"}, {"case", kCase}, {"on", {fan}}, {"fan", {{"flow", 8}, {"pressure", 25}}}, {"vector", {1, 0, 0}}, {"heatsink", {hs}}, {"name", tr("Fan").toStdString()}},
                label);
  json settings = {{"case", kCase},
                   {"air", "cfd"},
                   {"ambient", 25},
                   {"cfd", {{"enclosure", encl}, {"quality", "normal"}, {"radiation", true}, {"buoyancy", false}}},
                   {"materials", {{board, {{"pcb", {{"layers", 4}}}}}, {soc, {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon"}}},
                                  {pmic, {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon"}}}}}};
  m_hooks.write("study", {{"kind", "thermal"}, {"name", studyName().toStdString()}, {"settings", settings}, {"run", false}}, label);
  reload();
  m_status->setText(tr("The example is ready: look through the pages, then Run."));
}
