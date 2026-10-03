#include "ProgressStrip.hpp"

#include <QHBoxLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>

#include <algorithm>

#include "Theme.hpp"

// ---------------------------------------------------------------- ProgressStrip
static QProgressBar* makeThinBar(QWidget* parent) {
  auto* bar = new QProgressBar(parent);
  bar->setTextVisible(false);
  bar->setFixedSize(110, 6);
  bar->setRange(0, 100);
  bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  return bar;
}

ProgressStrip::ProgressStrip(QWidget* parent) : QWidget(parent) {
  setObjectName("progressStrip");
  setFixedHeight(20);  // must fit inside the 24 px status bar (1 px border + item margins)
  auto* l = new QHBoxLayout(this);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(6);
  m_title = new QLabel(this);
  m_title->setObjectName("progressTitle");
  // The title takes every spare pixel (its width comes from the layout, not the text, so the bars never
  // shift as the phase changes); text longer than that is elided in the middle.
  m_title->setMinimumWidth(240);
  m_title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  m_others = new QLabel(this);
  m_others->setObjectName("progressOthers");
  m_others->setFont(theme::mono(11));
  m_others->hide();
  m_phaseBar = makeThinBar(this);
  m_phasePct = new QLabel(this);
  m_phasePct->setObjectName("tertiary");
  m_phasePct->setFont(theme::mono(11));
  m_phasePct->setFixedWidth(30);
  m_overallLabel = new QLabel(tr("Overall"), this);
  m_overallLabel->setObjectName("tertiary");
  m_overallBar = makeThinBar(this);
  m_overallPct = new QLabel(this);
  m_overallPct->setObjectName("tertiary");
  m_overallPct->setFont(theme::mono(11));
  m_overallPct->setFixedWidth(30);
  m_cancel = new QPushButton(tr("Cancel"), this);
  m_cancel->setObjectName("progressCancel");
  m_cancel->setFixedHeight(18);
  m_cancel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  m_cancel->setFocusPolicy(Qt::NoFocus);
  m_cancel->setCursor(Qt::PointingHandCursor);
  l->addWidget(m_title, 1);
  l->addWidget(m_others);
  l->addWidget(m_phaseBar);
  l->addWidget(m_phasePct);
  l->addSpacing(16);
  l->addWidget(m_overallLabel);
  l->addWidget(m_overallBar);
  l->addWidget(m_overallPct);
  l->addSpacing(16);
  l->addWidget(m_cancel);
  l->addSpacing(8);
  connect(m_cancel, &QPushButton::clicked, this, [this] {
    m_cancel->setEnabled(false);
    m_cancel->setText(tr("Cancelling\u2026"));
    emit cancelRequested();
  });
  hide();
}

void ProgressStrip::setTitle(const QString& text) {
  m_fullTitle = text;
  m_title->setText(m_title->fontMetrics().elidedText(text, Qt::ElideMiddle, m_title->width()));
  m_title->setToolTip(text);
}

void ProgressStrip::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  if (!m_fullTitle.isEmpty()) setTitle(m_fullTitle);  // re-elide for the new width
}

void ProgressStrip::begin(const QString& title, bool twoBars) {
  setTitle(title);
  m_cancel->setEnabled(true);
  m_cancel->setText(tr("Cancel"));
  m_phaseBar->setRange(0, 100);
  m_phaseBar->setValue(0);
  m_phasePct->clear();
  m_overallLabel->setVisible(twoBars);
  m_overallBar->setVisible(twoBars);
  m_overallPct->setVisible(twoBars);
  m_overallBar->setValue(0);
  m_overallPct->clear();
  show();
}

void ProgressStrip::setPhase(const QString& text, int percent) {
  setTitle(text);
  if (percent < 0) {
    m_phaseBar->setRange(0, 0);  // indeterminate (marching)
    m_phasePct->clear();
  } else {
    m_phaseBar->setRange(0, 100);
    m_phaseBar->setValue(std::clamp(percent, 0, 100));
    m_phasePct->setText(QString::number(std::clamp(percent, 0, 100)) + "%");
  }
}

void ProgressStrip::setOverall(int percent) {
  m_overallBar->setRange(0, 100);
  m_overallBar->setValue(std::clamp(percent, 0, 100));
  m_overallPct->setText(QString::number(std::clamp(percent, 0, 100)) + "%");
}

void ProgressStrip::setOthers(const QStringList& titles) {
  m_others->setVisible(!titles.isEmpty());
  m_others->setText(titles.isEmpty() ? QString() : QStringLiteral("+%1").arg(titles.size()));
  QStringList lines{tr("Also running:")};
  for (const QString& t : titles) lines << QString::fromUtf8("· ") + t;
  m_others->setToolTip(titles.isEmpty() ? QString() : lines.join(QChar('\n')));
  m_others->setAccessibleName(titles.isEmpty() ? QString() : tr("%1 more jobs running").arg(titles.size()));
}

QString ProgressStrip::othersText() const { return m_others->isVisibleTo(this) ? m_others->text() : QString(); }

void ProgressStrip::finish() {
  setOthers({});
  hide();
}
