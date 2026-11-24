#include "AgentActivity.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "AgentSentences.hpp"
#include "Icons.hpp"

namespace {
QString css(const QColor& c) { return c.name(QColor::HexArgb); }
QPixmap mark(const QString& name, QColor Tokens::* role, int size, QWidget* w) {
  return icons::pixmap(name, theme::current().*role, size, w ? w->devicePixelRatioF() : qApp->devicePixelRatio());
}
QLabel* iconLabel(const QString& name, QColor Tokens::* role, int size, QWidget* w) {
  auto* l = new QLabel;
  l->setFixedSize(size, size);
  l->setPixmap(mark(name, role, size, w));
  return l;
}
// A row's text starts in the UI's direction (an RLM in Arabic), so a line that is all Latin (a step's name) still sits
// at the leading edge.
QString para(const QString& text) { return (qApp->layoutDirection() == Qt::RightToLeft ? QChar(0x200F) : QChar(0x200E)) + text; }
QToolButton* link(const QString& text, QWidget* parent) {
  auto* b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("toastAction"));
  b->setText(text);
  b->setCursor(Qt::PointingHandCursor);
  b->setAutoRaise(true);
  return b;
}
}  // namespace

// ---------------------------------------------------------------- rows
class ActivityRow : public QFrame {
 public:
  ActivityRow(ActivityPanel* panel, quint64 id) : QFrame(panel->m_list), m_panel(panel), m_id(id) {
    setObjectName(QStringLiteral("agentRow"));
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::TabFocus);
  }
  quint64 id() const { return m_id; }
  void setEvent(const ActivityEvent& e, bool multiple) {
    m_event = e;
    if (layout()) {
      QLayoutItem* item;
      while ((item = layout()->takeAt(0))) {
        if (item->widget()) item->widget()->deleteLater();
        if (item->layout()) clearLayout(item->layout());
        delete item;
      }
      delete layout();
    }
    m_actions.clear();
    const auto& t = theme::current();
    if (e.kind == ActivityEvent::Divider) {
      auto* row = new QHBoxLayout(this);
      row->setContentsMargins(8, 6, 8, 6);
      row->setSpacing(8);
      auto line = [&] {
        auto* f = new QFrame;
        f->setFixedHeight(1);
        f->setStyleSheet(QStringLiteral("background:%1").arg(css(t.line)));
        return f;
      };
      auto* label = new QLabel(e.text);
      label->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(css(t.fg3)));
      row->addWidget(line(), 1);
      row->addWidget(label);
      row->addWidget(line(), 1);
      setToolTip(e.at.toString(QStringLiteral("HH:mm:ss")));
      return;
    }
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(e.kind == ActivityEvent::NeedsYou ? 0 : 8, 6, 8, 6);
    outer->setSpacing(8);
    if (e.kind == ActivityEvent::NeedsYou) {
      auto* bar = new QFrame;
      bar->setFixedWidth(2);
      bar->setStyleSheet(QStringLiteral("background:%1").arg(css(t.amber)));
      outer->addWidget(bar);
      outer->addSpacing(6);
    }
    QColor Tokens::* role = &Tokens::fg2;
    if (e.kind == ActivityEvent::Running) role = &Tokens::fg3;
    if (e.kind == ActivityEvent::Note || e.icon == QLatin1String("aiMark")) role = &Tokens::hov;
    if (e.icon == QLatin1String("check") && e.kind == ActivityEvent::Note) role = &Tokens::green;
    if (e.issue || e.kind == ActivityEvent::NeedsYou) role = &Tokens::amber;
    if (e.kind == ActivityEvent::Quiet) role = &Tokens::fg3;
    const QString icon = e.kind == ActivityEvent::NeedsYou ? QStringLiteral("warning") : e.kind == ActivityEvent::Quiet ? QStringLiteral("issue") : e.icon;
    auto* ic = iconLabel(icon.isEmpty() ? QStringLiteral("aiMark") : icon, role, 16, this);
    auto* lead = new QVBoxLayout;
    lead->setContentsMargins(0, 1, 0, 0);
    lead->addWidget(ic);
    lead->addStretch();
    outer->addLayout(lead);
    auto* mid = new QVBoxLayout;
    mid->setSpacing(2);
    const bool dim = e.state == ActivityEvent::Undone || e.state == ActivityEvent::Gone || e.state == ActivityEvent::Dropped;
    auto* text = new QLabel(para(e.text));
    text->setWordWrap(true);
    text->setTextFormat(Qt::PlainText);
    text->setStyleSheet(QStringLiteral("color:%1;font-size:%2px").arg(css(dim || e.kind == ActivityEvent::Quiet ? t.fg3 : t.fg)).arg(e.kind == ActivityEvent::Quiet ? 12 : 13));
    text->setObjectName(QStringLiteral("agentRowText"));
    mid->addWidget(text);
    QString second = e.second;
    if (!e.tag.isEmpty()) second = second.isEmpty() ? e.tag : agentsay::Say::tr("%1 · %2").arg(second, e.tag);
    if (multiple && !e.client.isEmpty()) second = second.isEmpty() ? e.client : agentsay::Say::tr("%1 · %2").arg(second, e.client);
    if (!second.isEmpty()) {
      auto* s = new QLabel(para(second));
      s->setWordWrap(true);
      s->setTextFormat(Qt::PlainText);
      s->setStyleSheet(QStringLiteral("color:%1;font-size:12px").arg(css(e.kind == ActivityEvent::NeedsYou ? t.fg2 : t.fg3)));
      s->setObjectName(QStringLiteral("agentRowSecond"));
      mid->addWidget(s);
    }
    if (e.expanded && !e.children.isEmpty()) {
      QStringList lines;
      for (const auto& line : e.children) lines << para(line);
      auto* c = new QLabel(lines.join(QLatin1Char('\n')));
      c->setWordWrap(true);
      c->setTextFormat(Qt::PlainText);
      c->setStyleSheet(QStringLiteral("color:%1;font-size:12px;padding-left:8px").arg(css(t.fg2)));
      mid->addWidget(c);
    }
    if (e.kind == ActivityEvent::NeedsYou || e.kind == ActivityEvent::Quiet) {
      auto* buttons = new QHBoxLayout;
      buttons->setSpacing(6);
      if (!e.actionText.isEmpty()) {
        auto* b = new QPushButton(e.actionText);
        b->setObjectName(QStringLiteral("outline"));
        b->setFixedHeight(22);
        connect(b, &QPushButton::clicked, this, [this] { emit m_panel->activated(m_id, m_event.action); });
        buttons->addWidget(b);
        auto* later = link(agentsay::Say::tr("Not now"), this);
        connect(later, &QToolButton::clicked, this, [this] { emit m_panel->activated(m_id, QStringLiteral("notNow")); });
        buttons->addWidget(later);
      }
      if (!e.details.isEmpty()) {
        auto* d = link(m_details ? agentsay::Say::tr("Hide details") : agentsay::Say::tr("Details"), this);
        connect(d, &QToolButton::clicked, this, [this, multiple] {
          m_details = !m_details;
          setEvent(m_event, multiple);
        });
        buttons->addWidget(d);
      }
      buttons->addStretch();
      mid->addLayout(buttons);
      if (m_details && !e.details.isEmpty()) {
        auto* raw = new QLabel(e.details);
        raw->setObjectName(QStringLiteral("mono"));
        raw->setWordWrap(true);
        raw->setTextFormat(Qt::PlainText);
        raw->setTextInteractionFlags(Qt::TextSelectableByMouse);
        raw->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(css(t.fg3)));
        mid->addWidget(raw);
      }
    }
    outer->addLayout(mid, 1);
    // Trailing: a fold's count and chevron, a look's thumbnail, the hover actions.
    auto* trail = new QHBoxLayout;
    trail->setSpacing(2);
    if (e.kind == ActivityEvent::Reads) {
      auto* n = new QLabel(agentsay::lookups(e.reads));
      n->setObjectName(QStringLiteral("mono"));
      n->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(css(t.fg3)));
      trail->addWidget(n);
    }
    if (e.kind == ActivityEvent::Reads || (e.kind == ActivityEvent::Group && !e.children.isEmpty())) {
      auto* chevron = new QToolButton;
      chevron->setAutoRaise(true);
      chevron->setFixedSize(20, 20);
      chevron->setIcon(QIcon(mark(e.expanded ? QStringLiteral("chevronDown") : QStringLiteral("chevronRight"), &Tokens::fg2, 16, this)));
      chevron->setAccessibleName(e.expanded ? agentsay::Say::tr("Collapse") : agentsay::Say::tr("Expand"));
      connect(chevron, &QToolButton::clicked, this, [this, multiple] {
        if (auto* ev = m_panel->event(m_id)) {
          ev->expanded = !ev->expanded;
          setEvent(*ev, multiple);
        }
      });
      trail->addWidget(chevron);
    }
    const auto action = [&](const QString& label, const QString& what) {
      auto* b = link(label, this);
      b->setVisible(m_hover || hasFocus());
      connect(b, &QToolButton::clicked, this, [this, what] { emit m_panel->activated(m_id, what); });
      trail->addWidget(b);
      m_actions.push_back(b);
    };
    const bool change = e.kind == ActivityEvent::Change || (e.kind == ActivityEvent::Group && e.state == ActivityEvent::Applied) ||
                        (e.kind == ActivityEvent::Note && !e.subjects.empty()) || e.kind == ActivityEvent::Result;
    if (e.kind == ActivityEvent::Group && e.state == ActivityEvent::Staged) {
      action(agentsay::Say::tr("Show"), QStringLiteral("show"));
      action(agentsay::Say::tr("Discard"), QStringLiteral("discard"));
    } else if (change && e.state != ActivityEvent::Gone && e.state != ActivityEvent::Undone && e.state != ActivityEvent::Dropped) {
      // Show saved the camera: Back puts it there again (AgentBridge::rowAction), until the camera moves.
      if (e.canShow && !e.subjects.empty()) action(m_showing ? agentsay::Say::tr("Back") : agentsay::Say::tr("Show"), m_showing ? QStringLiteral("back") : QStringLiteral("show"));
      if (e.canUndo) action(agentsay::Say::tr("Undo"), QStringLiteral("undo"));
      else if (!e.op.empty() && e.canShow) action(agentsay::Say::tr("Show in timeline"), QStringLiteral("timeline"));
    }
    if (e.kind == ActivityEvent::NeedsYou || e.kind == ActivityEvent::Quiet) {
      auto* close = new QToolButton;
      close->setAutoRaise(true);
      close->setFixedSize(20, 20);
      close->setIcon(QIcon(mark(QStringLiteral("close"), &Tokens::fg3, 14, this)));
      close->setAccessibleName(agentsay::Say::tr("Dismiss"));
      connect(close, &QToolButton::clicked, this, [this] { emit m_panel->activated(m_id, QStringLiteral("dismiss")); });
      trail->addWidget(close);
    }
    if (!e.thumb.isNull()) {
      auto* thumb = new QToolButton;
      thumb->setAutoRaise(true);
      thumb->setIconSize(QSize(96, 64));
      thumb->setFixedSize(100, 68);
      thumb->setIcon(QIcon(e.thumb));
      thumb->setStyleSheet(QStringLiteral("QToolButton{border:1px solid %1;border-radius:3px;background:%2}").arg(css(t.line), css(t.bg4)));
      thumb->setToolTip(agentsay::Say::tr("Click to enlarge"));
      connect(thumb, &QToolButton::clicked, this, [this, thumb] { enlarge(thumb); });
      trail->addWidget(thumb);
    }
    auto* trailBox = new QVBoxLayout;
    trailBox->addLayout(trail);
    trailBox->addStretch();
    outer->addLayout(trailBox);
    QStringList tip;
    tip << e.at.toString(QStringLiteral("HH:mm:ss"));
    if (e.ms >= 0) tip << agentsay::Say::tr("took %1 s").arg(QString::number(e.ms / 1000.0, 'f', 1));
    if (!e.client.isEmpty()) tip << e.client;
    QString full = agentsay::plain(e.text);
    setToolTip(full + QStringLiteral("\n") + tip.join(QStringLiteral(" · ")));
    setAccessibleName(full);
  }
  void setShowing(bool on) {
    m_showing = on;
    setEvent(m_event, m_panel->m_multiple);
  }
  bool showing() const { return m_showing; }
  QStringList actionTexts() const {
    QStringList out;
    for (auto* b : m_actions) out << b->text();
    return out;
  }
 protected:
  void enterEvent(QEnterEvent*) override {
    m_hover = true;
    for (auto* b : m_actions) b->show();
    emit m_panel->hovered(m_id);
  }
  void leaveEvent(QEvent*) override {
    m_hover = false;
    if (!hasFocus())
      for (auto* b : m_actions) b->hide();
    emit m_panel->hovered(0);
  }
  void focusInEvent(QFocusEvent*) override {
    for (auto* b : m_actions) b->show();
  }
  void focusOutEvent(QFocusEvent*) override {
    if (!m_hover)
      for (auto* b : m_actions) b->hide();
  }
  void paintEvent(QPaintEvent* e) override {
    if (m_hover && m_event.kind != ActivityEvent::Divider) {
      QPainter p(this);
      p.fillRect(rect(), theme::current().bg3);
    }
    QFrame::paintEvent(e);
  }
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton && !m_event.subjects.empty()) emit m_panel->activated(m_id, QStringLiteral("select"));
    QFrame::mousePressEvent(e);
  }
  void mouseDoubleClickEvent(QMouseEvent*) override {
    if (!m_event.subjects.empty() && m_event.canShow) emit m_panel->activated(m_id, QStringLiteral("show"));
  }
  void contextMenuEvent(QContextMenuEvent* e) override {
    if (m_event.kind == ActivityEvent::Divider) return;
    QMenu menu(this);
    using agentsay::Say;
    const bool live = m_event.canShow && !m_event.subjects.empty() && m_event.state != ActivityEvent::Gone;
    menu.addAction(Say::tr("Select"), this, [this] { emit m_panel->activated(m_id, QStringLiteral("select")); })->setEnabled(live);
    menu.addAction(Say::tr("Zoom to"), this, [this] { emit m_panel->activated(m_id, QStringLiteral("show")); })->setEnabled(live);
    menu.addAction(Say::tr("Show in timeline"), this, [this] { emit m_panel->activated(m_id, QStringLiteral("timeline")); })->setEnabled(!m_event.op.empty() && m_event.canShow);
    if (m_event.canUndo) menu.addAction(Say::tr("Undo"), this, [this] { emit m_panel->activated(m_id, QStringLiteral("undo")); });
    menu.addSeparator();
    menu.addAction(Say::tr("Copy details"), this, [this] { emit m_panel->activated(m_id, QStringLiteral("copy")); });
    menu.exec(e->globalPos());
  }
 private:
  static void clearLayout(QLayout* l) {
    QLayoutItem* item;
    while ((item = l->takeAt(0))) {
      if (item->widget()) item->widget()->deleteLater();
      if (item->layout()) clearLayout(item->layout());
      delete item;
    }
  }
  void enlarge(QWidget* anchor) {
    if (m_event.picture.isNull() && m_event.thumb.isNull()) return;
    auto* popup = new QFrame(this, Qt::Popup);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setStyleSheet(QStringLiteral("QFrame{background:%1;border:1px solid %2;border-radius:4px}").arg(css(theme::current().bg3), css(theme::current().line)));
    auto* l = new QVBoxLayout(popup);
    l->setContentsMargins(8, 8, 8, 8);
    auto* caption = new QLabel(agentsay::Say::tr("What %1 saw").arg(m_event.client.isEmpty() ? agentsay::Say::tr("the AI assistant") : m_event.client));
    caption->setStyleSheet(QStringLiteral("border:none;color:%1;font-size:12px").arg(css(theme::current().fg2)));
    auto* image = new QLabel;
    image->setStyleSheet(QStringLiteral("border:none"));
    image->setPixmap(m_event.picture.isNull() ? m_event.thumb : m_event.picture);
    l->addWidget(caption);
    l->addWidget(image);
    popup->adjustSize();
    QPoint at = anchor->mapToGlobal(QPoint(anchor->width(), 0)) - QPoint(popup->width(), popup->height() + 4);
    popup->move(at);
    popup->show();
  }
  ActivityPanel* m_panel;
  quint64 m_id;
  ActivityEvent m_event;
  std::vector<QToolButton*> m_actions;
  bool m_hover = false, m_details = false, m_showing = false;
};

