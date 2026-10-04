#include "RichTip.hpp"

#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "Theme.hpp"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QVariantAnimation>
#include <algorithm>

namespace {
QPointer<RichTip> g_tip;
std::function<QAction*(const QString&)> g_lookup;
RichTip::ClipFactory g_clips;
std::function<bool(const QString&)> g_hasClip;
bool g_menus = false;
std::function<void(const QString&)> g_guide;
constexpr int kPad = 12, kMinText = 200, kMaxText = 336, kShowMs = 450, kGraceMs = 300, kGrowMs = 120;

// The key that expands a card and opens the guide from it: Help for this tool's, whatever the user made it (one chord).
bool helpKey(const QKeyEvent* e) {
  const QKeySequence help = keys::binding(QStringLiteral("help.current"));
  return help.count() == 1 && QKeySequence(e->keyCombination()) == help;
}

QFont titleFont() { return theme::ui(13, QFont::Medium); }
QFont bodyFont() { return theme::ui(12); }
QFont hintFont() { return theme::ui(11); }
int textHeight(const QFont& f, int width, const QString& text) {
  return text.isEmpty() ? 0 : QFontMetrics(f).boundingRect(QRect(0, 0, width, 100000), Qt::TextWordWrap, text).height();
}
}  // namespace

RichTip::RichTip(QWidget* owner) : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
  setObjectName("richTip");
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setFocusPolicy(Qt::NoFocus);
  m_show.setSingleShot(true);
  m_expand.setSingleShot(true);
  m_hide.setSingleShot(true);
  m_hide.setInterval(kGraceMs);
  connect(&m_show, &QTimer::timeout, this, [this] { if (m_target && QGuiApplication::mouseButtons() == Qt::NoButton) present(State::Compact); });
  connect(&m_expand, &QTimer::timeout, this, [this] { if (m_state == State::Compact) present(State::Expanded); });
  connect(&m_hide, &QTimer::timeout, this, &RichTip::hideTip);
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { if (m_state != State::Hidden) { relayout(m_state); place(false); update(); } });
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] { if (m_state != State::Hidden) { relayout(m_state); place(false); update(); } });  // its key, the hint
  qApp->installEventFilter(this);
}

RichTip* RichTip::instance() {
  if (!g_tip) {
    QWidget* owner = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
      if (w->inherits("QMainWindow")) owner = w;
    g_tip = new RichTip(owner);
  }
  return g_tip;
}

void RichTip::attach(QWidget* w, const QString& commandId) {
  if (!w) return;
  RichTip* tip = instance();
  tip->m_attached.insert(w, commandId);
  connect(w, &QObject::destroyed, tip, [tip, w] { tip->m_attached.remove(w); });
}

void RichTip::detach(QWidget* w) {
  if (!g_tip) return;
  g_tip->m_attached.remove(w);
  if (g_tip->m_target == w) g_tip->hideTip();
}

QString RichTip::attachedId(const QWidget* w) { return g_tip ? g_tip->m_attached.value(const_cast<QWidget*>(w)) : QString(); }

void RichTip::setActionLookup(std::function<QAction*(const QString&)> lookup) { g_lookup = std::move(lookup); }
void RichTip::setClipFactory(ClipFactory factory, std::function<bool(const QString&)> has) {
  g_clips = std::move(factory);
  g_hasClip = std::move(has);
}

void RichTip::setGuideHook(std::function<void(const QString&)> guide) { g_guide = std::move(guide); }

void RichTip::setMenuCards(bool on) {
  g_menus = on;
  if (on) instance();  // the filter is there before the first menu opens
}

QAction* RichTip::commandEntry(const QMenu* menu, const QPoint& pos) {
  QAction* a = menu ? menu->actionAt(pos) : nullptr;
  return a && !a->isSeparator() && !a->menu() && !a->objectName().isEmpty() && help::find(a->objectName()) ? a : nullptr;
}

int RichTip::mode() { return QSettings().value("ui/tips", 2).toInt(); }

QWidget* RichTip::attachedAt(QObject* o) const {
  for (QObject* p = o; p && p->isWidgetType(); p = p->parent()) {
    auto* w = static_cast<QWidget*>(p);
    if (m_attached.contains(w)) return w;
    if (w->isWindow()) break;
  }
  return nullptr;
}

QAction* RichTip::actionFor() const {
  if (m_entry) return m_entry;
  if (auto* button = qobject_cast<QToolButton*>(m_target.data()); button && button->defaultAction()) return button->defaultAction();
  return g_lookup ? g_lookup(m_id) : nullptr;
}

