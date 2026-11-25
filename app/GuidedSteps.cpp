#include "GuidedSteps.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "Theme.hpp"

#include <algorithm>

GuidedSteps::GuidedSteps(const QString& prefix, QWidget* parent) : QWidget(parent) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto* head = new QVBoxLayout();
  head->setContentsMargins(12, 8, 12, 4);
  head->setSpacing(2);
  m_goal = new QLabel(this);
  m_goal->setObjectName(prefix + "Goal");
  QFont f = m_goal->font();
  f.setBold(true);
  f.setPointSizeF(f.pointSizeF() * 0.85);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
  m_goal->setFont(f);
  m_title = new QLabel(this);
  m_title->setObjectName(prefix + "Title");
  m_title->setWordWrap(true);
  m_title->setStyleSheet(QString("font-size: %1px; font-weight: 600;").arg(theme::px(16)));  // the stylesheet's size wins over a font
  head->addWidget(m_goal);
  head->addWidget(m_title);
  outer->addLayout(head);
  m_goal->hide();
  m_title->hide();
  m_tabs = new QTabBar(this);
  m_tabs->setObjectName(prefix + "Steps");
  m_tabs->setExpanding(true);
  m_tabs->setDrawBase(false);
  m_tabs->setUsesScrollButtons(false);
  m_tabs->setElideMode(Qt::ElideRight);
  outer->addWidget(m_tabs);
  m_pages = new QStackedWidget(this);
  outer->addWidget(m_pages, 1);
  m_footer = new QHBoxLayout();
  m_footer->setContentsMargins(12, 6, 12, 10);
  m_stepText = new QLabel(this);
  m_stepText->setObjectName(prefix + "StepText");
  m_stepText->setProperty("role", "secondary");
  m_footer->addWidget(m_stepText);
  m_footer->addStretch(1);
  m_back = new QPushButton(tr("Back"), this);
  m_back->setObjectName(prefix + "Back");
  m_next = new QPushButton(tr("Next"), this);
  m_next->setObjectName(prefix + "Next");
  m_next->setDefault(true);
  m_footer->addWidget(m_back);
  m_footer->addWidget(m_next);
  outer->addLayout(m_footer);
  connect(m_tabs, &QTabBar::currentChanged, this, &GuidedSteps::show);
  connect(m_back, &QPushButton::clicked, this, [this] { setCurrent(std::max(0, current() - 1)); });
  connect(m_next, &QPushButton::clicked, this, [this] {
    if (current() >= count() - 1) emit nextAtEnd();
    else setCurrent(current() + 1);
  });
}

void GuidedSteps::setHeader(const QString& goal, const QColor& color, const QString& title) {
  m_goal->setText(goal.toUpper());
  m_goal->setStyleSheet(color.isValid() ? QString("color: %1;").arg(color.name()) : QString());
  m_goal->setVisible(!goal.isEmpty());
  m_title->setText(title);
  m_title->setVisible(!title.isEmpty());
}

QString GuidedSteps::title() const { return m_title->text(); }

QVBoxLayout* GuidedSteps::addPage(const QString& tab, const QString& intro) {
  // Each page scrolls by itself (as tall as it is, never as the tallest), as wide as the panel.
  auto* scroll = new QScrollArea(m_pages);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* w = new QWidget(scroll);
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(12, 10, 12, 8);
  v->setSpacing(10);
  auto* text = new QLabel(intro, w);
  text->setWordWrap(true);
  text->setObjectName("secondary");
  text->setVisible(!intro.isEmpty());
  v->addWidget(text);
  auto* content = new QVBoxLayout();  // the caller's: the stretch stays under it whatever it adds later
  content->setContentsMargins(0, 0, 0, 0);
  content->setSpacing(10);
  v->addLayout(content);
  v->addStretch(1);
  scroll->setWidget(w);
  m_pages->addWidget(scroll);
  m_names.push_back(tab);
  m_nextTexts.emplace_back();
  m_done.push_back(false);
  m_intros.push_back(text);
  {
    const QSignalBlocker quiet(m_tabs);  // the first tab is current without a page shown to the caller (it is building)
    m_tabs->addTab(tab);
  }
  m_pages->setCurrentIndex(std::max(0, current()));
  nav(current());
  return content;
}

QWidget* GuidedSteps::page(int i) const {
  auto* s = qobject_cast<QScrollArea*>(m_pages->widget(i));
  return s ? s->widget() : nullptr;
}

void GuidedSteps::setIntro(int i, const QString& text) {
  if (i < 0 || size_t(i) >= m_intros.size()) return;
  m_intros[size_t(i)]->setText(text);
  m_intros[size_t(i)]->setVisible(!text.isEmpty());
}

int GuidedSteps::count() const { return m_tabs->count(); }
int GuidedSteps::current() const { return m_tabs->currentIndex(); }

void GuidedSteps::setCurrent(int i) {
  if (count() == 0) return;
  i = std::clamp(i, 0, count() - 1);
  if (i == current()) show(i);
  else m_tabs->setCurrentIndex(i);  // show() through currentChanged
}

void GuidedSteps::setDone(int i, bool done) {
  if (i < 0 || size_t(i) >= m_done.size() || m_done[size_t(i)] == done) return;
  m_done[size_t(i)] = done;
  m_tabs->setTabText(i, (done ? QStringLiteral("✓ ") : QString()) + m_names[size_t(i)]);
}

bool GuidedSteps::done(int i) const { return i >= 0 && size_t(i) < m_done.size() && m_done[size_t(i)]; }

void GuidedSteps::setNextText(int i, const QString& text) {
  if (i < 0 || size_t(i) >= m_nextTexts.size()) return;
  m_nextTexts[size_t(i)] = text;
  if (i == current()) nav(i);
}

void GuidedSteps::setLastHasNext(bool on) {
  m_lastHasNext = on;
  nav(current());
}

void GuidedSteps::setNextEnabled(bool on) { m_next->setEnabled(on); }

void GuidedSteps::show(int i) {
  if (i < 0 || i >= m_pages->count()) return;
  m_pages->setCurrentIndex(i);
  nav(i);
  emit stepShown(i);
}

void GuidedSteps::nav(int i) {
  if (i < 0) return;
  m_back->setEnabled(i > 0);
  m_next->setVisible(i < count() - 1 || m_lastHasNext);
  const QString text = size_t(i) < m_nextTexts.size() ? m_nextTexts[size_t(i)] : QString();
  m_next->setText(text.isEmpty() ? tr("Next") : text);
  m_next->setMinimumWidth(m_next->fontMetrics().horizontalAdvance(m_next->text()) + 28);  // "Next: how much air" whole
  m_stepText->setText(tr("Step %1 of %2").arg(i + 1).arg(count()));
}

Debounce::Debounce(int ms, std::function<void()> fn, QObject* parent) : QObject(parent), m_fn(std::move(fn)) {
  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(ms);
  connect(m_timer, &QTimer::timeout, this, [this] {
    if (m_fn) m_fn();
  });
}

void Debounce::poke() { m_timer->start(); }

void Debounce::flush() {
  if (!m_timer->isActive()) return;
  m_timer->stop();
  if (m_fn) m_fn();
}

void Debounce::cancel() { m_timer->stop(); }
bool Debounce::pending() const { return m_timer->isActive(); }