// ---------------------------------------------------------------- panel
ActivityPanel::ActivityPanel(QWidget* parent) : QWidget(parent) {
  using agentsay::Say;
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);
  // The connection strip.
  m_strip = new QFrame;
  m_strip->setObjectName(QStringLiteral("agentStrip"));
  auto* strip = new QHBoxLayout(m_strip);
  strip->setContentsMargins(8, 6, 6, 6);
  strip->setSpacing(8);
  m_stripIcon = new QLabel;
  m_stripIcon->setFixedSize(24, 24);
  m_stripIcon->setAlignment(Qt::AlignCenter);
  strip->addWidget(m_stripIcon, 0, Qt::AlignTop);
  auto* lines = new QVBoxLayout;
  lines->setSpacing(2);
  m_line1 = new QLabel;
  m_line1->setObjectName(QStringLiteral("agentStripLine1"));
  m_line1->setTextFormat(Qt::PlainText);
  m_line1->setWordWrap(true);
  auto* second = new QHBoxLayout;
  second->setSpacing(6);
  m_pill = new QToolButton;
  m_pill->setObjectName(QStringLiteral("agentAccessPill"));
  m_pill->setCursor(Qt::PointingHandCursor);
  connect(m_pill, &QToolButton::clicked, this, [this] { emit accessMenuRequested(m_pill->mapToGlobal(QPoint(0, m_pill->height()))); });
  m_line2 = new QLabel;
  m_line2->setObjectName(QStringLiteral("agentStripLine2"));
  m_line2->setTextFormat(Qt::PlainText);
  m_line2->setWordWrap(true);
  m_link = link(QString(), this);
  connect(m_link, &QToolButton::clicked, this, &ActivityPanel::stripLinkActivated);
  second->addWidget(m_pill);
  second->addWidget(m_line2, 1);
  second->addWidget(m_link);
  lines->addWidget(m_line1);
  lines->addLayout(second);
  strip->addLayout(lines, 1);
  m_more = new QToolButton;
  m_more->setAutoRaise(true);
  m_more->setFixedSize(24, 24);
  m_more->setAccessibleName(Say::tr("More"));
  m_more->setToolTip(Say::tr("More"));
  connect(m_more, &QToolButton::clicked, this, [this] { emit stripMenuRequested(m_more->mapToGlobal(QPoint(0, m_more->height()))); });
  strip->addWidget(m_more, 0, Qt::AlignTop);
  auto* stripBox = new QVBoxLayout;
  stripBox->setContentsMargins(8, 8, 8, 4);
  stripBox->addWidget(m_strip);
  root->addLayout(stripBox);
  // The feed, or the empty state.
  m_pages = new QStackedWidget;
  m_empty = new QWidget;
  auto* empty = new QVBoxLayout(m_empty);
  empty->setContentsMargins(20, 16, 20, 16);
  empty->setSpacing(8);
  empty->addStretch();
  m_emptyIcon = new QLabel;
  m_emptyIcon->setAlignment(Qt::AlignCenter);
  m_emptyTitle = new QLabel;
  m_emptyTitle->setObjectName(QStringLiteral("agentEmptyTitle"));
  m_emptyTitle->setAlignment(Qt::AlignCenter);
  m_emptyTitle->setWordWrap(true);
  m_emptyText = new QLabel;
  m_emptyText->setAlignment(Qt::AlignCenter);
  m_emptyText->setWordWrap(true);
  m_promptCard = new QFrame;
  m_promptCard->setObjectName(QStringLiteral("agentPrompt"));
  auto* card = new QHBoxLayout(m_promptCard);
  card->setContentsMargins(8, 6, 6, 6);
  m_prompt = new QLabel;
  m_prompt->setObjectName(QStringLiteral("mono"));
  m_prompt->setWordWrap(true);
  m_prompt->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_copy = new QPushButton(Say::tr("Copy"));
  m_copy->setObjectName(QStringLiteral("secondary"));
  connect(m_copy, &QPushButton::clicked, this, [this] {
    QGuiApplication::clipboard()->setText(m_prompt->text());
    m_copy->setText(agentsay::Say::tr("Copied"));
    QTimer::singleShot(1500, m_copy, [this] { m_copy->setText(agentsay::Say::tr("Copy")); });
    emit promptCopied(m_prompt->text());
  });
  card->addWidget(m_prompt, 1);
  card->addWidget(m_copy, 0, Qt::AlignTop);
  m_emptyButton = new QPushButton;
  m_emptyButton->setObjectName(QStringLiteral("primary"));
  connect(m_emptyButton, &QPushButton::clicked, this, &ActivityPanel::emptyActionRequested);
  m_emptyExample = new QLabel;
  m_emptyExample->setAlignment(Qt::AlignCenter);
  m_emptyExample->setWordWrap(true);
  empty->addWidget(m_emptyIcon);
  empty->addWidget(m_emptyTitle);
  empty->addWidget(m_emptyText);
  empty->addWidget(m_promptCard);
  empty->addWidget(m_emptyButton, 0, Qt::AlignCenter);
  empty->addWidget(m_emptyExample);
  empty->addStretch();
  m_scroll = new QScrollArea;
  m_scroll->setWidgetResizable(true);
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list = new QWidget;
  m_list->setObjectName(QStringLiteral("agentFeed"));
  m_listLayout = new QVBoxLayout(m_list);
  m_listLayout->setContentsMargins(0, 0, 0, 0);
  m_listLayout->setSpacing(0);
  m_listLayout->addStretch();
  m_scroll->setWidget(m_list);
  m_pages->addWidget(m_empty);
  m_pages->addWidget(m_scroll);
  root->addWidget(m_pages, 1);
  // The footer.
  auto* footer = new QFrame;
  footer->setObjectName(QStringLiteral("agentFooter"));
  footer->setFixedHeight(44);
  auto* row = new QHBoxLayout(footer);
  row->setContentsMargins(12, 0, 12, 0);
  m_follow = new QCheckBox(Say::tr("Zoom to its changes"));
  m_follow->setToolTip(Say::tr("Frame each change in the view as the AI assistant applies it."));
  connect(m_follow, &QCheckBox::toggled, this, &ActivityPanel::followToggled);
  m_stop = new QPushButton(Say::tr("Stop"));
  m_stop->setObjectName(QStringLiteral("agentStop"));
  m_stop->setEnabled(false);
  m_stop->setToolTip(Say::tr("Stop the AI assistant's unfinished work and remove its preview. Changes it already made stay; Undo removes them."));
  connect(m_stop, &QPushButton::clicked, this, &ActivityPanel::stopRequested);
  row->addWidget(m_follow);
  row->addStretch();
  row->addWidget(m_stop);
  root->addWidget(footer);
  connect(theme::notifier(), &theme::Notifier::changed, this, &ActivityPanel::restyle);
  restyle();
  updatePage();
}

