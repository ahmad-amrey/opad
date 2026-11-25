#include "SimulateStart.hpp"

#include <QEnterEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

#include "SimChecks.hpp"
#include "SimPictures.hpp"
#include "Theme.hpp"

namespace {
constexpr int kCardW = 300, kCardH = 262, kPad = 14, kPicPad = 10;

QColor alpha(QColor c, double a) {
  c.setAlphaF(float(a));
  return c;
}
}  // namespace

// ---------------------------------------------------------------- a card
CheckCard::CheckCard(const QString& check, QWidget* parent) : QAbstractButton(parent), m_check(check) {
  setObjectName("checkCard_" + check);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_Hover);
  if (const simchecks::Check* c = simchecks::find(check)) {
    setText(c->question);
    setAccessibleName(c->question);
    setToolTip(c->line);
  }
  setFixedSize(kCardW, kCardH);
}

QSize CheckCard::sizeHint() const { return {kCardW, kCardH}; }
QSize CheckCard::minimumSizeHint() const { return sizeHint(); }

void CheckCard::setReadiness(const QString& chip, int state) {
  m_chip = chip, m_state = state;
  if (const simchecks::Check* c = simchecks::find(m_check)) setAccessibleDescription(simpic::description(m_check) + ". " + c->line + " " + c->time + ". " + chip + ".");
  update();
}

void CheckCard::enterEvent(QEnterEvent* e) {
  m_hover = true;
  update();
  QAbstractButton::enterEvent(e);
}

void CheckCard::leaveEvent(QEvent* e) {
  m_hover = false;
  update();
  QAbstractButton::leaveEvent(e);
}

QRectF CheckCard::chipRect() const {
  // The readiness chip: bottom row, after the time chip (mirrored in a right-to-left layout).
  const QFontMetrics fm(font());
  const simchecks::Check* c = simchecks::find(m_check);
  const double timeW = c ? fm.horizontalAdvance(c->time) + 16 : 0;
  const double w = fm.horizontalAdvance(m_chip) + 16, h = fm.height() + 6;
  QRectF r(kPad + timeW + 6, height() - kPad - h, w, h);
  if (layoutDirection() == Qt::RightToLeft) r.moveLeft(width() - r.right());
  return r;
}

void CheckCard::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton && m_state == 1 && chipRect().contains(e->position())) {  // Needs <solver>: where to get it
    setDown(false);
    emit solverClicked();
    return;
  }
  QAbstractButton::mouseReleaseEvent(e);
}

