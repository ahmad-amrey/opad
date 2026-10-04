#include "Ribbon.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QFrame>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "Icons.hpp"
#include "Theme.hpp"

// ---------------------------------------------------------------- RibbonLayout
QList<QAction*> RibbonLayout::Group::actions() const {
  QList<QAction*> out;
  for (const Item& item : items) out << item.action;
  return out;
}

RibbonLayout::Space& RibbonLayout::addWorkspace(const QString& id, const Workspace& w) {
  if (Space* s = workspace(id)) return *s;
  spaces.append(Space{id, w, {}});
  return spaces.last();
}

RibbonLayout::Tab* RibbonLayout::addTab(const QString& ws, const QString& id, const QString& title, const QList<QList<QAction*>>& groups) {
  Space* s = workspace(ws);
  if (!s) return nullptr;
  s->tabs.append(Tab{id, title, {}});
  Tab& t = s->tabs.last();
  for (const QList<QAction*>& actions : groups) {
    Group g;
    for (QAction* a : actions) g.items << Item{a};
    t.groups << g;
  }
  return &t;
}

RibbonLayout::Tab* RibbonLayout::addContextualTab(const QString& ws, const QString& id, const QString& title, QColor Tokens::* accent) {
  Tab* t = addTab(ws, id, title);
  if (t) {
    t->contextual = true;
    t->accent = accent;
  }
  return t;
}

bool RibbonLayout::addGroup(const QString& id, const QList<QAction*>& actions) {
  Tab* t = tab(id);
  if (!t) return false;
  Group g;
  for (QAction* a : actions) g.items << Item{a};
  t->groups << g;
  return true;
}

RibbonLayout::Group* RibbonLayout::addGroup(const QString& tabId, const QString& id, const QString& title) {
  if (Group* g = group(id)) return g;
  Tab* t = tab(tabId);
  if (!t) return nullptr;
  t->groups << Group{id, title, {}};
  return &t->groups.last();
}

bool RibbonLayout::addAction(const QString& id, QAction* action, Size size, const QList<QAction*>& variants, bool primary) {
  Group* g = group(id);
  if (g) g->items << Item{action, size, variants, primary};
  return g;
}

RibbonLayout::Space* RibbonLayout::workspace(const QString& id) {
  for (Space& s : spaces)
    if (s.id == id) return &s;
  return nullptr;
}

RibbonLayout::Tab* RibbonLayout::tab(const QString& id) {
  for (Space& s : spaces)
    for (Tab& t : s.tabs)
      if (t.id == id) return &t;
  return nullptr;
}

RibbonLayout::Group* RibbonLayout::group(const QString& id) {
  if (id.isEmpty()) return nullptr;
  for (Space& s : spaces)
    for (Tab& t : s.tabs)
      for (Group& g : t.groups)
        if (g.id == id) return &g;
  return nullptr;
}

int RibbonLayout::index(const QString& id) const {
  for (int i = 0; i < spaces.size(); ++i)
    if (spaces[i].id == id) return i;
  return -1;
}

namespace {
RibbonBar::CommandButtonHook& commandButtonHook() {
  static RibbonBar::CommandButtonHook hook;
  return hook;
}
}  // namespace

void RibbonBar::setCommandButtonHook(CommandButtonHook hook) { commandButtonHook() = std::move(hook); }

void RibbonBar::commandButton(QWidget* button, const QString& commandId) {
  if (commandButtonHook() && !commandId.isEmpty()) commandButtonHook()(button, commandId);
}

// ---------------------------------------------------------------- SegmentButton
SegmentButton::SegmentButton(QAction* action, const QString& hint, bool primary, QWidget* parent) : QToolButton(parent), m_hint(hint) {
  setObjectName(primary ? "segmentPrimary" : "segment");
  setDefaultAction(action);
  setToolButtonStyle(Qt::ToolButtonTextOnly);
  setCheckable(action->isCheckable());
  setAutoRaise(true);
  setFocusPolicy(Qt::NoFocus);
  setFont(theme::ui(12));
  RibbonBar::commandButton(this, action->objectName());
}

void SegmentButton::setIconOnly(bool on) {
  m_iconOnly = on;
  updateGeometry();
  update();
}

