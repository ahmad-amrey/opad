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
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <set>

#include "AppDocument.hpp"
#include "I18n.hpp"
#include "DesignPanels.hpp"
#include "HelpClip.hpp"
#include "Jobs.hpp"
#include "SearchCombo.hpp"
#include "Theme.hpp"
#include "Tracking.hpp"
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

CoolingAssistant::CoolingAssistant(Hooks hooks, QWidget* parent) : QWidget(parent), m_hooks(std::move(hooks)) {
  setObjectName("coolingAssistant");
  m_syncTimer = new QTimer(this);
  m_syncTimer->setSingleShot(true);
  m_syncTimer->setInterval(300);
  connect(m_syncTimer, &QTimer::timeout, this, [this] { sync(); });
  // Changed from elsewhere (undo, the Simulation panel, an agent) while it is open: the pages follow, unless they hold a change
  // of their own still to write.
  if (AppDocument* d = m_hooks.document ? m_hooks.document() : nullptr)
    connect(d, &AppDocument::changed, this, [this] {
      const AppDocument* doc = m_hooks.document();
      if (m_writing || !isVisible() || m_syncTimer->isActive() || !doc) return;
      if (std::make_pair(doc->generation, doc->revision) != m_loaded) reload();
    });
  // A panel (ToolPanel, SimulateArea): the steps as tabs over the page shown, Back and Next under it.
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  m_steps = new QTabBar(this);
  m_steps->setObjectName("coolingSteps");
  m_steps->setExpanding(true);
  m_steps->setDrawBase(false);
  for (const QString& s : {tr("Box"), tr("Heat"), tr("Air"), tr("Run")}) m_steps->addTab(s);
  outer->addWidget(m_steps);
  m_pages = new QStackedWidget(this);
  outer->addWidget(m_pages, 1);
  auto* nav = new QHBoxLayout();
  nav->setContentsMargins(12, 6, 12, 10);
  auto* back = m_back = new QPushButton(tr("Back"), this);
  auto* next = m_next = new QPushButton(tr("Next"), this);
  next->setObjectName("coolingNext");
  nav->addStretch(1);
  nav->addWidget(back);
  nav->addWidget(next);
  outer->addLayout(nav);
  connect(m_steps, &QTabBar::currentChanged, this, &CoolingAssistant::showStep);
  if (m_hooks.onFilterApplied) m_hooks.onFilterApplied(this, [this] { filterApplied(); });
  connect(back, &QPushButton::clicked, this, [this] { m_steps->setCurrentIndex(std::max(0, m_steps->currentIndex() - 1)); });
  connect(next, &QPushButton::clicked, this, [this] { m_steps->setCurrentIndex(std::min(steps() - 1, m_steps->currentIndex() + 1)); });
  // Each page scrolls by itself (as tall as it is, never as the tallest), as wide as the panel.
  auto page = [this](const QString&, const QString& intro) {  // its title is its tab
    auto* scroll = new QScrollArea(m_pages);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* w = new QWidget(scroll);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(12, 10, 12, 8);
    v->setSpacing(10);
    v->addWidget(text(intro, w));
    scroll->setWidget(w);
    m_pages->addWidget(scroll);
    return v;
  };

  // An Add button: on while it picks (it then says Done); Remove takes out the rows chosen.
  auto adder = [this](const QString& label, const char* object) {
    auto* b = new QPushButton(label, this);
    b->setObjectName(object);
    b->setCheckable(true);
    b->setProperty("label", label);
    return b;
  };
  // ---- 1. the box
  {
    QVBoxLayout* v = page(tr("The box"), tr("The air is solved inside the box and in a margin of the room around it: its holes are the vents. Add "
                                            "every body the box is made of (a base and its lid, a frame and its panels): click Add, then the bodies "
                                            "in the view or the browser, and Done. Remove takes out the ones chosen in the list."));
    m_engine = text(QString(), this);
    v->addWidget(m_engine);
    v->addWidget(text(tr("The box's bodies:"), this, "sectionHeader"));
    m_walls = new QListWidget(this);
    m_walls->setObjectName("coolingWalls");
    m_walls->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->addWidget(m_walls, 1);
    auto* row = new QHBoxLayout();
    m_boxAdd = adder(tr("Add"), "coolingBoxAdd");
    m_boxAdd->setToolTip(tr("Then click the box's bodies in the view or the browser; each click adds one"));
    m_boxRemove = new QPushButton(tr("Remove"), this);
    m_boxRemove->setObjectName("coolingBoxRemove");
    row->addWidget(m_boxAdd);
    row->addWidget(m_boxRemove);
    row->addStretch(1);
    v->addLayout(row);
    connect(m_boxAdd, &QPushButton::toggled, this, [this](bool on) { on ? startPicking(Pick::Box) : stopPicking(); });
    connect(m_boxRemove, &QPushButton::clicked, this, [this] {
      std::set<std::string> gone;
      for (const QListWidgetItem* item : m_walls->selectedItems()) gone.insert(item->data(Qt::UserRole).toString().toStdString());
      if (gone.empty()) return;
      std::erase_if(m_picked, [&](const std::string& id) { return gone.count(id) > 0; });
      showPicked();
      if (m_picking == Pick::Box) reselect();
    });
    connect(m_walls, &QListWidget::itemSelectionChanged, this, [this] {
      m_boxRemove->setEnabled(!m_walls->selectedItems().isEmpty());
      lightChosen();  // which body a row is: lit in the view
    });
    m_example = new QPushButton(tr("Build the example: a board in a vented box"), this);
    m_example->setObjectName("coolingExample");
    m_example->setToolTip(tr("A 110 x 80 x 40 mm ABS box with a 30 mm intake fan, three exhaust slots at the height vent_z, a board, a 4 W chip "
                             "under a finned heatsink and a 0.5 W power chip: everything set up to run."));
    v->addWidget(m_example);
    connect(m_example, &QPushButton::clicked, this, [this] { buildExample(); });
  }
  // ---- 2. heat
  {
    QVBoxLayout* v = page(tr("Heat"), tr("Add each part that makes heat and type its power: click Add part, then the part in the view or the "
                                         "browser. A chip joined into its board has no body of its own: click Add face, then its face in the view, "
                                         "and type that face's power. Tick Board for a printed circuit board (add it with no power if it makes "
                                         "none): its copper spreads heat along it far better than through it (give its copper layers). A chip "
                                         "with no material is taken as silicon."));
    m_heat = new QTableWidget(0, 4, this);
    m_heat->setObjectName("coolingHeat");
    m_heat->setHorizontalHeaderLabels({tr("Part"), tr("Watts"), tr("Board"), tr("Layers")});
    m_heat->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c : {1, 2, 3}) m_heat->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    m_heat->horizontalHeaderItem(3)->setToolTip(tr("Copper layers"));
    m_heat->verticalHeader()->hide();
    m_heat->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_heat->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_heat->setItemDelegateForColumn(1, new RangeDelegate(0, 1000, 2, m_heat));
    m_heat->setItemDelegateForColumn(3, new RangeDelegate(1, 32, 0, m_heat));
    v->addWidget(m_heat, 1);
    auto* row = new QHBoxLayout();
    m_partAdd = adder(tr("Add part"), "coolingHeatAddPart");
    m_partAdd->setToolTip(tr("Then click the parts that make heat in the view or the browser; each click adds one"));
    m_faceAdd = adder(tr("Add face"), "coolingHeatAddFace");
    m_faceAdd->setToolTip(tr("Then click a face in the view (the Faces filter is on meanwhile): a chip joined into its board"));
    m_heatRemove = new QPushButton(tr("Remove"), this);
    m_heatRemove->setObjectName("coolingHeatRemove");
    m_heatRemove->setEnabled(false);
    row->addWidget(m_partAdd);
    row->addWidget(m_faceAdd);
    row->addWidget(m_heatRemove);
    row->addStretch(1);
    v->addLayout(row);
    connect(m_partAdd, &QPushButton::toggled, this, [this](bool on) { on ? startPicking(Pick::Parts) : stopPicking(); });
    connect(m_faceAdd, &QPushButton::toggled, this, [this](bool on) { on ? startPicking(Pick::Faces) : stopPicking(); });
    connect(m_heatRemove, &QPushButton::clicked, this, [this] {
      std::set<int> rows;
      for (const QModelIndex& i : m_heat->selectionModel()->selectedRows()) rows.insert(i.row());
      for (auto r = rows.rbegin(); r != rows.rend(); ++r) m_heat->removeRow(*r);
      if (m_picking == Pick::Parts || m_picking == Pick::Faces) reselect();
      scheduleSync();
    });
    connect(m_heat, &QTableWidget::itemChanged, this, [this] {  // a power, a board's tick or layers
      scheduleSync();
      lightChosen();  // a board ticked is lit as one
    });
    connect(m_heat, &QTableWidget::itemSelectionChanged, this, [this] {
      m_heatRemove->setEnabled(m_heat->selectionModel()->hasSelection());
      lightChosen();  // which part or face a row is: lit in the view (a face too, whatever the filter)
    });
  }
  // ---- 3. air
  {
    QVBoxLayout* v = page(tr("Air"), tr("How the air moves through the box. With fans: Add a fan and set it up by its openings: an axial fan's "
                                        "opening, a blower's inlet and outlet. Draw… makes a sketch on the fan's face (project its edges, draw the "
                                        "opening in them, Finish), or click a sketch's profile or a flat face. Then its datasheet's max airflow and "
                                        "max static pressure (or a library fan), and the heatsink it blows through. With natural convection there "
                                        "are no fans: the air rises where the parts warm it and leaves through the vents, so put vents low and high."));
    m_fansOn = new QRadioButton(tr("Fans"), this);
    m_fansOn->setObjectName("coolingFans");
    m_mixed = new QRadioButton(tr("Fans and warm air rising (slower)"), this);
    m_mixed->setObjectName("coolingMixed");
    m_natural = new QRadioButton(tr("Natural convection (no fans)"), this);
    m_natural->setObjectName("coolingNatural");
    m_fansOn->setChecked(true);
    v->addWidget(m_fansOn);
    v->addWidget(m_mixed);
    v->addWidget(m_natural);
    m_airNote = text(QString(), this);
    m_airNote->setObjectName("coolingAirNote");
    v->addWidget(m_airNote);
    auto airNote = [this] {
      m_airNote->setText((m_airChanged.isEmpty() ? QString() : m_airChanged + "\n") + (m_natural->isChecked() ? tr("No fans: the air rises where the parts warm it and leaves through the vents, so put vents low and high.")
                         : m_mixed->isChecked() ? tr("The fans' flow and the warm air rising, solved together: closer where the fans move the air slowly "
                                                     "and on the box's outside walls, but the run takes several times longer.")
                                                : tr("Only the fans move the air (quickest). Warm air rising is left out, so the temperatures come out "
                                                     "a little high where the air is slow and on the box's outside walls: on the safe side.")));
    };
    m_showAirNote = airNote;
    airNote();
    auto* columns = new QVBoxLayout();  // the fans, then the one chosen's form under them (a panel is narrow)
    auto* left = new QVBoxLayout();
    m_fanList = new QListWidget(this);
    m_fanList->setObjectName("coolingFanList");
    m_fanList->setMaximumHeight(96);
    left->addWidget(m_fanList, 1);
    auto* buttons = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add a fan"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    add->setObjectName("coolingFanAdd");
    remove->setObjectName("coolingFanRemove");
    buttons->addWidget(add);
    buttons->addWidget(remove);
    left->addLayout(buttons);
    columns->addLayout(left);
    m_fanForm = new QWidget(this);
    m_fanRows = new QFormLayout(m_fanForm);
    m_fanRows->setContentsMargins(0, 6, 0, 0);
    m_fanType = new QComboBox(m_fanForm);
    m_fanType->setObjectName("coolingFanType");
    m_fanType->addItem(tr("Axial fan (the air through it)"), "axial");
    m_fanType->addItem(tr("Blower (in on top, out of its side)"), "blower");
    m_fanRows->addRow(tr("Type:"), m_fanType);
    // Its openings: a closed region of a sketch drawn on the fan's face (Draw…: a sketch on the face, its outline projected,
    // the opening drawn in it), or a flat face; picked among the sketches' regions in the view.
    auto opening = [this](PickBox*& pick, QPushButton*& draw, QPushButton*& flip, const char* name) {
      auto* row = new QHBoxLayout();
      pick = new PickBox(m_fanForm);
      pick->setObjectName(QString("cooling%1Pick").arg(name));
      draw = new QPushButton(tr("Draw…"), m_fanForm);
      draw->setObjectName(QString("cooling%1Draw").arg(name));
      draw->setToolTip(tr("A sketch on the fan's face: project its edges, draw the opening in it, Finish; it becomes this opening"));
      flip = new QPushButton(tr("Flip"), m_fanForm);
      flip->setObjectName(QString("cooling%1Flip").arg(name));
      flip->setToolTip(tr("Turns this opening's way round, when the arrow in the view points the wrong way (out of the housing for an "
                          "outlet, into it for an inlet)"));
      row->addWidget(pick, 1);
      row->addWidget(draw);
      row->addWidget(flip);
      return row;
    };
    m_fanRows->addRow(tr("Outlet:"), opening(m_outletPick, m_outletDraw, m_outletFlip, "Outlet"));
    m_fanRows->addRow(tr("Inlet:"), opening(m_inletPick, m_inletDraw, m_inletFlip, "Inlet"));
    m_flip = new QPushButton(tr("Flip"), m_fanForm);
    m_flip->setObjectName("coolingWayFlip");
    m_wayText = text(QString(), m_fanForm);
    auto* way = new QHBoxLayout();
    way->addWidget(m_wayText, 1);
    way->addWidget(m_flip);
    m_fanRows->addRow(tr("Air:"), way);
    m_fanClip = new ClipView(QString(), m_fanForm);
    m_fanClip->setObjectName("coolingFanClip");
    m_fanRows->addRow(m_fanClip);
    // Its datasheet: a library fan, or the two numbers every datasheet gives (max airflow, max static pressure) in its units,
    // with the P-Q curve read off its chart when it has one; the speed it runs at (PWM, a lower voltage) scales them.
    m_model = new QComboBox(m_fanForm);
    m_model->setObjectName("coolingFanModel");
    search_combo::enable(m_model);
    m_fanRows->addRow(tr("Fan model:"), m_model);
    auto withUnit = [this](QDoubleSpinBox*& box, QComboBox*& unit, const char* name, const std::vector<std::pair<QString, QString>>& units) {
      auto* row = new QHBoxLayout();
      box = new QDoubleSpinBox(m_fanForm);
      box->setObjectName(name);
      box->setRange(0.001, 100000);
      box->setDecimals(3);
      box->setKeyboardTracking(false);
      unit = new QComboBox(m_fanForm);
      unit->setObjectName(QString(name) + "Unit");
      for (const auto& [label, id] : units) unit->addItem(label, id);
      row->addWidget(box, 1);
      row->addWidget(unit);
      return row;
    };
    m_fanRows->addRow(tr("Max airflow:"), withUnit(m_flow, m_flowUnit, "coolingFanFlow",
                                                   {{"CFM", "cfm"}, {QStringLiteral("m³/h"), "m3h"}, {QStringLiteral("m³/min"), "m3min"}, {"l/s", "ls"}}));
    m_fanRows->addRow(tr("Max static pressure:"), withUnit(m_pressure, m_pressureUnit, "coolingFanPressure",
                                                           {{QStringLiteral("inH₂O"), "inH2O"}, {QStringLiteral("mmH₂O"), "mmH2O"}, {"Pa", "Pa"}}));
    m_curve = new QLineEdit(m_fanForm);
    m_curve->setObjectName("coolingFanCurve");
    m_curve->setPlaceholderText(tr("airflow pressure; airflow pressure; … (optional)"));
    m_curve->setToolTip(tr("Points read off the datasheet's P-Q chart, in the units above, each an airflow and the static pressure at it. "
                           "Without them the curve is a straight line from the max static pressure to the max airflow."));
    m_fanRows->addRow(tr("P-Q curve:"), m_curve);
    m_speed = new QDoubleSpinBox(m_fanForm);
    m_speed->setObjectName("coolingFanSpeed");
    m_speed->setRange(10, 150);
    m_speed->setDecimals(0);
    m_speed->setSuffix(" %");
    m_speed->setKeyboardTracking(false);
    m_speed->setToolTip(tr("Of its rated speed (the datasheet's): its airflow scales with the speed, its pressure with the speed squared"));
    m_fanRows->addRow(tr("Speed:"), m_speed);
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
    for (QComboBox* box : {m_fanType, m_model, m_sinkMaterial}) {  // as narrow as the panel (the longest item would widen it)
      box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
      box->setMinimumContentsLength(8);
    }
    columns->addWidget(m_fanForm, 1);
    v->addLayout(columns, 1);
    auto edit = [this](auto change) {
      return [this, change](auto&&...) {
        if (m_fanLoading || m_fan < 0 || m_fan >= int(m_fanSpecs.size())) return;
        change(m_fanSpecs[size_t(m_fan)]);
        showFans();
      };
    };
    m_ventText = text(QString(), this);
    m_ventText->setObjectName("coolingVents");
    v->addWidget(m_ventText);
    m_ventsShown = new QCheckBox(tr("Show the vents found in the view"), this);
    m_ventsShown->setObjectName("coolingVentsShown");
    m_ventsShown->setVisible(false);
    v->addWidget(m_ventsShown);
    connect(m_ventsShown, &QCheckBox::toggled, this, [this] { lightChosen(); });
    auto fanPick = [this] { return m_picking == Pick::Outlet || m_picking == Pick::Inlet || m_picking == Pick::Sink; };
    connect(m_natural, &QRadioButton::toggled, this, [this, add, remove, fanPick](bool natural) {
      const bool on = !natural;  // the fans, with or without the warm air rising
      m_fanList->setEnabled(on);
      m_fanForm->setEnabled(on);
      add->setEnabled(on);
      remove->setEnabled(on);
      m_ventText->setVisible(on);
      m_ventsShown->setVisible(on && !m_ventOpenings.empty());
      if (!on && fanPick()) stopPicking();
    });
    for (QRadioButton* r : {m_fansOn, m_mixed, m_natural})
      connect(r, &QRadioButton::toggled, this, [this, airNote](bool on) {
        if (!on) return;
        if (!m_reloading) m_airChanged.clear();  // the user's own choice
        airNote();
        scheduleSync();
        showEstimate();
      });
    connect(add, &QPushButton::clicked, this, [this] {
      m_fanSpecs.push_back({});
      m_fan = int(m_fanSpecs.size()) - 1;
      showFans();
      startPicking(Pick::Outlet);  // its opening picked among the sketches' regions (or Draw…)
    });
    connect(remove, &QPushButton::clicked, this, [this, fanPick] {
      if (m_fan < 0 || m_fan >= int(m_fanSpecs.size())) return;
      m_fanSpecs.erase(m_fanSpecs.begin() + m_fan);
      m_fan = std::min(m_fan, int(m_fanSpecs.size()) - 1);
      showFans();
      if (fanPick()) reselect();
    });
    connect(m_fanList, &QListWidget::currentRowChanged, this, [this, fanPick](int row) {
      if (m_fanLoading || row < 0 || row == m_fan) return;
      m_fan = row;
      showFan();
      lightChosen();
      if (fanPick()) reselect();  // this fan's picks shown
    });
    connect(m_fanType, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) {
      f.type = m_fanType->currentData().toString();
      if (!f.model.isEmpty() && QString::fromStdString(air::fan_from(json(f.model.toStdString())).kind) != f.type) f.model.clear();  // the library's of that type
      if (f.type != "blower" && m_picking == Pick::Inlet) startPicking(Pick::Outlet);
    }));
    connect(m_outletPick, &QPushButton::clicked, this, [this] { startPicking(Pick::Outlet); });
    connect(m_inletPick, &QPushButton::clicked, this, [this] { startPicking(Pick::Inlet); });
    connect(m_sinkPick, &QPushButton::clicked, this, [this] { startPicking(Pick::Sink); });
    auto drawing = [this] {
      for (size_t i = 0; i < m_fanSpecs.size(); ++i) m_fanSpecs[i].drawing = int(i) == m_fan;
    };
    connect(m_outletDraw, &QPushButton::clicked, this, [this, drawing] {
      stopPicking();
      drawing();
      if (m_hooks.draw) m_hooks.draw(m_fan, false);
    });
    connect(m_inletDraw, &QPushButton::clicked, this, [this, drawing] {
      stopPicking();
      drawing();
      if (m_hooks.draw) m_hooks.draw(m_fan, true);
    });
    connect(m_outletPick, &PickBox::cleared, this, edit([this](FanSpec& f) {
      f.outlet = json(), f.normal = {0, 0, 0}, f.flip = false, f.outletLoops.clear();
      if (m_picking == Pick::Outlet) reselect();
    }));
    connect(m_inletPick, &PickBox::cleared, this, edit([this](FanSpec& f) {
      f.inlet = json(), f.inletLoops.clear();
      if (m_picking == Pick::Inlet) reselect();
    }));
    connect(m_sinkPick, &PickBox::cleared, this, edit([this](FanSpec& f) {
      f.sink.clear();
      if (m_picking == Pick::Sink) m_hooks.select({});
    }));
    connect(m_flip, &QPushButton::clicked, this, edit([](FanSpec& f) { f.flip = !f.flip; }));
    connect(m_outletFlip, &QPushButton::clicked, this, edit([](FanSpec& f) { f.flipOut = !f.flipOut; }));
    connect(m_inletFlip, &QPushButton::clicked, this, edit([](FanSpec& f) { f.flipIn = !f.flipIn; }));
    connect(m_model, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) {
      f.model = m_model->currentData().toString();
      if (f.model.isEmpty()) return;
      const air::Fan spec = air::fan_from(json(f.model.toStdString()));  // its numbers shown, in the units chosen
      f.flow = spec.Qmax / air::flow_unit(f.flowUnit.toStdString()), f.pressure = spec.Pmax / air::pressure_unit(f.pressureUnit.toStdString());
      f.curve.clear();
    }));
    connect(m_flow, &QDoubleSpinBox::valueChanged, this, edit([this](FanSpec& f) { f.flow = m_flow->value(); }));
    connect(m_pressure, &QDoubleSpinBox::valueChanged, this, edit([this](FanSpec& f) { f.pressure = m_pressure->value(); }));
    // Another unit: the same fan, its numbers (and its curve's) converted.
    connect(m_flowUnit, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) {
      const QString to = m_flowUnit->currentData().toString();
      const double k = air::flow_unit(f.flowUnit.toStdString()) / air::flow_unit(to.toStdString());
      f.flow *= k;
      for (auto& p : f.curve) p[0] *= k;
      f.flowUnit = to;
    }));
    connect(m_pressureUnit, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) {
      const QString to = m_pressureUnit->currentData().toString();
      const double k = air::pressure_unit(f.pressureUnit.toStdString()) / air::pressure_unit(to.toStdString());
      f.pressure *= k;
      for (auto& p : f.curve) p[1] *= k;
      f.pressureUnit = to;
    }));
    connect(m_curve, &QLineEdit::editingFinished, this, edit([this](FanSpec& f) {
      f.curve.clear();
      for (const QString& point : m_curve->text().split(';', Qt::SkipEmptyParts)) {
        const QStringList v = point.simplified().replace(',', ' ').split(' ', Qt::SkipEmptyParts);
        bool a = false, b = false;
        if (v.size() == 2) {
          const double q = v[0].toDouble(&a), p = v[1].toDouble(&b);
          if (a && b && q >= 0 && p >= 0) f.curve.push_back({q, p});
        }
      }
    }));
    connect(m_speed, &QDoubleSpinBox::valueChanged, this, edit([this](FanSpec& f) { f.speed = m_speed->value(); }));
    connect(m_sinkMaterial, &QComboBox::currentIndexChanged, this, edit([this](FanSpec& f) { f.material = m_sinkMaterial->currentData().toString(); }));
  }
  // ---- 4. run
  {
    QVBoxLayout* v = page(tr("Run"), tr("Quick gives a first answer in minutes and is for comparing; Normal is the answer to trust; Fine checks it. "
                                        "The parts' radiation to each other and to the room is counted too."));
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
    m_quality->setCurrentIndex(0);  // Quick: a first answer in minutes
    g->addWidget(m_quality, 1, 1);
    m_estimate = text(QString(), this);
    m_estimate->setObjectName("coolingEstimate");
    g->addWidget(m_estimate, 3, 0, 1, 3);
    m_radiation = new QCheckBox(tr("Radiation"), this);
    m_radiation->setChecked(true);
    g->addWidget(m_radiation, 2, 1);
    connect(m_ambient, &QDoubleSpinBox::valueChanged, this, [this] { scheduleSync(); });
    connect(m_quality, &QComboBox::currentIndexChanged, this, [this] {
      scheduleSync();
      showEstimate();
    });
    connect(m_radiation, &QCheckBox::toggled, this, [this] { scheduleSync(); });
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
  m_steps->setCurrentIndex(0);
}