void CheckCard::paintEvent(QPaintEvent*) {
  const simchecks::Check* c = simchecks::find(m_check);
  if (!c) return;
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setLayoutDirection(layoutDirection());
  const bool hot = m_hover || hasFocus() || isDown();
  const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  // The picture across the top, as the design has it (300 x 104), on the view's ground.
  const double picW = width() - 2 * kPicPad;
  const QRectF pic(kPicPad, kPicPad, picW, picW * simpic::kHeight / simpic::kWidth);
  p.setPen(QPen(hot ? t.sel : t.line, hasFocus() ? 2 : 1));
  p.setBrush(isDown() ? t.bg4 : t.bg3);
  p.drawRoundedRect(box, 10, 10);
  const Qt::Alignment lead = QStyle::visualAlignment(layoutDirection(), Qt::AlignLeft);
  const QColor goal = simchecks::goalColor(c->goal, t);
  QFont small = font();
  small.setBold(true);
  small.setPointSizeF(font().pointSizeF() * 0.85);
  small.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
  const QFontMetrics sm(small);
  p.setPen(Qt::NoPen);
  p.setBrush(t.highContrast ? t.bg : alpha(t.vp, 0.55));
  p.drawRoundedRect(pic, 6, 6);
  simpic::paint(p, pic, c->id, t);
  double y = pic.bottom() + 8;
  p.setFont(small);
  p.setPen(goal);
  p.drawText(QRectF(kPad, y, width() - 2 * kPad, sm.height()), lead | Qt::AlignVCenter, simchecks::goalName(c->goal).toUpper());
  y += sm.height() + 4;
  QFont q = font();
  q.setBold(true);
  q.setPointSizeF(font().pointSizeF() * 1.12);
  p.setFont(q);
  p.setPen(t.fg);
  const QRectF qr = QFontMetrics(q).boundingRect(QRect(kPad, int(y), width() - 2 * kPad, 200), int(lead) | Qt::TextWordWrap, c->question);
  p.drawText(QRectF(kPad, y, width() - 2 * kPad, qr.height()), int(lead) | Qt::TextWordWrap, c->question);
  y += qr.height() + 4;
  p.setFont(font());
  p.setPen(t.fg2);
  const QFontMetrics fm(font());
  const QRectF lineBox(kPad, y, width() - 2 * kPad, height() - kPad - (fm.height() + 6) - 6 - y);
  p.save();
  p.setClipRect(lineBox);
  p.drawText(lineBox, int(lead) | Qt::TextWordWrap, c->line);
  p.restore();
  // The chips: how long, whether it can run here.
  const double h = fm.height() + 6;
  QRectF time(kPad, height() - kPad - h, fm.horizontalAdvance(c->time) + 16, h);
  if (layoutDirection() == Qt::RightToLeft) time.moveLeft(width() - time.right());
  p.setPen(Qt::NoPen);
  p.setBrush(t.bg4);
  p.drawRoundedRect(time, h / 2, h / 2);
  p.setPen(t.fg);
  p.drawText(time, Qt::AlignCenter, c->time);
  if (!m_chip.isEmpty()) {
    const QColor col = m_state == 0 ? t.green : m_state == 1 ? t.amber : t.fg3;
    const QRectF r = chipRect();
    p.setPen(t.highContrast ? QPen(col, 1) : Qt::NoPen);
    p.setBrush(t.highContrast ? QColor(Qt::transparent) : alpha(col, 0.18));
    p.drawRoundedRect(r, h / 2, h / 2);
    p.setPen(col);
    if (m_state == 1) {
      QFont u = font();
      u.setUnderline(true);  // a link: where to get the solver
      p.setFont(u);
    }
    p.drawText(r, Qt::AlignCenter, m_chip);
  }
}