QSize SegmentButton::sizeHint() const {
  QFontMetrics fm(theme::ui(12)), mm(theme::mono(11));
  int w = 20 + (m_iconOnly ? 16 : fm.horizontalAdvance(text()));
  if (!m_hint.isEmpty()) w += (m_iconOnly ? 4 : 6) + mm.horizontalAdvance(m_hint);
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
  // Label (or icon) then key hint in reading order (mirrored in a right-to-left UI), both on one baseline: centring each
  // in its own rect put Arabic labels, drawn with a fallback font, off the digits' line.
  QFontMetrics fm(theme::ui(12)), mm(theme::mono(11));
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  p.setLayoutDirection(Qt::LeftToRight);  // positions below are absolute
  const int baseline = (height() + fm.ascent() - fm.descent()) / 2;
  const int textW = m_iconOnly ? 16 : fm.horizontalAdvance(text());
  const int hintW = m_hint.isEmpty() ? 0 : mm.horizontalAdvance(m_hint);
  const int gap = m_iconOnly ? 4 : 6;
  const int at = rtl ? width() - 10 - textW : 10;
  if (m_iconOnly) {
    const QString name = defaultAction() ? defaultAction()->data().toString() : QString();
    const QPixmap pix = name.isEmpty() ? icon().pixmap(QSize(16, 16), devicePixelRatioF(), isEnabled() ? QIcon::Normal : QIcon::Disabled)
                                       : icons::pixmap(name, textColor, 16, devicePixelRatioF());
    p.drawPixmap(at, (height() - 16) / 2, pix);
  } else {
    p.drawText(at, baseline, text());
  }
  if (!m_hint.isEmpty()) {
    p.setFont(theme::mono(11));
    p.setPen(isChecked() && primary ? t.onsel : t.fg3);
    p.drawText(rtl ? width() - 10 - textW - gap - hintW : 10 + textW + gap, baseline, m_hint);
  }
}

// ---------------------------------------------------------------- SearchField
SearchField::SearchField(QWidget* parent) : QAbstractButton(parent) {
  setFixedSize(sizeHint());
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::NoFocus);
  setToolTip(tr("Search commands (S)"));
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { setFixedSize(sizeHint()); });  // the text size
}

void SearchField::setCompact(bool on) {
  if (on == m_compact) return;
  m_compact = on;
  setFixedSize(sizeHint());
  update();
}

// As wide as its words at the text size (200 px at least), as high as a line of them (24 px at least).
QSize SearchField::sizeHint() const { return QSize(m_compact ? kCompact : fullWidth(), std::max(kHeight, QFontMetrics(theme::ui(12)).height() + 6)); }
int SearchField::fullWidth() const { return std::max(kFull, 30 + QFontMetrics(theme::ui(12)).horizontalAdvance(tr("Search commands")) + 34 + QFontMetrics(theme::mono(11)).horizontalAdvance("S")); }

void SearchField::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(underMouse() ? t.fg3 : t.line, 1));
  p.setBrush(t.bg);
  p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 3, 3);
  const int y = (height() - 16) / 2;
  if (m_compact) return p.drawPixmap((width() - 16) / 2, y, icons::pixmap("search", t.fg3, 16, devicePixelRatioF()));
  // Icon, placeholder, key badge in reading order; positions are absolute, so mirror them by hand.
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  p.setLayoutDirection(Qt::LeftToRight);
  p.drawPixmap(rtl ? width() - 24 : 8, y, icons::pixmap("search", t.fg3, 16, devicePixelRatioF()));
  p.setFont(theme::ui(12));
  p.setPen(t.fg3);
  p.drawText(QRect(rtl ? 34 : 30, 0, width() - 64, height()), Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), tr("Search commands"));
  const QFontMetrics mm(theme::mono(11));
  const QSize keySize(std::max(18, mm.horizontalAdvance("S") + 8), std::max(16, mm.height()));
  QRect key(QPoint(rtl ? 6 : width() - keySize.width() - 6, (height() - keySize.height()) / 2), keySize);
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg4);
  p.drawRoundedRect(key, 3, 3);
  p.setFont(theme::mono(11));
  p.setPen(t.fg2);
  p.drawText(key, Qt::AlignCenter, "S");
}

// ---------------------------------------------------------------- WorkspaceChip
WorkspaceChip::WorkspaceChip(QWidget* parent) : QAbstractButton(parent) {
  setFixedHeight(sizeHint().height());
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { setFixedHeight(sizeHint().height()); });
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::NoFocus);
}

void WorkspaceChip::setWorkspace(const Workspace& w) {
  m_ws = w;
  updateGeometry();
  update();
}

QSize WorkspaceChip::sizeHint() const {
  QFontMetrics fm(theme::ui(13, QFont::Medium)), mm(theme::mono(11));
  return QSize(8 + 16 + 8 + fm.horizontalAdvance(m_ws.name) + 8 + mm.horizontalAdvance(m_ws.key) + 8 + 12 + 10, std::max(26, fm.height() + 4));
}

