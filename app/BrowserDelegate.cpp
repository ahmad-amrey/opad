#include "BrowserDelegate.hpp"

#include <QAbstractItemView>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QLineEdit>
#include <QPainter>
#include <QTextOption>
#include <QToolTip>

#include <algorithm>

#include "Icons.hpp"
#include "Theme.hpp"

using browser::kIdRole;
using browser::kNameRole;
using browser::kEyeX;
using browser::kSwatchX;
using browser::kTypeX;
using browser::kNameX;

// ---------------------------------------------------------------- BrowserDelegate
void BrowserDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const {
  const Tokens& t = theme::current();
  std::string id = index.data(kIdRole).toString().toStdString();
  const bool isDoc = index.data(Qt::UserRole).toString() == "document";
  const opad::Node* n = isDoc ? nullptr : m_doc->node(id);
  const browser::Decoration d = decoration(index);
  QFont nameFont = theme::ui(13);
  if (d.italic) nameFont.setItalic(true);
  if (d.bold) nameFont.setBold(true);
  p->save();
  p->setLayoutDirection(Qt::LeftToRight);  // fixed columns (see BrowserPanel); otherwise AlignLeft means right in an RTL UI
  p->setRenderHint(QPainter::Antialiasing);
  QRect r = opt.rect;
  const int fullW = opt.widget ? opt.widget->width() : r.right();
  QRect full(0, r.top(), fullW, r.height());
  if (opt.state & QStyle::State_Selected) {
    p->fillRect(full, t.selbg);  // design: one translucent tint across the whole row, indent included
  } else if (opt.state & QStyle::State_MouseOver) {
    p->fillRect(full, t.bg3);
  }
  // The name up to the decorators' badges (cut short before them), or as it always was when there are none.
  auto drawName = [&](const QString& name, int right) {
    const std::vector<QRect> badges = badgeRects(d, r, right);
    paintBadges(p, d, badges);
    p->setFont(nameFont);
    const int width = badges.empty() ? r.width() - kNameX : std::max(10, badges.back().left() - 6 - (r.left() + kNameX));
    p->drawText(QRect(r.left() + kNameX, r.top(), width, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                badges.empty() ? name : QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, width));
  };
  const QString rowKind = index.data(Qt::UserRole).toString();
  if (rowKind == "folder" || rowKind == "sketch" || rowKind == "provided") {
    const opad::SketchItem* sk = rowKind == "sketch" ? m_doc->scene.sketch(id) : nullptr;
    const bool editing=index.data(Qt::UserRole+8).toBool();
    const bool off = editing?!index.data(Qt::UserRole+9).toBool():sk && !sk->visible;
    const bool grey = off || d.dim;
    const qreal ratio = p->device()->devicePixelRatioF();
    if (sk || editing) p->drawPixmap(r.left() + kEyeX, r.top() + 6, icons::pixmap(off ? "hide" : "eye", grey ? t.fg3 : t.fg2, 16, ratio));
    const QString own = index.data(browser::kIconRole).toString();  // a provided folder's or row's
    const QString icon = !d.typeIcon.isEmpty() ? d.typeIcon : sk ? QString("sketch") : !own.isEmpty() ? own : QString("open");
    p->drawPixmap(r.left() + kTypeX, r.top() + 6, icons::pixmap(icon, sk && !sk->error.empty() ? t.red : grey ? t.fg3 : t.fg2, 16, ratio));
    p->setFont(theme::ui(13));
    p->setPen(grey ? t.fg3 : rowKind == "folder" ? t.fg2 : t.fg);
    drawName(index.data(kNameRole).toString(), r.right() - 6);
    p->restore();
    return;
  }
  if (!n && !isDoc) { p->restore(); return; }
  bool hidden = n && !n->visible;
  QColor text = hidden || d.dim ? t.fg3 : t.fg;
  QColor iconColor = hidden || d.dim ? t.fg3 : t.fg2;
  const qreal dpr = p->device()->devicePixelRatioF();
  int y = r.top() + 6;
  p->drawPixmap(r.left() + kEyeX, y, icons::pixmap(hidden ? "hide" : "eye", iconColor, 16, dpr));
  QRectF sw(r.left() + kSwatchX, r.top() + 9, 10, 10);
  p->setPen(QPen(t.line, 1));
  const bool isBody = n && n->kind == opad::Node::Kind::Body;
  if (isBody) p->setBrush(n->has_color ? QColor::fromRgbF(n->color[0], n->color[1], n->color[2]) : (hidden ? t.fg3 : t.fg2));
  else p->setBrush(t.bg);  // components and the document: hollow square
  p->drawRoundedRect(sw, 2, 2);
  QString typeIcon = isDoc ? "doc" : isBody ? (n->representation=="drawing2d" ? "drawing" : n->representation=="mesh" ? "mesh" : "body") : "component";
  const auto category=index.data(Qt::UserRole+4).toString();
  if(category=="drawing2d") typeIcon="drawing"; else if(category=="mesh") typeIcon="mesh";
  if (!d.typeIcon.isEmpty()) typeIcon = d.typeIcon;
  p->drawPixmap(r.left() + kTypeX, y, icons::pixmap(typeIcon, n && n->body_missing ? t.red : iconColor, 16, dpr));
  if (isDoc) {
    p->setFont(theme::ui(13));
    p->setPen(text);
    drawName(index.data(kNameRole).toString(), r.right() - 6);
    p->restore();
    return;
  }

  int x = r.left() + kNameX;
  int right = builtinBadges(p, r, n, text);
  const std::vector<QRect> badges = badgeRects(d, r, right);
  paintBadges(p, d, badges);
  if (!badges.empty()) right = badges.back().left() - 6;
  p->setFont(nameFont);
  p->setPen(text);
  QString name = index.data(kNameRole).toString();
  if (n->kind == opad::Node::Kind::Body && n->representation != "solid")
    name += n->representation == "mesh" ? tr(" [mesh]") : tr(" [2D]");
  p->drawText(QRect(x, r.top(), std::max(10, right - x), r.height()), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, std::max(10, right - x)));
  p->restore();
}