// ---------------------------------------------------------------- the page
SimulateStart::SimulateStart(QWidget* parent) : QWidget(parent) {
  setObjectName("simulateStart");
  auto* outer = new QHBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  // Left: the checks made in this document.
  auto* side = new QWidget(this);
  side->setObjectName("simulateStartSide");
  side->setFixedWidth(232);
  auto* sv = new QVBoxLayout(side);
  sv->setContentsMargins(12, 14, 12, 12);
  m_madeTitle = new QLabel(tr("MY CHECKS"), side);
  m_madeTitle->setObjectName("secondary");
  QFont tf = m_madeTitle->font();
  tf.setBold(true);
  tf.setPointSizeF(tf.pointSizeF() * 0.85);
  m_madeTitle->setFont(tf);
  sv->addWidget(m_madeTitle);
  m_made = new QListWidget(side);
  m_made->setObjectName("simulateMade");
  m_made->setFrameShape(QFrame::NoFrame);
  m_made->setWordWrap(true);
  m_made->setResizeMode(QListView::Adjust);
  m_made->setUniformItemSizes(false);
  m_made->setTextElideMode(Qt::ElideNone);
  sv->addWidget(m_made, 1);
  outer->addWidget(side);
  connect(m_made, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
    emit madeChosen(it->data(Qt::UserRole).toString().toStdString(), it->data(Qt::UserRole + 1).toString());
  });
  connect(m_made, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
    emit madeChosen(it->data(Qt::UserRole).toString().toStdString(), it->data(Qt::UserRole + 1).toString());
  });
  // Centre: the question, the box, the chips, the cards.
  auto* scroll = new QScrollArea(this);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* body = new QWidget(scroll);
  body->setObjectName("simulateStartBody");
  auto* v = new QVBoxLayout(body);
  v->setContentsMargins(32, 28, 32, 24);
  v->setSpacing(14);
  auto* title = new QLabel(tr("What do you want to find out?"), body);
  title->setObjectName("simulateStartTitle");
  title->setStyleSheet(QString("font-size: %1px; font-weight: 600;").arg(theme::px(24)));  // the stylesheet's size wins over a font
  v->addWidget(title);
  auto* sub = new QLabel(tr("Pick the question closest to yours. You can change anything later."), body);
  sub->setObjectName("secondary");
  v->addWidget(sub);
  m_search = new QLineEdit(body);
  m_search->setObjectName("simulateStartSearch");
  m_search->setPlaceholderText(tr("Or describe it: \"air through a pipe that gets narrower\""));
  m_search->setAccessibleName(tr("Describe what you want to check"));
  m_search->setClearButtonEnabled(true);
  m_search->setMinimumHeight(36);
  v->addWidget(m_search);
  connect(m_search, &QLineEdit::textChanged, this, [this] { relayout(); });
  auto* chips = new QHBoxLayout();
  chips->setSpacing(8);
  QStringList ids = {QString()};
  for (simchecks::Goal g : {simchecks::Goal::Air, simchecks::Goal::Heat, simchecks::Goal::Strength, simchecks::Goal::Movement}) ids << simchecks::goalId(g);
  for (const QString& id : ids) {
    simchecks::Goal g;
    auto* b = new QPushButton(id.isEmpty() ? tr("All") : (simchecks::goalOf(id, &g) ? simchecks::goalName(g) : id), body);
    b->setObjectName("simulateChip_" + (id.isEmpty() ? QString("all") : id));
    b->setCheckable(true);
    b->setChecked(id.isEmpty());
    b->setCursor(Qt::PointingHandCursor);
    b->setProperty("goal", id);
    chips->addWidget(b);
    m_chips << b;
    connect(b, &QPushButton::clicked, this, [this, id] { setGoal(id); });
  }
  chips->addStretch(1);
  v->addLayout(chips);
  m_grid = new QWidget(body);
  m_gridLayout = new QGridLayout(m_grid);
  m_gridLayout->setContentsMargins(0, 0, 0, 0);
  m_gridLayout->setSpacing(12);
  for (const auto& c : simchecks::all()) {
    auto* card = new CheckCard(c.id, m_grid);
    m_cards << card;
    connect(card, &QAbstractButton::clicked, this, [this, id = c.id] { emit checkChosen(id); });
    connect(card, &CheckCard::solverClicked, this, [this, id = c.id] {
      const simchecks::Readiness r = simchecks::readiness(*simchecks::find(id), m_engines);
      if (!r.solver.isEmpty()) emit solverRequested(r.solver);
    });
  }
  v->addWidget(m_grid);
  m_none = new QLabel(tr("No check matches those words. Try other words, or open the Advanced tools."), body);
  m_none->setObjectName("secondary");
  m_none->hide();
  v->addWidget(m_none);
  auto* foot = new QHBoxLayout();
  auto* hint = new QLabel(tr("Not listed? Describe it above, or ask your AI assistant to set it up."), body);
  hint->setObjectName("secondary");
  hint->setWordWrap(true);
  foot->addWidget(hint, 1);
  auto* adv = new QPushButton(tr("I know what I want: open the Advanced tools"), body);
  adv->setObjectName("simulateStartAdvanced");
  adv->setFlat(true);
  foot->addWidget(adv);
  connect(adv, &QPushButton::clicked, this, &SimulateStart::advancedRequested);
  v->addLayout(foot);
  v->addStretch(1);
  scroll->setWidget(body);
  outer->addWidget(scroll, 1);
  auto chipStyle = [this] {
    const Tokens& t = theme::current();
    for (QPushButton* b : m_chips)
      b->setStyleSheet(QString("QPushButton { border-radius: %1px; padding: 4px 14px; } QPushButton:checked { border: 1px solid %2; background: %3; color: %4; }")
                           .arg(theme::px(14))
                           .arg(t.sel.name(), t.selbg.name(), t.highContrast ? t.onsel.name() : t.fg.name()));
  };
  chipStyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, [this, chipStyle] {
    chipStyle();
    update();
  });
  setEngines(opad::json::object());
  setMade({});
}