void WorkspaceChip::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setLayoutDirection(Qt::LeftToRight);  // positions are absolute; mirrored by hand below
  p.setPen(QPen(t.line, 1));
  p.setBrush(underMouse() || isDown() ? t.bg3 : t.bg2);
  p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 3, 3);
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  QFont nameFont = theme::ui(13, QFont::Medium);
  QFontMetrics fm(nameFont), mm(theme::mono(11));
  const int baseline = (height() + fm.ascent() - fm.descent()) / 2;
  int x = 8;  // distance from the leading edge
  auto place = [&](int w) {
    const int at = rtl ? width() - x - w : x;
    x += w + 8;
    return at;
  };
  p.drawPixmap(place(16), (height() - 16) / 2, icons::pixmap(m_ws.icon, isEnabled() ? t.sel : t.fg3, 16, devicePixelRatioF()));
  p.setFont(nameFont);
  p.setPen(isEnabled() ? t.fg : t.fg3);
  p.drawText(place(fm.horizontalAdvance(m_ws.name)), baseline, m_ws.name);
  p.setFont(theme::mono(11));
  p.setPen(t.fg3);
  p.drawText(place(mm.horizontalAdvance(m_ws.key)), baseline, m_ws.key);
  p.drawPixmap(place(12), (height() - 12) / 2, icons::pixmap("chevronDown", t.fg3, 12, devicePixelRatioF()));
}

// ---------------------------------------------------------------- workspace list (dropdown under the chip)
// 372 px, bg3, 4 px padding: one row per workspace (icon, name 500, mono key, description 12 px, op types 11 px,
// check on the active one, which is filled sel).
namespace {
class WorkspaceRow : public QFrame {
 public:
  WorkspaceRow(const Workspace& w, bool active, std::function<void()> chosen, QWidget* parent) : QFrame(parent), m_chosen(std::move(chosen)) {
    const Tokens& t = theme::current();
    setObjectName("wsRow");
    setCursor(Qt::PointingHandCursor);
    const QString fg = theme::css(active ? t.onsel : t.fg), sub = theme::css(active ? t.onsel : t.fg2), key = theme::css(active ? t.onsel : t.fg3);
    setStyleSheet(QString("QFrame#wsRow { background: %1; border-radius: 2px; } QFrame#wsRow:hover { background: %2; } QLabel { background: transparent; }")
                      .arg(active ? theme::css(t.sel) : "transparent", theme::css(active ? t.sel : t.bg4)));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 8, 8, 8);
    row->setSpacing(10);
    auto* icon = new QLabel(this);
    icon->setPixmap(icons::pixmap(w.icon, active ? t.onsel : t.fg2, 16, devicePixelRatioF()));
    row->addWidget(icon, 0, Qt::AlignTop);
    auto* col = new QVBoxLayout();
    col->setSpacing(3);
    auto* head = new QHBoxLayout();
    auto* name = new QLabel(w.name, this);
    name->setStyleSheet(QString("color: %1; font-weight: 500;").arg(fg));
    auto* k = new QLabel(w.key, this);
    k->setFont(theme::mono(11));
    k->setStyleSheet(QString("color: %1;").arg(key));
    head->addWidget(name, 1);
    head->addWidget(k);
    col->addLayout(head);
    // Wrapped labels in a popup sized by adjustSize() report one line's height, so give them their width and height.
    auto wrapped = [&](const QString& text, int px) {
      auto* l = new QLabel(text, this);
      l->setWordWrap(true);
      l->setFont(theme::ui(px));
      l->setStyleSheet(QString("color: %1; font-size: %2px;").arg(sub).arg(theme::px(px)));
      const int width = 372 - 10 - 16 - 16 - 10 - 10 - 12;  // popup minus padding, row padding, icon, gaps, check
      l->setFixedWidth(width);
      l->setFixedHeight(QFontMetrics(theme::ui(px)).boundingRect(QRect(0, 0, width, 1000), Qt::TextWordWrap, text).height() + 2);
      col->addWidget(l);
    };
    wrapped(w.description, 12);
    wrapped(w.ops, 11);
    row->addLayout(col, 1);
    auto* check = new QLabel(this);
    check->setFixedWidth(12);
    if (active) check->setPixmap(icons::pixmap("check", t.onsel, 12, devicePixelRatioF()));
    row->addWidget(check, 0, Qt::AlignTop);
  }
 protected:
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())) m_chosen();
  }
 private:
  std::function<void()> m_chosen;
};
}  // namespace

void RibbonBar::showWorkspaceMenu() {
  const Tokens& t = theme::current();
  auto* menu = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  menu->setObjectName("wsMenu");
  menu->setStyleSheet(QString("QFrame#wsMenu { background: %1; border: 1px solid %2; border-radius: 3px; }").arg(theme::css(t.bg3), theme::css(t.line)));
  menu->setFixedWidth(372);
  auto* col = new QVBoxLayout(menu);
  col->setContentsMargins(5, 5, 5, 5);  // 4 px padding + the border
  col->setSpacing(2);
  for (int i = 0; i < m_workspaces.size(); ++i)
    if (!m_workspaces[i].contextual) col->addWidget(new WorkspaceRow(m_workspaces[i], i == m_workspace, [this, menu, i] {
      menu->close();
      setWorkspace(i);
    }, menu));
  menu->adjustSize();
  menu->setFixedSize(menu->size());  // a popup has no business being resized from outside
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  const QPoint under = m_chip->mapToGlobal(QPoint(rtl ? m_chip->width() - menu->width() : 0, m_chip->height() + 4));
  menu->move(under);
  menu->show();
}

