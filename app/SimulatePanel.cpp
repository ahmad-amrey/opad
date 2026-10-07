#include "SimulatePanel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "Icons.hpp"
#include "PanelFooter.hpp"
#include "SimPlot.hpp"

namespace {
QLabel* header(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setObjectName("sectionHeader");
  return l;
}

QLabel* note(QWidget* parent) {
  auto* l = new QLabel(parent);
  l->setObjectName("secondary");
  l->setWordWrap(true);
  return l;
}

// A combo that shrinks with the panel (its longest entry would widen the panel past its edge) and elides.
void shrinkable(QComboBox* box) {
  box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  box->setMinimumContentsLength(6);
  box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void fill(QComboBox* box, const SimulatePanel::Entries& entries, const QString& current) {
  const QSignalBlocker block(box);
  box->clear();
  for (const auto& [id, text] : entries) box->addItem(text, id);
  const int at = box->findData(current);
  box->setCurrentIndex(at >= 0 ? at : (box->count() ? 0 : -1));
}

constexpr int kSliderSteps = 2000;
}  // namespace

SimulatePanel::SimulatePanel(QWidget* parent) : QWidget(parent) {
  setObjectName("simulatePanel");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto* scroll = new QScrollArea(this);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* body = new QWidget(scroll);
  auto* layout = new QVBoxLayout(body);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(6);

  // ---- the mechanism and its joints
  layout->addWidget(header(tr("MECHANISM"), body));
  m_mechanism = note(body);
  m_mechanism->setObjectName("simMechanism");
  layout->addWidget(m_mechanism);
  m_joints = new QComboBox(body);
  m_joints->setObjectName("simJoints");
  m_joints->setToolTip(tr("The joint whose value the slider sets; the parts joined to it follow"));
  layout->addWidget(m_joints);
  auto* valueRow = new QHBoxLayout;
  m_jointSlider = new QSlider(Qt::Horizontal, body);
  m_jointSlider->setObjectName("simJointSlider");
  m_jointSlider->setRange(0, kSliderSteps);
  m_jointSlider->setToolTip(tr("Drag to move the mechanism; it is written to the document when you let go"));
  m_jointValue = new QDoubleSpinBox(body);
  m_jointValue->setObjectName("simJointValue");
  m_jointValue->setDecimals(2);
  m_jointValue->setRange(-1e6, 1e6);
  m_jointValue->setKeyboardTracking(false);
  m_jointValue->setFixedWidth(88);
  m_jointUnit = new QLabel(body);
  valueRow->addWidget(m_jointSlider, 1);
  valueRow->addWidget(m_jointValue);
  valueRow->addWidget(m_jointUnit);
  layout->addLayout(valueRow);

  layout->addWidget(header(tr("NEW JOINT"), body));
  auto* kindRow = new QHBoxLayout;
  m_kind = new QComboBox(body);
  m_kind->setObjectName("simJointKind");
  const std::pair<const char*, QString> kinds[] = {
      {"revolute", tr("Revolute: turns about an axis")},       {"slider", tr("Slider: slides along an axis")},
      {"cylindrical", tr("Cylindrical: turns and slides")},    {"ball", tr("Ball: turns any way about a point")},
      {"planar", tr("Planar: slides and turns on a plane")},  {"pin_slot", tr("Pin in slot")},
      {"screw", tr("Screw: advances as it turns")},           {"rigid", tr("Rigid: moves as one")},
      {"ground", tr("Ground: held where it is")},              {"gear", tr("Gear relation between two joints")},
      {"rack_pinion", tr("Rack and pinion relation")},        {"lead_screw", tr("Lead screw relation")}};
  for (const auto& [id, text] : kinds) m_kind->addItem(text, QString(id));
  m_addJoint = new QPushButton(tr("Add"), body);
  m_addJoint->setObjectName("simAddJoint");
  kindRow->addWidget(m_kind, 1);
  kindRow->addWidget(m_addJoint);
  layout->addLayout(kindRow);
  auto* relationRow = new QHBoxLayout;
  m_relationLabel = new QLabel(body);
  m_relation = new QDoubleSpinBox(body);
  m_relation->setObjectName("simRelationValue");
  m_relation->setDecimals(4);
  m_relation->setRange(-1e5, 1e5);
  relationRow->addWidget(m_relationLabel);
  relationRow->addWidget(m_relation, 1);
  layout->addLayout(relationRow);
  m_jointHint = note(body);
  layout->addWidget(m_jointHint);

  // ---- studies
  layout->addWidget(header(tr("STUDY"), body));
  auto* studyRow = new QHBoxLayout;
  m_studies = new QComboBox(body);
  m_studies->setObjectName("simStudies");
  m_run = new QPushButton(tr("Run"), body);
  m_run->setObjectName("simRun");
  m_run->setToolTip(tr("Run the study on the model as it is now"));
  studyRow->addWidget(m_studies, 1);
  studyRow->addWidget(m_run);
  layout->addLayout(studyRow);
  auto* newRow = new QHBoxLayout;
  m_newKind = new QComboBox(body);
  m_newKind->setObjectName("simNewKind");
  m_newKind->addItem(tr("Motion (kinematic)"), "motion");
  m_newKind->addItem(tr("Dynamic (gravity, motors, contacts)"), "dynamic");
  m_newKind->addItem(tr("Static stress"), "static");
  m_newKind->addItem(tr("Vibration modes"), "modal");
  m_new = new QPushButton(tr("New study"), body);
  m_new->setObjectName("simNewStudy");
  newRow->addWidget(m_newKind, 1);
  newRow->addWidget(m_new);
  layout->addLayout(newRow);
  m_summary = note(body);
  m_summary->setObjectName("simSummary");
  m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(m_summary);

  m_motionRows = new QWidget(body);
  auto* motion = new QVBoxLayout(m_motionRows);
  motion->setContentsMargins(0, 0, 0, 0);
  motion->setSpacing(6);
  auto* frameRow = new QHBoxLayout;
  m_play = new QToolButton(m_motionRows);
  m_play->setObjectName("simPlay");
  m_play->setAutoRaise(true);
  m_play->setFixedSize(28, 28);
  m_play->setIcon(icons::themed("explodePlay", 16));
  m_play->setToolTip(tr("Play the study's frames (Space)"));
  m_frames = new QSlider(Qt::Horizontal, m_motionRows);
  m_frames->setObjectName("simFrame");
  m_time = new QLabel(m_motionRows);
  m_time->setObjectName("mono");
  m_time->setMinimumWidth(72);
  m_time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  frameRow->addWidget(m_play);
  frameRow->addWidget(m_frames, 1);
  frameRow->addWidget(m_time);
  motion->addLayout(frameRow);
  m_series = new QComboBox(m_motionRows);
  m_series->setObjectName("simSeries");
  m_series->setToolTip(tr("What the chart shows"));
  motion->addWidget(m_series);
  m_plot = new SimPlot(m_motionRows);
  m_plot->setMinimumHeight(170);
  motion->addWidget(m_plot);
  layout->addWidget(m_motionRows);

  m_resultRows = new QWidget(body);
  auto* results = new QHBoxLayout(m_resultRows);
  results->setContentsMargins(0, 0, 0, 0);
  m_fields = new QComboBox(m_resultRows);
  m_fields->setObjectName("simField");
  m_results = new QToolButton(m_resultRows);
  m_results->setObjectName("simResults");
  m_results->setText(tr("Result map"));
  m_results->setCheckable(true);
  m_results->setToolTip(tr("Colour the parts by the chosen result, deformed and scaled to be seen"));
  results->addWidget(m_fields, 1);
  results->addWidget(m_results);
  layout->addWidget(m_resultRows);
  for (QComboBox* box : {m_joints, m_kind, m_studies, m_newKind, m_series, m_fields}) shrinkable(box);
  layout->addStretch(1);
  scroll->setWidget(body);
  outer->addWidget(scroll, 1);
  m_footer = new PanelFooter(this);
  m_footer->setPrimaryVisible(false);
  m_footer->setCancel(tr("Close"));
  outer->addWidget(m_footer);

  connect(m_joints, &QComboBox::currentIndexChanged, this, [this] { emit jointChosen(m_joints->currentData().toString()); });
  auto sliderValue = [this] { return m_lo + (m_hi - m_lo) * m_jointSlider->value() / double(kSliderSteps); };
  connect(m_jointSlider, &QSlider::valueChanged, this, [this, sliderValue] {
    const QSignalBlocker block(m_jointValue);
    m_jointValue->setValue(sliderValue());
    if (m_jointSlider->isSliderDown()) emit jointValueChanging(sliderValue());
    else emit jointValueChosen(sliderValue());  // a key or a click on the groove
  });
  connect(m_jointSlider, &QSlider::sliderReleased, this, [this, sliderValue] { emit jointValueChosen(sliderValue()); });
  connect(m_jointValue, &QDoubleSpinBox::valueChanged, this, [this](double v) { emit jointValueChosen(v); });
  connect(m_kind, &QComboBox::currentIndexChanged, this, &SimulatePanel::jointKindChanged);
  connect(m_addJoint, &QPushButton::clicked, this, [this] { emit addJointRequested(jointKind()); });
  connect(m_studies, &QComboBox::currentIndexChanged, this, [this] { emit studyChosen(m_studies->currentData().toString()); });
  connect(m_new, &QPushButton::clicked, this, [this] { emit newStudyRequested(m_newKind->currentData().toString()); });
  connect(m_run, &QPushButton::clicked, this, &SimulatePanel::runRequested);
  connect(m_play, &QToolButton::clicked, this, &SimulatePanel::playRequested);
  connect(m_frames, &QSlider::valueChanged, this, &SimulatePanel::frameChosen);
  connect(m_series, &QComboBox::currentIndexChanged, this, &SimulatePanel::seriesChosen);
  connect(m_fields, &QComboBox::currentIndexChanged, this, [this] { emit fieldChosen(m_fields->currentData().toString()); });
  connect(m_results, &QToolButton::toggled, this, &SimulatePanel::resultsToggled);
  connect(m_plot, &SimPlot::framePicked, this, [this](int f) { m_frames->setValue(f); });
  jointKindChanged();
  setJointValue(false, 0, 1, 0, {});
  setRun({}, 0, false);
}

QString SimulatePanel::jointKind() const { return m_kind->currentData().toString(); }

double SimulatePanel::relationValue() const { return m_relation->isVisible() ? m_relation->value() : 0; }

void SimulatePanel::jointKindChanged() {
  const QString k = jointKind();
  const bool relation = k == "gear" || k == "rack_pinion" || k == "lead_screw" || k == "screw";
  m_relationLabel->setVisible(relation);
  m_relation->setVisible(relation);
  if (k == "gear") {
    m_relationLabel->setText(tr("Ratio"));
    m_relation->setValue(-1);
    m_jointHint->setText(tr("Pick two revolute joints in the Simulation folder; the second turns ratio times the first (negative: the other way, as meshing spur gears do)"));
  } else if (k == "rack_pinion") {
    m_relationLabel->setText(tr("Pitch radius (mm)"));
    m_relation->setValue(20);
    m_jointHint->setText(tr("Pick the pinion's revolute joint, then the rack's slider joint, in the Simulation folder"));
  } else if (k == "lead_screw") {
    m_relationLabel->setText(tr("Lead (mm per turn)"));
    m_relation->setValue(2);
    m_jointHint->setText(tr("Pick the screw's revolute joint, then the nut's slider joint, in the Simulation folder"));
  } else if (k == "screw") {
    m_relationLabel->setText(tr("Pitch (mm per turn)"));
    m_relation->setValue(1.5);
    m_jointHint->setText(tr("Pick a circular edge or a cylindrical face of the moving part, then one of the part it moves in (none: the world)"));
  } else {
    m_jointHint->setText(tr("Pick a circular edge, a cylindrical or flat face, or a vertex of the moving part, then optionally one of the part it moves in (none: the world)"));
  }
}

void SimulatePanel::setMechanism(const QString& status) { m_mechanism->setText(status); }

void SimulatePanel::setJoints(const Entries& joints, const QString& current) {
  fill(m_joints, joints, current);
  m_joints->setEnabled(!joints.empty());
}

void SimulatePanel::setJointValue(bool enabled, double lo, double hi, double value, const QString& unit) {
  m_lo = lo;
  m_hi = hi > lo ? hi : lo + 1;
  const QSignalBlocker a(m_jointSlider), b(m_jointValue);
  m_jointSlider->setEnabled(enabled);
  m_jointValue->setEnabled(enabled);
  m_jointSlider->setValue(int(std::lround((value - m_lo) / (m_hi - m_lo) * kSliderSteps)));
  m_jointValue->setValue(value);
  m_jointUnit->setText(unit);
}

void SimulatePanel::setStudies(const Entries& studies, const QString& current) {
  fill(m_studies, studies, current);
  m_run->setEnabled(!studies.empty());
}

void SimulatePanel::setRun(const QString& summary, int frames, bool structural) {
  m_summary->setText(summary.isEmpty() ? tr("No results yet: Run computes them.") : summary);
  m_motionRows->setVisible(frames > 1 && !structural);
  m_resultRows->setVisible(structural);
  const QSignalBlocker block(m_frames);
  m_frames->setRange(0, std::max(0, frames - 1));
}

void SimulatePanel::setFrame(int frame, const QString& time) {
  {
    const QSignalBlocker block(m_frames);
    m_frames->setValue(frame);
  }
  m_time->setText(time);
  m_plot->setCursor(frame);
}

void SimulatePanel::setSeries(const QStringList& names, int current) {
  const QSignalBlocker block(m_series);
  m_series->clear();
  m_series->addItems(names);
  m_series->setCurrentIndex(std::clamp(current, 0, std::max(0, int(names.size()) - 1)));
}

void SimulatePanel::setFields(const Entries& fields, const QString& current) { fill(m_fields, fields, current); }

void SimulatePanel::setPlaying(bool playing) {
  m_play->setIcon(icons::themed(playing ? "explodePause" : "explodePlay", 16));
  m_play->setToolTip(playing ? tr("Pause (Space)") : tr("Play the study's frames (Space)"));
}

void SimulatePanel::setResultsShown(bool shown) {
  const QSignalBlocker block(m_results);
  m_results->setChecked(shown);
}
