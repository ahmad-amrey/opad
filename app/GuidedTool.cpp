#include "GuidedTool.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>

#include "Icons.hpp"
#include "Theme.hpp"

QPixmap stepRing(StepState state, int number, qreal dpr) {
  const Tokens& t = theme::current();
  QPixmap pm(QSize(16, 16) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor ring = state == StepState::Pending ? t.fg3 : t.sel;
  p.setPen(QPen(ring, 1.5));
  p.setBrush(state == StepState::Done ? QBrush(t.sel) : QBrush(Qt::NoBrush));
  p.drawEllipse(QRectF(1, 1, 14, 14));
  if (state == StepState::Done) {
    p.setPen(QPen(t.onsel, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF({QPointF(4.6, 8.2), QPointF(7.1, 10.6), QPointF(11.4, 5.6)}));
  } else {
    p.setFont(theme::mono(10));
    p.setPen(ring);
    p.drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, QString::number(number));
  }
  return pm;
}

// ---------------------------------------------------------------- PromptBar
PromptBar::PromptBar(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setFixedHeight(32);
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void PromptBar::set(const QString& icon, const QString& title, const QList<ToolStep>& steps, const QString& hints) {
  m_icon = icon;
  m_title = title;
  m_steps = steps;
  m_hints = hints;
  resize(sizeHint());
  update();
}

QList<PromptBar::Piece> PromptBar::pieces() const {
  QFontMetrics title(theme::ui(13, QFont::Medium)), text(theme::ui(13)), hint(theme::mono(11));
  QList<Piece> out;
  out << Piece{Piece::Icon, {}, {}, 0, 16} << Piece{Piece::Title, m_title, {}, 0, title.horizontalAdvance(m_title)} << Piece{Piece::Rule, {}, {}, 0, 1};
  bool waitingSeen = false;
  for (int i = 0; i < m_steps.size(); ++i) {
    const ToolStep& s = m_steps[i];
    StepState state = !s.picked.isEmpty() ? StepState::Done : waitingSeen ? StepState::Pending : StepState::Waiting;
    if (state == StepState::Waiting) waitingSeen = true;
    if (i > 0) out << Piece{Piece::Arrow, QString::fromUtf8("›"), {}, 0, text.horizontalAdvance(QString::fromUtf8("›"))};
    const QString label = s.picked.isEmpty() ? s.label : s.picked;
    out << Piece{Piece::Ring, {}, state, i + 1, 16} << Piece{Piece::Text, label, state, 0, text.horizontalAdvance(label)};
  }
  out << Piece{Piece::Rule, {}, {}, 0, 1} << Piece{Piece::Hint, m_hints, {}, 0, hint.horizontalAdvance(m_hints)};
  return out;
}

QSize PromptBar::sizeHint() const {
  int w = 10;
  for (const Piece& p : pieces()) w += p.width + 8;
  return QSize(w + 2, 32);
}

void PromptBar::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setLayoutDirection(Qt::LeftToRight);  // absolute positions, mirrored by hand
  p.fillRect(rect(), t.vp);               // an overlay on the native viewport cannot be translucent
  p.setPen(QPen(t.sel, 1));
  p.setBrush(t.bg2);
  p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 3, 3);
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  QFontMetrics fm(theme::ui(13));
  const int baseline = (height() + fm.ascent() - fm.descent()) / 2;
  int x = 10;
  for (const Piece& piece : pieces()) {
    const int at = rtl ? width() - x - piece.width : x;
    switch (piece.kind) {
      case Piece::Icon: p.drawPixmap(at, 8, icons::pixmap(m_icon, t.sel, 16, devicePixelRatioF())); break;
      case Piece::Rule: p.setPen(QPen(t.line, 1)); p.drawLine(at, 8, at, height() - 8); break;
      case Piece::Ring: p.drawPixmap(at, 8, stepRing(piece.state, piece.number, devicePixelRatioF())); break;
      case Piece::Title:
        p.setFont(theme::ui(13, QFont::Medium));
        p.setPen(t.fg);
        p.drawText(at, baseline, piece.text);
        break;
      case Piece::Text:
        p.setFont(theme::ui(13));
        p.setPen(piece.state == StepState::Waiting ? t.fg : piece.state == StepState::Done ? t.fg2 : t.fg3);
        p.drawText(at, baseline, piece.text);
        break;
      case Piece::Arrow:
        p.setFont(theme::ui(13));
        p.setPen(t.fg3);
        p.drawText(at, baseline, rtl ? QString::fromUtf8("‹") : piece.text);
        break;
      case Piece::Hint:
        p.setFont(theme::mono(11));
        p.setPen(t.fg3);
        p.drawText(at, baseline, piece.text);
        break;
    }
    x += piece.width + 8;
  }
}