void RichTip::showFor(QWidget* target, State state, QAction* entry) {
  if (target != m_target || entry != m_entry) delete m_clip;  // another command: its own clip
  m_target = target;
  m_entry = entry;
  m_id = entry ? entry->objectName() : m_attached.value(target);
  m_suppressed = nullptr;
  m_show.stop();
  m_hide.stop();
  if (state == State::Hidden || !target) hideTip();
  else present(state);
}

void RichTip::hideTip() {
  m_show.stop();
  m_expand.stop();
  m_hide.stop();
  if (m_grow) m_grow->stop();
  if (m_state != State::Hidden) m_lastShown.restart();  // browse mode: the next button within the grace shows at once
  m_state = State::Hidden;
  delete m_clip;
  hide();
}

// A press, a key, a wheel or a drag: hidden, and quiet on this target until the pointer leaves it (no browse mode).
void RichTip::dismiss() {
  m_suppressed = m_target;
  m_suppressedEntry = m_entry;
  hideTip();
  m_lastShown.invalidate();
}

void RichTip::openGuide() {
  const QString id = m_id;
  QPointer<QWidget> menu = m_entry ? m_target : nullptr;
  dismiss();
  for (; menu && qobject_cast<QMenu*>(menu.data()); menu = menu->parentWidget()) menu->close();  // the entry's menu and those it opened from
  g_guide(id);
}

// The attached widget, or the menu and its command entry, under the pointer changed (nullptr: none; a menu without an
// entry: the pointer is on no command of it).
void RichTip::hover(QWidget* target, QAction* entry) {
  if (!target || (!entry && qobject_cast<QMenu*>(target) && !m_attached.contains(target))) {
    m_show.stop();
    m_expand.stop();
    m_target = nullptr;
    m_entry = nullptr;
    m_suppressed = nullptr;
    m_suppressedEntry = nullptr;
    if (m_state != State::Hidden && !m_hide.isActive()) m_hide.start();
    return;
  }
  m_hide.stop();
  if (target == m_target && entry == m_entry) return;
  m_target = target;
  m_entry = entry;
  m_id = entry ? entry->objectName() : m_attached.value(target);
  m_expand.stop();
  if (suppressed()) return;  // pressed: quiet until the pointer leaves it
  m_suppressed = nullptr;
  m_suppressedEntry = nullptr;
  if (mode() < 2) return hideTip();
  if (m_state != State::Hidden || (m_lastShown.isValid() && m_lastShown.elapsed() < kGraceMs)) return present(State::Compact);
  if (isVisible()) hideTip();
  m_show.start(kShowMs);
}

void RichTip::present(State state) {
  auto* menu = m_entry ? qobject_cast<QMenu*>(m_target.data()) : nullptr;
  m_menu = menu ? QRect(menu->mapToGlobal(QPoint(0, 0)), menu->size()) : QRect();
  if (menu) m_anchor = QRect(menu->mapToGlobal(menu->actionGeometry(m_entry).topLeft()), menu->actionGeometry(m_entry).size());
  else if (m_target) m_anchor = QRect(m_target->mapToGlobal(QPoint(0, 0)), m_target->size());
  const bool grow = m_state == State::Compact && state == State::Expanded && isVisible() && clips::animations();
  m_show.stop();
  m_expand.stop();
  m_state = state;
  relayout(state);
  place(grow);
  show();
  raise();
  update();
  if (state == State::Compact && m_expandable) m_expand.start(std::clamp(QSettings().value("ui/tipExpandMs", 1200).toInt(), 500, 5000));
}