bool ActivityPanel::event(QEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  const QEvent::Type scaleChange = QEvent::DevicePixelRatioChange;
#else
  const QEvent::Type scaleChange = QEvent::ScreenChangeInternal;  // Qt 6.4 (Ubuntu 24.04): no DevicePixelRatioChange yet
#endif
  if (e->type() == QEvent::LayoutDirectionChange || e->type() == scaleChange) restyle();
  return QWidget::event(e);
}

void ActivityPanel::styleStrip() {
  const auto& t = theme::current();
  m_strip->setStyleSheet(QStringLiteral("QFrame#agentStrip{background:%1;border-radius:3px}"
                                        "QLabel#agentStripLine1{font-weight:500;color:%2}"
                                        "QLabel#agentStripLine2{font-size:12px;color:%3}"
                                        "QToolButton#agentAccessPill{font-size:11px;color:%4;background:%5;border:1px solid %6;border-radius:3px;padding:0 5px}")
                             .arg(css(t.bg3), css(m_stripState.amber ? t.amber : t.fg), css(t.fg2), css(t.fg), css(t.bg4), css(t.line)));
  QPixmap circle(24 * devicePixelRatioF(), 24 * devicePixelRatioF());
  circle.setDevicePixelRatio(devicePixelRatioF());
  circle.fill(Qt::transparent);
  {
    QPainter p(&circle);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(t.bg4);
    p.drawEllipse(QRectF(0, 0, 24, 24));
    p.drawPixmap(4, 4, mark(QStringLiteral("aiMark"), m_stripState.active ? &Tokens::hov : &Tokens::fg3, 16, this));
  }
  m_stripIcon->setPixmap(circle);
}

