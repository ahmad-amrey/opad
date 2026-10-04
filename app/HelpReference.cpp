#include "HelpReference.hpp"

#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace {
QLabel* wrapped(const char* name, QWidget* parent) {
  auto* l = new QLabel(parent);
  l->setObjectName(name);
  l->setWordWrap(true);
  l->setTextFormat(Qt::PlainText);
  return l;
}

// The "more tools" menu buttons are entries of the ribbon, not commands to learn.
bool listed(const CommandHelp& h) { return !h.id.section('.', -1).startsWith("more"); }
}  // namespace

// ---------------------------------------------------------------- CommandPreview
CommandPreview::CommandPreview(Size size, QWidget* parent) : QWidget(parent), m_size(size) {
  setObjectName("commandPreview");
  const bool full = size == Size::Full;
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(full ? 16 : 12, 12, full ? 16 : 12, 12);
  v->setSpacing(8);
  auto* head = new QHBoxLayout();
  head->setSpacing(8);
  m_icon = new QLabel(this);
  m_icon->setFixedSize(20, 20);
  m_title = wrapped("panelTitle", this);
  // The caps read left to right in every language ("Ctrl" before "F"): their own left-to-right box, which right to left
  // places on the other side as a whole.
  m_keyBox = new QWidget(this);
  m_keyBox->setObjectName("keyCaps");
  m_keyBox->setLayoutDirection(Qt::LeftToRight);
  m_keys = new QHBoxLayout(m_keyBox);
  m_keys->setContentsMargins(0, 0, 0, 0);
  m_keys->setSpacing(4);
  head->addWidget(m_icon, 0, Qt::AlignTop);
  head->addWidget(m_title, 1);
  head->addWidget(m_keyBox, 0, Qt::AlignTop);
  v->addLayout(head);
  m_summary = wrapped("secondary", this);
  v->addWidget(m_summary);
  m_clip = new ClipView(QString(), this);
  m_clip->setFixedSize(full ? QSize(480, 270) : QSize(288, 162));
  v->addWidget(m_clip, 0, Qt::AlignHCenter);
  if (full) {
    m_steps = new QListWidget(this);
    m_steps->setObjectName("guideSteps");
    m_steps->setFrameShape(QFrame::NoFrame);
    m_steps->setFocusPolicy(Qt::NoFocus);
    m_steps->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    m_steps->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    v->addWidget(m_steps);
    connect(m_steps, &QListWidget::currentRowChanged, this, [this](int row) { if (row >= 0) m_clip->setStep(row - 1); });
  }
  m_details = wrapped("details", this);
  m_details->setVisible(full);
  v->addWidget(m_details);
  m_requirement = wrapped("requirement", this);
  v->addWidget(m_requirement);
  v->addStretch(1);
  connect(theme::notifier(), &theme::Notifier::changed, this, &CommandPreview::refresh);
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] {  // its key, and the keys its texts and steps name
    refresh();
    const QList<clips::Step> steps = clips::steps(m_clip->clip());
    for (int i = 0; m_steps && i < steps.size() && i + 1 < m_steps->count(); ++i) m_steps->item(i + 1)->setText(stepText(i, steps[i]));
  });
}

QString CommandPreview::stepText(int i, const clips::Step& step) { return QString("%1  %2").arg(i + 1).arg(clips::caption(step)); }

QStringList CommandPreview::keyCaps() const {
  QStringList out;
  for (QLabel* l : m_keyBox->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) out << l->text();
  return out;
}

bool CommandPreview::showsRequirement() const { return !m_requirement->isHidden() && !m_requirement->text().isEmpty(); }

void CommandPreview::setCommand(const QString& id, QAction* action) {
  disconnect(m_changed);
  m_action = action;
  if (action) m_changed = connect(action, &QAction::changed, this, &CommandPreview::refresh);  // shortcut, availability
  if (id == m_id) return refresh();
  m_id = id;
  const CommandHelp* h = help::find(id);
  const QString clip = h && clips::has(h->clip) ? h->clip : QString();
  m_clip->setClip(clip);
  m_clip->setVisible(!clip.isEmpty());
  if (m_steps) {
    const QSignalBlocker quiet(m_steps);
    m_steps->clear();
    if (!clip.isEmpty()) {
      m_steps->addItem(tr("All steps"));
      const QList<clips::Step> steps = clips::steps(clip);
      for (int i = 0; i < steps.size(); ++i) m_steps->addItem(stepText(i, steps[i]));
      m_steps->setCurrentRow(0);
    }
    m_steps->setVisible(!clip.isEmpty());
  }
  refresh();
}