void RichTip::relayout(State state) {
  const CommandHelp* h = help::find(m_id);
  QAction* a = actionFor();
  QString label = a ? a->text() : m_id;
  label.remove('&').remove(QString::fromUtf8("…"));
  m_title = h && !h->title.isEmpty() ? h->title : label;
  m_summary = h ? help::expand(h->summary) : QString();
  const QString details = h ? help::expand(h->details) : QString();
  const bool enabled = (!m_target || m_target->isEnabled()) && (!a || a->isEnabled());
  m_requirement = enabled ? QString() : h && !h->requirement.isEmpty() ? help::requirement(*h) : tr("Not available right now.");
  m_keys = keys::caps(keys::binding(a));  // the user's key, also while a sketch holds it
  m_icon = a && icons::has(a->data().toString()) ? a->data().toString() : QString();
  const bool expanded = state == State::Expanded;
  m_details = expanded ? details : QString();
  const bool clip = g_clips && h && (!g_hasClip || g_hasClip(h->clip));
  m_expandable = !details.isEmpty() || clip;
  // The expand key is Help for this tool's, as the user bound it; without one Shift alone expands and no key opens the guide.
  const QString expandKey = keys::text(QStringLiteral("help.current"));
  m_hint = !expanded && m_expandable ? (expandKey.isEmpty() ? tr("Shift for more") : tr("Shift or %1 for more").arg(expandKey))
           : g_guide && !expandKey.isEmpty() ? tr("%1 for the tool guide").arg(expandKey) : QString();
  if (!expanded) delete m_clip;
  else if (!m_clip && clip && (m_clip = g_clips(h->clip, this)) && m_clip->parentWidget() != this) m_clip->setParent(this);

  // One width for both states (it never jumps sideways when the card grows): what the longest text needs, clamped.
  const QFontMetrics title(titleFont()), body(bodyFont()), key(theme::mono(11));
  m_keyRects.clear();
  // A multi-chord key's separator is plain text between the caps.
  auto capWidth = [&key](const QString& k) { return k == keys::kThen ? key.horizontalAdvance(k) : key.horizontalAdvance(k) + 10; };
  int keysWidth = 0;
  for (const QString& k : m_keys) keysWidth += capWidth(k) + (keysWidth ? 4 : 0);
  const int iconWidth = m_icon.isEmpty() ? 0 : 28;
  int natural = iconWidth + title.horizontalAdvance(m_title) + (keysWidth ? 16 + keysWidth : 0);
  for (const QString& text : {m_summary, details}) natural = std::max(natural, body.horizontalAdvance(text));
  if (!m_requirement.isEmpty()) natural = std::max(natural, 20 + body.horizontalAdvance(m_requirement));
  if (clip) natural = std::max(natural, kClip.width());
  const int w = std::clamp(natural, kMinText, kMaxText), x = kMargin + kPad;
  int y = kMargin + 10;
  const int titleWidth = w - iconWidth - (keysWidth ? keysWidth + 12 : 0);
  const int row = std::max({20, textHeight(titleFont(), titleWidth, m_title)});
  m_iconRect = iconWidth ? QRect(x, y + (row - 20) / 2, 20, 20) : QRect();
  m_titleRect = QRect(x + iconWidth, y, titleWidth, row);
  int kx = x + w - keysWidth;
  for (const QString& k : m_keys) {
    const int kw = capWidth(k);
    m_keyRects << QRect(kx, y + (row - 18) / 2, kw, 18);
    kx += kw + 4;
  }
  y += row + 4;
  auto block = [&](QRect& r, const QString& text, const QFont& font, int gap, int indent = 0) {
    if (text.isEmpty()) { r = QRect(); return; }
    y += gap;
    r = QRect(x + indent, y, w - indent, textHeight(font, w - indent, text));
    y += r.height();
  };
  block(m_summaryRect, m_summary, bodyFont(), 0);
  block(m_detailsRect, m_details, bodyFont(), 8);
  if (m_clip) {
    y += 10;
    m_clipRect = QRect(x + (w - kClip.width()) / 2, y, kClip.width(), kClip.height());
    y += kClip.height();
    m_clip->setGeometry(QStyle::visualRect(layoutDirection(), QRect(0, 0, w + 2 * (kMargin + kPad), y), m_clipRect));
    m_clip->show();
  } else {
    m_clipRect = QRect();
  }
  block(m_requirementRect, m_requirement, bodyFont(), 8, 20);
  block(m_hintRect, m_hint, hintFont(), 6);
  y += 10 + kMargin;
  resize(w + 2 * (kMargin + kPad), y);
}