int CoolingAssistant::step() const { return m_steps->currentIndex(); }
int CoolingAssistant::steps() const { return m_steps->count(); }

void CoolingAssistant::open(int at) {
  // Found again when it was closed or the document changed since: else the pages stay as they are (a page switch).
  const AppDocument* d = m_hooks.document();
  const std::pair<unsigned long long, unsigned long long> state = d ? std::make_pair(d->generation, d->revision) : std::make_pair(0ull, 0ull);
  if (!isVisible() || state != m_loaded) reload();
  const int was = step();
  if (at >= 0) m_steps->setCurrentIndex(std::clamp(at, 0, steps() - 1));
  if (m_hooks.panel) m_hooks.panel(true);  // its panel shown over the view
  else QWidget::show();
  if (step() == was) showStep(was);  // shown now: the page's things selected (a page switch did it already)
}

void CoolingAssistant::hideEvent(QHideEvent* e) {
  if (m_syncTimer->isActive()) sync();  // a change still to write
  stopPicking();  // the view's clicks select again
  if (m_hooks.light) m_hooks.light({});  // what its rows lit goes
  if (m_hooks.fanGlyphs) m_hooks.fanGlyphs({});
  QWidget::hideEvent(e);
}

void CoolingAssistant::dismiss() {
  if (m_hooks.panel) m_hooks.panel(false);
  else hide();
}