// ---------------------------------------------------------------- RibbonGroup
namespace {
const QString kDrop = QString::fromUtf8(" ▾");
constexpr int kInner = 2, kGap = 4;
}  // namespace

RibbonGroup::RibbonGroup(const RibbonLayout::Group& group, QWidget* parent) : QWidget(parent), m_title(group.title) {
  m_menu = new QMenu(this);
  auto tool = [this] {
    auto* b = new QToolButton(this);
    b->setObjectName("ribbonTool");
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setFont(theme::ui(11));
    return b;
  };
  for (const RibbonLayout::Item& item : group.items) {
    QAction* a = item.action;
    if (!a) continue;
    QToolButton* b = tool();
    if (QMenu* menu = a->menu()) {
      // A menu button drops the action's menu down itself. As the button's default action it showed Qt's fallback:
      // a menu with one entry, the action, whose submenu then held the tools.
      auto sync = [a, b] {
        b->setText(a->iconText() + kDrop);  // as a tool button shows its action: the short name (its menu entry keeps the long one)
        b->setIcon(a->icon());
        b->setToolTip(a->toolTip());
        b->setEnabled(a->isEnabled());
      };
      sync();
      connect(a, &QAction::changed, b, sync);
      connect(b, &QToolButton::clicked, b, [b, menu] { menu->popup(b->mapToGlobal(QPoint(0, b->height()))); });
    } else {
      b->setDefaultAction(a);
      QList<QAction*> variants = item.variants;
      variants.removeAll(nullptr);
      if (!variants.isEmpty()) {  // a split button: the arrow drops the variants
        auto* drop = new QMenu(b);
        drop->addActions(variants);
        b->setMenu(drop);
        b->setPopupMode(QToolButton::MenuButtonPopup);
      }
      if (item.primary) {  // the tab's main verb: filled in the accent colour, its icon drawn in the text colour on it
        b->setProperty("ribbonPrimary", true);
        auto paint = [a, b] {
          const Tokens& t = theme::current();
          b->setIcon(icons::icon(a->data().toString(), t.onsel, QColor(t.onsel.red(), t.onsel.green(), t.onsel.blue(), 140)));
        };
        connect(a, &QAction::changed, b, paint);  // after the button took the action's own icon again
        connect(theme::notifier(), &theme::Notifier::changed, b, paint, Qt::QueuedConnection);  // after the window's icons
        paint();
      }
    }
    RibbonBar::commandButton(b, a->objectName());  // disabled buttons and menu buttons included
    m_menu->addAction(a);
    for (QAction* v : item.variants)
      if (v) m_menu->addAction(v);
    connect(a, &QAction::changed, this, &RibbonGroup::actionChanged);
    if (item.size == RibbonLayout::Size::Icon) b->setAccessibleName(a->iconText());
    m_slots << Slot{a, item.size, b, item.primary};
  }
  m_titleButton = new QToolButton(this);
  m_titleButton->setObjectName("ribbonGroupTitle");
  m_titleButton->setText(m_title.toUpper() + kDrop);
  m_titleButton->setToolTip(tr("Every tool of %1").arg(m_title));
  m_titleButton->setFocusPolicy(Qt::NoFocus);
  m_titleButton->setCursor(Qt::PointingHandCursor);
  m_titleButton->setVisible(!m_title.isEmpty());
  m_collapsed = tool();
  m_collapsed->setText((m_title.isEmpty() ? tr("More") : m_title) + kDrop);
  m_collapsed->hide();
  for (QToolButton* b : {m_titleButton, m_collapsed})
    connect(b, &QToolButton::clicked, this, [this, b] { m_menu->popup(b->mapToGlobal(QPoint(0, b->height()))); });
  m_probe = tool();
  m_probe->hide();
  style(m_collapsed, Large);
  if (!m_slots.isEmpty()) m_collapsed->setIcon(m_slots.first().action->icon());  // measured as it will show
}

int RibbonGroup::toolRow() { return theme::px(kRow); }
int RibbonGroup::toolsBox() { return std::max(kTools, 3 * toolRow() + 2); }
int RibbonGroup::titleBox() { return theme::px(kTitle); }
int RibbonGroup::stripHeight() { return kTop + toolsBox() + titleBox() + 4; }

QList<QToolButton*> RibbonGroup::buttons() const {
  QList<QToolButton*> out;
  for (const Slot& s : m_slots) out << s.button;
  return out;
}

