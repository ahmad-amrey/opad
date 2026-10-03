#include "SectionPanel.hpp"

#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "Icons.hpp"
#include "Theme.hpp"
#include "opad/inspect.hpp"

// ---------------------------------------------------------------- SectionPanel
SectionPanel::SectionPanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(8);
  m_state = new QLabel(this);
  m_state->setObjectName("secondary");
  m_state->setWordWrap(true);  // the hint is longer than the panel is wide: it was cut off mid-word
  layout->addWidget(m_state);
  auto* axisLabel = new QLabel(tr("AXIS"), this);
  axisLabel->setObjectName("sectionHeader");
  layout->addWidget(axisLabel);
  auto* seg = new QWidget(this);
  seg->setObjectName("segmented");
  auto* sl = new QHBoxLayout(seg);
  sl->setContentsMargins(1, 1, 1, 1);
  sl->setSpacing(0);
  const char* names[] = {"X", "Y", "Z", "Pick face"};
  for (int i = 0; i < 4; ++i) {
    auto* b = new QToolButton(seg);
    b->setObjectName("segmentPrimary");
    b->setText(names[i]);
    b->setCheckable(true);
    b->setChecked(i == 2);
    b->setFont(i < 3 ? theme::mono(12) : theme::ui(12));
    b->setFixedHeight(26);
    b->setAutoRaise(true);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);  // fill the segment: the checked one was a box round its letter
    sl->addWidget(b, 1);
    m_axisButtons << b;
    connect(b, &QToolButton::clicked, this, [this, i] {
      if (i == 3) { beginPick(); return; }
      for (int k = 0; k < 4; ++k) m_axisButtons[k]->setChecked(k == i);
      m_pick = false;
      m_axis = i;
      emitChange();
    });
  }
  layout->addWidget(seg);
  auto* offLabel = new QLabel(tr("OFFSET"), this);
  offLabel->setObjectName("sectionHeader");
  layout->addWidget(offLabel);
  auto* row = new QHBoxLayout();
  m_slider = new QSlider(Qt::Horizontal, this);
  m_slider->setRange(0, 1000);
  m_slider->setValue(500);
  row->addWidget(m_slider, 1);
  m_value = new QLineEdit(this);
  m_value->setObjectName("mono");
  m_value->setFixedWidth(88);
  m_value->setFont(theme::mono(12));
  m_value->setAlignment(Qt::AlignRight);
  row->addWidget(m_value);
  layout->addLayout(row);
  auto* toggles = new QHBoxLayout();
  m_flipButton = new QToolButton(this);
  m_flipButton->setObjectName("segment");
  m_flipButton->setText(tr("Flip   Shift+X"));
  m_flipButton->setCheckable(true);
  m_flipButton->setFixedHeight(28);
  m_capButton = new QToolButton(this);
  m_capButton->setObjectName("segment");
  m_capButton->setText(tr("Cap faces"));
  m_capButton->setCheckable(true);
  m_capButton->setChecked(true);
  m_capButton->setFixedHeight(28);
  auto styleToggles = [this] {
    for (QToolButton* b : {m_flipButton, m_capButton})
      b->setStyleSheet(QString("QToolButton { border: 1px solid %1; border-radius: 3px; padding: 0 10px; } QToolButton:checked { background: %2; border-color: %3; }")
                           .arg(theme::css(theme::current().line), theme::css(theme::current().selbg), theme::css(theme::current().sel)));
  };
  styleToggles();
  connect(theme::notifier(), &theme::Notifier::changed, this, styleToggles);
  for (QToolButton* b : {m_flipButton, m_capButton}) toggles->addWidget(b);
  toggles->addStretch();
  layout->addLayout(toggles);
  auto* namedLabel = new QLabel(tr("NAMED SECTIONS"), this);
  namedLabel->setObjectName("sectionHeader");
  layout->addWidget(namedLabel);
  m_named = new QListWidget(this);
  layout->addWidget(m_named, 1);
  auto* save = new QPushButton(tr("Save as named section"), this);
  save->setObjectName("primary");
  layout->addWidget(save);

  connect(m_slider, &QSlider::valueChanged, this, [this](int) { emitChange(); });
  connect(m_value, &QLineEdit::editingFinished, this, [this] { setAlong(m_value->text().section(' ', 0, 0).toDouble()); });
  connect(m_flipButton, &QToolButton::toggled, this, [this](bool on) { m_flip = on; emitChange(); });
  connect(m_capButton, &QToolButton::toggled, this, [this](bool) { emitChange(); });
  connect(save, &QPushButton::clicked, this, [this] {
    QString name = QString("Section %1").arg(m_doc->scene.sections.size() + 1);
    emit saveRequested(name, origin(), normal());
  });
  connect(m_named, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { applyNamed(it->data(Qt::UserRole).toString().toStdString()); });
  connect(doc, &AppDocument::changed, this, &SectionPanel::rebuild);
  rebuild();
  emitChange();
}