void CoolingAssistant::escape() {
  if (m_picking != Pick::None) stopPicking();  // the Add button that was on goes off first
  else dismiss();
}

void CoolingAssistant::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) escape();
  else QWidget::keyPressEvent(e);
}

void CoolingAssistant::showStep(int i) {
  if (i >= 0 && i < m_pages->count()) m_pages->setCurrentIndex(i);
  if (i == 2) estimateVents();
  m_back->setEnabled(i > 0);
  m_next->setVisible(i < steps() - 1);  // the last page runs (its Run button)
  // A page picks only while one of its Add buttons (or a fan's pick boxes) is on; the rows chosen in its list are lit in the
  // view in the colour of what they are (a selection's blue over them would turn a heat source's red purple).
  stopPicking();
  lightChosen();
}

// ---------------------------------------------------------------- the pages from the document
void CoolingAssistant::reload() {
  if (m_syncTimer->isActive()) sync();  // a change of the pages still to write goes first
  m_reloading = true;
  struct Done {
    bool& flag;
    ~Done() { flag = false; }
  } done{m_reloading};
  AppDocument* d = m_hooks.document();
  const bool sameDocument = d && m_loaded.first == d->generation && m_loaded.first != ~0ull;
  if (!sameDocument) m_fanSpecs.clear(), m_fan = -1;  // another document: no fan of the last one kept
  m_sameDocument = sameDocument;
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
  m_heat->setRowCount(0);
  m_status->clear();
  if (!d || !d->hasDocument) {
    m_fanSpecs.clear();
    m_fan = -1;
    showPicked();
    return showFans();
  }
  const opad::Scene& s = d->scene;
  // Nothing is guessed: the lists are what the cooling study and its loads hold (empty for a new one).
  const opad::Study* st = s.study(studyId());
  const json settings = st ? st->def.value("settings", json::object()) : json::object();
  {  // the Run page as the study has it; a new one: Quick (a first answer in minutes), radiation counted, a 25 °C room
    const json cfd = settings.value("cfd", json::object());
    const int q = m_quality->findData(QString::fromStdString(cfd.value("quality", std::string("quick"))));
    m_quality->setCurrentIndex(q >= 0 ? q : 0);
    m_radiation->setChecked(cfd.value("radiation", true));
    m_ambient->setValue(settings.value("ambient", 25.0));
  }
  const json e = settings.value("cfd", json::object()).value("enclosure", json());
  for (const auto& w : e.is_array() ? e : json::array({e}))
    if (w.is_string() && pickable(w.get<std::string>()) && std::find(m_picked.begin(), m_picked.end(), w.get<std::string>()) == m_picked.end())
      m_picked.push_back(w.get<std::string>());
  showPicked();
  // The heat table: each body that makes heat (with its power), each board, each face (a chip joined into its board).
  std::vector<std::string> order;
  std::map<std::string, double> watts;
  std::vector<std::pair<opad::Ref, double>> faces;
  for (const auto& l : s.loads) {
    if (l.load_case != kCase || l.kind != "heat") continue;
    const double each = l.def.value("value", 0.0) / double(std::max<size_t>(1, l.refs.size()));
    for (const auto& r : l.refs) {
      if (r.kind == opad::Ref::Kind::Face) {
        faces.push_back({r, each});
      } else if (pickable(r.body)) {
        if (!watts.count(r.body)) order.push_back(r.body);
        watts[r.body] += each;
      }
    }
  }
  std::map<std::string, int> boards;
  const json materials = settings.value("materials", json::object());
  for (const auto& [id, m] : materials.items())
    if (m.is_object() && m.contains("pcb") && pickable(id)) {
      boards[id] = m["pcb"].value("layers", 4);
      if (!watts.count(id)) order.push_back(id);
    }
  for (const auto& id : order) addHeatBody(id, watts.count(id) ? watts[id] : 0.0, boards.count(id) > 0, boards.count(id) ? boards[id] : 4);
  for (const auto& [f, w] : faces) addHeatFace(f, w);
  std::vector<std::string> all = s.all_bodies();
  m_example->setVisible(all.empty());
  loadFans(s);
  showStep(step());
}