void RibbonGroup::style(QToolButton* b, int mode) const {
  const QString size = mode == Large ? QString() : QStringLiteral("small");
  if (b->property("ribbonSize").toString() != size) {
    b->setProperty("ribbonSize", size);
    b->style()->unpolish(b);
    b->style()->polish(b);
  }
  b->setToolButtonStyle(mode == Large ? Qt::ToolButtonTextUnderIcon : mode == Small ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
  b->setIconSize(mode == Large ? QSize(24, 24) : QSize(16, 16));
}

// The size a tool asks for at a level, measured on the hidden probe styled the same way (the tools stay as they are).
QSize RibbonGroup::measure(const Slot& s, int mode) {
  style(m_probe, mode);
  m_probe->setMenu(s.button->menu());  // a split button's arrow is part of its size
  m_probe->setPopupMode(s.button->popupMode());
  m_probe->setIcon(s.button->icon());
  m_probe->setText(QString());  // a changed text drops the cached size hint
  m_probe->setText(s.button->text());
  return m_probe->sizeHint();
}

int RibbonGroup::nextLevel(int level) {
  const bool pinned = std::any_of(m_slots.begin(), m_slots.end(), [](const Slot& s) { return s.pinned; });
  for (int next = level + 1; next <= (pinned ? Icons : Collapsed); ++next)
    if (widthAt(next) < widthAt(level)) return next;
  return -1;
}

int RibbonGroup::modeAt(const Slot& s, int level) const {
  if (s.size == RibbonLayout::Size::Icon) return Icons;
  if (s.pinned) return s.size == RibbonLayout::Size::Large ? Large : Small;
  return level == Icons ? Icons : level == Large && s.size == RibbonLayout::Size::Large ? Large : Small;
}

int RibbonGroup::widthAt(int level) {
  QString signature;
  for (const Slot& s : m_slots) signature += (s.action->isVisible() ? "+" : "-") + s.button->text() + '\n';
  if (signature != m_signature) {
    m_signature = signature;
    m_widths.fill(-1);
  }
  if (m_widths[level] >= 0) return m_widths[level];
  int width = 0;
  if (level == Collapsed) {
    width = m_collapsed->sizeHint().width();
  } else {
    int stacked = 0, column = 0;
    for (const Slot& s : m_slots) {
      if (!s.action->isVisible()) continue;
      const int mode = modeAt(s, level);
      const int w = measure(s, mode).width();
      if (mode == Large) {
        if (stacked) width += column + kGap;
        stacked = column = 0;
        width += w + kGap;
      } else {
        column = std::max(column, w);
        if (++stacked == 3) {
          width += column + kGap;
          stacked = column = 0;
        }
      }
    }
    if (stacked) width += column + kGap;
    width = std::max(0, width - kGap);
    if (!m_title.isEmpty()) width = std::max(width, m_titleButton->sizeHint().width());
  }
  m_probe->setMenu(nullptr);
  return m_widths[level] = width + 2 * kInner;
}

void RibbonGroup::setLevel(int level) {
  m_level = level;
  const int width = widthAt(level);
  resize(width, stripHeight());
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  auto place = [&](QWidget* w, const QRect& r) { w->setGeometry(rtl ? QRect(width - r.right() - 1, r.y(), r.width(), r.height()) : r); };
  const bool collapsed = level == Collapsed;
  m_collapsed->setVisible(collapsed);
  m_titleButton->setVisible(!collapsed && !m_title.isEmpty());
  if (collapsed) {
    for (const Slot& s : m_slots) s.button->hide();
    for (const Slot& s : m_slots)
      if (s.action->isVisible()) {
        m_collapsed->setIcon(s.action->icon());
        break;
      }
    place(m_collapsed, QRect(kInner, kTop, width - 2 * kInner, toolsBox() + titleBox()));
    return;
  }
  // Large tools one per column, small ones three to a column (as wide as the widest of them), centred in the group.
  QList<QPair<QWidget*, QRect>> boxes;
  QList<QToolButton*> stack;
  int x = 0, column = 0;
  auto flush = [&] {
    for (int r = 0; r < stack.size(); ++r) boxes << qMakePair(static_cast<QWidget*>(stack[r]), QRect(x, kTop + 1 + r * toolRow(), column, toolRow()));
    if (!stack.isEmpty()) x += column + kGap;
    stack.clear();
    column = 0;
  };
  for (const Slot& s : m_slots) {
    s.button->setVisible(s.action->isVisible());
    if (!s.action->isVisible()) continue;
    const int mode = modeAt(s, level);
    style(s.button, mode);
    const int w = s.button->sizeHint().width();
    if (mode == Large) {
      flush();
      boxes << qMakePair(static_cast<QWidget*>(s.button), QRect(x, kTop, w, toolsBox()));
      x += w + kGap;
    } else {
      stack << s.button;
      column = std::max(column, w);
      if (stack.size() == 3) flush();
    }
  }
  flush();
  const int offset = kInner + std::max(0, (width - 2 * kInner - std::max(0, x - kGap)) / 2);
  for (const auto& [w, r] : boxes) place(w, r.translated(offset, 0));
  if (!m_title.isEmpty()) place(m_titleButton, QRect(kInner, kTop + toolsBox(), width - 2 * kInner, titleBox()));
}

void RibbonGroup::actionChanged() {
  QString signature;
  for (const Slot& s : m_slots) signature += (s.action->isVisible() ? "+" : "-") + s.button->text() + '\n';
  if (signature != m_signature) emit widthsChanged();  // widthAt measures again
}

// ---------------------------------------------------------------- RibbonPage
RibbonPage::RibbonPage(const RibbonLayout::Tab& tab, QWidget* parent) : QWidget(parent), m_id(tab.id) {
  for (const RibbonLayout::Group& g : tab.groups) {
    if (std::none_of(g.items.begin(), g.items.end(), [](const RibbonLayout::Item& i) { return i.action; })) continue;
    auto* group = new RibbonGroup(g, this);
    connect(group, &RibbonGroup::widthsChanged, this, &RibbonPage::fit);
    m_groups << group;
  }
  setFixedHeight(RibbonGroup::stripHeight());
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] {  // the text size
    setFixedHeight(RibbonGroup::stripHeight());
    for (RibbonGroup* g : m_groups) g->remeasure();
    fit();
  });
}