// Below the target, its leading edges aligned; above it when the screen has no room below; always on the screen.
void RichTip::place(bool animate) {
  QScreen* screen = m_target ? m_target->screen() : QGuiApplication::screenAt(m_anchor.center());
  if (!screen) screen = QGuiApplication::primaryScreen();
  const QRect avail = screen->availableGeometry();
  const QSize size = this->size();
  QRect final;
  if (!m_menu.isEmpty()) {  // a menu entry: beside the menu, the title level with the entry; the other side without room
    const int after = m_menu.right() + 1 + 2 - kMargin, before = m_menu.left() - 2 + kMargin - size.width();
    const bool fitsAfter = after + size.width() <= avail.right() + 1, fitsBefore = before >= avail.left();
    int x = layoutDirection() == Qt::RightToLeft ? (fitsBefore || !fitsAfter ? before : after) : (fitsAfter || !fitsBefore ? after : before);
    x = std::clamp(x, avail.left(), std::max(avail.left(), avail.right() + 1 - size.width()));
    m_below = true;
    final = QRect(x, std::clamp(m_anchor.center().y() - kMargin - 20, avail.top(), std::max(avail.top(), avail.bottom() + 1 - size.height())), size.width(), size.height());
  } else {
    int x = layoutDirection() == Qt::RightToLeft ? m_anchor.right() + 1 + kMargin - size.width() : m_anchor.left() - kMargin;
    x = std::clamp(x, avail.left(), std::max(avail.left(), avail.right() + 1 - size.width()));
    const int below = m_anchor.bottom() + 1 + 4 - kMargin, above = m_anchor.top() - 4 + kMargin - size.height();
    m_below = below + size.height() <= avail.bottom() + 1 || above < avail.top();
    final = QRect(x, m_below ? below : above, size.width(), size.height());
  }
  if (m_grow) m_grow->stop();
  if (!animate) return setGeometry(final);
  // Compact -> expanded: the height grows over 120 ms from the edge next to the target.
  auto* grow = new QVariantAnimation(this);
  m_grow = grow;
  grow->setDuration(kGrowMs);
  grow->setEasingCurve(QEasingCurve::OutCubic);
  grow->setStartValue(std::min(geometry().height(), final.height()));
  grow->setEndValue(final.height());
  connect(grow, &QVariantAnimation::valueChanged, this, [this, final](const QVariant& v) {
    const int h = v.toInt();
    setGeometry(final.x(), m_below ? final.y() : final.bottom() + 1 - h, final.width(), h);
  });
  setGeometry(final.x(), m_below ? final.y() : final.bottom() + 1 - grow->startValue().toInt(), final.width(), grow->startValue().toInt());
  grow->start(QAbstractAnimation::DeleteWhenStopped);
}