void CommandPreview::refresh() {
  const Tokens& t = theme::current();
  const CommandHelp* h = help::find(m_id);
  QString label = m_action ? m_action->text() : m_id;
  label.remove('&').remove(QString::fromUtf8("…"));
  m_title->setText(h && !h->title.isEmpty() ? h->title : label);
  const QString icon = m_action ? m_action->data().toString() : QString();
  m_icon->setPixmap(icons::has(icon) ? icons::pixmap(icon, t.sel, 20, devicePixelRatioF()) : QPixmap());
  m_icon->setVisible(icons::has(icon));
  while (QLayoutItem* it = m_keys->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  for (const QString& k : keys::caps(keys::binding(m_action.data()))) {  // the user's key, also while a sketch holds it
    auto* cap = new QLabel(k, m_keyBox);
    if (k != keys::kThen) cap->setObjectName("keycap");  // a multi-chord key's separator is plain text
    m_keys->addWidget(cap, 0, Qt::AlignTop);
  }
  m_keyBox->setVisible(m_keys->count() > 0);
  m_summary->setText(h ? help::expand(h->summary) : QString());
  m_details->setText(h ? help::expand(h->details) : QString());
  m_details->setVisible(m_size == Size::Full && !m_details->text().isEmpty());
  const bool available = !m_action || m_action->isEnabled();
  m_requirement->setText(available || m_id.isEmpty() ? QString() : QString::fromUtf8("⚠  ") + (h && !h->requirement.isEmpty() ? help::requirement(*h) : tr("Not available right now.")));
  m_requirement->setStyleSheet(QString("color: %1;").arg(theme::css(t.amber)));
  m_requirement->setVisible(!m_requirement->text().isEmpty());
  m_details->setStyleSheet(QString("color: %1;").arg(theme::css(t.fg)));
}

// ---------------------------------------------------------------- CommandReference
CommandReference::CommandReference(std::function<QAction*(const QString&)> lookup, QWidget* parent)
    : QWidget(parent, Qt::Window), m_lookup(std::move(lookup)) {
  setObjectName("commandReference");
  setWindowTitle(tr("Tool guide"));
  setAttribute(Qt::WA_StyledBackground);
  resize(940, 640);
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  auto* left = new QWidget(this);
  left->setFixedWidth(320);
  auto* lv = new QVBoxLayout(left);
  lv->setContentsMargins(12, 12, 8, 12);
  lv->setSpacing(8);
  m_search = new QLineEdit(left);
  m_search->setObjectName("paletteInput");
  m_search->setPlaceholderText(tr("Search commands…"));
  m_search->setClearButtonEnabled(true);
  m_search->addAction(icons::icon("search", theme::current().fg3), QLineEdit::LeadingPosition);
  m_list = new QTreeWidget(left);
  m_list->setObjectName("referenceList");
  m_list->setColumnCount(2);
  m_list->setHeaderHidden(true);
  m_list->setRootIsDecorated(false);
  m_list->setIndentation(8);
  m_list->setUniformRowHeights(true);
  m_list->header()->setStretchLastSection(false);
  m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  lv->addWidget(m_search);
  lv->addWidget(m_list, 1);
  h->addWidget(left);
  auto* rule = new QFrame(this);
  rule->setFixedWidth(1);
  rule->setObjectName("referenceRule");
  h->addWidget(rule);
  auto* scroll = new QScrollArea(this);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  m_preview = new CommandPreview(CommandPreview::Size::Full, scroll);
  scroll->setWidget(m_preview);
  h->addWidget(scroll, 1);
  connect(m_search, &QLineEdit::textChanged, this, &CommandReference::refill);
  connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
    const QString id = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (!id.isEmpty()) m_preview->setCommand(id, m_lookup ? m_lookup(id) : nullptr);
  });
  // Up and Down in the search field walk the list, as in the command palette.
  m_search->installEventFilter(this);
  auto restyle = [rule] { rule->setStyleSheet(QString("background: %1;").arg(theme::css(theme::current().line))); };
  restyle();
  connect(theme::notifier(), &theme::Notifier::changed, this, [this, restyle] { restyle(); refill(); });
  connect(keys::notifier(), &keys::Notifier::changed, this, &CommandReference::refill);  // the key column, and search by key, at once (the shown command stays)
  m_stale.setSingleShot(true);
  m_stale.setInterval(100);
  connect(&m_stale, &QTimer::timeout, this, [this] { if (isVisible()) updateItems(); });
  refill();
}

void CommandReference::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) close();
  else QWidget::keyPressEvent(e);
}

void CommandReference::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  updateItems();  // changed while it was closed
}