void SimulateStart::paintEvent(QPaintEvent*) {
  QPainter p(this);
  const Tokens& t = theme::current();
  p.fillRect(rect(), t.bg);
  if (auto* side = findChild<QWidget*>("simulateStartSide")) {
    QRect r = side->geometry();
    p.fillRect(r, t.bg2);
    p.setPen(t.line);
    const int x = layoutDirection() == Qt::RightToLeft ? r.left() : r.right();
    p.drawLine(x, r.top(), x, r.bottom());
  }
}

void SimulateStart::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  relayout();
}

void SimulateStart::setEngines(const opad::json& engines) {
  m_engines = engines;
  for (CheckCard* c : m_cards) {
    const simchecks::Readiness r = simchecks::readiness(*simchecks::find(c->check()), engines);
    c->setReadiness(r.chip, int(r.state));
  }
}

void SimulateStart::setMade(const std::vector<Made>& made) {
  m_made->clear();
  for (const auto& m : made) {
    auto* it = new QListWidgetItem(m.name + "\n" + m.result, m_made);
    it->setData(Qt::UserRole, QString::fromStdString(m.study));
    it->setData(Qt::UserRole + 1, m.check);
  }
  if (made.empty()) {
    auto* it = new QListWidgetItem(tr("None yet: a check you start is kept here, with its result."), m_made);
    it->setFlags(Qt::NoItemFlags);
  }
}

void SimulateStart::setQuery(const QString& text) { m_search->setText(text); }

void SimulateStart::setGoal(const QString& goal) {
  m_goal = goal;
  for (QPushButton* b : m_chips) b->setChecked(b->property("goal").toString() == goal);
  relayout();
}

QStringList SimulateStart::shown() const {
  QStringList out;
  for (int i = 0; i < m_gridLayout->count(); ++i)
    if (auto* c = qobject_cast<CheckCard*>(m_gridLayout->itemAt(i)->widget())) out << c->check();
  return out;
}

void SimulateStart::relayout() {
  // The cards that match the words and the goal, best match first (the catalogue's order among equals).
  std::vector<std::pair<int, CheckCard*>> keep;
  const QString q = m_search->text().trimmed();
  for (int i = 0; i < m_cards.size(); ++i) {
    CheckCard* c = m_cards[i];
    const simchecks::Check* chk = simchecks::find(c->check());
    if (!m_goal.isEmpty() && simchecks::goalId(chk->goal) != m_goal) continue;
    const int score = simchecks::match(*chk, q);
    if (score > 0) keep.push_back({score, c});
  }
  std::stable_sort(keep.begin(), keep.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  while (m_gridLayout->count()) m_gridLayout->takeAt(0);
  for (CheckCard* c : m_cards) c->hide();
  // The width the cards have: the page less the side list, the margins and a scroll bar (the grid's own is not laid out yet).
  const int avail = std::max(kCardW, width() - 232 - 64 - 18);
  m_columns = std::max(1, (avail + 12) / (kCardW + 12));
  for (size_t i = 0; i < keep.size(); ++i) {
    m_gridLayout->addWidget(keep[i].second, int(i) / m_columns, int(i) % m_columns, Qt::AlignLeft | Qt::AlignTop);
    keep[i].second->show();
  }
  for (int c = 0; c <= 12; ++c) m_gridLayout->setColumnStretch(c, 0);
  m_gridLayout->setColumnStretch(m_columns, 1);
  m_none->setVisible(keep.empty());
}
