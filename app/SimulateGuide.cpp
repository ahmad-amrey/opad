#include "SimulateGuide.hpp"

#include <QAction>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "CommandHelp.hpp"
#include "KeyText.hpp"
#include "Theme.hpp"

QList<SimulateGuide::UseCase> SimulateGuide::useCases() {
  auto x = [](const QString& s) { return help::expand(s); };
  QList<UseCase> list = {
      {tr("Make a hinged part move"),
       tr("Join a lid, a door or an arm to what holds it, and turn it with the slider."),
       {tr("Open the Simulate workspace ({key:workspace.simulate}) and its Simulation panel."),
        tr("Hold the frame still: pick it with the Bodies filter ({key:select.bodies}), choose Ground in the panel's New joint list and press Add."),
        tr("Pick the circular edge of the hinge's hole on the part that moves (the Edges filter, {key:select.edges}), then {fixed:ctrl}-click the frame "
           "it hangs on."),
        tr("Choose Revolute and press Add. The message says how many degrees of freedom the mechanism has now: 1 for a hinge."),
        tr("Drag the slider under MECHANISM: the part turns. Letting go writes the position into the document; Undo takes it back.")},
       {"workspace.simulate", "simulate.panel", "select.bodies", "select.edges", "simulate.joint"},
       tr("Slider, Cylindrical, Ball, Planar, Pin in slot and Screw joints are made the same way: first where the joint is (a circular edge or a "
          "round face gives the axis, a flat face its normal), then the part it is fixed to. With nothing picked second, the part is joined to "
          "the world.")},
      {tr("A mechanism's travel (motion study)"),
       tr("Turn a crank or push a slider through its range and see where every part goes, with its speed and acceleration, frame by frame."),
       {tr("Join the parts first (Make a hinged part move); the panel should show 1 degree of freedom."),
        tr("Choose the joint that drives it (the crank) in the panel's joint list."),
        tr("Under STUDY choose Motion and press New study: the joint turns a full turn (or slides 50 mm) in 2 seconds, and the study runs at once."),
        tr("Press Play ({fixed:space}) or drag the frame slider: the parts move in the view."),
        tr("Choose what the chart shows in the list above it: each joint's value, speed and acceleration.")},
       {"simulate.panel", "simulate.motion", "simulate.play", "simulate.run"},
       tr("A mechanism that cannot reach a position (a linkage at its dead point) stops there and says why. Other drives, longer runs and traced "
          "points are study settings an agent or opad-cli sets: see the Simulation guide in the documentation.")},
      {tr("Gears, racks and lead screws"),
       tr("Make two joints move together at a fixed ratio: a gear pair, a rack and pinion or a lead screw."),
       {tr("Make each gear a part of its own, turning on a Revolute joint to the frame (Make a hinged part move). Gear in Design makes involute "
           "gears that mesh."),
        tr("In the browser's Simulation folder click the first joint, then {fixed:ctrl}-click the second."),
        tr("In the panel choose Gear relation and type the ratio: how far the second turns per turn of the first. A 20-tooth gear driving a "
           "40-tooth one is -0.5 (meshing gears turn opposite ways). Press Add."),
        tr("Drag the slider on either joint: both turn.")},
       {"design.gear", "simulate.panel", "simulate.joint"},
       tr("Rack and pinion: pick the pinion's Revolute joint first, then the rack's Slider, and type the pinion's pitch radius in mm. Lead "
          "screw: the screw's joint first, then the nut's, and the lead in mm per turn.")},
      {tr("Let it swing or fall (dynamic study)"),
       tr("Gravity, masses and inertia: a pendulum swinging or a lid dropping shut, and the forces in its joints while it does."),
       {tr("Give the parts their materials (Material, {key:inspect.material}) so their masses are right; steel is assumed otherwise."),
        tr("Join them (Make a hinged part move). A part in no joint stays where it is."),
        tr("Under STUDY choose Dynamic and press New study: one second under gravity (down, along -Z), solved by Project Chrono."),
        tr("Play it. The chart lists each joint's value and speed, the forces and torques in the joints, and the kinetic, potential and total "
           "energy.")},
       {"inspect.material", "simulate.panel", "simulate.dynamic", "simulate.play"},
       tr("Motors, springs, friction, contacts between parts and longer runs are study settings an agent or opad-cli sets. A check worth making: "
          "with no motor and no friction, the total energy stays flat.")},
      {tr("Will it hold? (static stress)"),
       tr("Hold a part where it is fixed, load it, and see its stress, how far it bends and its safety factor."),
       {tr("Give the part its material (Material, {key:inspect.material}); steel is assumed otherwise."),
        tr("With the Faces filter ({key:select.faces}) click the faces that are held (bolted, clamped, glued) and press Fixed support."),
        tr("Click the faces the load acts on and press Force (type x, y, z in N: 0, 0, -500 pushes down with 500 N) or Pressure (in MPa)."),
        tr("Press Static stress study. The parts are meshed and solved with CalculiX; the panel gives the peak von Mises stress, the largest "
           "displacement and each body's safety factor against its yield strength."),
        tr("The Result map colours the parts; choose von Mises stress or Displacement in the list beside it. The bending is exaggerated so that "
           "it can be seen.")},
       {"inspect.material", "select.faces", "simulate.fixed", "simulate.force", "simulate.pressure", "simulate.static", "simulate.results"},
       tr("A sharp inside corner is a stress peak that grows as the mesh gets finer: read the stress a little away from it, or round the corner "
          "with a fillet. Gravity load adds the parts' own weight.")},
      {tr("A bolted joint"),
       tr("Tighten a bolt and see the parts it clamps and the bolt's own stress."),
       {tr("Model the bolt as one body (head, shank and nut) through the holes in the plates. Faces that touch are bonded."),
        tr("Fix the plate that is held (Faces filter, Fixed support)."),
        tr("Pick the bolt with the Bodies filter ({key:select.bodies}) and press Bolt preload; type the preload in N."),
        tr("Add the service load if there is one, then press Static stress study. The panel gives the bolt's shank stress (about the preload "
           "over its area) and every part's safety factor.")},
       {"select.faces", "simulate.fixed", "select.bodies", "simulate.boltPreload", "simulate.static"},
       tr("The shank is cut half way and pulled together by the preload, as a tightened bolt is.")},
      {tr("Natural frequencies (vibration modes)"),
       tr("Find the frequencies a part rings at, to keep them away from a motor's speed or another shaking."),
       {tr("Fix the faces that are held (Faces filter, Fixed support). Forces do not matter here."),
        tr("Press Vibration modes study: the panel lists the natural frequencies in Hz."),
        tr("Choose Mode 1, Mode 2... in the list beside Result map to see each shape on the parts.")},
       {"select.faces", "simulate.fixed", "simulate.modal", "simulate.results"},
       tr("A frequency near a running speed (rpm / 60) means resonance: stiffen the part, or change its mass, to move it.")},
      {tr("A 3D-printed part"),
       tr("Simulate a part as your printer makes it, from your slicer's settings: walls, skins and infill, weaker between layers."),
       {tr("Set it up as in Will it hold? and run the Static stress study once."),
        tr("Press Printed part and tick The bodies are 3D printed."),
        tr("Press Import slicer profile… and choose your PrusaSlicer, OrcaSlicer, Bambu Studio or Cura settings, a .3mf project or a G-code "
           "file; or set the material, layers, walls and infill yourself."),
        tr("Choose the Build direction: which way is up on the printer's bed."),
        tr("Press OK: the study runs again. The panel gives the plastic it takes and how it fails first (along the roads, across them, between "
           "layers, in compression) and on which layer. Choose Failure index in the result list to see where: 1 fails."),
        tr("Try another build direction or infill pattern and compare: a part standing up usually splits between its layers first.")},
       {"simulate.static", "simulate.print", "simulate.results"},
       tr("The filament values are typical printed-test-bar data; your printer's differ, most between layers. An agent can set the values you "
          "measured.")},
      {tr("Keep a part cool (thermal study)"),
       tr("How hot a chip, a motor or an LED gets on its heatsink: in still air, with a fan, or warming up over time."),
       {tr("Give each part its material (Material, {key:inspect.material}): an aluminium heatsink conducts 3 times better than steel."),
        tr("On the Thermal tab, pick the part that makes the heat (the Bodies filter, {key:select.bodies}) and press Heat source; type its power "
           "in W."),
        tr("Say how the heat leaves. With a fan: pick the heatsink, press Fan, choose the fan (or type its flow and pressure) and the way the "
           "air goes along the fins. In still air: pick the heatsink and press Convection, Natural. Radiation and Fixed temperature work the same "
           "way."),
        tr("Press Thermal study and choose Steady (where it settles) or Over time (how fast it warms up)."),
        tr("The panel gives the hottest temperature of each part, where the heat went and, for a fan, its air flow, the pressure it works "
           "against, how much the air warms and the heatsink's resistance in °C/W. The Result map shows the temperatures; over time, Play "
           "shows it warming.")},
       {"inspect.material", "select.bodies", "simulate.heat", "simulate.fan", "simulate.convection", "simulate.thermal", "simulate.results"},
       tr("The fins are found in the heatsink's shape: straight plate fins along the air. The fan's air is taken to go through the fins, as in a "
          "duct or with a shroud; air that goes round them cools less. With OpenFOAM installed, Steady, with the air solved works out where the "
          "air really goes and draws its streamlines (minutes rather than seconds).")},
  };
  for (UseCase& u : list) {
    for (QString& s : u.steps) s = x(s);
    u.tip = x(u.tip);
  }
  return list;
}