void CommandReference::updateItems() {
  const Tokens& t = theme::current();
  for (QTreeWidgetItemIterator it(m_list); *it; ++it) {
    const QString id = (*it)->data(0, Qt::UserRole).toString();
    QAction* a = id.isEmpty() || !m_lookup ? nullptr : m_lookup(id);
    if (!a) continue;
    (*it)->setText(1, keys::text(keys::binding(a)));
    if (a->isEnabled()) (*it)->setData(0, Qt::ForegroundRole, QVariant());
    else (*it)->setForeground(0, t.fg3);
  }
}

bool CommandReference::eventFilter(QObject* o, QEvent* e) {
  if (o == m_search && e->type() == QEvent::KeyPress) {
    const int key = static_cast<QKeyEvent*>(e)->key();
    if (key == Qt::Key_Down || key == Qt::Key_Up) {
      QTreeWidgetItem* at = m_list->currentItem();
      QTreeWidgetItem* next = at ? (key == Qt::Key_Down ? m_list->itemBelow(at) : m_list->itemAbove(at)) : m_list->topLevelItem(0);
      while (next && next->data(0, Qt::UserRole).toString().isEmpty()) next = key == Qt::Key_Down ? m_list->itemBelow(next) : m_list->itemAbove(next);
      if (next) m_list->setCurrentItem(next);
      return true;
    }
  }
  return QWidget::eventFilter(o, e);
}

void CommandReference::refill() {
  const Tokens& t = theme::current();
  const QString keep = current(), query = m_search->text().trimmed();
  const QSignalBlocker quiet(m_list);
  m_list->clear();
  QHash<QString, QTreeWidgetItem*> groups;
  for (const QString& area : help::areas()) {
    auto* g = new QTreeWidgetItem(m_list, {area});
    g->setFlags(Qt::ItemIsEnabled);
    QFont f = g->font(0);
    f.setWeight(QFont::DemiBold);
    g->setFont(0, f);
    g->setForeground(0, t.fg3);
    groups.insert(area, g);
  }
  QTreeWidgetItem* select = nullptr;
  for (const CommandHelp& h : help::all()) {
    QAction* a = m_lookup ? m_lookup(h.id) : nullptr;
    if (!listed(h) || (m_lookup && !a) || (!query.isEmpty() && !help::matches(h, query))) continue;  // not in this build: not listed
    auto* item = new QTreeWidgetItem(groups.value(help::group(h.id)), {h.title, keys::text(keys::binding(a))});
    item->setData(0, Qt::UserRole, h.id);
    item->setToolTip(0, help::expand(h.summary));
    item->setForeground(1, t.fg3);
    item->setFont(1, theme::mono(11));
    const QString icon = a ? a->data().toString() : QString();
    item->setIcon(0, icons::has(icon) ? icons::icon(icon, t.fg2) : QIcon());
    if (a && !a->isEnabled()) item->setForeground(0, t.fg3);  // not available now: its card says what it needs
    if (a) connect(a, &QAction::changed, &m_stale, qOverload<>(&QTimer::start), Qt::UniqueConnection);
  }
  for (QTreeWidgetItem* g : groups) g->setHidden(g->childCount() == 0);
  m_list->expandAll();
  for (QTreeWidgetItemIterator it(m_list); *it; ++it) {  // the command shown before, else the first one listed
    const QString id = (*it)->data(0, Qt::UserRole).toString();
    if (id.isEmpty() || (*it)->parent()->isHidden()) continue;
    if (!select) select = *it;
    if (id == keep) { select = *it; break; }
  }
  if (select) {
    m_list->setCurrentItem(select);
    m_list->scrollToItem(select);
    m_preview->setCommand(select->data(0, Qt::UserRole).toString(), m_lookup ? m_lookup(select->data(0, Qt::UserRole).toString()) : nullptr);
  }
}

QString CommandReference::current() const { return m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole).toString() : QString(); }

QStringList CommandReference::shown() const {
  QStringList out;
  for (QTreeWidgetItemIterator it(m_list); *it; ++it)
    if (const QString id = (*it)->data(0, Qt::UserRole).toString(); !id.isEmpty()) out << id;
  return out;
}

void CommandReference::setFilter(const QString& text) { m_search->setText(text); }

void CommandReference::open(const QString& id) {
  if (!id.isEmpty() && help::find(id)) {
    if (!shown().contains(id)) m_search->clear();
    for (QTreeWidgetItemIterator it(m_list); *it; ++it)
      if ((*it)->data(0, Qt::UserRole).toString() == id) {
        m_list->setCurrentItem(*it);
        m_list->scrollToItem(*it, QAbstractItemView::PositionAtCenter);
        break;
      }
  }
  show();
  raise();
  activateWindow();
  m_search->setFocus();
}