void ActivityPanel::restyle() {
  const auto& t = theme::current();
  styleStrip();
  m_more->setIcon(QIcon(mark(QStringLiteral("more"), &Tokens::fg2, 16, this)));
  m_emptyIcon->setPixmap(mark(QStringLiteral("aiMark"), &Tokens::hov, 32, this));
  m_emptyTitle->setStyleSheet(QStringLiteral("font-size:16px;font-weight:500;color:%1").arg(css(t.fg)));
  m_emptyText->setStyleSheet(QStringLiteral("font-size:12px;color:%1").arg(css(t.fg2)));
  m_emptyExample->setStyleSheet(QStringLiteral("font-size:12px;color:%1").arg(css(t.fg3)));
  m_promptCard->setStyleSheet(QStringLiteral("QFrame#agentPrompt{background:%1;border-radius:3px}").arg(css(t.bg3)));
  m_list->setStyleSheet(QStringLiteral("QWidget#agentFeed{background:transparent}"));
  for (size_t i = 0; i < m_rows.size(); ++i) m_rows[i]->setEvent(m_events[i], m_multiple);
}

void ActivityPanel::updatePage() { m_pages->setCurrentIndex(m_events.empty() ? 0 : 1); }

ActivityRow* ActivityPanel::rowFor(quint64 id) const {
  for (size_t i = 0; i < m_events.size(); ++i)
    if (m_events[i].id == id) return m_rows[i];
  return nullptr;
}

