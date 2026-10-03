#include "CheckPanel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

#include "Units.hpp"

namespace {
// A length typed in the shown unit (UI-123); the check takes mm, kept in the box's "mm" property.
void lengthBox(QDoubleSpinBox* box, double mm, double maxMm) {
  auto show = [box, maxMm] {
    QSignalBlocker block(box);
    box->setDecimals(units::decimalsFor(0.01));
    box->setRange(0, units::toDisplay(units::Kind::Length, maxMm));
    box->setSuffix(' ' + units::symbol(units::Kind::Length));
    box->setValue(units::toDisplay(units::Kind::Length, box->property("mm").toDouble()));
  };
  box->setProperty("mm", mm);
  box->setSingleStep(0.1);
  QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [box](double v) { box->setProperty("mm", units::fromDisplay(units::Kind::Length, v)); });
  QObject::connect(units::notifier(), &units::Notifier::changed, box, show);
  show();
}
}  // namespace

CheckPanel::CheckPanel(QWidget* parent) : QWidget(parent) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(12, 10, 12, 10);
  v->setSpacing(8);
  // Interference: the gap below which two parts count as too close.
  m_interference = new QWidget(this);
  auto* fi = new QFormLayout(m_interference);
  fi->setContentsMargins(0, 0, 0, 0);
  m_clearance = new QDoubleSpinBox(m_interference);
  lengthBox(m_clearance, 0, 1000);
  m_clearance->setToolTip(tr("Also list pairs closer than this; 0 lists only overlaps"));
  fi->addRow(tr("Clearance"), m_clearance);
  v->addWidget(m_interference);
  // Print: the way layers stack, the steepest overhang printed without support, the thinnest wall.
  m_print = new QWidget(this);
  auto* fp = new QFormLayout(m_print);
  fp->setContentsMargins(0, 0, 0, 0);
  m_direction = new QComboBox(m_print);
  for (const char* d : {"+z", "-z", "+x", "-x", "+y", "-y"}) m_direction->addItem(QString::fromLatin1(d).toUpper(), QString::fromLatin1(d));
  fp->addRow(tr("Build direction"), m_direction);
  m_overhang = new QDoubleSpinBox(m_print);
  m_overhang->setRange(0, 89);
  m_overhang->setValue(45);
  m_overhang->setSuffix(tr(" deg"));
  fp->addRow(tr("Overhang"), m_overhang);
  m_minWall = new QDoubleSpinBox(m_print);
  lengthBox(m_minWall, 0.8, 100);
  fp->addRow(tr("Minimum wall"), m_minWall);
  v->addWidget(m_print);
  auto* row = new QHBoxLayout();
  m_status = new QLabel(this);
  m_status->setObjectName("secondary");
  m_status->setWordWrap(true);
  m_run = new QPushButton(tr("Check"), this);
  m_run->setObjectName("primary");
  row->addWidget(m_status, 1);
  row->addWidget(m_run);
  v->addLayout(row);
  m_list = new QListWidget(this);
  m_list->setWordWrap(true);
  v->addWidget(m_list);
  v->addStretch(1);  // a panel taller than its content keeps the room at the bottom, not between the options and Check
  connect(m_run, &QPushButton::clicked, this, &CheckPanel::runRequested);
  connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0 && static_cast<size_t>(row) < m_findings.size()) emit findingActivated(m_findings[static_cast<size_t>(row)]);
  });
  begin(Mode::Interference);
  connect(units::notifier(), &units::Notifier::changed, this, [this] { if (!m_result.is_null()) setResult(m_result); });
}

void CheckPanel::begin(Mode mode) {
  m_mode = mode;
  m_interference->setVisible(mode == Mode::Interference);
  m_print->setVisible(mode == Mode::Print);
  m_findings.clear();
  m_list->clear();
  m_status->clear();
  m_result = opad::json();
  showFindings();
}

void CheckPanel::showFindings() {
  const int rows = m_list->count();
  m_list->setVisible(rows > 0);
  if (rows > 0) m_list->setFixedHeight(std::min(10, rows) * std::max(20, m_list->sizeHintForRow(0)) + 2 * m_list->frameWidth() + 4);  // its rows, up to ten
  emit contentResized();
}

opad::json CheckPanel::options() const {
  if (m_mode == Mode::Interference) return {{"clearance_mm", m_clearance->property("mm").toDouble()}};
  return {{"build_direction", m_direction->currentData().toString().toStdString()}, {"overhang_deg", m_overhang->value()}, {"min_wall_mm", m_minWall->property("mm").toDouble()}};
}