QList<int> RibbonPage::levels() const {
  QList<int> out;
  for (RibbonGroup* g : m_groups) out << g->level();
  return out;
}

int RibbonPage::widthAt(const QList<int>& levels) {
  int width = 2 * kPad + kSeparator * std::max<int>(0, m_groups.size() - 1);
  for (int i = 0; i < m_groups.size(); ++i) width += m_groups[i]->widthAt(levels[i]);
  return width;
}

QSize RibbonPage::minimumSizeHint() const {
  return QSize(const_cast<RibbonPage*>(this)->widthAt(QList<int>(m_groups.size(), RibbonGroup::Collapsed)), RibbonGroup::stripHeight());
}

void RibbonPage::fit() {
  QList<int> levels(m_groups.size(), RibbonGroup::Large), steps(m_groups.size(), 0);
  while (widthAt(levels) > width()) {
    int step = -1;  // the rightmost of the groups that took the fewest steps and can still save room
    for (int i = static_cast<int>(m_groups.size()) - 1; i >= 0; --i)
      if (m_groups[i]->nextLevel(levels[i]) >= 0 && (step < 0 || steps[i] < steps[step])) step = i;
    if (step < 0) break;
    levels[step] = m_groups[step]->nextLevel(levels[step]);
    ++steps[step];
  }
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  int x = kPad;
  for (int i = 0; i < m_groups.size(); ++i) {
    RibbonGroup* g = m_groups[i];
    g->setLevel(levels[i]);
    g->move(rtl ? width() - x - g->width() : x, 0);
    x += g->width() + kSeparator;
  }
  update();
}

void RibbonPage::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  fit();
}

void RibbonPage::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setPen(QPen(theme::current().line, 1));
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  for (int i = 0; i + 1 < m_groups.size(); ++i) {
    const QRect g = m_groups[i]->geometry();
    const int x = rtl ? g.left() - kSeparator / 2 - 1 : g.right() + 1 + kSeparator / 2;
    p.drawLine(x, RibbonGroup::kTop + 8, x, RibbonGroup::kTop + RibbonGroup::toolsBox() + RibbonGroup::titleBox() - 6);
  }
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
  m_tabs->setElideMode(Qt::ElideNone);
  m_tabs->setFixedHeight(theme::px(28));  // taller at a larger text size (UI-124)
  m_tabs->setFocusPolicy(Qt::NoFocus);
  m_tabs->setIconSize(QSize(8, 8));
  m_tabRow = new QWidget(this);
  m_tabRow->setObjectName("ribbonTabRow");
  auto* tabLayout = new QHBoxLayout(m_tabRow);
  tabLayout->setContentsMargins(kRowMargin, 0, kRowMargin, 0);
  tabLayout->setSpacing(0);
  m_chip = new WorkspaceChip(m_tabRow);
  m_chip->hide();  // until a workspace is added
  connect(m_chip, &QAbstractButton::clicked, this, &RibbonBar::showWorkspaceMenu);
  tabLayout->addWidget(m_chip);
  tabLayout->addSpacing(kChipGap);
  tabLayout->addWidget(m_tabs);
  tabLayout->addStretch();
  tabLayout->addSpacing(kClusterGap);
  m_cluster = new QWidget(m_tabRow);
  m_cluster->setObjectName("ribbonCluster");
  auto* cluster = new QHBoxLayout(m_cluster);
  cluster->setContentsMargins(0, 0, 0, 0);
  cluster->setSpacing(8);
  for (QHBoxLayout** part : {&m_quick, &m_searchSlot, &m_corner, &m_settingsSlot}) {
    *part = new QHBoxLayout();
    (*part)->setContentsMargins(0, 0, 0, 0);
    (*part)->setSpacing(part == &m_quick ? 2 : 8);
    cluster->addLayout(*part);
  }
  tabLayout->addWidget(m_cluster);
  layout->addWidget(m_tabRow);

  m_strip = new QWidget(this);
  m_strip->setObjectName("ribbonStrip");
  m_strip->setFixedHeight(RibbonGroup::stripHeight());
  m_stripLayout = new QHBoxLayout(m_strip);
  m_stripLayout->setContentsMargins(4, 0, 8, 0);
  m_stripLayout->setSpacing(4);
  m_stack = new QStackedWidget(m_strip);
  m_stripLayout->addWidget(m_stack, 1);
  m_right = new QHBoxLayout();
  m_right->setContentsMargins(0, 0, 0, 0);
  m_right->setSpacing(4);
  m_stripLayout->addLayout(m_right);
  layout->addWidget(m_strip);
  connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
    if (m_filling || m_workspace < 0 || i < 0) return;
    Tabs& set = m_tabSets[m_workspace];
    const Entry& e = set.entries[set.row[i]];
    set.current = e.id;
    m_stack->setCurrentWidget(e.page);
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
    m_tabs->setFixedHeight(theme::px(28));
    m_strip->setFixedHeight(RibbonGroup::stripHeight());
    refillTabs();  // the contextual tabs' accent
  });
}

