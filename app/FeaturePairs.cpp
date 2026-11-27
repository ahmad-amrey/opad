// FeaturePanel, point pairs (InputSpec::pairs_with; Align's from_points / to_points): the list of pairs under the two
// inputs, a row each with what each side has, what the pair is off by after the fit and Remove, and the fit's summary.
#include <QHBoxLayout>
#include <QToolButton>

#include <algorithm>

#include "DesignPanels.hpp"
#include "I18n.hpp"
#include "Theme.hpp"
#include "Units.hpp"

namespace {
int count(const opad::json& p) { return p.is_array() ? int(p.size()) : p.is_null() ? 0 : 1; }
}  // namespace

std::pair<QString, QString> FeaturePanel::pairInputs() const {
  if (!m_spec) return {};
  for (const auto& in : m_spec->inputs) {
    if (in.pairs_with.empty()) continue;
    const auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it == m_widgets.end() || it->second.row->isHidden()) return {};
    return {QString::fromStdString(in.name), QString::fromStdString(in.pairs_with)};  // the first declared is the From side
  }
  return {};
}

int FeaturePanel::completePairs() const {
  const auto [from, to] = pairInputs();
  return from.isEmpty() ? 0 : std::min(count(picks(from)), count(picks(to)));
}

void FeaturePanel::setPairFit(const std::vector<double>& residuals, int dropped, double rms) {
  if (residuals == m_pairResiduals && dropped == m_pairDropped && rms == m_pairRms) return;
  m_pairResiduals = residuals;
  m_pairDropped = dropped;
  m_pairRms = rms;
  m_pairShown.clear();  // rebuilt with the new values
  refreshPairList();
}

void FeaturePanel::removePair(int index) {
  const auto [from, to] = pairInputs();
  if (from.isEmpty()) return;
  for (const QString& side : {from, to}) {
    opad::json p = picks(side);
    if (p.is_array() && index >= 0 && index < int(p.size())) p.erase(p.begin() + index);
    setPicks(side, p);
  }
  m_pairResiduals.clear();  // the fit is made again without it
  m_pairDropped = -1;
  m_pairShown.clear();
  refreshPairList();
  emit pairsEdited();
  emit inputsChanged();
}

void FeaturePanel::buildPairList() {
  m_pairList = new QWidget(this);
  m_pairList->setObjectName("pairList");
  auto* v = new QVBoxLayout(m_pairList);
  v->setContentsMargins(126, 0, 0, 4);  // under the boxes, as a fit row is
  v->setSpacing(2);
  m_pairRows = new QVBoxLayout;
  m_pairRows->setSpacing(2);
  v->addLayout(m_pairRows);
  m_pairSummary = new QLabel(m_pairList);
  m_pairSummary->setObjectName("tertiary");
  m_pairSummary->setWordWrap(true);
  v->addWidget(m_pairSummary);
  m_rows->addWidget(m_pairList);
}

void FeaturePanel::refreshPairList() {
  if (!m_pairList) return;
  const auto [from, to] = pairInputs();
  m_pairList->setVisible(!from.isEmpty());
  if (from.isEmpty()) return;
  const int nf = count(picks(from)), nt = count(picks(to));
  const int rows = std::max(nf, nt);
  // Rebuilt only when what it shows changed: refreshVisibility comes here on every pick and activation.
  const QString shown = QString("%1/%2/%3/%4").arg(nf).arg(nt).arg(m_active).arg(units::format(units::Kind::Length, 1.0));
  if (shown == m_pairShown) return;
  m_pairShown = shown;
  while (QLayoutItem* it = m_pairRows->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  const Tokens& t = theme::current();
  const bool fitted = !m_pairResiduals.empty();
  for (int i = 0; i < rows; ++i) {
    auto* row = new QWidget(m_pairList);
    row->setObjectName("pairRow");
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto* number = new QLabel(QString::number(i + 1), row);
    number->setFixedWidth(18);
    number->setObjectName("secondary");
    QString state;
    if (i < nf && i < nt) state = tr("Both picked");
    else if (i < nf) state = m_active == to ? tr("Pick its match on the target") : tr("Target point missing");
    else state = tr("Point on the bodies missing");
    QString off;
    QColor colour = t.fg3;
    if (fitted && i < int(m_pairResiduals.size()) && i < nf && i < nt) {
      off = units::format(units::Kind::Length, m_pairResiduals[size_t(i)]);
      if (i == m_pairDropped) {
        off = tr("%1, left out").arg(off);
        colour = t.red;
      }
    }
    auto* text = new QLabel(off.isEmpty() ? state : off, row);
    text->setObjectName("pairText");
    text->setStyleSheet(QString("color: %1;").arg(theme::css(colour)));
    text->setToolTip(state);
    auto* remove = new QToolButton(row);
    remove->setText(tr("Remove"));
    remove->setToolTip(tr("Remove this pair (both of its points)"));
    remove->setAutoRaise(true);
    connect(remove, &QToolButton::clicked, this, [this, i] { removePair(i); });
    h->addWidget(number);
    h->addWidget(text, 1);
    h->addWidget(remove);
    m_pairRows->addWidget(row);
  }
  const int complete = std::min(nf, nt);
  QString summary;
  if (complete < 3) summary = tr("Pick a point on the bodies, then its match on the target, in turn: three pairs or more (%1 so far).").arg(complete);
  else if (fitted && m_pairDropped >= 0)
    summary = tr("Off by %1 rms over the pairs used; pair %2 is far off the others and was left out.").arg(units::format(units::Kind::Length, m_pairRms)).arg(m_pairDropped + 1);
  else if (fitted) summary = tr("Off by %1 rms over %2 pairs.").arg(units::format(units::Kind::Length, m_pairRms)).arg(complete);
  m_pairSummary->setText(summary);
  m_pairSummary->setVisible(!summary.isEmpty());
  emit contentResized();
}

QString FeaturePanel::pairListText() const {
  QStringList out;
  if (!m_pairRows) return {};
  for (int i = 0; i < m_pairRows->count(); ++i) {
    QWidget* row = m_pairRows->itemAt(i)->widget();
    if (!row) continue;
    QStringList parts;
    for (const QLabel* l : row->findChildren<QLabel*>()) parts << l->text();
    out << parts.join(' ');
  }
  if (m_pairSummary && !m_pairSummary->text().isEmpty()) out << m_pairSummary->text();
  return out.join('\n');
}
