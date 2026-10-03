#include <QTimer>
#include <QMessageBox>
#include "Notes.hpp"

#include <QDialogButtonBox>
#include <QComboBox>
#include <QKeyEvent>
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
namespace {
class DragHandle : public QObject {
 public:
  DragHandle(QWidget* card, QWidget* handle, std::function<void()> moved)
      : QObject(card), card(card), moved(std::move(moved)) {handle->installEventFilter(this);handle->setCursor(Qt::SizeAllCursor);handle->setToolTip(tr("Drag to move this note"));}
 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if(event->type()==QEvent::MouseButtonPress) {
      auto* e=static_cast<QMouseEvent*>(event);
      if(e->button()==Qt::LeftButton){dragging=true;offset=e->globalPosition().toPoint()-card->mapToGlobal(QPoint());return true;}
    } else if(event->type()==QEvent::MouseMove && dragging) {
      auto* e=static_cast<QMouseEvent*>(event);auto* parent=card->parentWidget();
      QPoint at=parent->mapFromGlobal(e->globalPosition().toPoint()-offset);
      at.setX(std::clamp(at.x(),0,std::max(0,parent->width()-card->width())));
      at.setY(std::clamp(at.y(),0,std::max(0,parent->height()-card->height())));
      card->move(at);card->raise();if(moved)moved();return true;
    } else if(event->type()==QEvent::MouseButtonRelease && dragging) {dragging=false;return true;}
    return false;
  }
 private:
  QWidget* card;std::function<void()> moved;bool dragging=false;QPoint offset;
};
}
void makeDraggable(QWidget* card,QWidget* handle,std::function<void()> moved) {new DragHandle(card,handle,std::move(moved));}
const std::vector<Style>& styles() {
  static const std::vector<Style> all = {{"ok", QT_TRANSLATE_NOOP("notes", "OK"), "check", &Tokens::green, Qt::SolidLine, 1.5},
                                         {"warning", QT_TRANSLATE_NOOP("notes", "Warning"), "warning", &Tokens::amber, Qt::DashLine, 1.5},
                                         {"issue", QT_TRANSLATE_NOOP("notes", "Issue"), "issue", &Tokens::red, Qt::SolidLine, 2.5},
                                         {"note", QT_TRANSLATE_NOOP("notes", "Note"), "annotate", &Tokens::sel, Qt::DotLine, 1.5},
                                         {"ai_agent", QT_TRANSLATE_NOOP("notes", "AI agent notes"), "annotate", &Tokens::hov, Qt::SolidLine, 2.5}};
  return all;
}

const Style& style(const std::string& id) {
  for (const auto& s : styles())
    if (id == s.id) return s;
  return styles()[3];
}

const std::vector<Pen>& pens() {
  static const std::vector<Pen> all = {{"red", QT_TRANSLATE_NOOP("notes", "Red"), QColor("#ef5350")},
                                       {"green", QT_TRANSLATE_NOOP("notes", "Green"), QColor("#4cc978")},
                                       {"blue", QT_TRANSLATE_NOOP("notes", "Blue"), QColor("#4d91ef")},
                                       {"white", QT_TRANSLATE_NOOP("notes", "White"), QColor("#f4f5f7")}};
  return all;
}

QColor penColor(const std::string& id) {
  for (const auto& p : pens())
    if (id == p.id) return p.color;
  return pens()[2].color;
}

const std::vector<int>& penWidths() {
  static const std::vector<int> all = {1, 2, 4, 8};
  return all;
}
}  // namespace notes