// ---------------------------------------------------------------- ToolStepsPanel
ToolStepsPanel::ToolStepsPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 8, 0, 0);
  layout->setSpacing(0);
  auto* steps = new QWidget(this);
  m_stepRows = new QVBoxLayout(steps);
  m_stepRows->setContentsMargins(8, 0, 8, 8);
  m_stepRows->setSpacing(2);
  layout->addWidget(steps);
  auto* rule = new QFrame(this);
  rule->setFixedHeight(1);
  rule->setObjectName("toolRule");
  layout->addWidget(rule);

  auto* summary = new QWidget(this);
  auto* sl = new QVBoxLayout(summary);
  sl->setContentsMargins(12, 8, 12, 8);
  sl->setSpacing(2);
  auto* head = new QHBoxLayout();
  m_title = new QLabel(summary);
  m_title->setObjectName("panelTitle");
  m_state = new QLabel(summary);
  m_state->setObjectName("tertiary");
  m_state->setFont(theme::mono(11));
  head->addWidget(m_title, 1);
  head->addWidget(m_state);
  sl->addLayout(head);
  m_subtitle = new QLabel(summary);
  m_subtitle->setObjectName("secondary");
  m_subtitle->setWordWrap(true);
  sl->addWidget(m_subtitle);
  layout->addWidget(summary);

  m_grid = new QTreeWidget(this);
  m_grid->setColumnCount(2);
  m_grid->setHeaderHidden(true);
  m_grid->header()->setSectionResizeMode(0, QHeaderView::Fixed);
  m_grid->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_grid->setColumnWidth(0, 128);
  m_grid->setIndentation(0);
  m_grid->setRootIsDecorated(false);
  m_grid->setSelectionMode(QAbstractItemView::NoSelection);
  m_grid->setFocusPolicy(Qt::NoFocus);
  auto* gridRow = new QHBoxLayout();  // the grid lines stop 12 px short of the panel's edges, like the summary text
  gridRow->setContentsMargins(12, 0, 12, 0);
  gridRow->addWidget(m_grid);
  layout->addLayout(gridRow, 1);

  m_footer = new QWidget(this);
  m_footer->setFixedHeight(40);
  m_footer->setObjectName("toolFooter");
  auto* fl = new QHBoxLayout(m_footer);
  fl->setContentsMargins(12, 0, 12, 0);
  fl->setSpacing(8);
  auto* hint = new QLabel(tr("Writes one measurement op"), m_footer);
  hint->setObjectName("tertiary");
  hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // single line, clipped
  auto* clear = new QPushButton(tr("Clear   Esc"), m_footer);
  m_pin = new QPushButton(tr("Pin to document   P"), m_footer);
  m_pin->setObjectName("primary");
  for (QPushButton* b : {clear, m_pin}) b->setFocusPolicy(Qt::NoFocus);  // Esc / P / Enter stay with the main window
  fl->addWidget(hint, 1);
  fl->addWidget(clear);
  fl->addWidget(m_pin);
  layout->addWidget(m_footer);
  m_footer->hide();
  connect(clear, &QPushButton::clicked, this, &ToolStepsPanel::clearRequested);
  connect(m_pin, &QPushButton::clicked, this, &ToolStepsPanel::pinRequested);

  auto restyle = [this, rule] {
    const Tokens& t = theme::current();
    rule->setStyleSheet(QString("background: %1;").arg(theme::css(t.line)));
    m_grid->setStyleSheet(QString("QTreeWidget::item { border-bottom: 1px solid %1; }").arg(theme::css(t.line)));
    m_footer->setStyleSheet(QString("QWidget#toolFooter { border-top: 1px solid %1; }").arg(theme::css(t.line)));
  };
  restyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, restyle);
}

void ToolStepsPanel::setSteps(const QList<ToolStep>& steps, const QString& hover) {
  const Tokens& t = theme::current();
  while (QLayoutItem* it = m_stepRows->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  bool waitingSeen = false;
  for (int i = 0; i < steps.size(); ++i) {
    const ToolStep& s = steps[i];
    StepState state = !s.picked.isEmpty() ? StepState::Done : waitingSeen ? StepState::Pending : StepState::Waiting;
    if (state == StepState::Waiting) waitingSeen = true;
    auto* row = new QFrame(this);
    row->setObjectName("stepRow");
    row->setFixedHeight(28);
    row->setStyleSheet(state == StepState::Waiting
                           ? QString("QFrame#stepRow { background: %1; border: 1px solid %2; border-radius: 3px; } QLabel { background: transparent; }").arg(theme::css(t.selbg), theme::css(t.sel))
                           : QString("QFrame#stepRow { border: 1px solid transparent; } QLabel { background: transparent; }"));
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(6, 0, 8, 0);
    l->setSpacing(8);
    auto* ring = new QLabel(row);
    ring->setPixmap(stepRing(state, i + 1, devicePixelRatioF()));
    auto* label = new QLabel(s.label, row);
    label->setStyleSheet(QString("color: %1;").arg(theme::css(state == StepState::Pending ? t.fg3 : t.fg)));
    auto* target = new QLabel(state == StepState::Done ? s.picked : state == StepState::Waiting && !hover.isEmpty() ? tr("hover: %1").arg(hover) : QString(), row);
    target->setFont(theme::mono(11));
    target->setStyleSheet(QString("color: %1;").arg(theme::css(state == StepState::Waiting ? t.sel : t.fg2)));
    target->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    target->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    l->addWidget(ring);
    l->addWidget(label);
    l->addWidget(target, 1);
    m_stepRows->addWidget(row);
  }
}

void ToolStepsPanel::setSummary(const QString& title, const QString& subtitle, const QString& state) {
  m_title->setText(title);
  m_subtitle->setText(subtitle);
  m_state->setText(state);
}

void ToolStepsPanel::setResult(const QList<QPair<QString, QString>>& rows) {
  const Tokens& t = theme::current();
  m_grid->clear();
  for (const auto& [key, value] : rows) {
    auto* row = new QTreeWidgetItem(m_grid);
    row->setText(0, key);
    row->setForeground(0, t.fg2);
    row->setText(1, QChar(0x202A) + value + QChar(0x202C));  // values keep their order in a right-to-left UI
    row->setFont(1, theme::mono(12));
  }
}

void ToolStepsPanel::setFooter(bool visible, bool canPin) {
  m_footer->setVisible(visible);
  m_pin->setEnabled(canPin);
}
