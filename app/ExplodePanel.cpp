#include "ExplodePanel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "Icons.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"

namespace {
QLabel* header(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setObjectName("sectionHeader");
  return l;
}

// A segmented row of checkable buttons (the section panel's look); the caller fills it.
QWidget* segmented(QWidget* parent, QHBoxLayout*& row) {
  auto* seg = new QWidget(parent);
  seg->setObjectName("segmented");
  row = new QHBoxLayout(seg);
  row->setContentsMargins(1, 1, 1, 1);
  row->setSpacing(0);
  return seg;
}

QToolButton* segment(const QString& text, QWidget* parent) {
  auto* b = new QToolButton(parent);
  b->setObjectName("segmentPrimary");
  b->setText(text);
  b->setCheckable(true);
  b->setAutoRaise(true);
  b->setFixedHeight(26);
  b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  return b;
}
}  // namespace

ExplodePanel::ExplodePanel(QWidget* parent) : QWidget(parent) {
  setObjectName("explodePanel");
  setFocusPolicy(Qt::StrongFocus);  // Space plays while the panel has the focus
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto* body = new QWidget(this);
  auto* layout = new QVBoxLayout(body);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(6);
  m_switch = new QCheckBox(tr("Exploded"), body);
  m_switch->setObjectName("explodeSwitch");
  m_switch->setToolTip(tr("Off: the parts go back together and the view lets go of them (Shift+E opens this panel again)"));
  layout->addWidget(m_switch);
  m_status = new QLabel(body);
  m_status->setObjectName("secondary");
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  layout->addWidget(header(tr("LEVEL"), body));
  m_levelRow = segmented(body, m_levelLayout);
  m_levelRow->setToolTip(tr("How deep components split: 1 moves the top-level parts as wholes, All moves every part"));
  layout->addWidget(m_levelRow);

  layout->addWidget(header(tr("DISTANCE"), body));
  auto* distanceRow = new QHBoxLayout;
  m_play = new QToolButton(body);
  m_play->setObjectName("explodePlay");
  m_play->setAutoRaise(true);
  m_play->setIcon(icons::themed("explodePlay", 16));
  m_play->setFixedSize(28, 28);
  m_play->setToolTip(tr("Play: the parts move out, or back when they are out (Space)"));
  m_slider = new QSlider(Qt::Horizontal, body);
  m_slider->setObjectName("explodeDistance");
  m_slider->setRange(0, 1000);
  m_slider->setToolTip(tr("How far the parts have moved: 0 % assembled, 100 % exploded"));
  m_distance = new QLineEdit(body);
  m_distance->setObjectName("mono");
  m_distance->setFixedWidth(64);
  m_distance->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  m_distance->setToolTip(tr("Type a percentage (0 to 100) and press Enter"));
  distanceRow->addWidget(m_play);
  distanceRow->addWidget(m_slider, 1);
  distanceRow->addWidget(m_distance);
  layout->addLayout(distanceRow);

  layout->addWidget(header(tr("LAYOUT"), body));
  auto* grid = new QGridLayout;
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(6);
  m_mode = new QComboBox(body);
  m_mode->setObjectName("explodeMode");
  m_mode->addItem(tr("Radial"), "radial");
  m_mode->addItem(tr("Along an axis"), "axis");
  m_mode->addItem(tr("Stack"), "stack");
  m_mode->setToolTip(tr("Radial: away from the centre · Along an axis: both ways along it · Stack: piled up along it"));
  m_axis = new QComboBox(body);
  m_axis->setObjectName("explodeAxis");
  for (const QString& name : {tr("X"), tr("Y"), tr("Z"), tr("View up")}) m_axis->addItem(name);
  m_axis->setCurrentIndex(2);
  m_spacing = new QDoubleSpinBox(body);
  m_spacing->setObjectName("explodeSpacing");
  m_spacing->setRange(0, 10);
  m_spacing->setSingleStep(0.1);
  m_spacing->setDecimals(2);
  m_spacing->setSuffix(QStringLiteral(" ×"));
  m_spacing->setToolTip(tr("Times the automatic distance (in a stack, the gap)"));
  m_stages = new QComboBox(body);
  m_stages->setObjectName("explodeStages");
  // Every level at the distance the slider says, or the parts one after another. Not a slider cut into a stretch per
  // level (IP design-around, TODO 11 D4): a spec that has it (made by the CLI) shows it while it is the one in use.
  m_stages->addItem(tr("All together"), "together");
  m_stages->addItem(tr("One after another"), "units");
  m_stages->setToolTip(tr("The order the parts move in as the distance grows"));
  grid->addWidget(new QLabel(tr("Mode"), body), 0, 0);
  grid->addWidget(m_mode, 0, 1);
  grid->addWidget(m_axis, 0, 2);
  grid->addWidget(new QLabel(tr("Spacing"), body), 1, 0);
  grid->addWidget(m_spacing, 1, 1);
  grid->addWidget(m_stages, 1, 2);
  grid->setColumnStretch(1, 1);
  grid->setColumnStretch(2, 1);
  layout->addLayout(grid);
  m_attach = new QCheckBox(tr("Keep small parts with what they touch"), body);
  m_attach->setToolTip(tr("A small part (a capacitor, a solder joint) moves with the larger part it sits on"));
  m_lines = new QCheckBox(tr("Explode lines"), body);
  m_lines->setToolTip(tr("A dashed line from where each part was to where it is"));
  layout->addWidget(m_attach);
  layout->addWidget(m_lines);

  layout->addWidget(header(tr("DRAG A PART ALONG"), body));
  QHBoxLayout* dragRow = nullptr;
  QWidget* drag = segmented(body, dragRow);
  const QStringList axes{tr("Its direction"), tr("X"), tr("Y"), tr("Z")};
  for (int i = 0; i < axes.size(); ++i) {
    QToolButton* b = segment(axes[i], drag);
    b->setChecked(i == 0);
    dragRow->addWidget(b, i == 0 ? 2 : 1);
    m_dragButtons << b;
    connect(b, &QToolButton::clicked, this, [this, i] {
      m_dragAxis = i;
      for (int k = 0; k < m_dragButtons.size(); ++k) m_dragButtons[k]->setChecked(k == i);
      emit dragAxisChosen(i);
    });
  }
  layout->addWidget(drag);
  auto* dragHint = new QHBoxLayout;
  auto* hint = new QLabel(tr("Select a part, then drag it or its arrows, or type a distance along the first arrow"), body);
  hint->setObjectName("tertiary");
  hint->setWordWrap(true);
  auto* reset = new QPushButton(tr("Reset drags"), body);
  reset->setObjectName("outline");
  reset->setToolTip(tr("Every part back to its automatic place"));
  dragHint->addWidget(hint, 1);
  dragHint->addWidget(reset);
  layout->addLayout(dragHint);

  layout->addWidget(header(tr("VIEW"), body));
  m_views = new QComboBox(body);
  m_views->setObjectName("explodeViews");
  m_views->setToolTip(tr("Exploded views saved in the document; choosing one shows it"));
  layout->addWidget(m_views);
  layout->addStretch(1);
  outer->addWidget(body, 1);

  m_footer = new PanelFooter(this);
  m_update = m_footer->addSecondary(tr("Update view"));
  m_update->setToolTip(tr("Save these settings into the view they came from"));
  m_update->hide();
  m_footer->setCancel(tr("Close"));
  m_footer->setPrimary(tr("Save as view"), QString());
  outer->addWidget(m_footer);

  connect(m_switch, &QCheckBox::toggled, this, &ExplodePanel::switched);
  connect(m_play, &QToolButton::clicked, this, &ExplodePanel::playRequested);
  connect(m_slider, &QSlider::valueChanged, this, [this](int v) { emit distanceChosen(v / 1000.0); });
  connect(m_distance, &QLineEdit::editingFinished, this, [this] {
    if (!m_distance->isModified()) return;
    m_distance->setModified(false);
    bool ok = false;
    const double percent = m_distance->text().remove('%').trimmed().toDouble(&ok);
    if (ok) emit distanceChosen(std::clamp(percent / 100.0, 0.0, 1.0));
    else showDistance(m_slider->value() / 1000.0);
  });
  auto mode = [this] {
    m_axis->setEnabled(m_mode->currentData().toString() != "radial");
    emit modeChosen(m_mode->currentData().toString(), m_axis->currentIndex());
  };
  connect(m_mode, &QComboBox::activated, this, mode);
  connect(m_axis, &QComboBox::activated, this, mode);
  connect(m_spacing, &QDoubleSpinBox::valueChanged, this, &ExplodePanel::spacingChosen);
  connect(m_stages, &QComboBox::activated, this, [this] { emit stagesChosen(m_stages->currentData().toString()); });
  connect(m_attach, &QCheckBox::toggled, this, &ExplodePanel::attachSmallToggled);
  connect(m_lines, &QCheckBox::toggled, this, &ExplodePanel::linesToggled);
  connect(reset, &QPushButton::clicked, this, &ExplodePanel::resetRequested);
  connect(m_views, &QComboBox::activated, this, [this](int i) {
    const std::string id = m_views->itemData(i).toString().toStdString();
    if (!id.empty()) emit viewChosen(id);
  });
  connect(m_footer, &PanelFooter::accepted, this, &ExplodePanel::saveRequested);
  connect(m_update, &QPushButton::clicked, this, &ExplodePanel::updateRequested);
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { showPlaying(m_play->property("playing").toBool()); });
}