ActivityEvent* ActivityPanel::event(quint64 id) {
  for (auto& e : m_events)
    if (e.id == id) return &e;
  return nullptr;
}

void ActivityPanel::post(const ActivityEvent& e) {
  for (size_t i = 0; i < m_events.size(); ++i)
    if (m_events[i].id == e.id) {
      const bool expanded = m_events[i].expanded;
      m_events[i] = e;
      m_events[i].expanded = e.expanded || expanded;
      m_rows[i]->setEvent(m_events[i], m_multiple);
      return;
    }
  auto* bar = m_scroll->verticalScrollBar();
  const bool atEnd = bar->value() >= bar->maximum() - 4;
  m_events.push_back(e);
  auto* row = new ActivityRow(this, e.id);
  m_rows.push_back(row);
  row->setEvent(m_events.back(), m_multiple);
  m_listLayout->insertWidget(m_listLayout->count() - 1, row);
  while (m_events.size() > size_t(kMaxRows)) {
    m_rows.front()->deleteLater();
    m_rows.erase(m_rows.begin());
    m_events.erase(m_events.begin());
  }
  updatePage();
  if (atEnd) QTimer::singleShot(0, this, &ActivityPanel::scrollToEnd);
}

void ActivityPanel::remove(quint64 id) {
  if (id == m_showing) m_showing = 0;
  for (size_t i = 0; i < m_events.size(); ++i)
    if (m_events[i].id == id) {
      m_rows[i]->deleteLater();
      m_rows.erase(m_rows.begin() + i);
      m_events.erase(m_events.begin() + i);
      break;
    }
  updatePage();
}