SimulateGuide::SimulateGuide(std::function<QAction*(const QString&)> lookup, std::function<void(const QString&)> run, QWidget* parent)
    : QWidget(parent, Qt::Window), m_lookup(std::move(lookup)), m_run(std::move(run)) {
  setObjectName("simulateGuide");
  setWindowTitle(tr("Simulation guide"));
  setAttribute(Qt::WA_StyledBackground);
  resize(920, 600);
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  m_list = new QListWidget(this);
  m_list->setObjectName("referenceList");
  m_list->setFixedWidth(300);
  m_list->setFrameShape(QFrame::NoFrame);
  m_list->setWordWrap(true);  // the long titles in two lines rather than cut
  m_list->setTextElideMode(Qt::ElideNone);
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  h->addWidget(m_list);
  auto* rule = new QFrame(this);
  rule->setFixedWidth(1);
  rule->setObjectName("referenceRule");
  h->addWidget(rule);
  auto* right = new QWidget(this);
  auto* v = new QVBoxLayout(right);
  v->setContentsMargins(20, 16, 20, 16);
  v->setSpacing(10);
  m_title = new QLabel(right);
  m_title->setObjectName("panelTitle");
  m_goal = new QLabel(right);
  m_goal->setWordWrap(true);
  m_goal->setObjectName("secondary");
  m_steps = new QLabel(right);
  m_steps->setObjectName("simGuideSteps");
  m_steps->setWordWrap(true);
  m_steps->setTextFormat(Qt::RichText);
  auto* toolsTitle = new QLabel(tr("TOOLS IT USES: press one to start it"), right);
  toolsTitle->setObjectName("sectionHeader");
  m_tools = new QGridLayout();
  m_tools->setSpacing(6);
  m_tip = new QLabel(right);
  m_tip->setWordWrap(true);
  m_tip->setObjectName("secondary");
  auto* next = new QPushButton(tr("Next"), right);
  next->setObjectName("simGuideNext");
  auto* buttons = new QHBoxLayout();
  buttons->addStretch(1);
  buttons->addWidget(next);
  v->addWidget(m_title);
  v->addWidget(m_goal);
  v->addWidget(m_steps);
  v->addWidget(toolsTitle);
  v->addLayout(m_tools);
  v->addWidget(m_tip);
  v->addStretch(1);
  v->addLayout(buttons);
  h->addWidget(right, 1);
  auto restyle = [rule] { rule->setStyleSheet(QString("background: %1;").arg(theme::css(theme::current().line))); };
  restyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, restyle);
  connect(m_list, &QListWidget::currentRowChanged, this, &SimulateGuide::refresh);
  connect(next, &QPushButton::clicked, this, [this] { m_list->setCurrentRow((m_list->currentRow() + 1) % m_list->count()); });
  auto fill = [this] {
    const int keep = std::max(0, m_list->currentRow());
    m_cases = useCases();
    const QSignalBlocker quiet(m_list);
    m_list->clear();
    for (int i = 0; i < m_cases.size(); ++i) m_list->addItem(QString("%1  %2").arg(i + 1).arg(m_cases[i].title));
    m_list->setCurrentRow(std::min(keep, int(m_cases.size()) - 1));
    refresh();
  };
  fill();
  connect(keys::notifier(), &keys::Notifier::changed, this, fill);  // the keys the steps name
}