void ExplodePanel::rebuildLevels(int depth) {
  const int shown = std::clamp(depth, 1, 6);  // deeper models: the deepest numbers fold into All
  if (shown == m_depth) return;
  m_depth = shown;
  qDeleteAll(m_levels);
  m_levels.clear();
  for (int i = 1; i <= shown + 1; ++i) {
    const int levels = i > shown ? 0 : i;
    QToolButton* b = segment(levels ? QString::number(levels) : tr("All"), m_levelRow);
    b->setFont(levels ? theme::mono(12) : theme::ui(12));
    b->setProperty("levels", levels);
    m_levelLayout->addWidget(b, 1);
    m_levels << b;
    connect(b, &QToolButton::clicked, this, [this, levels] { emit levelsChosen(levels); });
  }
}

void ExplodePanel::showSpec(const opad::ExplodeSpec& spec, int depth, bool on, bool lines) {
  rebuildLevels(depth);
  const int chosen = spec.levels > std::clamp(depth, 1, 6) ? 0 : spec.levels;
  for (QToolButton* b : m_levels) b->setChecked(b->property("levels").toInt() == chosen);
  const QSignalBlocker a(m_switch), b(m_mode), c(m_axis), d(m_spacing), e(m_stages), f(m_attach), g(m_lines);
  m_switch->setChecked(on);
  m_mode->setCurrentIndex(std::max(0, m_mode->findData(QString::fromStdString(spec.mode))));
  int axis = 3;  // a direction of its own: the view's up when it was chosen
  for (int i = 0; i < 3; ++i)
    if (std::abs(std::abs(spec.axis[static_cast<size_t>(i)]) - std::hypot(spec.axis[0], spec.axis[1], spec.axis[2])) < 1e-9) axis = i;
  m_axis->setCurrentIndex(axis);
  m_axis->setEnabled(spec.mode != "radial");
  m_spacing->setValue(spec.spacing);
  const int levelled = m_stages->findData("levels");
  if (spec.stages == "levels" && levelled < 0) m_stages->addItem(tr("Level by level (as saved)"), "levels");
  else if (spec.stages != "levels" && levelled >= 0) m_stages->removeItem(levelled);
  m_stages->setCurrentIndex(std::max(0, m_stages->findData(QString::fromStdString(spec.stages))));
  m_attach->setChecked(spec.attach_small);
  m_lines->setChecked(lines);
}