void ActivityPanel::clear() {
  m_showing = 0;
  for (auto* r : m_rows) r->deleteLater();
  m_rows.clear();
  m_events.clear();
  updatePage();
}

void ActivityPanel::setStrip(const Strip& s) {
  if (s == m_stripState && m_strip->isVisible() == s.shown) return;
  m_stripState = s;
  m_strip->parentWidget()->setVisible(true);
  m_strip->setVisible(s.shown);
  m_line1->setText(para(s.line1));
  m_line2->setText(para(s.line2));
  m_line2->setVisible(!s.line2.isEmpty());
  m_pill->setText(s.pill);
  m_pill->setVisible(!s.pill.isEmpty());
  m_link->setText(s.link);
  m_link->setVisible(!s.link.isEmpty());
  styleStrip();
}

void ActivityPanel::setEmpty(const Empty& e) {
  m_emptyTitle->setText(e.title);
  m_emptyText->setText(e.text);
  m_emptyText->setVisible(!e.text.isEmpty());
  m_prompt->setText(e.prompt);
  m_promptCard->setVisible(!e.prompt.isEmpty());
  m_emptyButton->setText(e.button);
  m_emptyButton->setVisible(!e.button.isEmpty());
  m_emptyExample->setText(e.example);
  m_emptyExample->setVisible(!e.example.isEmpty());
}

