#include "Ribbon.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QFrame>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionButton>
#include <QVBoxLayout>

#include <functional>

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
  // Label then key hint in reading order (mirrored in a right-to-left UI), both on one baseline: centring each
  // in its own rect put Arabic labels, drawn with a fallback font, off the digits' line.
  QFontMetrics fm(theme::ui(12)), mm(theme::mono(11));
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  p.setLayoutDirection(Qt::LeftToRight);  // positions below are absolute
  const int baseline = (height() + fm.ascent() - fm.descent()) / 2;
  const int textW = fm.horizontalAdvance(text());
  const int hintW = m_hint.isEmpty() ? 0 : mm.horizontalAdvance(m_hint);
  p.drawText(rtl ? width() - 10 - textW : 10, baseline, text());
  if (!m_hint.isEmpty()) {
    p.setFont(theme::mono(11));
    p.setPen(isChecked() && primary ? t.onsel : t.fg3);
    p.drawText(rtl ? width() - 10 - textW - 6 - hintW : 10 + textW + 6, baseline, m_hint);
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
  // Icon, placeholder, key badge in reading order; positions are absolute, so mirror them by hand.
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  p.setLayoutDirection(Qt::LeftToRight);
  p.drawPixmap(rtl ? width() - 24 : 8, 6, icons::pixmap("search", t.fg3, 16, devicePixelRatioF()));
  p.setFont(theme::ui(12));
  p.setPen(t.fg3);
  p.drawText(QRect(rtl ? width() - 160 : 30, 0, 130, height()), Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), tr("Search commands"));
  QRect key(rtl ? 8 : width() - 26, 6, 18, 16);
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg4);
  p.drawRoundedRect(key, 3, 3);
  p.setFont(theme::mono(11));
  p.setPen(t.fg2);
  p.drawText(key, Qt::AlignCenter, "S");
}

// ---------------------------------------------------------------- WorkspaceChip
WorkspaceChip::WorkspaceChip(QWidget* parent) : QAbstractButton(parent) {
  setFixedHeight(26);
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
  return QSize(8 + 16 + 8 + fm.horizontalAdvance(m_ws.name) + 8 + mm.horizontalAdvance(m_ws.key) + 8 + 12 + 10, 26);
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
  p.drawPixmap(place(16), 5, icons::pixmap(m_ws.icon, isEnabled() ? t.sel : t.fg3, 16, devicePixelRatioF()));
  p.setFont(nameFont);
  p.setPen(isEnabled() ? t.fg : t.fg3);
  p.drawText(place(fm.horizontalAdvance(m_ws.name)), baseline, m_ws.name);
  p.setFont(theme::mono(11));
  p.setPen(t.fg3);
  p.drawText(place(mm.horizontalAdvance(m_ws.key)), baseline, m_ws.key);
  p.drawPixmap(place(12), 7, icons::pixmap("chevronDown", t.fg3, 12, devicePixelRatioF()));
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
      l->setStyleSheet(QString("color: %1; font-size: %2px;").arg(sub).arg(px));
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
  tabLayout->setSpacing(0);
  m_chip = new WorkspaceChip(tabRow);
  m_chip->hide();  // until a workspace is added
  connect(m_chip, &QAbstractButton::clicked, this, &RibbonBar::showWorkspaceMenu);
  tabLayout->addWidget(m_chip);
  tabLayout->addSpacing(12);
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
  connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
    if (m_workspace < 0 || i < 0) return;
    Tabs& set = m_tabSets[m_workspace];
    set.current = i;
    m_stack->setCurrentIndex(set.pages[i]);
  });
}

int RibbonBar::addWorkspace(const Workspace& w) {
  m_workspaces << w;
  m_tabSets << Tabs();
  return static_cast<int>(m_workspaces.size()) - 1;
}

void RibbonBar::setWorkspace(int index) {
  if (index < 0 || index >= m_workspaces.size() || index == m_workspace) return;
  m_workspace = -1;  // the tab bar's signals while it is refilled are not the user's
  while (m_tabs->count() > 0) m_tabs->removeTab(0);
  const Tabs& set = m_tabSets[index];
  for (const QString& title : set.titles) m_tabs->addTab(title);
  m_workspace = index;
  m_tabs->setCurrentIndex(set.current);
  if (!set.pages.isEmpty()) m_stack->setCurrentIndex(set.pages[set.current]);
  m_chip->setWorkspace(m_workspaces[index]);
  m_chip->setToolTip(m_workspaces[index].description);
  m_chip->show();
  emit workspaceChanged(index);
}

int RibbonBar::addTab(int workspace, const QString& title, const QList<QList<QAction*>>& groups) {
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
  Tabs& set = m_tabSets[workspace];
  set.titles << title;
  set.pages << m_stack->addWidget(page);
  return static_cast<int>(set.titles.size()) - 1;
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