bool SectionPanel::pickRange(double& dmin, double& dmax) const {
  opad::Vec3 lo, hi;
  if (!opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return false;
  dmin = 1e300;
  dmax = -1e300;
  for (int i = 0; i < 8; ++i) {
    const double x = (i & 1) ? hi[0] : lo[0], y = (i & 2) ? hi[1] : lo[1], z = (i & 4) ? hi[2] : lo[2];
    const double d = x * m_pickNormal[0] + y * m_pickNormal[1] + z * m_pickNormal[2];
    dmin = std::min(dmin, d);
    dmax = std::max(dmax, d);
  }
  return dmax > dmin;
}

opad::Vec3 SectionPanel::origin() const {
  if (m_pick) {
    // The slider slides the picked plane along its normal; 0..1000 spans the model in that direction.
    double dmin, dmax;
    if (!pickRange(dmin, dmax)) return m_pickOrigin;
    const double d = dmin + m_slider->value() / 1000.0 * (dmax - dmin);
    const double d0 = m_pickOrigin[0] * m_pickNormal[0] + m_pickOrigin[1] * m_pickNormal[1] + m_pickOrigin[2] * m_pickNormal[2];
    return {m_pickOrigin[0] + m_pickNormal[0] * (d - d0), m_pickOrigin[1] + m_pickNormal[1] * (d - d0), m_pickOrigin[2] + m_pickNormal[2] * (d - d0)};
  }
  opad::Vec3 lo{0, 0, 0}, hi{0, 0, 0};
  if (!opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return {0, 0, 0};
  opad::Vec3 o{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  o[m_axis] = lo[m_axis] + m_slider->value() / 1000.0 * (hi[m_axis] - lo[m_axis]);
  return o;
}

opad::Vec3 SectionPanel::normal() const {
  if (m_pick) return m_flip ? opad::Vec3{-m_pickNormal[0], -m_pickNormal[1], -m_pickNormal[2]} : m_pickNormal;
  opad::Vec3 n{0, 0, 0};
  n[m_axis] = m_flip ? 1 : -1;
  return n;
}

bool SectionPanel::caps() const { return m_capButton->isChecked(); }

void SectionPanel::setAlong(double along) {
  if (m_pick) {
    double dmin, dmax;
    if (pickRange(dmin, dmax)) m_slider->setValue(static_cast<int>(std::clamp((along - dmin) / (dmax - dmin), 0.0, 1.0) * 1000));
    return;
  }
  opad::Vec3 lo, hi;
  if (!opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return;
  const double range = hi[m_axis] - lo[m_axis];
  if (range > 0) m_slider->setValue(static_cast<int>(std::clamp((along - lo[m_axis]) / range, 0.0, 1.0) * 1000));
}

void SectionPanel::setOrigin(const opad::Vec3& o) {
  // The slider's valueChanged re-emits planeChanged, so the view follows (at the slider's 1/1000 resolution).
  setAlong(m_pick ? o[0] * m_pickNormal[0] + o[1] * m_pickNormal[1] + o[2] * m_pickNormal[2] : o[m_axis]);
}

void SectionPanel::emitChange() {
  const char axes[] = {'X', 'Y', 'Z'};
  opad::Vec3 o = origin();
  const double along = m_pick ? o[0] * m_pickNormal[0] + o[1] * m_pickNormal[1] + o[2] * m_pickNormal[2] : o[m_axis];  // pick mode: distance along the face normal
  m_value->setText(QString("%1 mm").arg(along, 0, 'f', 1));
  m_state->setText(!m_enabled ? tr("Section off · press X or use Inspect › Section to enable")
                   : m_pick ? tr("Section along the picked face = %1 mm · drag the slider or the plane's edge, Shift+X flips").arg(along, 0, 'f', 1)
                            : tr("Section %1 = %2 mm · drag the slider or the plane's edge, Shift+X flips").arg(axes[m_axis]).arg(o[m_axis], 0, 'f', 1));
  emit planeChanged();
}

void SectionPanel::setEnabled(bool on) {
  if (m_enabled == on) return;
  m_enabled = on;
  emitChange();
  emit enabledChanged(on);
}

void SectionPanel::flip() { m_flipButton->setChecked(!m_flipButton->isChecked()); }

void SectionPanel::beginPick() {
  m_pick = true;
  for (int k = 0; k < 4; ++k) m_axisButtons[k]->setChecked(k == 3);
  m_state->setText(tr("Pick face · click a planar face in the 3D view"));
  emit pickRequested();
}

void SectionPanel::setFromFace(const opad::Vec3& origin, const opad::Vec3& normal) {
  m_pick = true;
  m_pickOrigin = origin;
  m_pickNormal = normal;
  for (int k = 0; k < 4; ++k) m_axisButtons[k]->setChecked(k == 3);
  double dmin, dmax;
  if (pickRange(dmin, dmax)) {  // start the slider at the face itself, so the plane does not jump
    const double d0 = origin[0] * normal[0] + origin[1] * normal[1] + origin[2] * normal[2];
    QSignalBlocker block(m_slider);
    m_slider->setValue(static_cast<int>(std::clamp((d0 - dmin) / (dmax - dmin), 0.0, 1.0) * 1000));
  }
  emitChange();
}

void SectionPanel::rebuild() {
  m_named->clear();
  const char axes[] = {'X', 'Y', 'Z'};
  for (const auto& s : m_doc->scene.sections) {
    int axis = std::fabs(s.normal[0]) > 0.9 ? 0 : std::fabs(s.normal[1]) > 0.9 ? 1 : 2;
    auto* it = new QListWidgetItem(icons::themed("section", 16), QString("%1        %2 = %3 mm").arg(QString::fromStdString(s.name)).arg(axes[axis]).arg(s.origin[axis], 0, 'f', 1));
    it->setData(Qt::UserRole, QString::fromStdString(s.id));
    it->setToolTip(tr("Double-click to apply"));
    m_named->addItem(it);
  }
}

void SectionPanel::applyNamed(const std::string& id) {
  for (const auto& s : m_doc->scene.sections)
    if (s.id == id) {
      m_enabled = true;
      setFromFace(s.origin, s.normal);
      emit enabledChanged(true);
      return;
    }
}