void SimulateGuide::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

void SimulateGuide::open(int useCase) {
  m_list->setCurrentRow(std::clamp(useCase, 0, int(m_list->count()) - 1));
  QWidget::show();
  raise();
  activateWindow();
}

int SimulateGuide::current() const { return m_list->currentRow(); }
int SimulateGuide::count() const { return m_list->count(); }

void SimulateGuide::refresh() {
  const int i = m_list->currentRow();
  if (i < 0 || i >= m_cases.size()) return;
  const UseCase& u = m_cases[i];
  m_title->setText(u.title);
  m_goal->setText(u.goal);
  QString html = "<ol style=\"margin-left: 0px; -qt-list-indent: 1;\">";
  for (const QString& s : u.steps) html += "<li style=\"margin-bottom: 6px;\">" + s.toHtmlEscaped() + "</li>";
  m_steps->setText(html + "</ol>");
  m_tip->setText(u.tip.isEmpty() ? QString() : tr("Tip: %1").arg(u.tip));
  m_tip->setVisible(!u.tip.isEmpty());
  for (const auto& c : m_watch) disconnect(c);
  m_watch.clear();
  qDeleteAll(m_buttons);
  m_buttons.clear();
  int n = 0;
  for (const QString& id : u.commands) {
    QAction* a = m_lookup ? m_lookup(id) : nullptr;
    if (!a) continue;
    const QString key = keys::text(id);
    auto* b = new QPushButton(a->icon(), help::title(id) + (key.isEmpty() ? QString() : "  " + key), this);
    b->setObjectName("simGuideTool");
    b->setProperty("command", id);
    const CommandHelp* rec = help::find(id);
    if (rec) b->setToolTip(rec->summary);
    auto availability = [b, a, rec] {
      b->setEnabled(a->isEnabled());
      if (!a->isEnabled() && rec && !rec->requirement.isEmpty()) b->setToolTip(rec->summary + "\n" + help::requirement(*rec));
    };
    availability();
    m_watch << connect(a, &QAction::changed, b, availability);
    connect(b, &QPushButton::clicked, this, [this, id] {
      if (m_run) m_run(id);
    });
    m_tools->addWidget(b, n / 3, n % 3);
    m_buttons << b;
    ++n;
  }
}
