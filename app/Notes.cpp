#include "Notes.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "AppDocument.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Viewport.hpp"

namespace notes {
const std::vector<Style>& styles() {
  static const std::vector<Style> all = {{"ok", QT_TRANSLATE_NOOP("notes", "OK"), "check", &Tokens::green, Qt::SolidLine, 1.5},
                                         {"warning", QT_TRANSLATE_NOOP("notes", "Warning"), "warning", &Tokens::amber, Qt::DashLine, 1.5},
                                         {"issue", QT_TRANSLATE_NOOP("notes", "Issue"), "issue", &Tokens::red, Qt::SolidLine, 2.5},
                                         {"note", QT_TRANSLATE_NOOP("notes", "Note"), "annotate", &Tokens::sel, Qt::DotLine, 1.5}};
  return all;
}

const Style& style(const std::string& id) {
  for (const auto& s : styles())
    if (id == s.id) return s;
  return styles().back();
}
}  // namespace notes

// ---------------------------------------------------------------- NoteCard
NoteCard::NoteCard(const NoteInfo& note, QWidget* parent) : QFrame(parent), m_note(note) {
  const Tokens& t = theme::current();
  const notes::Style& look = notes::style(note.style);
  const bool open = note.state == "open", resolved = note.state == "resolved";
  const QColor tint = open ? t.*look.color : resolved ? t.fg3 : t.red;
  setObjectName("card");
  setProperty("state", note.state);
  if (open) setStyleSheet(QString("QFrame#card { border: 1px %1 %2; }").arg(look.line == Qt::SolidLine ? "solid" : look.line == Qt::DashLine ? "dashed" : "dotted", tint.name()));
  setCursor(Qt::PointingHandCursor);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(8, 8, 8, 8);
  v->setSpacing(6);

  auto* head = new QHBoxLayout();
  head->setSpacing(6);
  // The tag: its icon in its colour; on an open note a click re-tags it.
  auto* tag = new QToolButton(this);
  tag->setIcon(QIcon(icons::pixmap(look.icon, tint, 14, devicePixelRatioF())));
  tag->setIconSize(QSize(14, 14));
  tag->setFixedSize(18, 18);
  tag->setStyleSheet("QToolButton { border: none; background: transparent; padding: 0; } QToolButton::menu-indicator { image: none; width: 0; }");
  tag->setToolTip(i18n::t(look.label));
  if (open) {
    auto* menu = new QMenu(tag);
    for (const auto& s : notes::styles()) {
      QAction* a = menu->addAction(QIcon(icons::pixmap(s.icon, t.*s.color, 16, devicePixelRatioF())), i18n::t(s.label));
      a->setCheckable(true);
      a->setChecked(note.style == s.id);
      const std::string id = s.id;
      connect(a, &QAction::triggered, this, [this, id] { if (id != m_note.style) emit styleRequested(m_note.id, id); });
    }
    tag->setMenu(menu);
    tag->setPopupMode(QToolButton::InstantPopup);
  }
  head->addWidget(tag);
  auto* author = new QLabel(QString::fromStdString(note.by), this);
  author->setFont(theme::ui(13, QFont::Medium));
  head->addWidget(author);
  auto* time = new QLabel(QString::fromStdString(note.ts).left(16).replace('T', ' '), this);
  time->setObjectName("secondary");
  head->addWidget(time, 1);
  auto* id = new QLabel(QString::fromStdString(note.id.substr(0, 8)), this);
  id->setObjectName("tertiary");
  id->setFont(theme::mono(11));
  head->addWidget(id);
  v->addLayout(head);

  auto* text = new QLabel(QString::fromStdString(note.text), this);
  text->setWordWrap(true);
  text->setTextFormat(Qt::PlainText);
  v->addWidget(text);

  if (!open) {
    auto* row = new QHBoxLayout();
    auto* mark = new QLabel(this);
    mark->setPixmap(icons::pixmap(resolved ? "check" : "warning", resolved ? t.green : t.red, 14, devicePixelRatioF()));
    row->addWidget(mark);
    auto* l = new QLabel(resolved ? tr("resolved") : tr("unresolved · target %1 no longer exists").arg(QString::fromStdString(note.body.substr(0, 8))), this);
    l->setStyleSheet(QString("color:%1; font-size:11px;").arg((resolved ? t.green : t.red).name()));
    row->addWidget(l, 1);
    v->addLayout(row);
  }

  auto* foot = new QHBoxLayout();
  auto* target = new QLabel(note.target, this);
  target->setObjectName("tertiary");
  target->setFont(theme::mono(11));
  foot->addWidget(target, 1);
  auto* btn = new QPushButton(resolved ? tr("Restore") : tr("Resolve"), this);
  btn->setObjectName("outline");
  if (!resolved) btn->setIcon(QIcon(icons::pixmap("check", t.fg, 14, devicePixelRatioF())));
  foot->addWidget(btn);
  v->addLayout(foot);
  connect(btn, &QPushButton::clicked, this, [this, resolved] {
    if (resolved) emit restoreRequested(m_note.id);
    else emit resolveRequested(m_note.id);
  });
}

void NoteCard::mousePressEvent(QMouseEvent* e) {
  QFrame::mousePressEvent(e);
  emit pressed();
}

// ---------------------------------------------------------------- NoteCards
NoteCards::NoteCards(AppDocument* doc, Viewport* viewport, QObject* parent) : QObject(parent), m_doc(doc), m_viewport(viewport) {
  m_shown = QSettings().value("ui/notes", true).toBool();
  connect(doc, &AppDocument::changed, this, &NoteCards::rebuild);
  connect(theme::notifier(), &theme::Notifier::changed, this, &NoteCards::rebuild);
  connect(viewport, &Viewport::notesMoved, this, &NoteCards::layout);
}