// The rows chosen on the page shown, lit in the view in the colour of what they are: the box's bodies, a part that makes heat
// (a board as one), a face (a chip joined into its board), the current fan with its heatsink. Choosing a row says which is which.
void CoolingAssistant::lightChosen() {
  if (!m_hooks.light || !isVisible()) return;  // closed: hideEvent put its lights out, and the browser's rows may light now
  std::vector<std::pair<opad::Ref, std::string>> lit;
  auto body = [](const std::string& id) {
    opad::Ref r;
    r.body = id;
    return r;
  };
  if (step() == 0) {
    for (const QListWidgetItem* item : m_walls->selectedItems()) lit.push_back({body(item->data(Qt::UserRole).toString().toStdString()), "box"});
  } else if (step() == 1) {
    for (const QModelIndex& i : m_heat->selectionModel()->selectedRows()) {
      const QTableWidgetItem* part = m_heat->item(i.row(), 0);
      if (!part) continue;
      if (const QString face = part->data(kFaceRole).toString(); !face.isEmpty()) lit.push_back({opad::Ref::parse(face.toStdString()), "heat"});
      else
        lit.push_back({body(part->data(Qt::UserRole).toString().toStdString()),
                       m_heat->item(i.row(), 2) && m_heat->item(i.row(), 2)->checkState() == Qt::Checked ? "board" : "heat"});
    }
  }
  std::vector<json> fans;  // the current fan's openings and the air's way through them, as the solver will take them
  if (step() == 2 && !naturalConvection() && m_fan >= 0 && m_fan < int(m_fanSpecs.size())) {
    const FanSpec& f = m_fanSpecs[size_t(m_fan)];
    for (const auto& id : f.on) lit.push_back({body(id), "fan"});
    for (const json& o : {f.outlet, f.inlet})  // an opening that is a face: lit as the fan
      if (o.is_object() && o.contains("body")) try {
          lit.push_back({opad::Ref::from_json(o), "fan"});
        } catch (const std::exception&) {
        }
    for (const auto& id : f.sink) lit.push_back({body(id), "heatsink"});
    const opad::Vec3 way = f.vector();
    if (!f.outletLoops.empty()) fans.push_back({{"loops", f.outletLoops}, {"centre", f.centre}, {"vector", way}});
    if (f.type == "blower" && !f.inletLoops.empty()) {  // the air going in: against the inlet's normal out of the housing
      opad::Vec3 n = f.inletNormal;
      if ((n[0] * (f.inletCentre[0] - f.centre[0]) + n[1] * (f.inletCentre[1] - f.centre[1]) + n[2] * (f.inletCentre[2] - f.centre[2])) < 0)
        n = {-n[0], -n[1], -n[2]};
      if (f.flipIn) n = {-n[0], -n[1], -n[2]};
      fans.push_back({{"loops", f.inletLoops}, {"centre", f.inletCentre}, {"vector", opad::Vec3{-n[0], -n[1], -n[2]}}, {"inward", true}});
    }
    if (f.outlet.is_null() && !f.on.empty()) fans.push_back({{"refs", f.on}, {"vector", way}});  // an older setup's
  }
  if (step() == 2 && !naturalConvection() && m_ventsShown->isChecked())  // the vents found: a disk of each one's area, the air's way
    for (const auto& o : m_ventOpenings) {
      const opad::Vec3 c = o.value("centre", opad::Vec3{0, 0, 0});
      opad::Vec3 n = o.value("normal", opad::Vec3{0, 0, 1});
      const double r = std::sqrt(std::max(0.0, o.value("area_mm2", 0.0)) / M_PI);
      const std::string side = o.value("side", std::string());
      const bool in = side == "intake" || side == "fan_intake";  // the air going in: the arrow comes to it
      opad::Vec3 u = std::fabs(n[0]) < 0.9 ? opad::Vec3{0, n[2], -n[1]} : opad::Vec3{-n[2], 0, n[0]};
      const double ul = std::hypot(u[0], u[1], u[2]);
      u = {u[0] / ul, u[1] / ul, u[2] / ul};
      const opad::Vec3 w{n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
      std::vector<opad::Vec3> loop;
      for (int k = 0; k < 24; ++k) {
        const double a = 2 * M_PI * k / 24;
        loop.push_back({c[0] + r * (std::cos(a) * u[0] + std::sin(a) * w[0]), c[1] + r * (std::cos(a) * u[1] + std::sin(a) * w[1]),
                        c[2] + r * (std::cos(a) * u[2] + std::sin(a) * w[2])});
      }
      fans.push_back({{"loops", {loop}}, {"centre", c}, {"vector", in ? opad::Vec3{-n[0], -n[1], -n[2]} : n}, {"inward", in}});
    }
  m_hooks.light(lit);
  if (m_hooks.fanGlyphs) m_hooks.fanGlyphs(fans);
}

std::vector<std::string> CoolingAssistant::walls() const { return m_picked; }

bool CoolingAssistant::pickable(const std::string& id) const {
  const AppDocument* d = m_hooks.document();
  const opad::Node* n = d ? d->scene.node(id) : nullptr;
  return n && n->kind == opad::Node::Kind::Body && d->scene.effectively_visible(id);
}

// The kind of run the Run page would start: its quality and how the air moves ("normal-mixed").
QString CoolingAssistant::runKind() const {
  return m_quality->currentData().toString() + "-" + (naturalConvection() ? "natural" : warmAirRises() ? "mixed" : "fans");
}

// How long a run takes, before it is started: measured on a board in a vented box (Quick with fans: 10 min, 360k cells:
// meshing 2 min, the flow 5, the heat 2.5; with the warm air rising too, 22 min: the flow solved again in rounds with the heat);
// Normal has about 3.4 times the cells and 2.5 times the flow's iterations, Fine 11 and 3.75. The last run of the same kind on
// this computer says more.
void CoolingAssistant::showEstimate() {
  if (!m_estimate) return;
  const QString q = m_quality->currentData().toString();
  const bool rising = warmAirRises();
  QString guess = q == "quick" ? (rising ? tr("about 20 minutes") : tr("about 10 minutes"))
                  : q == "fine" ? (rising ? tr("most of a day") : tr("several hours"))
                                : (rising ? tr("a few hours") : tr("about an hour"));
  QString text = tr("Expect %1 for a box like the example (a board, a few chips, a heatsink; more parts take longer).").arg(guess);
  const double last = QSettings().value("cooling/lastRun/" + runKind(), 0.0).toDouble();
  if (last > 0) text += " " + tr("The last run like this here took %n minute(s).", "", std::max(1, int(std::lround(last / 60))));
  m_estimate->setText(text);
}

bool CoolingAssistant::naturalConvection() const { return m_natural->isChecked(); }
bool CoolingAssistant::warmAirRises() const { return m_natural->isChecked() || m_mixed->isChecked(); }

// ---------------------------------------------------------------- picking: an Add button on
void CoolingAssistant::startPicking(Pick what) {
  if (!isVisible()) return syncAdd();
  const Pick was = m_picking;
  m_picking = Pick::None;  // switching the filter clears the selection: no pick taken
  const QString filter = what == Pick::Faces || what == Pick::Outlet || what == Pick::Inlet ? "select.faces" : "select.bodies";  // pickFilter()
  QString before = filter;
  if (m_hooks.filter) {
    before = m_hooks.filter(filter);
    if (was == Pick::None) m_filterBefore = before;
  }
  if (was == Pick::None && m_hooks.accumulate) m_hooks.accumulate(true);  // clicks gather, as a feature's picks
  m_picking = what;
  // The view's picks taken once its targets are built (filterApplied); a filter that did not switch (its command refused)
  // waits for nothing, and a switch never reported waits 3 s at most: clicks were ignored for good otherwise.
  m_awaiting = before != filter && m_hooks.onFilterApplied && m_hooks.filter(QString()) == filter;
  if (m_awaiting)
    QTimer::singleShot(3000, this, [this, what] {
      if (m_awaiting && m_picking == what) m_awaiting = false, reselect();
    });
  const bool profiles = what == Pick::Outlet || what == Pick::Inlet;
  if (m_hooks.profiles && profiles != (was == Pick::Outlet || was == Pick::Inlet)) m_hooks.profiles(profiles);  // the sketches' regions offered
  if (!m_awaiting) reselect();
  syncAdd();
  showFan();
}

QString CoolingAssistant::pickFilter() const {
  return m_picking == Pick::Faces || m_picking == Pick::Outlet || m_picking == Pick::Inlet ? "select.faces" : "select.bodies";
}

// Faces can be selected only once the Faces filter has built their pick targets (a sliced job), and a filter switched (here,
// by hand, by a key) clears the selection, not the lists: once the page's filter is applied what they hold is selected again.
void CoolingAssistant::filterApplied() {
  if (m_picking == Pick::None || !m_hooks.filter || m_hooks.filter(QString()) != pickFilter()) return;
  m_awaiting = false;
  reselect();
}

void CoolingAssistant::stopPicking() {
  if (m_picking == Pick::None) return syncAdd();
  if (m_hooks.profiles && (m_picking == Pick::Outlet || m_picking == Pick::Inlet)) m_hooks.profiles(false);
  m_picking = Pick::None;
  m_awaiting = false;
  if (m_hooks.accumulate) m_hooks.accumulate(false);
  if (m_hooks.filter && !m_filterBefore.isEmpty()) m_hooks.filter(m_filterBefore);  // the filter the view had
  m_filterBefore.clear();
  if (m_hooks.select) m_hooks.select({});  // the picks were shown selected; now the rows chosen say which is which, in their colour
  syncAdd();
  showFan();
}

void CoolingAssistant::syncAdd() {
  for (auto [button, what] : {std::pair{m_boxAdd, Pick::Box}, std::pair{m_partAdd, Pick::Parts}, std::pair{m_faceAdd, Pick::Faces}}) {
    const QSignalBlocker quiet(button);
    const bool on = m_picking == what;
    button->setChecked(on);
    button->setText(on ? tr("Done") : button->property("label").toString());  // on: it picks until clicked again
  }
}

void CoolingAssistant::reselect() {
  if (!m_hooks.select || m_picking == Pick::None) return;
  if (m_picking == Pick::Faces) return m_hooks.select(heatFaces());
  const FanSpec* fan = m_fan >= 0 && m_fan < int(m_fanSpecs.size()) ? &m_fanSpecs[size_t(m_fan)] : nullptr;
  if (m_picking == Pick::Outlet || m_picking == Pick::Inlet) {  // its opening: a face, or a sketch's region (a candidate)
    const json p = !fan ? json() : m_picking == Pick::Outlet ? fan->outlet : fan->inlet;
    if (p.is_object() && p.contains("sketch")) {
      m_hooks.select({});
      if (m_hooks.selectCandidate) m_hooks.selectCandidate(p.dump());
      return;
    }
    std::vector<opad::Ref> refs;
    try {
      if (p.is_object()) refs.push_back(opad::Ref::from_json(p));
    } catch (const std::exception&) {
    }
    return m_hooks.select(refs);
  }
  const std::vector<std::string> ids = m_picking == Pick::Box     ? m_picked
                                       : m_picking == Pick::Parts ? heatBodies()
                                       : !fan                     ? std::vector<std::string>{}
                                                                  : fan->sink;
  std::vector<opad::Ref> refs;
  for (const auto& id : ids) {
    opad::Ref r;
    r.body = id;  // a body, or a component (one part): its row in the browser, its bodies in the view
    refs.push_back(r);
  }
  m_hooks.select(refs);  // what the list holds, shown selected in the view and the browser
}

void CoolingAssistant::selectionChanged(const std::vector<std::string>& ids, const std::vector<opad::Ref>& refs, const std::vector<std::string>& candidates) {
  // A filter switched (by the page, by hand) clears the selection, not the lists: shown again once applied (filterApplied).
  // A sketch's profile clicked for a fan's opening is one whatever the filter.
  const bool profile = (m_picking == Pick::Outlet || m_picking == Pick::Inlet) &&
                       std::any_of(candidates.begin(), candidates.end(), [](const std::string& c) { return c.find("\"sketch\"") != std::string::npos; });
  if (trace::enabled() && (m_picking == Pick::Outlet || m_picking == Pick::Inlet))
    trace::log(QString("cooling: opening pick: %1 profile(s), %2 face(s)%3").arg(candidates.size()).arg(refs.size()).arg(m_awaiting ? ", waiting for the filter" : ""));
  if (m_picking == Pick::None || (!profile && (m_awaiting || (m_hooks.filterSwitching && m_hooks.filterSwitching())))) return;
  const AppDocument* d = m_hooks.document();
  if (!d) return;
  // Another filter switched to while picking (the ribbon, a key): its picks are something else, the page's are kept.
  if (!profile && m_hooks.filter && m_hooks.filter(QString()) != pickFilter()) return;
  struct Written {  // whatever was added, written a moment later
    CoolingAssistant* self;
    ~Written() { self->scheduleSync(); }
  } written{this};
  // An Add button's list only grows: what is clicked is added; a click that took a listed one out of the selection (the
  // view's clicks toggle) shows it selected again. Remove takes rows out.
  auto restore = [this] { QTimer::singleShot(0, this, [this] { reselect(); }); };
  if (m_picking == Pick::Faces) {
    const std::vector<opad::Ref> had = heatFaces();
    bool dropped = had.size() > refs.size();
    for (const auto& r : refs)
      if (r.kind == opad::Ref::Kind::Face && pickable(r.body) && std::none_of(had.begin(), had.end(), [&](const opad::Ref& f) { return sameRef(f, r); }))
        addHeatFace(r, 1.0);
    for (const auto& f : had)
      dropped = dropped || std::none_of(refs.begin(), refs.end(), [&](const opad::Ref& r) { return sameRef(f, r); });
    if (dropped) restore();
    return;
  }
  FanSpec* fan = m_fan >= 0 && m_fan < int(m_fanSpecs.size()) ? &m_fanSpecs[size_t(m_fan)] : nullptr;
  if (m_picking == Pick::Outlet || m_picking == Pick::Inlet) {
    // One opening: the region or the face clicked last (a click on another replaces it).
    if (!fan) return;
    json& field = m_picking == Pick::Outlet ? fan->outlet : fan->inlet;
    json next;
    for (const auto& c : candidates) {
      if (c.find("\"sketch\"") == std::string::npos) continue;
      json picked = json::parse(c, nullptr, false);
      if (picked.is_object() && picked.contains("region")) picked = picked["region"];  // a curve: the closed loop it bounds
      if (picked.is_object() && picked != field) next = picked;
    }
    for (const auto& r : refs)
      if (next.is_null() && r.kind == opad::Ref::Kind::Face && pickable(r.body) && r.to_json() != field) next = r.to_json();
    if (next.is_null() || next.is_discarded()) {
      if (!field.is_null()) restore();  // a click that took it out: shown again
      return;
    }
    setProfile(m_fan, m_picking == Pick::Inlet, next);
    if (refs.size() + candidates.size() > 1) restore();  // the one left selected
    return;
  }
  if (m_picking == Pick::Sink) {
    // Bodies or components, each one part: a component picked in the browser stands for all its bodies.
    if (!fan) return;
    std::vector<std::string>& field = fan->sink;
    auto listed = [&](const std::string& id) { return std::find(field.begin(), field.end(), id) != field.end(); };
    auto within = [&](const std::string& id) {  // under a component listed or picked with it: that component stands for it
      for (const opad::Node* n = d->scene.node(id); n && !n->parent.empty(); n = d->scene.node(n->parent))
        if (listed(n->parent) || std::find(ids.begin(), ids.end(), n->parent) != ids.end()) return true;
      return false;
    };
    bool changed = false;
    for (const auto& id : ids)
      if (d->scene.node(id) && std::find(m_picked.begin(), m_picked.end(), id) == m_picked.end() && !listed(id) && !within(id))
        field.push_back(id), changed = true;  // not the box's walls
    auto shown = [&](const std::string& id) {  // selected, or a component whose bodies the view reports
      if (std::find(ids.begin(), ids.end(), id) != ids.end()) return true;
      const auto under = d->scene.bodies_under(id);
      return std::any_of(under.begin(), under.end(), [&](const std::string& b) { return b != id && std::find(ids.begin(), ids.end(), b) != ids.end(); });
    };
    if (!std::all_of(field.begin(), field.end(), shown)) restore();
    if (changed) showFans();
    return;
  }
  // Bodies (a component stands for its bodies), shown ones only.
  std::vector<std::string> now;
  auto take = [&](const std::string& id) {
    if (pickable(id) && std::find(now.begin(), now.end(), id) == now.end()) now.push_back(id);
  };
  for (const auto& id : ids)
    if (const opad::Node* n = d->scene.node(id)) {
      if (n->kind == opad::Node::Kind::Body) take(id);
      else
        for (const auto& b : d->scene.bodies_under(id)) take(b);
    }
  const std::vector<std::string> had = m_picking == Pick::Box ? m_picked : heatBodies();
  bool dropped = false;
  for (const auto& id : had) dropped = dropped || std::find(now.begin(), now.end(), id) == now.end();
  for (const auto& id : now) {
    if (std::find(had.begin(), had.end(), id) != had.end()) continue;
    if (m_picking == Pick::Box) m_picked.push_back(id);
    else if (std::find(m_picked.begin(), m_picked.end(), id) == m_picked.end()) addHeatBody(id, 1.0, false, 4);  // not a wall of the box
  }
  if (m_picking == Pick::Box) showPicked();
  if (dropped) restore();
}

void CoolingAssistant::showPicked() {
  const AppDocument* d = m_hooks.document();
  const QSignalBlocker quiet(m_walls);
  m_walls->clear();
  for (const auto& id : m_picked) {
    auto* item = new QListWidgetItem(d ? nameOf(d->scene, id) : QString::fromStdString(id), m_walls);
    item->setData(Qt::UserRole, QString::fromStdString(id));
    item->setToolTip(item->text());
  }
  m_boxRemove->setEnabled(false);
  scheduleSync();
}

std::vector<std::string> CoolingAssistant::heatBodies() const {
  std::vector<std::string> out;
  for (int r = 0; r < m_heat->rowCount(); ++r)
    if (const QTableWidgetItem* part = m_heat->item(r, 0); part && part->data(kFaceRole).toString().isEmpty())
      out.push_back(part->data(Qt::UserRole).toString().toStdString());
  return out;
}

std::vector<opad::Ref> CoolingAssistant::heatFaces() const {
  std::vector<opad::Ref> out;
  for (int r = 0; r < m_heat->rowCount(); ++r)
    if (const QTableWidgetItem* part = m_heat->item(r, 0); part && !part->data(kFaceRole).toString().isEmpty())
      out.push_back(opad::Ref::parse(part->data(kFaceRole).toString().toStdString()));
  return out;
}

// Plain cells (an editor only while one is typed in): a widget per cell took seconds for a table of a thousand parts.
void CoolingAssistant::addHeatBody(const std::string& id, double watts, bool board, int layers) {
  const AppDocument* d = m_hooks.document();
  const int r = m_heat->rowCount();
  m_heat->insertRow(r);
  auto* part = new QTableWidgetItem(d ? nameOf(d->scene, id) : QString::fromStdString(id));
  part->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
  part->setData(Qt::UserRole, QString::fromStdString(id));
  m_heat->setItem(r, 0, part);
  auto* w = new QTableWidgetItem();
  w->setData(Qt::EditRole, watts);
  m_heat->setItem(r, 1, w);
  auto* b = new QTableWidgetItem();
  b->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
  b->setCheckState(board ? Qt::Checked : Qt::Unchecked);
  m_heat->setItem(r, 2, b);
  auto* l = new QTableWidgetItem();
  l->setData(Qt::EditRole, layers);
  m_heat->setItem(r, 3, l);
}

void CoolingAssistant::addHeatFace(const opad::Ref& f, double watts) {
  const AppDocument* d = m_hooks.document();
  const int r = m_heat->rowCount();
  m_heat->insertRow(r);
  auto* part = new QTableWidgetItem(tr("%1, face %2").arg(d ? nameOf(d->scene, f.body) : QString::fromStdString(f.body)).arg(f.index + 1));
  part->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
  part->setData(kFaceRole, QString::fromStdString(f.str()));
  part->setToolTip(tr("A face that makes heat: a chip joined into its board"));
  m_heat->setItem(r, 0, part);
  auto* w = new QTableWidgetItem();
  w->setData(Qt::EditRole, watts);
  m_heat->setItem(r, 1, w);
  for (int c : {2, 3}) {  // a face is no board
    m_heat->setItem(r, c, new QTableWidgetItem());
    m_heat->item(r, c)->setFlags(Qt::NoItemFlags);
  }
}

// ---------------------------------------------------------------- the fans
opad::Vec3 CoolingAssistant::FanSpec::vector() const {
  opad::Vec3 v = written;
  if (std::hypot(normal[0], normal[1], normal[2]) > 0.5) {
    v = normal;
    if (type == "blower") {  // out of its side: away from its inlet (or turned by Flip)
      if (v[0] * (centre[0] - inletCentre[0]) + v[1] * (centre[1] - inletCentre[1]) + v[2] * (centre[2] - inletCentre[2]) < 0) v = {-v[0], -v[1], -v[2]};
      return flipOut ? opad::Vec3{-v[0], -v[1], -v[2]} : v;
    }
  }
  return flip ? opad::Vec3{-v[0], -v[1], -v[2]} : v;
}

// As fan_from takes it (sim/airflow.hpp): a library fan by its id, or the datasheet's numbers under their units' keys, the
// chart's points between its two ends; and the speed.
opad::json CoolingAssistant::FanSpec::spec() const {
  json j;
  if (!model.isEmpty()) {
    j = {{"model", model.toStdString()}};
  } else {
    const std::string fu = flowUnit.toStdString(), pu = pressureUnit.toStdString();
    j = {{"kind", type.toStdString()}, {fu == "m3h" ? "flow" : fu, flow}, {pu == "Pa" ? "pressure" : pu, pressure}};
    if (!curve.empty()) {
      json points = json::array({{0.0, pressure}});
      for (const auto& p : curve)
        if (p[0] > 0 && p[1] > 0 && p[0] < flow && p[1] < pressure) points.push_back({p[0], p[1]});
      points.push_back({flow, 0.0});
      j["curve"] = points;
      j["curve_units"] = {{"flow", fu}, {"pressure", pu}};
    }
  }
  if (speed != 100) j["speed"] = speed;
  return j;
}

void CoolingAssistant::loadFans(const opad::Scene& s) {
  // A fan not written yet (its openings still being drawn: Draw… makes a sketch, which reloads the pages) is kept.
  std::vector<FanSpec> pending;
  bool current = false;
  for (size_t i = 0; i < m_fanSpecs.size(); ++i)
    if (!m_fanSpecs[i].held()) {
      current = current || int(i) == m_fan;
      pending.push_back(m_fanSpecs[i]);
    }
  m_fanSpecs.clear();
  const opad::Study* st = s.study(studyId());
  const json settings = st ? st->def.value("settings", json::object()) : json::object();
  const json mats = settings.value("materials", json::object());
  for (const auto& l : s.loads) {
    if (l.load_case != kCase || l.kind != "fan") continue;
    FanSpec f;
    for (const auto& r : l.refs) f.on.push_back(r.body);
    f.type = QString::fromStdString(l.def.value("type", std::string("axial")));
    const json fan = l.def.value("fan", json("80x25"));
    const json model = fan.is_string() ? fan : fan.is_object() ? fan.value("model", json()) : json();
    if (model.is_string()) f.model = QString::fromStdString(model.get<std::string>());
    if (fan.is_object()) f.speed = fan.value("speed", 100.0);
    if (fan.is_object() && !model.is_string()) {  // its datasheet's numbers, in the units it was given in
      for (const char* k : {"flow", "cfm", "m3min", "ls"})
        if (fan.contains(k)) f.flow = fan[k].get<double>(), f.flowUnit = QString(k) == "flow" ? "m3h" : k;
      for (const char* k : {"pressure", "inH2O", "mmH2O"})
        if (fan.contains(k)) f.pressure = fan[k].get<double>(), f.pressureUnit = QString(k) == "pressure" ? "Pa" : k;
      const json units = fan.value("curve_units", json::object());
      if (fan.contains("curve")) f.flowUnit = QString::fromStdString(units.value("flow", std::string("m3h"))), f.pressureUnit = QString::fromStdString(units.value("pressure", std::string("Pa")));
      for (const auto& p : fan.value("curve", json::array()))
        if (p.is_array() && p.size() == 2 && p[0].get<double>() > 0 && p[1].get<double>() > 0) f.curve.push_back({p[0].get<double>(), p[1].get<double>()});
    } else if (!f.model.isEmpty()) {
      try {
        const air::Fan spec = air::fan_from(json(f.model.toStdString()));
        f.flow = spec.Qmax / air::flow_unit("cfm"), f.pressure = spec.Pmax / air::pressure_unit("inH2O");
      } catch (const std::exception&) {
      }
    }
    f.written = l.def.value("vector", opad::Vec3{1, 0, 0});
    if (const json o = l.def.value("outlet", json()); o.is_string() || (o.is_object() && !o.contains("centre"))) f.outlet = o;
    if (const json o = l.def.value("inlet", json()); o.is_string() || o.is_object()) f.inlet = o;
    f.flipOut = l.def.value("flip_outlet", false), f.flipIn = l.def.value("flip_inlet", false);
    f.loaded = true;
    for (const auto& h : l.def.value("heatsink", json::array()))
      if (h.is_string()) f.sink.push_back(h.get<std::string>());
    // The heatsink's material, when the study set one for it (each of its bodies has it).
    for (const auto& id : f.sink)
      for (const auto& b : s.node(id) && s.node(id)->kind != opad::Node::Kind::Body ? s.bodies_under(id) : std::vector<std::string>{id})
        if (mats.contains(b) && mats[b].is_object() && mats[b].contains("material")) f.material = QString::fromStdString(mats[b].value("material", ""));
    if (f.outlet.is_null() && f.on.empty()) continue;
    m_fanSpecs.push_back(f);
  }
  // How the air moves: the study keeps whether the air rises (cfd.buoyancy), the case its fans. The choice made here stays as
  // long as the study agrees with it (Fans and warm air rising with no fan written yet is the air rising and no fan, as
  // natural convection is): opening the panel again never changes it. Otherwise it is read from the study (the air rising
  // and fans: both; the air rising alone: natural convection; else fans), and a choice that had to change says why.
  const bool rising = st && settings.value("cfd", json::object()).value("buoyancy", false);
  const int written = int(m_fanSpecs.size());  // the case's fan loads
  QRadioButton* was = m_fansOn->isChecked() ? m_fansOn : m_mixed->isChecked() ? m_mixed : m_natural;
  const bool agrees = m_sameDocument && (was == m_fansOn ? !rising : rising) && !(was == m_natural && written > 0);
  QRadioButton* now = agrees ? was : rising ? (written == 0 && pending.empty() ? m_natural : m_mixed) : m_fansOn;
  m_airChanged = m_sameDocument && now != was
                     ? tr("Shown as “%1”: the study was changed outside this panel (%2).")
                           .arg(now->text(), rising ? tr("the air rising, %n fan load(s)", "", written) : tr("the air not rising, %n fan load(s)", "", written))
                     : QString();
  now->setChecked(true);
  if (m_showAirNote) m_showAirNote();
  m_fan = m_fanSpecs.empty() ? -1 : 0;
  for (size_t i = 0; i < pending.size(); ++i) {
    if (current && (i == 0 || pending[i].drawing)) m_fan = int(m_fanSpecs.size());
    m_fanSpecs.push_back(pending[i]);
  }
  if (m_fan < 0 && !m_fanSpecs.empty()) m_fan = 0;
  for (int i = 0; i < int(m_fanSpecs.size()); ++i) measureOutlet(i);  // their outlines and ways, read on a worker
  showFans();
}

QString CoolingAssistant::profileText(const opad::json& p) const {
  const AppDocument* d = m_hooks.document();
  if (p.is_null() || !d) return QString();
  if (p.is_object() && p.contains("sketch")) {
    const std::string id = p.value("sketch", std::string());
    for (const auto& k : d->scene.sketches)
      if (k.id == id) return tr("Profile of %1").arg(QString::fromStdString(k.name));
    return tr("A sketch's profile");
  }
  try {
    const opad::Ref r = opad::Ref::from_json(p);
    return tr("%1, face %2").arg(nameOf(d->scene, r.body)).arg(r.index + 1);
  } catch (const std::exception&) {
    return QString();
  }
}

void CoolingAssistant::setProfile(int fan, bool inlet, const opad::json& profile) {
  for (size_t i = 0; i < m_fanSpecs.size(); ++i)  // the fan Draw… was pressed for (the pages may have been reloaded since)
    if (m_fanSpecs[i].drawing) fan = int(i), m_fanSpecs[i].drawing = false;
  if (fan < 0 || fan >= int(m_fanSpecs.size())) return;
  m_fan = fan;
  FanSpec& f = m_fanSpecs[size_t(fan)];
  (inlet ? f.inlet : f.outlet) = profile;
  (inlet ? f.inletLoops : f.outletLoops).clear();
  if (!inlet) f.normal = {0, 0, 0}, f.flip = false;
  if (!f.outlet.is_null()) f.on.clear();  // drawn: no body stands for it any more
  showFans();
  measureOutlet(fan);
}

void CoolingAssistant::showFans() {
  m_fanLoading = true;
  m_fanList->clear();
  for (size_t i = 0; i < m_fanSpecs.size(); ++i) {
    const FanSpec& f = m_fanSpecs[i];
    const QString what = f.type == "blower" ? tr("blower") : tr("axial fan");
    QString model = f.model;
    for (const auto& lib : air::fans())
      if (QString::fromStdString(lib.id) == f.model) model = QString::fromStdString(lib.name);
    if (model.isEmpty())
      model = QString("%1 %2, %3 %4").arg(f.flow, 0, 'g', 3).arg(m_flowUnit->itemText(std::max(0, m_flowUnit->findData(f.flowUnit))))
                  .arg(f.pressure, 0, 'g', 3).arg(m_pressureUnit->itemText(std::max(0, m_pressureUnit->findData(f.pressureUnit))));
    m_fanList->addItem(tr("Fan %1 (%2): %3").arg(i + 1).arg(what, f.drawn() || !f.on.empty() ? model : tr("draw or pick its openings")));
  }
  m_fanList->setCurrentRow(m_fan);
  m_fanLoading = false;
  showFan();
  scheduleSync();
  if (step() == 2) lightChosen();  // the current fan and its heatsink as picked now
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
  const bool blower = f.type == "blower";
  m_fanType->setCurrentIndex(std::max(0, m_fanType->findData(f.type)));
  m_outletPick->set(f.outlet.is_null() ? 0 : 1, f.outlet.is_null() && !f.on.empty() ? tr("%1 (an older setup: draw its opening)").arg(names(f.on).join(", ")) : profileText(f.outlet),
                    m_picking == Pick::Outlet, !f.outlet.is_null());
  m_inletPick->set(f.inlet.is_null() ? 0 : 1, profileText(f.inlet), m_picking == Pick::Inlet, !f.inlet.is_null());
  if (auto* label = qobject_cast<QLabel*>(m_fanRows->itemAt(1, QFormLayout::LabelRole) ? m_fanRows->itemAt(1, QFormLayout::LabelRole)->widget() : nullptr))
    label->setText(blower ? tr("Outlet:") : tr("Opening:"));
  m_fanRows->setRowVisible(2, blower);  // a blower's inlet
  m_flip->setVisible(!blower);  // a blower's way is out of its outlet, away from its inlet: each opening's own Flip
  m_outletFlip->setVisible(blower && !f.outlet.is_null());
  m_inletFlip->setVisible(blower && !f.inlet.is_null());
  const bool measured = std::hypot(f.normal[0], f.normal[1], f.normal[2]) > 0.5;
  m_wayText->setText(!f.outlet.is_null() && !measured ? tr("Reading its opening…")
                     : f.outlet.is_null() && f.on.empty() ? tr("Draw or pick its opening first.")
                                                          : tr("Out along %1.").arg(QString(QChar(0x202A)) + wayText(f.vector()) + QChar(0x202C)));
  m_fanClip->setClip(blower ? "simulate.fanBlower" : "simulate.fanAxial");
  m_fanClip->setVisible(!f.drawn());  // how its openings are drawn, until they are
  // The library's fans of its type, then its datasheet.
  m_model->clear();
  for (const auto& lib : air::fans())
    if (QString::fromStdString(lib.kind) == f.type) m_model->addItem(QString::fromStdString(lib.name), QString::fromStdString(lib.id));
  m_model->addItem(tr("From its datasheet"), QString());
  m_model->setCurrentIndex(std::max(0, m_model->findData(f.model)));
  if (m_model->currentData().toString() != f.model) m_model->setCurrentIndex(m_model->count() - 1);
  const bool custom = f.model.isEmpty();
  m_flowUnit->setCurrentIndex(std::max(0, m_flowUnit->findData(f.flowUnit)));
  m_pressureUnit->setCurrentIndex(std::max(0, m_pressureUnit->findData(f.pressureUnit)));
  m_flow->setValue(f.flow);
  m_pressure->setValue(f.pressure);
  m_flow->setEnabled(custom);  // a library fan's numbers shown, as its datasheet has them
  m_pressure->setEnabled(custom);
  QStringList points;
  for (const auto& p : f.curve) points << QString("%1 %2").arg(p[0], 0, 'g', 4).arg(p[1], 0, 'g', 4);
  m_curve->setText(points.join("; "));
  m_fanRows->setRowVisible(m_curve, custom);
  m_speed->setValue(f.speed);
  m_sinkPick->set(int(f.sink.size()), names(f.sink).join(", "), m_picking == Pick::Sink, true);
  m_sinkMaterial->setCurrentIndex(std::max(0, m_sinkMaterial->findData(f.material)));
  m_sinkMaterial->setEnabled(!f.sink.empty());
  m_fanLoading = false;
}

// The openings' outlines and normals (sim::fan_profile: a sketch's regions are kernel work), on a worker.
void CoolingAssistant::measureOutlet(int fan) {
  AppDocument* d = m_hooks.document();
  JobRunner* jobs = m_hooks.jobs ? m_hooks.jobs() : nullptr;
  if (!d || !jobs || fan < 0 || fan >= int(m_fanSpecs.size())) return;
  const json outlet = m_fanSpecs[size_t(fan)].outlet, inlet = m_fanSpecs[size_t(fan)].type == "blower" ? m_fanSpecs[size_t(fan)].inlet : json();
  if (outlet.is_null() && inlet.is_null()) return;
  auto doc = std::make_shared<const opad::Document>(d->doc);
  auto scene = std::make_shared<const opad::Scene>(d->scene);
  struct Read {
    opad::sim::FanProfile out, in;
    bool hasOut = false, hasIn = false;
    std::string error;
  };
  auto m = std::make_shared<Read>();
  jobs->async(tr("Reading the fan's openings"), [doc, scene, outlet, inlet, m](Progress) {
    try {
      if (!outlet.is_null()) m->out = opad::sim::fan_profile(*doc, *scene, outlet), m->hasOut = true;
      if (!inlet.is_null()) m->in = opad::sim::fan_profile(*doc, *scene, inlet), m->hasIn = true;
    } catch (const std::exception& e) {
      m->error = e.what();
    }
  }, [this, self = QPointer<CoolingAssistant>(this), fan, outlet, inlet, m](bool ok, const QString&) {
    if (!self || !ok || fan >= int(m_fanSpecs.size())) return;
    FanSpec& f = m_fanSpecs[size_t(fan)];
    if (f.outlet != outlet) return;  // picked again meanwhile
    auto loops = [](const opad::sim::FanProfile& p) {
      std::vector<std::vector<opad::Vec3>> out;
      for (const auto& l : p.loops) out.push_back(std::vector<opad::Vec3>(l.begin(), l.end()));
      return out;
    };
    if (m->hasOut) {
      f.normal = m->out.normal, f.centre = m->out.centre, f.outletLoops = loops(m->out);
      if (f.loaded && f.type != "blower")  // an axial fan's Flip is its written way against the opening's normal
        f.flip = f.written[0] * f.normal[0] + f.written[1] * f.normal[1] + f.written[2] * f.normal[2] < 0;
      f.loaded = false;
    }
    if (m->hasIn && f.inlet == inlet) f.inletNormal = m->in.normal, f.inletCentre = m->in.centre, f.inletLoops = loops(m->in);
    if (!m->error.empty()) m_status->setText(i18n::t(QString::fromStdString(m->error)));
    showFans();  // its way known: the fan load written, its openings drawn
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
  if (walls().empty()) {
    m_status->setText(tr("Tick the box's bodies on the first page."));
    m_steps->setCurrentIndex(0);
    return false;
  }
  bool heat = false;
  for (int r = 0; r < m_heat->rowCount(); ++r) heat = heat || m_heat->item(r, 1)->data(Qt::EditRole).toDouble() > 0;
  if (!heat) {
    m_status->setText(tr("Nothing makes heat: add a part (or a face) with its power on the Heat page."));
    m_steps->setCurrentIndex(1);
    return false;
  }
  if (!naturalConvection()) {
    if (m_fanSpecs.empty()) {
      m_status->setText(tr("No fan: add one on the Air page (Add a fan, then draw or pick its opening), or choose natural convection."));
      m_steps->setCurrentIndex(2);
      return false;
    }
    for (size_t i = 0; i < m_fanSpecs.size(); ++i) {
      const FanSpec& f = m_fanSpecs[i];
      if (f.drawn() || (f.outlet.is_null() && !f.on.empty() && f.type != "blower")) continue;
      m_status->setText(f.outlet.is_null() ? tr("Fan %1 has no opening: draw it on the fan's face (Draw…) or pick a sketch's profile.").arg(i + 1)
                                           : tr("Blower %1 has no inlet: draw where its air comes in (Draw…) or pick a sketch's profile.").arg(i + 1));
      m_fan = int(i);
      showFans();
      m_steps->setCurrentIndex(2);
      return false;
    }
  }
  sync();
  return true;
}

void CoolingAssistant::scheduleSync() {
  if (!m_reloading && isVisible()) m_syncTimer->start();
}

// What the pages hold, written as far as it goes: a heat source per row with a power, a fan load per fan picked, the study with
// the box, the boards and the heatsinks' materials. Diffed against the case's heat and fan loads: one that the pages still
// hold is changed in place (its name and id kept), one they no longer hold is deleted; the case's other loads (a convection
// added by hand) are left alone. All of it one step to undo.
void CoolingAssistant::sync() {
  m_syncTimer->stop();
  AppDocument* d = m_hooks.document();
  if (!d || !d->hasDocument || m_reloading || d->browse) return;
  const opad::Scene& s = d->scene;
  struct Want {
    std::string kind;
    json refs, args;
    QString name;
    bool keep = false;  // its fan's way being measured: the load stays as it is meanwhile
    bool used = false;
  };
  std::vector<Want> want;
  json materials = json::object();
  for (int r = 0; r < m_heat->rowCount(); ++r) {
    const double watts = std::clamp(m_heat->item(r, 1)->data(Qt::EditRole).toDouble(), 0.0, 1000.0);
    const QString name = tr("Heat") + " · " + m_heat->item(r, 0)->text();
    if (const QString face = m_heat->item(r, 0)->data(kFaceRole).toString(); !face.isEmpty()) {
      if (watts > 0) want.push_back({"heat", json::array({opad::Ref::parse(face.toStdString()).to_json()}), {{"value", watts}}, name});
      continue;
    }
    const std::string id = m_heat->item(r, 0)->data(Qt::UserRole).toString().toStdString();
    if (watts > 0) {  // a part with no power is a board (or kept for later)
      want.push_back({"heat", json::array({id}), {{"value", watts}}, name});
      if (opad::material_of(d->doc, s, id).id.empty()) materials[id] = {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon (assumed)"}};
    }
    if (m_heat->item(r, 2)->checkState() == Qt::Checked)
      materials[id] = {{"pcb", {{"layers", std::clamp(m_heat->item(r, 3)->data(Qt::EditRole).toInt(), 1, 32)}}}};
  }
  const bool fans = !naturalConvection();
  for (size_t i = 0; i < m_fanSpecs.size(); ++i) {
    const FanSpec& f = m_fanSpecs[i];
    if (f.outlet.is_null() && f.on.empty()) continue;  // nothing drawn yet: no load
    if (fans) {
      json args = {{"fan", f.spec()}, {"heatsink", f.sink}, {"type", f.type.toStdString()}};
      Want w{"fan", json::array(), args, (f.type == "blower" ? tr("Blower") : tr("Fan")) + QString(" %1").arg(i + 1)};
      if (!f.outlet.is_null()) {
        args["outlet"] = f.outlet;
        if (f.type == "blower" && !f.inlet.is_null()) args["inlet"] = f.inlet;
        if (f.type == "blower" && f.flipOut) args["flip_outlet"] = true;
        if (f.type == "blower" && f.flipIn) args["flip_inlet"] = true;
        if (f.type != "blower" && std::hypot(f.normal[0], f.normal[1], f.normal[2]) > 0.5) args["vector"] = f.vector();
        w.args = args;
        w.keep = f.type == "blower" ? f.inlet.is_null() : std::hypot(f.normal[0], f.normal[1], f.normal[2]) < 0.5;  // not all there yet
      } else {  // an older setup's: on its body, along its vector
        args["vector"] = f.written;
        w.args = args, w.refs = json(f.on), w.name = tr("Fan") + " · " + nameOf(s, f.on.front());
      }
      want.push_back(w);
    }
    if (!f.material.isEmpty())  // the heatsink one part of one material: each of its bodies
      for (const auto& id : f.sink)
        for (const auto& b : s.node(id) && s.node(id)->kind != opad::Node::Kind::Body ? s.bodies_under(id) : std::vector<std::string>{id})
          materials[b] = {{"material", f.material.toStdString()}};
  }
  // The writes, decided before any is made (without a batch each write resolves the scene again).
  std::vector<std::pair<std::string, json>> writes;
  for (const auto& l : s.loads) {
    if (l.load_case != kCase || (l.kind != "heat" && l.kind != "fan")) continue;
    const json refs = l.def.value("refs", json::array());
    // A fan is the same load while its outlet is (or, an older one, its body); the rest is changed in place.
    auto same = [&](const Want& x) {
      if (x.kind != "fan") return x.refs == refs;
      return x.args.contains("outlet") ? l.def.value("outlet", json()) == x.args["outlet"] : !l.def.contains("outlet") && x.refs == refs;
    };
    auto w = std::find_if(want.begin(), want.end(), [&](const Want& x) { return !x.used && x.kind == l.kind && same(x); });
    if (w != want.end() && !w->keep) {
      bool unset = false;  // a key that cannot be unset in place (a blower's inlet as an axial fan): made again
      for (const char* k : {"across", "inlet", "vector", "heatsink", "flip_outlet", "flip_inlet"}) unset = unset || (l.def.contains(k) && !w->args.contains(k) && !(std::string(k) == "heatsink" && l.def[k].empty()));
      if (unset) w = want.end();
    }
    if (w == want.end()) {
      writes.push_back({"delete", {{"target", l.id}}});
      continue;
    }
    w->used = true;
    if (w->keep) continue;
    json set = json::object();
    for (const auto& [k, v] : w->args.items())
      if (l.def.value(k, json()) != v && !(k == "heatsink" && v.empty() && !l.def.contains(k))) set[k] = v;
    if (!set.empty()) {
      set["id"] = l.id;
      writes.push_back({"load", set});
    }
  }
  for (const Want& w : want)
    if (!w.used && !w.keep) {
      json args = w.args;
      args["kind"] = w.kind, args["case"] = kCase, args["on"] = w.refs, args["name"] = w.name.toStdString();
      if (w.kind == "fan" && args["heatsink"].empty()) args.erase("heatsink");
      writes.push_back({"load", args});
    }
  const std::vector<std::string> box = walls();
  json cfd = {{"quality", m_quality->currentData().toString().toStdString()}, {"radiation", m_radiation->isChecked()}, {"buoyancy", warmAirRises()}};
  if (!box.empty()) cfd["enclosure"] = box.size() == 1 ? json(box.front()) : json(box);
  json settings = {{"case", kCase}, {"air", "cfd"}, {"ambient", m_ambient->value()}, {"cfd", cfd}};
  if (!materials.empty()) settings["materials"] = materials;
  const std::string id = studyId();
  const opad::Study* st = s.study(id);
  if (st && st->def.value("settings", json()) != settings) writes.push_back({"study", {{"id", id}, {"settings", settings}, {"run", false}}});
  else if (!st && (!box.empty() || !want.empty() || !materials.empty()))
    writes.push_back({"study", {{"kind", "thermal"}, {"name", studyName().toStdString()}, {"settings", settings}, {"run", false}}});
  if (writes.empty()) return estimateVents();  // asked of the document as written (its revision is part of the question)
  m_writing = true;
  auto all = [&] {
    for (const auto& [command, args] : writes) m_hooks.write(command, args, tr("Thermal setup"));
  };
  if (m_hooks.batch) m_hooks.batch(tr("Thermal setup"), all);
  else all();
  m_writing = false;
  m_loaded = {d->generation, d->revision};
  estimateVents();
}

// Whether the box's vents let the fans blow what they can (sim::vent_estimate, on a worker): said under the fans, in the
// current fan's units, against the flow the heat needs.
void CoolingAssistant::estimateVents() {
  AppDocument* d = m_hooks.document();
  JobRunner* jobs = m_hooks.jobs ? m_hooks.jobs() : nullptr;
  const int run = ++m_ventRun;
  std::vector<json> defs;
  for (size_t i = 0; i < m_fanSpecs.size(); ++i) {
    const FanSpec& f = m_fanSpecs[i];
    if (!f.drawn() && (f.on.empty() || !f.outlet.is_null())) continue;
    json def = {{"fan", f.spec()}, {"type", f.type.toStdString()}, {"vector", f.vector()},
                {"name", ((f.type == "blower" ? tr("Blower") : tr("Fan")) + QString(" %1").arg(i + 1)).toStdString()}};  // as its load is named
    if (!f.outlet.is_null()) def["outlet"] = f.outlet;
    if (!f.inlet.is_null()) def["inlet"] = f.inlet;
    if (f.flipOut) def["flip_outlet"] = true;
    if (f.flipIn) def["flip_inlet"] = true;
    if (f.outlet.is_null()) def["refs"] = f.on;
    defs.push_back(def);
  }
  const std::vector<std::string> box = walls();
  if (!d || !jobs || naturalConvection() || box.empty() || defs.empty()) {
    m_ventKey.clear();
    if (m_ventJob) m_ventJob->cancel();
    m_ventText->setText(box.empty() || naturalConvection() ? QString() : tr("Once a fan's openings are set, this says whether the box's vents let it blow what it can."));
    return;
  }
  double watts = 0;
  for (int r = 0; r < m_heat->rowCount(); ++r) watts += std::max(0.0, m_heat->item(r, 1)->data(Qt::EditRole).toDouble());
  const QString unit = m_fan >= 0 && m_fan < int(m_fanSpecs.size()) ? m_fanSpecs[size_t(m_fan)].flowUnit : QString("cfm");
  // The same question again (the Air tab shown again, a change that touches none of it) asks nothing: one estimate at a time.
  // What it depends on: the bodies (their shapes' keys, where they are, shown or not) and the sketches, not the document's
  // revision (any write, a rename, moves that).
  json shapes = json::array();
  for (const auto& id : d->scene.all_bodies())
    if (const opad::Node* n = d->scene.node(id)) shapes.push_back({id, n->body_key, d->scene.world(id).m, d->scene.effectively_visible(id)});
  json sketches = json::array();
  for (const auto& k : d->scene.sketches) sketches.push_back({k.id, std::hash<std::string>{}(k.geometry.dump()), k.frame.to_json()});
  const std::string key = json{{"walls", box}, {"fans", defs}, {"watts", watts}, {"ambient", m_ambient->value()}, {"unit", unit.toStdString()},
                               {"shapes", shapes}, {"sketches", sketches}, {"generation", d->generation}}.dump();
  if (key == m_ventKey) {
    --m_ventRun;  // the answer coming (or come) is still this one's
    return;
  }
  m_ventKey = key;
  if (m_ventJob) m_ventJob->cancel();  // its worker stops at its next hole
  auto doc = std::make_shared<const opad::Document>(d->doc);
  auto scene = std::make_shared<const opad::Scene>(d->scene);
  auto out = std::make_shared<json>();
  const double ambient = m_ambient->value();
  const QString phase = tr("Estimating the vents: hole %1 of %2");
  m_ventText->setText(tr("Estimating the vents…"));
  m_ventJob = jobs->async(tr("Estimating the vents"), [doc, scene, box, defs, watts, ambient, out, phase](Progress p) {
    try {
      *out = opad::sim::vent_estimate(*doc, *scene, box, defs, watts, ambient, [&p, &phase](int done, int of) {
        if (done % 8 == 0 || done == of) p.setPhase(phase.arg(done).arg(of), of > 0 ? 100 * done / of : -1);
        return !p.cancelled();
      });
    } catch (const std::exception& e) {
      *out = {{"error", e.what()}};
    }
  }, [this, self = QPointer<CoolingAssistant>(this), run, out, unit, watts](bool ok, const QString&) {
    if (!self || run != m_ventRun) return;  // a later one asked meanwhile: its answer is the one shown
    m_ventJob = nullptr;
    if (!ok) {  // cancelled from the status bar: asked again next time
      m_ventKey.clear();
      m_ventText->setText(tr("The vents' estimate was stopped."));
      return;
    }
    const json& e = *out;
    if (e.contains("error")) return m_ventText->setText(i18n::t(QString::fromStdString(e["error"].get<std::string>())));
    const double k = 1 / (3600 * air::flow_unit(unit.toStdString()));  // m3/h -> the unit
    const QString u = m_flowUnit->itemText(std::max(0, m_flowUnit->findData(unit)));
    auto flow = [&](const char* key) { return QString::number(e.value(key, 0.0) * k, 'g', 3) + " " + u; };
    auto side = [&](const char* key) {
      const json& v = e[key];
      if (v.value("openings", 0) == 0) return v.value("fan", false) ? tr("the fans' own cut-outs") : tr("none (the fans' own holes, below)");
      return tr("%1 mm² in %n opening(s)", "", v.value("openings", 0)).arg(std::lround(v.value("area_mm2", 0.0)));
    };
    const std::string by = e.value("limited_by", std::string());
    const json limiting = e.value("limiting", json::object());
    const QString limitingFan = QString::fromStdString(limiting.value("fan", std::string()));
    // Each fan whose intake or outlet goes through holes of its own (a perforated panel or a grille right in front of it).
    QStringList own;
    for (const auto& f : e.value("fans", json::array()))
      for (const char* which : {"intake", "outlet"}) {
        const json p = f.value(which, json::object());
        const QString fan = QString::fromStdString(f.value("name", std::string())), body = QString::fromStdString(p.value("body", std::string()));
        if (p.value("through", std::string()) == "mount")
          own << (std::string(which) == "intake" ? tr("%1 draws straight through its cut-out in %2.") : tr("%1 blows straight out through its cut-out in %2.")).arg(fan, body);
        else if (p.value("through", std::string()) == "holes")
          own << (std::string(which) == "intake" ? tr("%1 draws through %n hole(s) in %2 in front of its intake (%3 mm², %4 Pa across them).", "", p.value("openings", 0))
                                                 : tr("%1 blows through %n hole(s) in %2 in front of its outlet (%3 mm², %4 Pa across them).", "", p.value("openings", 0)))
                     .arg(fan, body).arg(std::lround(p.value("area_mm2", 0.0))).arg(p.value("pressure_Pa", 0.0), 0, 'f', 1);
        if (p.contains("round_m3h") || p.contains("round_mm2"))  // not sealed against them: the box's air comes round too
          own.back() += " " + tr("It is %1 mm off them: the box's air also comes round through that gap (%2 mm²)%3.")
                                  .arg(p.value("gap_mm", 0.0), 0, 'f', 1)
                                  .arg(std::lround(p.value("round_mm2", 0.0)))
                                  .arg(p.contains("round_m3h") ? tr(", %1 of it").arg(QString::number(p.value("round_m3h", 0.0) * k, 'g', 2) + " " + u) : QString());
      }
    QString text;
    if (by == "vents") {  // sealed: said first and plainly, not as a flow
      text = tr("No vents found: no air goes through the box, the fans only stir the air inside. Cut vents through its walls (an "
                "intake and an exhaust), or choose natural convection if it is sealed on purpose.");
    } else {
      text = tr("Vents (holes through the box's walls, listed in the tip): intake %1, exhaust %2.").arg(side("intake"), side("exhaust")) + " ";
      text += tr("Through them the fans give about %1 of their %2 free flow (%3%)").arg(flow("flow_m3h"), flow("free_flow_m3h")).arg(std::lround(100 * e.value("fraction", 0.0)));
      text += by == "intake"       ? tr(", held back most by the intakes.")
              : by == "exhaust"    ? tr(", held back most by the exhausts.")
              : by == "fan_intake" ? tr(", held back most by the holes in front of %1's intake.").arg(limitingFan)
              : by == "fan_outlet" ? tr(", held back most by the holes in front of %1's outlet.").arg(limitingFan)
                                   : tr(": the vents hardly hold them back.");
      if (!own.isEmpty()) text += " " + own.join(" ");
    }
    if (by != "vents" && watts > 0 && e.contains("rise_K")) {
      // What the heat does to that air (it leaves warmer by the heat over its mass flow times its specific heat), and the
      // flow that keeps the rise to 10 K, a usual limit for electronics.
      const double rise = e.value("rise_K", 0.0);
      text += " " + (rise > 100 ? tr("With %1 W the air barely moves through the box: it would warm by far more than 100 K.").arg(watts, 0, 'g', 3)
                                : tr("With %1 W the air leaves about %2 K warmer than the room.").arg(watts, 0, 'g', 3).arg(rise, 0, 'f', 1));
      if (rise > 10)
        text += " " + tr("Keeping that to 10 K needs about %1: %2").arg(flow("needed_m3h"))
                          .arg(by == "fans"         ? tr("a stronger fan, or another.")
                               : by == "fan_intake" ? tr("more or larger holes in front of %1's intake, or a stronger fan.").arg(limitingFan)
                               : by == "fan_outlet" ? tr("more or larger holes in front of %1's outlet, or a stronger fan.").arg(limitingFan)
                                                    : tr("more %1 area, or a stronger fan.").arg(by == "intake" ? tr("intake") : tr("exhaust")));
    }
    const json left = e.value("left_out", json::object());
    if (left.value("blind", 0) + left.value("filled", 0) > 0)
      text += " " + tr("Not counted: %1 outline(s) on the walls that are no hole (a boss, a foot, a pocket), %2 hole(s) a part fills (a connector, a screw).")
                        .arg(left.value("blind", 0)).arg(left.value("filled", 0));
    for (const auto& n : e.value("notes", json::array()))
      if (by != "vents") text += " (" + i18n::t(QString::fromStdString(n.get<std::string>())) + ")";
    m_ventText->setText(text);
    // Which they are: the list in its tip, and drawn in the view on request.
    m_ventOpenings = e.value("openings", json::array());
    QStringList rows;
    int shown = 0;
    for (const auto& o : m_ventOpenings) {
      if (++shown > 15) {
        rows << tr("… and %n more", "", int(m_ventOpenings.size()) - 15);
        break;
      }
      const opad::Vec3 c = o.value("centre", opad::Vec3{0, 0, 0});
      const std::string side = o.value("side", std::string());
      rows << tr("%1: %2 mm² at (%3, %4, %5) mm")
                  .arg(side == "intake"       ? tr("intake")
                       : side == "exhaust"    ? tr("exhaust")
                       : side == "fan_intake" ? tr("in front of %1's intake").arg(QString::fromStdString(o.value("fan", std::string())))
                       : side == "fan_outlet" ? tr("in front of %1's outlet").arg(QString::fromStdString(o.value("fan", std::string())))
                                              : tr("the fan's own opening"))
                  .arg(std::lround(o.value("area_mm2", 0.0)))
                  .arg(c[0], 0, 'f', 1).arg(c[1], 0, 'f', 1).arg(c[2], 0, 'f', 1);
    }
    m_ventText->setToolTip(tr("The vents are the holes through the box's flat walls (an outline that is no hole, or a hole a part fills, is not "
                              "counted); those ahead of the fans' air are exhausts, the others intakes. Largest first:") + "\n" + rows.join("\n"));
    m_ventsShown->setVisible(!m_ventOpenings.empty());
    if (m_ventsShown->isChecked()) lightChosen();
  });
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
  if (const double seconds = s.value("seconds", json::object()).value("total", 0.0); seconds > 0) {  // how long it took here, for the next estimate
    QSettings().setValue("cooling/lastRun/" + runKind(), seconds);
    showEstimate();
  }
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
  // The fan by its opening: a 28 mm circle on a sketch across the middle of its block (which is then air), blowing along +X;
  // its datasheet: 4.7 CFM free, 0.1 inH2O shut off.
  opad::design::Sketch opening;
  opening.add_circle(opening.add_point(0, 0), 14);
  const json sk = m_hooks.write("sketch", {{"plane", {{"origin", {7.5, 40, 20}}, {"normal", {1, 0, 0}}, {"x", {0, 1, 0}}}}, {"geometry", opening.to_json()}, {"name", "Fan opening"}}, label);
  m_hooks.write("load", {{"kind", "fan"}, {"case", kCase}, {"outlet", {{"sketch", sk.value("sketch_id", std::string())}}}, {"type", "axial"},
                         {"fan", {{"cfm", 4.7}, {"inH2O", 0.1}}}, {"vector", {1, 0, 0}}, {"heatsink", {hs}}, {"name", (tr("Fan") + " 1").toStdString()}},
                label);
  (void)fan;
  json settings = {{"case", kCase},
                   {"air", "cfd"},
                   {"ambient", 25},
                   {"cfd", {{"enclosure", encl}, {"quality", "quick"}, {"radiation", true}, {"buoyancy", false}}},
                   {"materials", {{board, {{"pcb", {{"layers", 4}}}}}, {soc, {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon"}}},
                                  {pmic, {{"k", 150}, {"cp", 700}, {"density", 2.33}, {"name", "Silicon"}}}}}};
  m_hooks.write("study", {{"kind", "thermal"}, {"name", studyName().toStdString()}, {"settings", settings}, {"run", false}}, label);
  reload();
  m_status->setText(tr("The example is ready: look through the pages, then Run."));
}