// The trailing badges of a node row, right to left: instance count, lock, missing body. Paints them when `p` is set
// (badgeAt lays them out without); returns the x left of the last one.
int BrowserDelegate::builtinBadges(QPainter* p, const QRect& r, const opad::Node* n, const QColor& text) const {
  const Tokens& t = theme::current();
  const qreal dpr = p ? p->device()->devicePixelRatioF() : 1.0;
  int right = r.right() - 6;
  if (n->kind == opad::Node::Kind::Body) {
    auto it = m_doc->scene.instance_count.find(n->body_key);
    if (it != m_doc->scene.instance_count.end() && it->second > 1) {
      QString badge = QString::fromUtf8("×%1").arg(it->second);
      QFontMetrics mm(theme::mono(11));
      int w = mm.horizontalAdvance(badge) + 10;
      QRect br(right - w, r.top() + 6, w, 16);
      if (p) {
        p->setPen(Qt::NoPen);
        p->setBrush(t.bg4);
        p->drawRoundedRect(br, 8, 8);
        p->setPen(text);
        p->setFont(theme::mono(11));
        p->drawText(br, Qt::AlignCenter, badge);
      }
      right -= w + 6;
    }
  }
  if (n->locked) {
    if (p) p->drawPixmap(right - 12, r.top() + 8, icons::pixmap("lock", t.fg2, 12, dpr));
    right -= 18;
  }
  if (n->body_missing) {
    QString msg = tr("missing body");
    int w = QFontMetrics(theme::ui(11)).horizontalAdvance(msg);
    if (p) {
      p->setFont(theme::ui(11));
      p->setPen(t.red);
      p->drawText(QRect(right - w, r.top(), w, r.height()), Qt::AlignVCenter, msg);
    }
    right -= w + 4;
    if (p) p->drawPixmap(right - 14, r.top() + 7, icons::pixmap("warning", t.red, 14, dpr));
    right -= 18;
  }
  return right;
}

// Right to left from `right`, 16 px high like the instance count; the ones that would leave the name less than 24 px
// are not shown.
std::vector<QRect> BrowserDelegate::badgeRects(const browser::Decoration& d, const QRect& r, int right) const {
  std::vector<QRect> out;
  const QFontMetrics fm(theme::ui(11));
  for (const browser::Badge& b : d.badges) {
    const int text = b.text.isEmpty() ? 0 : fm.horizontalAdvance(b.text);
    const int w = (b.icon.isEmpty() ? 0 : 12) + (!b.icon.isEmpty() && text ? 3 : 0) + text + (b.fill ? 10 : 0);
    if (right - w < r.left() + kNameX + 24) break;
    out.push_back(QRect(right - w, r.top() + 6, w, 16));
    right -= w + 6;
  }
  return out;
}