void RichTip::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  const int m = kMargin;
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF frame = QRectF(rect()).adjusted(m, m, -m, -m);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0, 0, 0, t.dark ? 18 : 9));  // shadow 0 2 6, as the tool panels
  for (int i = m; i >= 1; --i) p.drawRoundedRect(frame.adjusted(-i, 2 - i, i, std::min(i + 2, m)), 5 + i, 5 + i);
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg2);
  p.drawRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
  // Laid out left to right; mirrored here for right-to-left languages (text alignment follows on its own).
  auto at = [this](const QRect& r) { return QStyle::visualRect(layoutDirection(), rect(), r); };
  const int text = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap;
  if (!m_icon.isEmpty()) p.drawPixmap(at(m_iconRect), icons::pixmap(m_icon, t.sel, 20, devicePixelRatioF()));
  p.setFont(titleFont());
  p.setPen(t.fg);
  p.drawText(at(m_titleRect), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_title);
  p.setFont(theme::mono(11));
  const QList<QRect> caps = keyRects();
  p.setLayoutDirection(Qt::LeftToRight);  // a key's name reads left to right: "]" alone came out mirrored in Arabic
  for (int i = 0; i < caps.size(); ++i) {
    const QRect r = caps[i];
    if (m_keys[i] != keys::kThen) {
      p.setPen(QPen(t.line, 1));
      p.setBrush(t.bg4);
      p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }
    p.setPen(t.fg2);
    p.drawText(r, Qt::AlignCenter, m_keys[i]);
  }
  p.setLayoutDirection(layoutDirection());
  p.setFont(bodyFont());
  p.setPen(t.fg2);
  p.drawText(at(m_summaryRect), text, m_summary);
  p.setPen(t.fg);
  if (!m_details.isEmpty()) p.drawText(at(m_detailsRect), text, m_details);
  if (m_clipRect.isValid()) {
    p.setPen(Qt::NoPen);
    p.setBrush(t.vp);
    p.drawRoundedRect(at(m_clipRect), 4, 4);
  }
  if (!m_requirement.isEmpty()) {  // amber warning triangle, then the reason
    const QRect r = at(QRect(m_requirementRect.left() - 20, m_requirementRect.top() + 1, 14, 14));
    QPainterPath triangle;
    triangle.moveTo(r.center().x() + 0.5, r.top() + 1);
    triangle.lineTo(r.right() + 0.5, r.bottom());
    triangle.lineTo(r.left() + 0.5, r.bottom());
    triangle.closeSubpath();
    p.setPen(QPen(t.amber, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(triangle);
    p.drawLine(QPointF(r.center().x() + 0.5, r.top() + 5.5), QPointF(r.center().x() + 0.5, r.bottom() - 4));
    p.drawPoint(QPointF(r.center().x() + 0.5, r.bottom() - 1.8));
    p.setPen(t.amber);
    p.drawText(at(m_requirementRect), text, m_requirement);
  }
  if (!m_hint.isEmpty()) {
    p.setFont(hintFont());
    p.setPen(t.fg3);
    p.drawText(at(m_hintRect), text, m_hint);
  }
}

// The caps as painted: left to right in every language ("Ctrl" before "F"); right to left mirrors the group, not each cap.
QList<QRect> RichTip::keyRects() const {
  if (m_keyRects.isEmpty()) return {};
  QRect group;
  for (const QRect& r : m_keyRects) group |= r;
  const int dx = QStyle::visualRect(layoutDirection(), rect(), group).left() - group.left();
  QList<QRect> out;
  for (const QRect& r : m_keyRects) out << r.translated(dx, 0);
  return out;
}

bool RichTip::eventFilter(QObject* o, QEvent* e) {
  if (m_attached.isEmpty() && !g_menus) return false;
  switch (e->type()) {
    case QEvent::ToolTip: {  // rich: the card instead of Qt's tooltip; off: nothing; plain: Qt's with the summary
      QWidget* at = attachedAt(o);
      if (!at || mode() != 1) return at;
      const QString id = m_attached.value(at);
      auto* button = qobject_cast<QToolButton*>(at);
      QAction* a = button && button->defaultAction() ? button->defaultAction() : g_lookup ? g_lookup(id) : nullptr;
      const CommandHelp* h = help::find(id);
      const QString text = a ? help::tooltip(a) : h ? "<qt><b>" + h->title.toHtmlEscaped() + "</b><br>" + h->summary.toHtmlEscaped() + "</qt>" : QString();
      if (text.isEmpty()) return false;
      QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), text, at);
      return true;
    }
    case QEvent::MouseMove: {
      auto* me = static_cast<QMouseEvent*>(e);
      // The same move again, propagated to a parent (a disabled button passes it on): only the first receiver counts.
      if (m_lastReceiver && me->timestamp() == m_lastStamp && me->globalPosition() == m_lastGlobal && o->isWidgetType() && m_lastReceiver->isWidgetType() &&
          static_cast<QWidget*>(o)->isAncestorOf(static_cast<QWidget*>(m_lastReceiver.data())))
        return false;
      m_lastReceiver = o;
      m_lastStamp = me->timestamp();
      m_lastGlobal = me->globalPosition();
      if (me->buttons() != Qt::NoButton) {  // a drag
        if (m_state != State::Hidden || m_show.isActive()) dismiss();
        return false;
      }
      if (o == this || (o->isWidgetType() && isAncestorOf(static_cast<QWidget*>(o)))) { m_hide.stop(); return false; }  // reading the card
      if (auto* menu = g_menus ? qobject_cast<QMenu*>(o) : nullptr; menu && !m_attached.contains(menu)) hover(menu, commandEntry(menu, me->position().toPoint()));
      else hover(attachedAt(o));
      return false;
    }
    case QEvent::Leave:
      if (o == this || (m_target && (o == m_target.data() || o == m_target->window()))) hover(nullptr);
      return false;
    case QEvent::Hide:
      if (o == m_target.data()) { hideTip(); m_target = nullptr; m_entry = nullptr; }
      return false;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::NonClientAreaMouseButtonPress:
    case QEvent::Wheel:
      if (m_state != State::Hidden || m_show.isActive()) dismiss();
      return false;
    case QEvent::WindowDeactivate:
      if (m_target && o == m_target->window()) hideTip();
      return false;
    case QEvent::ShortcutOverride:  // Help for this tool's key over a command: its card, not the window's shortcut
      if (helpKey(static_cast<QKeyEvent*>(e)) && hovering() && mode() == 2) { e->accept(); return true; }
      return false;
    case QEvent::KeyPress: {
      if (!hovering()) return false;
      const int key = static_cast<QKeyEvent*>(e)->key();
      const bool help = helpKey(static_cast<QKeyEvent*>(e));
      if (key == Qt::Key_Shift || help) {
        if (help) e->accept();  // taken here: not offered to the parents too (the second delivery would open the guide)
        if (help && g_guide && mode() == 2 && (m_state == State::Expanded || (m_state == State::Compact && !m_expandable))) {
          openGuide();
          return true;
        }
        if (m_state != State::Expanded && mode() == 2) present(State::Expanded);
        return help;
      }
      if (key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta || static_cast<QKeyEvent*>(e)->isAutoRepeat()) return false;
      dismiss();
      return false;
    }
    default:
      return false;
  }
}