void CheckPanel::setRunning(const QString& status) {
  m_result = opad::json();
  m_findings.clear();
  m_list->clear();
  m_status->setText(status);
  m_run->setEnabled(false);
  showFindings();
}

void CheckPanel::setFailed(const QString& error) {
  m_status->setText(error);
  m_run->setEnabled(true);
  showFindings();
}

void CheckPanel::setResult(const opad::json& r) {
  m_result = r;
  m_run->setEnabled(true);
  m_findings.clear();
  m_list->clear();
  auto name = [](const opad::json& j, const char* key) { return QString::fromStdString(j.value(key, std::string())); };
  if (m_mode == Mode::Interference) {
    for (const auto& f : r.value("items", opad::json::array())) {
      const QString pair = tr("%1 and %2").arg(name(f, "a_name"), name(f, "b_name"));
      m_list->addItem(f.value("kind", "") == "interference" ? tr("%1: overlap %2").arg(pair, units::format(units::Kind::Volume, f.value("volume_mm3", 0.0)))
                                                             : tr("%1: %2 apart").arg(pair, units::format(units::Kind::Length, f.value("distance_mm", 0.0))));
      m_findings.push_back(f);
    }
    const int overlaps = r.value("interferences", 0), close = r.value("too_close", 0);
    QString status = overlaps == 0 && close == 0 ? tr("No interference. Bodies checked: %1.").arg(r.value("bodies", 0))
                                                 : tr("Overlapping pairs: %1, too close: %2. Bodies checked: %3.").arg(overlaps).arg(close).arg(r.value("bodies", 0));
    if (r.contains("truncated")) status += tr(" Not every pair was checked: select fewer bodies.");
    m_status->setText(status);
    showFindings();
    return;
  }
  int total = 0;
  for (const auto& body : r.value("items", opad::json::array())) {
    const QString who = name(body, "name");
    auto add = [&](const QString& text, const opad::json& faces, const char* kind) {
      m_list->addItem(text);
      m_findings.push_back({{"body", body.value("id", "")}, {"faces", faces}, {"kind", kind}});
      ++total;
    };
    // A mesh (STL...) has no faces to name: its findings are regions of triangles.
    const bool mesh = body.value("mesh", false);
    auto faces = [](const opad::json& f) { return f.contains("faces") ? f["faces"] : opad::json::array({f.value("face", 0)}); };
    for (const auto& o : body.value("overhangs", opad::json::array()))
      add(mesh ? tr("%1: overhang up to %2° over %3").arg(who).arg(o.value("overhang_deg", 0.0), 0, 'f', 0).arg(units::format(units::Kind::Area, o.value("area_mm2", 0.0), 1))
               : tr("%1: face %2 overhangs %3°").arg(who).arg(o.value("face", 0)).arg(o.value("overhang_deg", 0.0), 0, 'f', 0),
          faces(o), "overhang");
    for (const auto& w : body.value("thin_walls", opad::json::array()))
      add(mesh ? tr("%1: wall %2 thin over %3 triangles").arg(who, units::format(units::Kind::Length, w.value("thickness_mm", 0.0))).arg(w.value("triangles", 0))
               : tr("%1: wall %2 at face %3").arg(who, units::format(units::Kind::Length, w.value("thickness_mm", 0.0))).arg(w.value("face", 0)),
          faces(w), "thin_wall");
    if (const int more = body.value("more_overhangs", 0) + body.value("more_thin_walls", 0); more > 0)
      add(tr("%1: %2 smaller findings not listed").arg(who).arg(more), opad::json::array(), "more");
    for (const auto& t : body.value("thin_features", opad::json::array()))
      add(tr("%1: face %2 is %3 wide").arg(who).arg(t.value("face", 0)).arg(units::format(units::Kind::Length, t.value("width_mm", 0.0))), opad::json::array({t.value("face", 0)}), "thin_feature");
    if (body.value("contact_area_mm2", 0.0) <= 0)
      add(tr("%1: nothing rests on the build plate").arg(who), opad::json::array(), "no_contact");
  }
  m_status->setText(total == 0 ? tr("Nothing to report. Bodies checked: %1.").arg(r.value("bodies", 0)) : tr("Findings: %1. Bodies checked: %2.").arg(total).arg(r.value("bodies", 0)));
  showFindings();
}

// The options and the status, and the findings when there are any: a fixed height left an empty list as half the panel.
QSize CheckPanel::preferredSize(int width) const {
  QLayout* box = layout();
  const int w = std::max(1, width);
  const int h = box->hasHeightForWidth() ? box->heightForWidth(w) : box->sizeHint().height();
  return {width, std::clamp(h, 96, 520)};
}

void CheckPanel::activate(int row) { m_list->setCurrentRow(row); }