void RibbonBar::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  fitTabRow();
}

// The cluster keeps its size and the tabs theirs (never elided): search gives up its field first.
void RibbonBar::fitTabRow() {
  if (!m_search) return;
  const int tabs = 2 * kRowMargin + (m_chip->isVisibleTo(this) ? m_chip->sizeHint().width() + kChipGap : 0) + m_tabs->sizeHint().width() + kClusterGap;
  const int cluster = m_cluster->sizeHint().width() - m_search->sizeHint().width() + m_search->fullWidth();
  if (m_search->compact() == tabs + cluster > width()) return;
  m_search->setCompact(!m_search->compact());
  m_cluster->layout()->activate();  // now, not a frame later with the tabs under the cluster
  m_tabRow->layout()->activate();
}

int RibbonBar::addWorkspace(const Workspace& w) {
  m_workspaces << w;
  m_tabSets << Tabs();
  return static_cast<int>(m_workspaces.size()) - 1;
}

void RibbonBar::setWorkspace(int index) {
  if (index < 0 || index >= m_workspaces.size() || index == m_workspace) return;
  m_workspace = index;
  refillTabs();
  m_chip->setWorkspace(m_workspaces[index]);
  m_chip->setToolTip(m_workspaces[index].description);
  m_chip->show();
  fitTabRow();
  emit workspaceChanged(index);
}

int RibbonBar::addTab(int workspace, const RibbonLayout::Tab& tab) {
  auto* page = new RibbonPage(tab, m_stack);
  m_stack->addWidget(page);
  Tabs& set = m_tabSets[workspace];
  set.entries << Entry{tab.id, tab.title, page, tab.contextual, !tab.contextual, tab.accent};
  if (set.current.isEmpty() && !tab.contextual) set.current = tab.id;
  if (workspace == m_workspace) refillTabs();
  return static_cast<int>(set.entries.size()) - 1;
}

void RibbonBar::refillTabs() {
  if (m_workspace < 0) return;
  Tabs& set = m_tabSets[m_workspace];
  m_filling = true;  // the tab bar's signals while it is refilled are not the user's
  while (m_tabs->count() > 0) m_tabs->removeTab(0);
  set.row.clear();
  for (const bool contextual : {true, false})
    for (int i = 0; i < set.entries.size(); ++i) {
      const Entry& e = set.entries[i];
      if (e.contextual != contextual || !e.shown) continue;
      const int at = m_tabs->addTab(e.title);
      if (e.contextual) {  // a dot in its accent before the title, and the title in it where the style lets it
        const QColor accent = theme::current().*(e.accent ? e.accent : &Tokens::amber);
        QPixmap dot(QSize(8, 8) * devicePixelRatioF());
        dot.setDevicePixelRatio(devicePixelRatioF());
        dot.fill(Qt::transparent);
        QPainter p(&dot);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(QRectF(0.5, 0.5, 7, 7));
        p.end();
        m_tabs->setTabIcon(at, QIcon(dot));
        m_tabs->setTabTextColor(at, accent);
      }
      set.row << i;
    }
  int current = 0;
  for (int k = 0; k < set.row.size(); ++k)
    if (set.entries[set.row[k]].id == set.current) current = k;
  if (!set.row.isEmpty()) {
    m_tabs->setCurrentIndex(current);
    set.current = set.entries[set.row[current]].id;
    m_stack->setCurrentWidget(set.entries[set.row[current]].page);
  }
  m_filling = false;
  m_tabs->updateGeometry();  // QTabBar does not while hidden, and the row would keep its old width
  fitTabRow();
}

