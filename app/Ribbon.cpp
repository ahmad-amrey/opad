#include "Ribbon.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QFrame>
#include <QMenu>
#include <QPainter>
#include <QStyleOptionButton>
#include <QVBoxLayout>

#include "Icons.hpp"
#include "Theme.hpp"

// ---------------------------------------------------------------- SegmentButton
SegmentButton::SegmentButton(QAction* action, const QString& hint, bool primary, QWidget* parent) : QToolButton(parent), m_hint(hint) {
  setObjectName(primary ? "segmentPrimary" : "segment");
  setDefaultAction(action);
  setToolButtonStyle(Qt::ToolButtonTextOnly);
  setCheckable(action->isCheckable());
  setAutoRaise(true);
  setFocusPolicy(Qt::NoFocus);
  setFont(theme::ui(12));
}

QSize SegmentButton::sizeHint() const {
  QFontMetrics fm(theme::ui(12)), mm(theme::mono(11));
  int w = 20 + fm.horizontalAdvance(text());
  if (!m_hint.isEmpty()) w += 6 + mm.horizontalAdvance(m_hint);
  return QSize(w, 26);
}

void SegmentButton::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  bool primary = objectName() == "segmentPrimary";
  if (isChecked()) p.fillRect(rect().adjusted(1, 1, -1, -1), primary ? t.sel : t.bg3);
  else if (underMouse()) p.fillRect(rect().adjusted(1, 1, -1, -1), t.bg3);
  QColor textColor = isChecked() ? (primary ? t.onsel : t.fg) : t.fg2;
  if (!isEnabled()) textColor = t.fg3;
  p.setFont(theme::ui(12));
  p.setPen(textColor);
  QFontMetrics fm(theme::ui(12));
  int x = 10;
  p.drawText(QRect(x, 0, fm.horizontalAdvance(text()) + 2, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
  x += fm.horizontalAdvance(text()) + 6;
  if (!m_hint.isEmpty()) {
    p.setFont(theme::mono(11));
    p.setPen(isChecked() && primary ? t.onsel : t.fg3);
    p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignVCenter | Qt::AlignLeft, m_hint);
  }
}

// ---------------------------------------------------------------- SearchField
SearchField::SearchField(QWidget* parent) : QAbstractButton(parent) {
  setFixedSize(200, 28);
  setCursor(Qt::PointingHandCursor);
  setToolTip(tr("Search commands (S)"));
}

void SearchField::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(underMouse() ? t.fg3 : t.line, 1));
  p.setBrush(t.bg);
  p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 3, 3);
  p.drawPixmap(8, 6, icons::pixmap("search", t.fg3, 16, devicePixelRatioF()));
  p.setFont(theme::ui(12));
  p.setPen(t.fg3);
  p.drawText(QRect(30, 0, 130, height()), Qt::AlignVCenter | Qt::AlignLeft, tr("Search commands"));
  QRect key(width() - 26, 6, 18, 16);
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg4);
  p.drawRoundedRect(key, 3, 3);
  p.setFont(theme::mono(11));
  p.setPen(t.fg2);
  p.drawText(key, Qt::AlignCenter, "S");
}

// ---------------------------------------------------------------- RibbonBar
RibbonBar::RibbonBar(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  m_tabs = new QTabBar(this);
  m_tabs->setObjectName("ribbonTabs");
  m_tabs->setDrawBase(false);
  m_tabs->setExpanding(false);
  m_tabs->setFixedHeight(28);
  m_tabs->setFocusPolicy(Qt::NoFocus);
  auto* tabRow = new QWidget(this);
  auto* tabLayout = new QHBoxLayout(tabRow);
  tabLayout->setContentsMargins(8, 0, 8, 0);
  tabLayout->addWidget(m_tabs);
  tabLayout->addStretch();
  layout->addWidget(tabRow);

  m_strip = new QWidget(this);
  m_strip->setObjectName("ribbonStrip");
  m_strip->setFixedHeight(64);
  m_stripLayout = new QHBoxLayout(m_strip);
  m_stripLayout->setContentsMargins(8, 0, 8, 0);
  m_stripLayout->setSpacing(4);
  m_stack = new QStackedWidget(m_strip);
  m_stripLayout->addWidget(m_stack, 1);
  m_right = new QHBoxLayout();
  m_right->setContentsMargins(0, 0, 0, 0);
  m_right->setSpacing(8);
  m_stripLayout->addLayout(m_right);
  layout->addWidget(m_strip);
  connect(m_tabs, &QTabBar::currentChanged, m_stack, &QStackedWidget::setCurrentIndex);
}

int RibbonBar::addTab(const QString& title, const QList<QList<QAction*>>& groups) {
  auto* page = new QWidget(m_stack);
  auto* row = new QHBoxLayout(page);
  row->setContentsMargins(0, 4, 0, 4);
  row->setSpacing(4);
  bool first = true;
  for (const auto& group : groups) {
    if (!first) {
      auto* sep = new QFrame(page);
      sep->setObjectName("ribbonSep");
      sep->setFrameShape(QFrame::NoFrame);
      row->addWidget(sep);
    }
    first = false;
    for (QAction* a : group) {
      auto* b = new QToolButton(page);
      b->setObjectName("ribbonTool");
      b->setDefaultAction(a);
      b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
      b->setIconSize(QSize(24, 24));
      b->setFixedHeight(56);
      b->setAutoRaise(true);
      b->setFocusPolicy(Qt::NoFocus);
      b->setFont(theme::ui(11));
      row->addWidget(b);
    }
  }
  row->addStretch();
  m_stack->addWidget(page);
  return m_tabs->addTab(title);
}

void RibbonBar::setSelectFilters(const QList<QAction*>& filters, const QStringList& hints) {
  auto* label = new QLabel(tr("Select"), m_strip);
  label->setObjectName("ribbonLabel");
  m_right->addWidget(label);
  auto* seg = new QWidget(m_strip);
  seg->setObjectName("segmented");
  seg->setFixedHeight(28);
  auto* l = new QHBoxLayout(seg);
  l->setContentsMargins(1, 1, 1, 1);
  l->setSpacing(0);
  for (int i = 0; i < filters.size(); ++i) l->addWidget(new SegmentButton(filters[i], i < hints.size() ? hints[i] : QString(), false, seg));
  m_right->addWidget(seg);
}

void RibbonBar::setSearchAction(QAction* a) {
  auto* field = new SearchField(m_strip);
  connect(field, &QAbstractButton::clicked, a, &QAction::trigger);
  m_right->addWidget(field);
}

void RibbonBar::setSettingsMenu(QAction* a, QMenu* menu) {
  auto* b = new QToolButton(m_strip);
  b->setObjectName("ribbonSettings");
  b->setDefaultAction(a);
  b->setToolButtonStyle(Qt::ToolButtonIconOnly);
  b->setIconSize(QSize(20, 20));
  b->setAutoRaise(true);
  b->setFixedSize(28, 28);
  b->setFocusPolicy(Qt::NoFocus);
  b->setPopupMode(QToolButton::InstantPopup);
  b->setMenu(menu);
  m_right->addWidget(b);
}

void RibbonBar::setSettingsAction(QAction* a) {
  auto* b = new QToolButton(m_strip);
  b->setDefaultAction(a);
  b->setToolButtonStyle(Qt::ToolButtonIconOnly);
  b->setIconSize(QSize(20, 20));
  b->setAutoRaise(true);
  b->setFixedSize(28, 28);
  b->setFocusPolicy(Qt::NoFocus);
  m_right->addWidget(b);
}