void ActivityPanel::setBusy(bool working) {
  if (m_stop->isEnabled() == working) return;
  m_stop->setEnabled(working);
  m_stop->setObjectName(working ? QStringLiteral("primary") : QStringLiteral("agentStop"));
  m_stop->style()->unpolish(m_stop);
  m_stop->style()->polish(m_stop);
}

void ActivityPanel::setFollow(bool on) {
  QSignalBlocker block(m_follow);
  m_follow->setChecked(on);
}

void ActivityPanel::setMultiple(bool on) {
  if (m_multiple == on) return;
  m_multiple = on;
  refreshRows();
}

void ActivityPanel::refreshRows() {
  for (size_t i = 0; i < m_rows.size(); ++i) m_rows[i]->setEvent(m_events[i], m_multiple);
}

void ActivityPanel::setShowing(quint64 id) {
  if (id == m_showing) return;
  if (auto* old = rowFor(m_showing)) old->setShowing(false);
  m_showing = 0;
  if (auto* row = rowFor(id)) {
    row->setShowing(true);
    m_showing = id;
  }
}

void ActivityPanel::reveal(quint64 id) {
  auto* row = rowFor(id);
  if (!row) return;
  m_revealed = id;
  row->setFocus(Qt::OtherFocusReason);  // its actions show (focusInEvent)
  m_scroll->ensureWidgetVisible(row, 0, 24);
  // A panel just shown lays its rows out first: once more after that.
  QTimer::singleShot(0, this, [this, id] {
    if (auto* r = rowFor(id)) m_scroll->ensureWidgetVisible(r, 0, 24);
  });
}

QString ActivityPanel::actionsOf(quint64 id) const {
  auto* row = rowFor(id);
  return row ? row->actionTexts().join(QLatin1Char('|')) : QString();
}