void ExplodePanel::showDistance(double t) {
  const QSignalBlocker block(m_slider);
  m_slider->setValue(static_cast<int>(std::lround(t * 1000)));
  if (!m_distance->hasFocus()) m_distance->setText(QString::number(std::lround(t * 100)) + QStringLiteral(" %"));
}

void ExplodePanel::showPlaying(bool on) {
  m_play->setProperty("playing", on);
  m_play->setIcon(icons::themed(on ? "explodePause" : "explodePlay", 16));
  m_play->setToolTip(on ? tr("Pause (Space)") : tr("Play: the parts move out, or back when they are out (Space)"));
}

void ExplodePanel::showStatus(const QString& text) { m_status->setText(text); }

void ExplodePanel::showViews(const std::vector<std::pair<std::string, QString>>& views, const std::string& current) {
  const QSignalBlocker block(m_views);
  m_views->clear();
  m_views->addItem(views.empty() ? tr("No exploded view saved yet") : tr("Not saved"), QString());
  for (const auto& [id, name] : views) {
    m_views->addItem(icons::themed("explodedView", 16), name, QString::fromStdString(id));
    if (id == current) m_views->setCurrentIndex(m_views->count() - 1);
  }
  m_update->setVisible(!current.empty());
}

void ExplodePanel::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Space && e->modifiers() == Qt::NoModifier) {
    emit playRequested();
    return e->accept();
  }
  QWidget::keyPressEvent(e);
}