void BrowserDelegate::paintBadges(QPainter* p, const browser::Decoration& d, const std::vector<QRect>& rects) const {
  const Tokens& t = theme::current();
  const qreal dpr = p->device()->devicePixelRatioF();
  for (size_t i = 0; i < rects.size(); ++i) {
    const browser::Badge& b = d.badges[static_cast<int>(i)];
    QRect br = rects[i];
    if (b.fill) {
      p->setPen(Qt::NoPen);
      p->setBrush(t.*b.fill);
      p->drawRoundedRect(br, 8, 8);
      br.adjust(5, 0, -5, 0);
    }
    const bool rtl = b.text.isRightToLeft();  // the row is painted left to right: an Arabic label reads from its icon leftwards
    if (!b.icon.isEmpty()) {
      p->drawPixmap(rtl ? br.right() - 11 : br.left(), br.top() + 2, icons::pixmap(b.icon, t.*b.color, 12, dpr));
      if (rtl) br.setRight(br.right() - 15);
      else br.setLeft(br.left() + 15);
    }
    if (!b.text.isEmpty()) {
      QTextOption option(Qt::AlignCenter);
      option.setTextDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
      p->setFont(theme::ui(11));
      p->setPen(t.*b.color);
      p->drawText(QRectF(br), b.text, option);
    }
  }
}

browser::Decoration BrowserDelegate::decoration(const QModelIndex& index) const {
  browser::Decoration d;
  if (m_decorators.empty()) return d;
  browser::Row row;
  row.id = index.data(kIdRole).toString().toStdString();
  row.kind = index.data(Qt::UserRole).toString();
  row.folder = index.data(browser::kFolderRole).toString();
  if (row.kind == "body" || row.kind == "component") row.node = m_doc->node(row.id);
  for (const browser::Decorator& decorate : m_decorators) decorate(row, d);
  return d;
}

const browser::Badge* BrowserDelegate::badgeAt(const browser::Decoration& d, const QModelIndex& index, const QRect& row, const QPoint& pos, QRect* rect) const {
  if (d.badges.isEmpty()) return nullptr;
  const QString kind = index.data(Qt::UserRole).toString();
  const opad::Node* n = kind == "body" || kind == "component" ? m_doc->node(index.data(kIdRole).toString().toStdString()) : nullptr;
  if ((kind == "body" || kind == "component") && !n) return nullptr;  // not painted
  const std::vector<QRect> rects = badgeRects(d, row, n ? builtinBadges(nullptr, row, n, QColor()) : row.right() - 6);
  for (size_t i = 0; i < rects.size(); ++i)
    if (rects[i].contains(pos)) {
      if (rect) *rect = rects[i];
      return &d.badges[static_cast<int>(i)];
    }
  return nullptr;
}

// Tooltips: a badge's own, else the row's with the decorators' line under it.
bool BrowserDelegate::helpEvent(QHelpEvent* e, QAbstractItemView* view, const QStyleOptionViewItem& opt, const QModelIndex& index) {
  if (e->type() == QEvent::ToolTip && index.isValid() && !m_decorators.empty()) {
    const browser::Decoration d = decoration(index);
    QString tip;
    if (const browser::Badge* b = badgeAt(d, index, opt.rect, e->pos())) tip = b->tooltip;
    if (tip.isEmpty() && !d.tooltip.isEmpty()) {
      const QString own = index.data(Qt::ToolTipRole).toString();
      tip = own.isEmpty() ? d.tooltip : own + "\n" + d.tooltip;
    }
    if (!tip.isEmpty()) {
      QToolTip::showText(e->globalPos(), tip, view->viewport(), opt.rect);
      return true;
    }
  }
  return QStyledItemDelegate::helpEvent(e, view, opt, index);
}

QWidget* BrowserDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const {
  auto* e = new QLineEdit(parent);
  e->setFrame(true);
  return e;
}

void BrowserDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex&) const {
  editor->setGeometry(QRect(opt.rect.left() + kNameX - 4, opt.rect.top() + 1, opt.rect.width() - kNameX, opt.rect.height() - 2));
}
