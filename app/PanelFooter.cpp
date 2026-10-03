#include "PanelFooter.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>

#include "Theme.hpp"

namespace {
// A label and its key in one button, styled by the theme (QPushButton QLabel[footerRole]). The labels sit in a layout,
// which the push button's own size hint knows nothing about.
class FooterButton : public QPushButton {
 public:
  using QPushButton::QPushButton;
  QSize sizeHint() const override { return layout() ? layout()->sizeHint().expandedTo(QPushButton::sizeHint()) : QPushButton::sizeHint(); }
  QSize minimumSizeHint() const override { return sizeHint(); }
};
}  // namespace

PanelFooter::PanelFooter(QWidget* parent) : QWidget(parent) {
  setObjectName("panelFooter");
  setFixedHeight(44);
  m_row = new QHBoxLayout(this);
  m_row->setContentsMargins(12, 1, 12, 0);
  m_row->setSpacing(8);
  m_row->addStretch(1);
  m_cancel = button(tr("Cancel"), QStringLiteral("Esc"));
  m_primary = button(tr("OK"), QStringLiteral("Enter"));
  m_primary->setObjectName("primary");  // the accent (QPushButton#primary)
  m_row->addWidget(m_cancel);
  m_row->addWidget(m_primary);
  connect(m_cancel, &QPushButton::clicked, this, &PanelFooter::cancelled);
  connect(m_primary, &QPushButton::clicked, this, &PanelFooter::accepted);
}

QPushButton* PanelFooter::button(const QString& text, const QString& key) {
  auto* b = new FooterButton(this);
  b->setAutoDefault(false);
  auto* row = new QHBoxLayout(b);
  row->setContentsMargins(12, 0, 10, 0);
  row->setSpacing(8);
  auto* label = new QLabel(b);
  label->setObjectName("footerText");
  auto* hint = new QLabel(b);
  hint->setProperty("footerRole", "key");
  for (QLabel* l : {label, hint}) {
    l->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(l);
  }
  relabel(b, text, key);
  return b;
}

void PanelFooter::relabel(QPushButton* b, const QString& text, const QString& key) {
  b->findChild<QLabel*>("footerText")->setText(text);
  for (QLabel* l : b->findChildren<QLabel*>())
    if (l->property("footerRole").toString() == "key") {
      l->setText(key);
      l->setVisible(!key.isEmpty());
    }
  b->setAccessibleName(text);
  b->setToolTip(key.isEmpty() ? text : text + "  (" + key + ")");
  b->updateGeometry();
}

QPushButton* PanelFooter::addSecondary(const QString& text, const QString& key) {
  QPushButton* b = button(text, key);
  m_row->insertWidget(m_secondaries++, b);
  return b;
}

void PanelFooter::setPrimary(Primary kind, const QString& key) { relabel(m_primary, kind == Primary::Close ? tr("OK") : tr("Apply"), key); }
void PanelFooter::setPrimary(const QString& verb, const QString& key) { relabel(m_primary, verb, key); }
void PanelFooter::setCancel(const QString& text, const QString& key) { relabel(m_cancel, text, key); }
void PanelFooter::setPrimaryVisible(bool on) { m_primary->setVisible(on); }
void PanelFooter::setCancelVisible(bool on) { m_cancel->setVisible(on); }
void PanelFooter::setPrimaryEnabled(bool on) { m_primary->setEnabled(on); }

void PanelFooter::setKeysStayWithWindow(bool on) {
  for (QPushButton* b : {m_cancel, m_primary}) b->setFocusPolicy(on ? Qt::NoFocus : Qt::StrongFocus);
}

QString PanelFooter::text(QPushButton* b) {
  const QLabel* l = b ? b->findChild<QLabel*>("footerText") : nullptr;
  return l ? l->text() : QString();
}

QString PanelFooter::key(QPushButton* b) {
  if (b)
    for (const QLabel* l : b->findChildren<QLabel*>())
      if (l->property("footerRole").toString() == "key") return l->text();
  return QString();
}

QString PanelFooter::primaryText() const { return text(m_primary); }
QString PanelFooter::cancelText() const { return text(m_cancel); }

void PanelFooter::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setPen(QPen(theme::current().line, 1));
  p.drawLine(0, 0, width(), 0);
}
