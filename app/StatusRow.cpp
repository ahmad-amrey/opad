#include "StatusRow.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOption>
#include <QToolTip>
#include <algorithm>

#include "Theme.hpp"

void StatusBar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  QStyleOption opt;
  opt.initFrom(this);
  style()->drawPrimitive(QStyle::PE_PanelStatusBar, &opt, &p, this);
}

void ElidedLabel::paintEvent(QPaintEvent*) {
  QPainter p(this);
  const QRect r = contentsRect();
  p.setPen(palette().color(foregroundRole()));
  p.drawText(r, Qt::AlignVCenter | Qt::AlignLeading, fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
}

// ---------------------------------------------------------------- PathChip
PathChip::PathChip(QWidget* parent) : QWidget(parent) {
  setObjectName("pathChip");
  setFont(theme::mono(12));
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  setAttribute(Qt::WA_Hover);
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void PathChip::setText(const QString& text) {
  if (text == m_text) return;
  m_text = text;
  setAccessibleName(text);
  updateGeometry();
  update();
}

void PathChip::setFile(const QString& file) {
  m_file = file;
  setCursor(file.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
}

QSize PathChip::sizeHint() const {
  return QSize(std::min(fontMetrics().horizontalAdvance(m_text) + kMargin + 6, kMaxWidth), fontMetrics().height() + 6);
}

QSize PathChip::minimumSizeHint() const {
  const QSize s = sizeHint();
  return QSize(std::min(s.width(), kMinWidth), s.height());
}

bool PathChip::elided() const { return fontMetrics().horizontalAdvance(m_text) > width() - kMargin - 6; }

QMenu* PathChip::menu(QWidget* parent) {
  auto* m = new QMenu(parent);
  m->setObjectName("pathMenu");
  if (!m_file.isEmpty()) emit menuRequested(m);
  return m;
}

bool PathChip::event(QEvent* e) {
  if (e->type() == QEvent::ToolTip) {
    QString tip = toolTip();
    if (elided()) tip = tip.isEmpty() ? m_text : m_text + '\n' + tip;
    if (!m_file.isEmpty()) tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Click for Open file location and Copy path.");
    if (tip.isEmpty()) QToolTip::hideText();
    else QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), tip, this);
    return true;
  }
  return QWidget::event(e);
}

void PathChip::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  const bool active = m_hover && !m_file.isEmpty();
  if (active) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(t.bg3);
    p.drawRoundedRect(QRectF(rect()).adjusted(kMargin / 2.0, 2, -1, -2), 3, 3);
  }
  // A path reads left to right in every language; only its place follows the layout (the leading end).
  const QRect r = rect().adjusted(isRightToLeft() ? 6 : kMargin, 0, isRightToLeft() ? -kMargin : -6, 0);
  p.setPen(active ? t.fg : t.fg2);
  p.drawText(r, Qt::AlignVCenter | (isRightToLeft() ? Qt::AlignRight : Qt::AlignLeft), fontMetrics().elidedText(m_text, Qt::ElideMiddle, r.width()));
}

void PathChip::mousePressEvent(QMouseEvent* e) {
  if (m_file.isEmpty() || (e->button() != Qt::LeftButton && e->button() != Qt::RightButton)) return QWidget::mousePressEvent(e);
  QMenu* m = menu(this);
  m->setAttribute(Qt::WA_DeleteOnClose);
  if (m->isEmpty()) return m->deleteLater();
  m->popup(mapToGlobal(QPoint(isRightToLeft() ? width() - m->sizeHint().width() : 0, -m->sizeHint().height())));
}

void PathChip::enterEvent(QEnterEvent*) {
  m_hover = true;
  update();
}

void PathChip::leaveEvent(QEvent*) {
  m_hover = false;
  update();
}

// ---------------------------------------------------------------- StatusRow
StatusRow::StatusRow(QWidget* parent) : QWidget(parent) {
  setObjectName("statusRow");
  m_row = new QHBoxLayout(this);
  m_row->setContentsMargins(0, 0, 0, 0);
  m_row->setSpacing(4);
  m_path = new PathChip(this);
  m_row->addWidget(m_path);
}

void StatusRow::addChip(QWidget* chip) {
  chip->setParent(this);
  chip->setSizePolicy(QSizePolicy::Fixed, chip->sizePolicy().verticalPolicy());
  m_row->addWidget(chip);
}
