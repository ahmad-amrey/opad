#include "BrowserDelegate.hpp"

#include <QFontMetrics>
#include <QLineEdit>
#include <QPainter>

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
  const QString rowKind = index.data(Qt::UserRole).toString();
  if (rowKind == "folder" || rowKind == "sketch") {
    const opad::SketchItem* sk = rowKind == "sketch" ? m_doc->scene.sketch(id) : nullptr;
    const bool editing=index.data(Qt::UserRole+8).toBool();
    const bool off = editing?!index.data(Qt::UserRole+9).toBool():sk && !sk->visible;
    const qreal ratio = p->device()->devicePixelRatioF();
    if (sk || editing) p->drawPixmap(r.left() + kEyeX, r.top() + 6, icons::pixmap(off ? "hide" : "eye", off ? t.fg3 : t.fg2, 16, ratio));
    p->drawPixmap(r.left() + kTypeX, r.top() + 6, icons::pixmap(sk ? "sketch" : "open", sk && !sk->error.empty() ? t.red : off ? t.fg3 : t.fg2, 16, ratio));
    p->setFont(theme::ui(13));
    p->setPen(off ? t.fg3 : rowKind == "folder" ? t.fg2 : t.fg);
    p->drawText(QRect(r.left() + kNameX, r.top(), r.width() - kNameX, r.height()), Qt::AlignVCenter | Qt::AlignLeft, index.data(kNameRole).toString());
    p->restore();
    return;
  }
  if (!n && !isDoc) { p->restore(); return; }
  bool hidden = n && !n->visible;
  QColor text = hidden ? t.fg3 : t.fg;
  QColor iconColor = hidden ? t.fg3 : t.fg2;
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
  p->drawPixmap(r.left() + kTypeX, y, icons::pixmap(typeIcon, n && n->body_missing ? t.red : iconColor, 16, dpr));
  if (isDoc) {
    p->setFont(theme::ui(13));
    p->setPen(t.fg);
    p->drawText(QRect(r.left() + kNameX, r.top(), r.width() - kNameX, r.height()), Qt::AlignVCenter | Qt::AlignLeft, index.data(kNameRole).toString());
    p->restore();
    return;
  }

  int x = r.left() + kNameX;
  int right = r.right() - 6;
  // trailing badges, right to left
  if (n->kind == opad::Node::Kind::Body) {
    auto it = m_doc->scene.instance_count.find(n->body_key);
    if (it != m_doc->scene.instance_count.end() && it->second > 1) {
      QString badge = QString::fromUtf8("×%1").arg(it->second);
      QFontMetrics mm(theme::mono(11));
      int w = mm.horizontalAdvance(badge) + 10;
      QRect br(right - w, r.top() + 6, w, 16);
      p->setPen(Qt::NoPen);
      p->setBrush(t.bg4);
      p->drawRoundedRect(br, 8, 8);
      p->setPen(text);
      p->setFont(theme::mono(11));
      p->drawText(br, Qt::AlignCenter, badge);
      right -= w + 6;
    }
  }
  if (n->locked) {
    p->drawPixmap(right - 12, r.top() + 8, icons::pixmap("lock", t.fg2, 12, dpr));
    right -= 18;
  }
  if (n->body_missing) {
    p->setFont(theme::ui(11));
    p->setPen(t.red);
    QString msg = tr("missing body");
    int w = QFontMetrics(theme::ui(11)).horizontalAdvance(msg);
    p->drawText(QRect(right - w, r.top(), w, r.height()), Qt::AlignVCenter, msg);
    right -= w + 4;
    p->drawPixmap(right - 14, r.top() + 7, icons::pixmap("warning", t.red, 14, dpr));
    right -= 18;
  }
  p->setFont(theme::ui(13));
  p->setPen(text);
  QString name = index.data(kNameRole).toString();
  if (n->kind == opad::Node::Kind::Body && n->representation != "solid")
    name += n->representation == "mesh" ? tr(" [mesh]") : tr(" [2D]");
  p->drawText(QRect(x, r.top(), std::max(10, right - x), r.height()), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(theme::ui(13)).elidedText(name, Qt::ElideRight, std::max(10, right - x)));
  p->restore();
}

QWidget* BrowserDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const {
  auto* e = new QLineEdit(parent);
  e->setFrame(true);
  return e;
}

void BrowserDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex&) const {
  editor->setGeometry(QRect(opt.rect.left() + kNameX - 4, opt.rect.top() + 1, opt.rect.width() - kNameX, opt.rect.height() - 2));
}