// ---------------------------------------------------------------- NoteCard
NoteCard::NoteCard(const NoteInfo& note, QWidget* parent, AppDocument* doc) : QFrame(parent), m_note(note) {
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
  auto* title=new QLabel(i18n::t(look.label),this);title->setObjectName("noteDragHandle");v->addWidget(title);m_dragHandle=title;

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
  auto* time = new QLabel(i18n::localTime(note.ts), this);
  time->setObjectName("secondary");
  head->addWidget(time, 1);
  auto* id = new QLabel(QString::fromStdString(note.id.substr(0, 8)), this);
  id->setObjectName("tertiary");
  id->setFont(theme::mono(11));
  head->addWidget(id);
  v->addLayout(head);

  if(note.measurement) {
    auto* value=new QLabel(note.value,this); value->setWordWrap(true); value->setTextFormat(Qt::PlainText); v->addWidget(value);
  }
  auto* text = new QLabel(QString::fromStdString(note.text), this);
  text->setWordWrap(true);
  text->setTextFormat(Qt::PlainText);
  v->addWidget(text);
  for (const auto& comment : note.comments) {
    auto* reply = new QLabel(QString::fromStdString(comment.value("by", "") + ": " + comment.value("text", "")), this);
    reply->setTextFormat(Qt::PlainText); reply->setWordWrap(true); v->addWidget(reply);
  }
  if (open && doc) {
    auto* edit = new QPlainTextEdit(QString::fromStdString(note.text), this);
    edit->setMaximumHeight(90); edit->hide(); v->addWidget(edit);
    auto* buttons = new QHBoxLayout();
    auto* editButton = new QPushButton(tr("Edit"), this);
    auto* commentButton = new QPushButton(tr("Comment"), this);
    auto* saveButton = new QPushButton(tr("Save"), this); saveButton->hide();
    auto* cancelButton = new QPushButton(tr("Cancel"), this); cancelButton->hide();
    buttons->addWidget(editButton); buttons->addWidget(commentButton); buttons->addWidget(saveButton); buttons->addWidget(cancelButton); v->addLayout(buttons);
    auto commenting = std::make_shared<bool>(false);
    auto begin = [=, this](bool comment) {
      *commenting = comment; edit->setPlainText(comment ? QString() : QString::fromStdString(note.text));
      edit->setPlaceholderText(comment ? tr("Write a comment...") : tr("Edit note..."));
      edit->show(); saveButton->show(); cancelButton->show(); editButton->hide(); commentButton->hide(); edit->setFocus(); adjustSize();
    };
    connect(editButton, &QPushButton::clicked, this, [=] { begin(false); });
    connect(commentButton, &QPushButton::clicked, this, [=] { begin(true); });
    connect(cancelButton, &QPushButton::clicked, this, [=, this] {
      edit->hide(); saveButton->hide(); cancelButton->hide(); editButton->show(); commentButton->show(); adjustSize();
    });
    connect(saveButton, &QPushButton::clicked, this, [=] {
      const std::string value = edit->toPlainText().trimmed().toStdString();
      if (value.empty()) return;
      const bool comment = *commenting;
      QTimer::singleShot(0, doc, [doc, note, value, comment] {
        try {
          opad::json op;
          if (comment) {
            const auto* parent = doc->doc.find_op(note.id);
            if (!parent) return;
            const auto anchor=parent->type=="measurement" ? (parent->data.at("refs").empty()?opad::json("point/0,0,0"):parent->data.at("refs").front()) : parent->data.at("anchor");
            op = {{"op", "annotation"}, {"anchor", anchor}, {"text", value}, {"reply_to", note.id}};
          } else op = {{"op", "edit"}, {"target", note.id}, {"set", {{"text", value}}}};
          doc->run("append", {{"op", op}});
        } catch (const std::exception& e) { QMessageBox::warning(nullptr, tr("Note"), i18n::t(QString::fromUtf8(e.what()))); }
      });
    });
  }


  if (!open) {
    auto* row = new QHBoxLayout();
    auto* mark = new QLabel(this);
    mark->setPixmap(icons::pixmap(resolved ? "check" : "warning", resolved ? t.green : t.red, 14, devicePixelRatioF()));
    row->addWidget(mark);
    auto* l = new QLabel(resolved ? tr("resolved") : tr("unresolved · target %1 no longer exists").arg(QString::fromStdString(note.body.substr(0, 8))), this);
    l->setStyleSheet(QString("color:%1; font-size:%2px;").arg((resolved ? t.green : t.red).name()).arg(theme::px(11)));
    row->addWidget(l, 1);
    v->addLayout(row);
  }

  auto* foot = new QHBoxLayout();
  auto* target = new QLabel(note.target, this);
  target->setObjectName("tertiary");
  target->setFont(theme::mono(11));
  foot->addWidget(target, 1);
  auto* btn = new QPushButton(resolved ? tr("Restore") : note.measurement ? tr("Remove") : tr("Resolve"), this);
  btn->setObjectName("outline");
  if (!resolved) btn->setIcon(QIcon(icons::pixmap("check", t.fg, 14, devicePixelRatioF())));
  foot->addWidget(btn);
  if(doc && !note.measurement) {
    auto* remove=new QPushButton(tr("Delete"),this);remove->setObjectName("deleteNote");foot->addWidget(remove);
    connect(remove,&QPushButton::clicked,this,[doc,id=note.id] {
      QTimer::singleShot(0,doc,[doc,id] {try {doc->run("delete_annotation",{{"target",id}});} catch(const std::exception& e){QMessageBox::warning(nullptr,tr("Delete note"),i18n::t(QString::fromUtf8(e.what())));}});
    });
  }
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
void NoteCard::enableDragging() {notes::makeDraggable(this,m_dragHandle,[this]{emit moved();});}

// ---------------------------------------------------------------- NoteCards
NoteCards::NoteCards(AppDocument* doc, Viewport* viewport, QObject* parent) : QObject(parent), m_doc(doc), m_viewport(viewport) {
  m_shown = QSettings().value("ui/notes", true).toBool();
  connect(doc, &AppDocument::changed, this, &NoteCards::rebuild);
  connect(theme::notifier(), &theme::Notifier::changed, this, &NoteCards::rebuild);
  connect(viewport, &Viewport::notesMoved, this, &NoteCards::layout);
  connect(doc,&AppDocument::pathChanged,this,[this]{m_positions.clear();});
}
void NoteCards::setTypeFilter(const std::string& type) {m_type=type;rebuild();}

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
      if(!m_type.empty() && a.style!=m_type) continue;
      NoteInfo n;
      n.id = a.id; n.by = a.by; n.ts = a.ts; n.text = a.text; n.style = a.style; n.body = a.anchor.body; n.comments = a.comments;
      n.target = a.anchor.kind == opad::Ref::Kind::Point ? tr("point") : m_doc->nodeName(a.anchor.body);
      if (a.anchor.kind != opad::Ref::Kind::Body && a.anchor.kind != opad::Ref::Kind::Point) n.target += QString(" › %1 %2").arg(i18n::t(opad::Ref::kind_name(a.anchor.kind))).arg(a.anchor.index);
      auto* card = new NoteCard(n, m_viewport, m_doc);
      card->setAttribute(Qt::WA_NativeWindow);  // over the native 3D window, like the chips
      card->setFixedWidth(280);
      card->enableDragging();
      // A dragged card keeps its offset from the anchor, so it follows its object as the view orbits and pans
      // (an absolute position left it stuck on the screen).
      connect(card,&NoteCard::moved,this,[this,card]{QPoint at;if(m_viewport->noteAnchor(card->note().id,at))m_positions[card->note().id]=card->pos()-at;layout();});
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
    if(auto it=m_positions.find(card->note().id);it!=m_positions.end()) {
      best=QRect(at+it->second,size);
      best.moveLeft(std::clamp(best.left(),view.left(),std::max(view.left(),view.right()-size.width())));
      best.moveTop(std::clamp(best.top(),view.top(),std::max(view.top(),view.bottom()-size.height())));
    }
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
  m_viewport->setNoteTypeFilter(m_type);
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