void NoteCards::setShown(bool on) {
  if (m_shown == on) return;
  m_shown = on;
  QSettings().setValue("ui/notes", on);
  rebuild();
}

void NoteCards::rebuild() {
  for (NoteCard* c : m_cards) c->deleteLater();
  m_cards.clear();
  if (m_shown && m_doc->hasDocument) {
    for (const auto& a : m_doc->scene.annotations) {
      if (a.unresolved) continue;  // no anchor to stand beside; the panel lists it
      NoteInfo n;
      n.id = a.id; n.by = a.by; n.ts = a.ts; n.text = a.text; n.style = a.style; n.body = a.anchor.body;
      n.target = a.anchor.kind == opad::Ref::Kind::Point ? tr("point") : m_doc->nodeName(a.anchor.body);
      if (a.anchor.kind != opad::Ref::Kind::Body && a.anchor.kind != opad::Ref::Kind::Point) n.target += QString(" › %1 %2").arg(i18n::t(opad::Ref::kind_name(a.anchor.kind))).arg(a.anchor.index);
      auto* card = new NoteCard(n, m_viewport);
      card->setAttribute(Qt::WA_NativeWindow);  // over the native 3D window, like the chips
      card->setFixedWidth(280);
      card->adjustSize();
      card->hide();
      connect(card, &NoteCard::pressed, this, [this, card] { emit pressed(card->note().id, card->note().body); });
      connect(card, &NoteCard::resolveRequested, this, &NoteCards::resolveRequested);
      connect(card, &NoteCard::styleRequested, this, &NoteCards::styleRequested);
      m_cards.push_back(card);
    }
  }
  layout();
}

void NoteCards::layout() {
  // Each card stands up and to the right of its anchor; where that is taken or outside the view it tries the
  // other three corners, then further out. A note whose anchor has left the view has no card for now.
  const QRect view = m_viewport->rect().adjusted(8, 40, -8, -8);  // the chips own the top strip
  std::map<std::string, QPoint> ends;
  std::vector<QRect> taken;
  for (NoteCard* card : m_cards) {
    QPoint at;
    if (!m_viewport->noteAnchor(card->note().id, at) || !m_viewport->rect().contains(at)) { card->hide(); continue; }
    const QSize size = card->size();
    QRect best;
    for (int ring = 0; ring < 6 && best.isNull(); ++ring) {
      const int dx = 36 + ring * 40, dy = 48 + ring * (size.height() + 12) / 2;
      for (const QPoint& corner : {QPoint(dx, -dy - size.height()), QPoint(-dx - size.width(), -dy - size.height()), QPoint(dx, dy), QPoint(-dx - size.width(), dy)}) {
        const QRect r(at + corner, size);
        if (!view.contains(r)) continue;
        if (std::any_of(taken.begin(), taken.end(), [&](const QRect& o) { return o.adjusted(-6, -6, 6, 6).intersects(r); })) continue;
        best = r;
        break;
      }
    }
    if (best.isNull()) {  // a crowded or tiny view: clamped into it, overlapping if it must
      best = QRect(at + QPoint(36, -48 - size.height()), size);
      best.moveLeft(std::clamp(best.left(), view.left(), std::max(view.left(), view.right() - size.width())));
      best.moveTop(std::clamp(best.top(), view.top(), std::max(view.top(), view.bottom() - size.height())));
    }
    taken.push_back(best);
    if (card->pos() != best.topLeft()) card->move(best.topLeft());
    if (!card->isVisible()) { card->show(); card->raise(); }
    // The pointer ends on the card's edge, at the point nearest the anchor.
    if (!best.contains(at)) ends[card->note().id] = QPoint(std::clamp(at.x(), best.left(), best.right()), std::clamp(at.y(), best.top(), best.bottom()));
  }
  m_viewport->setNoteLeaders(ends, m_shown);
}

// ---------------------------------------------------------------- NoteDialog
NoteDialog::NoteDialog(const QString& where, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Note on %1").arg(where));
  auto* v = new QVBoxLayout(this);
  auto* row = new QHBoxLayout();
  row->setSpacing(4);
  for (const auto& s : notes::styles()) {
    auto* b = new QToolButton(this);
    b->setObjectName("segment");
    b->setCheckable(true);
    b->setAutoExclusive(true);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setIcon(QIcon(icons::pixmap(s.icon, theme::current().*s.color, 16, devicePixelRatioF())));
    b->setText(i18n::t(s.label));
    const std::string id = s.id;
    connect(b, &QToolButton::clicked, this, [this, id] { pick(id); });
    row->addWidget(b);
    m_tags.push_back(b);
  }
  row->addStretch();
  v->addLayout(row);
  m_text = new QPlainTextEdit(this);
  m_text->setPlaceholderText(tr("What should the next reader know?"));
  m_text->setMinimumSize(360, 110);
  v->addWidget(m_text, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Add note"));
  v->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  auto* enter = new QShortcut(QKeySequence("Ctrl+Return"), this);  // Return alone is a new line
  connect(enter, &QShortcut::activated, this, &QDialog::accept);
  pick(QSettings().value("notes/style", "note").toString().toStdString());
  m_text->setFocus();
}

QString NoteDialog::text() const { return m_text->toPlainText(); }

void NoteDialog::pick(const std::string& style) {
  m_style = notes::style(style).id;
  for (size_t i = 0; i < m_tags.size(); ++i) m_tags[i]->setChecked(m_style == notes::styles()[i].id);
  QSettings().setValue("notes/style", QString::fromStdString(m_style));
}