void ActivityPanel::scrollToEnd() { m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum()); }

QStringList ActivityPanel::lines() const {
  QStringList out;
  for (const auto& e : m_events) {
    QString line = agentsay::plain(e.text);
    if (e.kind == ActivityEvent::Reads) line += QStringLiteral(" · ") + agentsay::lookups(e.reads);
    if (!e.second.isEmpty()) line += QStringLiteral(" — ") + agentsay::plain(e.second);
    if (!e.tag.isEmpty()) line += QStringLiteral(" [") + e.tag + QStringLiteral("]");
    out << line;
    for (const auto& c : e.children) out << QStringLiteral("    ") + agentsay::plain(c);
  }
  return out;
}

// ---------------------------------------------------------------- chip
AgentChip::AgentChip(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("agentChip"));
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setAttribute(Qt::WA_Hover);
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void AgentChip::setState(const State& s) {
  m_state = s;
  setToolTip(s.tip);
  setAccessibleName(agentsay::plain(s.text));
  setAccessibleDescription(s.tip);
  updateGeometry();
  update();
}

QSize AgentChip::sizeHint() const {
  const QFontMetrics fm(font());
  int w = 4 + 14 + 4 + std::min(240, fm.horizontalAdvance(m_state.text)) + 6;
  if (!m_state.glyph.isEmpty()) w += 12 + 4;
  if (m_state.count > 0) w += fm.horizontalAdvance(QString::number(m_state.count)) + 14 + 4;
  return {w, 20};
}

void AgentChip::paintEvent(QPaintEvent*) {
  const auto& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRect r = rect().adjusted(0, (height() - 20) / 2, 0, -(height() - 20) / 2);
  if (m_hover || hasFocus()) {
    p.setPen(Qt::NoPen);
    p.setBrush(t.bg3);
    p.drawRoundedRect(r, 3, 3);
  }
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  int x = rtl ? width() - 4 - 14 : 4;
  const auto step = [&](int w) { x = rtl ? x - w : x + w; };
  const auto place = [&](int w) { return rtl ? x - w + (w == 14 ? 14 : w) - w : x; };
  (void)place;
  const int cy = r.center().y();
  const auto drawIcon = [&](const QString& name, QColor Tokens::* role, int size) {
    const int left = rtl ? x + 14 - size : x;
    p.drawPixmap(left, cy - size / 2 + 1, icons::pixmap(name, t.*role, size, devicePixelRatioF()));
  };
  if (m_state.glyphFirst && !m_state.glyph.isEmpty()) {
    drawIcon(m_state.glyph, &Tokens::amber, 10);
    step(10 + 4);
  }
  drawIcon(QStringLiteral("aiMark"), m_state.mark, 14);
  step(14 + 4);
  const QFontMetrics fm(font());
  const QString text = fm.elidedText(m_state.text, rtl ? Qt::ElideLeft : Qt::ElideRight, 240);
  const int tw = fm.horizontalAdvance(text);
  p.setPen(t.*m_state.label);
  const int tx = rtl ? x + 14 - tw : x;
  p.drawText(QRect(tx, r.top(), tw, r.height()), Qt::AlignVCenter, text);
  step(tw + 4);
  if (!m_state.glyphFirst && !m_state.glyph.isEmpty()) {
    drawIcon(m_state.glyph, &Tokens::fg2, 12);
    step(12 + 4);
  }
  if (m_state.count > 0) {
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);
    const QString n = QString::number(m_state.count);
    const int pw = QFontMetrics(small).horizontalAdvance(n) + 10;
    const QRect pill(rtl ? x + 14 - pw : x, cy - 7, pw, 14);
    p.setPen(Qt::NoPen);
    p.setBrush(t.bg4);
    p.drawRoundedRect(pill, 7, 7);
    p.setPen(t.fg);
    p.drawText(pill, Qt::AlignCenter, n);
  }
}

void AgentChip::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) emit clicked(mapToGlobal(QPoint(0, 0)));
}
void AgentChip::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Menu) emit clicked(mapToGlobal(QPoint(0, 0)));
  else QWidget::keyPressEvent(e);
}
void AgentChip::contextMenuEvent(QContextMenuEvent*) { emit clicked(mapToGlobal(QPoint(0, 0))); }
void AgentChip::enterEvent(QEnterEvent*) {
  m_hover = true;
  update();
}
void AgentChip::leaveEvent(QEvent*) {
  m_hover = false;
  update();
}