bool RibbonBar::setContextualTab(const QString& id, bool shown) {
  for (int w = 0; w < m_tabSets.size(); ++w) {
    Tabs& set = m_tabSets[w];
    for (Entry& e : set.entries) {
      if (e.id != id || !e.contextual) continue;
      if (e.shown != shown) {
        e.shown = shown;
        const bool currentIsContextual = std::any_of(set.entries.begin(), set.entries.end(), [&set](const Entry& o) { return o.id == set.current && o.contextual; });
        if (shown) {
          if (!currentIsContextual) set.beforeContextual = set.current;
          set.current = id;
        } else if (set.current == id) {
          set.current = set.beforeContextual;
        }
      }
      if (w == m_workspace) refillTabs();
      return true;
    }
  }
  return false;
}

QStringList RibbonBar::contextualTabs(int workspace) const {
  QStringList out;
  if (workspace >= 0 && workspace < m_tabSets.size())
    for (const Entry& e : m_tabSets[workspace].entries)
      if (e.contextual) out << e.id;
  return out;
}

bool RibbonBar::contextualTabShown(const QString& id) const {
  for (const Tabs& set : m_tabSets)
    for (const Entry& e : set.entries)
      if (e.id == id && e.contextual) return e.shown;
  return false;
}

QStringList RibbonBar::tabIds() const {
  QStringList out;
  if (m_workspace >= 0)
    for (const int i : m_tabSets[m_workspace].row) out << m_tabSets[m_workspace].entries[i].id;
  return out;
}

RibbonPage* RibbonBar::page(const QString& tabId) const {
  for (const Tabs& set : m_tabSets)
    for (const Entry& e : set.entries)
      if (e.id == tabId) return e.page;
  return nullptr;
}

RibbonPage* RibbonBar::currentPage() const { return qobject_cast<RibbonPage*>(m_stack->currentWidget()); }

void RibbonBar::setSelectFilters(const QList<QAction*>& filters, const QStringList& hints, QMenu* more) {
  m_selectButton = new QToolButton(m_strip);
  m_selectButton->setObjectName("ribbonSelect");
  m_selectButton->setText(tr("Select") + kDrop);
  m_selectButton->setFocusPolicy(Qt::NoFocus);
  m_selectButton->setCursor(Qt::PointingHandCursor);
  if (more) {
    m_selectButton->setMenu(more);
    m_selectButton->setPopupMode(QToolButton::InstantPopup);
  }
  m_right->addWidget(m_selectButton);
  auto* seg = new QWidget(m_strip);
  seg->setObjectName("segmented");
  seg->setFixedHeight(28);
  auto* l = new QHBoxLayout(seg);
  l->setContentsMargins(1, 1, 1, 1);
  l->setSpacing(0);
  for (int i = 0; i < filters.size(); ++i) {
    auto* b = new SegmentButton(filters[i], i < hints.size() ? hints[i] : QString(), false, seg);
    b->setIconOnly(true);
    l->addWidget(b);
  }
  m_right->addWidget(seg);
}

QToolButton* RibbonBar::addQuickAction(QAction* a, QMenu* steps) {
  auto* b = new QToolButton(m_cluster);
  b->setObjectName("ribbonQuick");
  b->setDefaultAction(a);
  b->setToolButtonStyle(Qt::ToolButtonIconOnly);
  b->setIconSize(QSize(16, 16));
  b->setAutoRaise(true);
  b->setFocusPolicy(Qt::NoFocus);
  if (steps) {
    b->setMenu(steps);
    b->setPopupMode(QToolButton::MenuButtonPopup);
  }
  b->setFixedSize(steps ? 38 : 24, 24);
  commandButton(b, a->objectName());
  m_quick->addWidget(b);
  fitTabRow();
  return b;
}

void RibbonBar::addTabRowWidget(QWidget* w) {
  w->setParent(m_cluster);
  m_corner->addWidget(w);
  fitTabRow();
}

void RibbonBar::setSearchAction(QAction* a) {
  m_search = new SearchField(m_cluster);
  connect(m_search, &QAbstractButton::clicked, a, &QAction::trigger);
  commandButton(m_search, a->objectName());
  m_searchSlot->addWidget(m_search);
  fitTabRow();
}

void RibbonBar::setSettingsMenu(QAction* a, QMenu* menu) {
  auto* b = new QToolButton(m_cluster);
  b->setObjectName("ribbonSettings");
  b->setDefaultAction(a);
  commandButton(b, a->objectName());
  b->setToolButtonStyle(Qt::ToolButtonIconOnly);
  b->setIconSize(QSize(18, 18));
  b->setAutoRaise(true);
  b->setFixedSize(24, 24);
  b->setFocusPolicy(Qt::NoFocus);
  if (menu) {
    b->setPopupMode(QToolButton::InstantPopup);
    b->setMenu(menu);
  }
  m_settingsSlot->addWidget(b);
  fitTabRow();
}
