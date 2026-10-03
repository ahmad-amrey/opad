#include "DynamicInput.hpp"
#include "Icons.hpp"
#include "InputKeys.hpp"
#include "Theme.hpp"
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegion>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <algorithm>

namespace {
QString hint(const QString& tip) {
  const QString keys=DynamicInput::tr("Type a value or an expression · Tab next box · Enter uses the values · Esc undoes · ↑/↓ step it (Shift ×10, Ctrl ×0.1)");
  return tip.isEmpty()?keys:keys+QLatin1Char('\n')+tip;
}
}

DynamicInput::DynamicInput(QWidget* view,QWidget* host) : QWidget(host?host:view),m_view(view),m_embedded(host!=nullptr) {
  setObjectName("dynamicInput");setLayoutDirection(Qt::LeftToRight);
  auto* row=new QHBoxLayout(this);row->setSpacing(4);
  if(m_embedded)row->setContentsMargins(0,0,0,0);
  else {
    // A native child over OpenGL paints every pixel (holes come out black on Windows); the corners go by a mask.
    setAttribute(Qt::WA_NativeWindow);setAttribute(Qt::WA_StyledBackground);setAutoFillBackground(true);setFixedHeight(28);
    row->setContentsMargins(8,2,4,2);
  }
  connect(theme::notifier(),&theme::Notifier::changed,this,[this]{m_look=-1;restyle();});
  restyle();
  if(!m_embedded)hide();
}

bool DynamicInput::takesKeysFrom(QWidget* view,QObject* target) {
  auto* widget=qobject_cast<QWidget*>(target);
  if(!widget || !view)return false;
  if(widget==view)return true;
  if(qobject_cast<QLineEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QTextEdit*>(widget) || qobject_cast<QPlainTextEdit*>(widget) || qobject_cast<QKeySequenceEdit*>(widget))return false;
  if(auto* combo=qobject_cast<QComboBox*>(widget);combo && combo->isEditable())return false;
  if(QApplication::activeModalWidget())return false;
  // QWidget::isAncestorOf stops at a window: a tool panel (a tool window owned by the main window) is not "in" it.
  const QWidget* main=view->window();const QWidget* window=widget->window();
  return window==main || (window->inherits("ToolPanel") && window->parentWidget() && window->parentWidget()->window()==main);
}

void DynamicInput::setFields(const QList<Field>& fields) {
  bool same=fields.size()==m_boxes.size();
  for(int i=0;same && i<fields.size();++i)same=fields[i].key==m_boxes[i].field.key && fields[i].option==m_boxes[i].field.option && fields[i].chip.isEmpty()==m_boxes[i].field.chip.isEmpty();
  if(!same) {
    const bool typedBefore=typed();
    giveBack();
    // Later: this can run inside a box's own key event (Enter placed the point, the next step has other boxes).
    for(auto& box:m_boxes) {
      box.edit->removeEventFilter(this);box.edit->disconnect(this);box.label->hide();box.edit->hide();box.label->deleteLater();box.edit->deleteLater();
      if(box.chip){box.chip->disconnect(this);box.chip->hide();box.chip->deleteLater();}
    }
    m_boxes.clear();m_current=-1;
    auto* row=static_cast<QHBoxLayout*>(layout());
    for(const auto& field:fields) {
      Box box;box.field=field;
      box.label=new QLabel(field.label,this);
      box.edit=new QLineEdit(this);box.edit->setObjectName("dynamicInput-"+field.key);box.edit->setFrame(false);
      box.edit->setToolTip(hint(field.tip));
      box.edit->installEventFilter(this);
      box.lock=box.edit->addAction(icons::icon("lock",theme::current().sel),QLineEdit::TrailingPosition);box.lock->setVisible(false);
      // Shown at once: a layout shows a new child of a visible widget only an event loop turn later, and until then the box
      // cannot take the keyboard (keys typed quickly after Enter went past it).
      row->addWidget(box.label);row->addWidget(box.edit);box.label->show();box.edit->show();
      const int index=int(m_boxes.size());
      if(!field.chip.isEmpty()) {
        box.chip=new QToolButton(this);box.chip->setObjectName("dynamicInputChip");box.chip->setFocusPolicy(Qt::NoFocus);box.chip->setCursor(Qt::PointingHandCursor);
        row->addWidget(box.chip);box.chip->show();
        connect(box.chip,&QToolButton::clicked,this,[this,index]{if(index<count())emit chipClicked(m_boxes[index].field.key);});
      }
      connect(box.edit,&QLineEdit::textEdited,this,[this,index]{edited(index);});
      m_boxes.push_back(box);
    }
    if(typedBefore)emit typedChanged();
    m_wasTyped=false;restyle();
  }
  bool changed=!same;
  for(int i=0;i<fields.size();++i) {
    auto& box=m_boxes[i];
    if(box.field.label!=fields[i].label){box.label->setText(fields[i].label);changed=true;}
    if(box.edit->placeholderText()!=fields[i].live){box.edit->setPlaceholderText(fields[i].live);changed=true;}
    if(box.chip && box.chip->text()!=fields[i].chip){box.chip->setText(fields[i].chip);changed=true;}
    if(box.field.tip!=fields[i].tip && box.problem.isEmpty())box.edit->setToolTip(hint(fields[i].tip));
    box.field=fields[i];
  }
  if(changed)fit();
}

bool DynamicInput::typed() const { return std::any_of(m_boxes.begin(),m_boxes.end(),[](const Box& box){return box.typed;}); }
bool DynamicInput::editing() const { return std::any_of(m_boxes.begin(),m_boxes.end(),[](const Box& box){return box.edit->hasFocus();}); }
QString DynamicInput::text(const QString& key) const {
  for(const auto& box:m_boxes)if(box.field.key==key)return box.typed?box.edit->text().trimmed():QString();
  return {};
}

void DynamicInput::edited(int index) {
  if(index<0 || index>=count())return;
  auto& box=m_boxes[index];
  const QString text=box.edit->text();
  if(!box.typed && !text.isEmpty())box.optionBefore=box.field.live;
  box.typed=!box.field.valued && !text.trimmed().isEmpty();
  if(box.field.option)emit optionEdited(box.field.key,box.typed?text.trimmed():box.optionBefore);
  restyle();fit();
  const QString key=box.field.key;
  if(typed()!=m_wasTyped){m_wasTyped=typed();emit typedChanged();}
  emit valueTyped(key);
}

void DynamicInput::setText(int index,const QString& text) {
  if(index<0 || index>=count())return;
  {QSignalBlocker block(m_boxes[index].edit);m_boxes[index].edit->setText(text);}
  edited(index);
}

void DynamicInput::select(int index) { makeCurrent(index,false); }

void DynamicInput::setProblem(const QString& key,const QString& problem) {
  for(auto& box:m_boxes)if(box.field.key==key && box.problem!=problem) {
    box.problem=problem;box.edit->setToolTip(problem.isEmpty()?hint(box.field.tip):problem);restyle();
  }
}

QString DynamicInput::problem(const QString& key) const {
  for(const auto& box:m_boxes)if(box.field.key==key)return box.problem;
  return {};
}

void DynamicInput::focusBox(int index) {
  auto* edit=m_boxes[index].edit;if(edit->hasFocus())return;
  if(!isActiveWindow())window()->activateWindow();  // typed into a tool panel: the keys that follow come here
  edit->setFocus(Qt::OtherFocusReason);
}

void DynamicInput::makeCurrent(int index,bool selectAll) {
  if(index<0 || index>=count())return;
  m_current=index;auto* edit=m_boxes[index].edit;m_boxes[index].before=edit->text();
  focusBox(index);
  if(selectAll)edit->selectAll();else edit->end(false);
  restyle();
}

void DynamicInput::type(const QString& text) {
  if(m_boxes.isEmpty() || text.isEmpty())return;
  const bool number=text.size()==1 && (inputkeys::valueChar(text.front().unicode()) || inputkeys::entryChar(text.front().unicode()));
  if(!number && (m_current<0 || m_current>=count() || !m_boxes[m_current].field.text))  // a letter: the text box's
    for(int i=0;i<count();++i)if(m_boxes[i].field.text){makeCurrent(i,false);break;}
  if(m_current<0 || m_current>=count())makeCurrent(0,m_boxes[0].field.valued);  // a value it holds: the first key replaces it
  else focusBox(m_current);  // keys that still arrive over the view (the box did not get the keyboard) go on in it
  const bool words=m_boxes[m_current].field.text;
  if(!words && m_keyHook && text.size()==1 && m_keyHook(m_current,text.front()))return;
  if(!words && text==QLatin1String(",")) {
    if(inputkeys::comma(count())==inputkeys::Comma::NextBox)return cycle(false);
    return type(QStringLiteral("."));
  }
  {QSignalBlocker block(m_boxes[m_current].edit);m_boxes[m_current].edit->insert(text);}
  edited(m_current);
}

void DynamicInput::cycle(bool back) {
  makeCurrent(inputkeys::cycle(m_current,count(),back),true);
}

bool DynamicInput::backspace() {
  if(m_current<0 || m_current>=count() || m_boxes[m_current].edit->text().isEmpty())return false;
  {QSignalBlocker block(m_boxes[m_current].edit);m_boxes[m_current].edit->backspace();}
  edited(m_current);return true;
}

void DynamicInput::dropTyped() {
  for(int i=0;i<count();++i) {
    auto& box=m_boxes[i];if(!box.typed && box.edit->text().isEmpty())continue;
    {QSignalBlocker block(box.edit);box.edit->clear();}
    edited(i);
  }
  m_current=-1;giveBack();restyle();
  emit dropped();
}

void DynamicInput::used() {
  for(auto& box:m_boxes){QSignalBlocker block(box.edit);box.edit->clear();box.typed=false;}
  m_current=-1;giveBack();restyle();fit();
  if(m_wasTyped){m_wasTyped=false;emit typedChanged();}
}

void DynamicInput::giveBack() {
  if(editing() && m_view)m_view->setFocus(Qt::OtherFocusReason);
}

void DynamicInput::placeNear(const QPoint& cursor) {
  m_cursor=cursor;
  if(m_embedded)return;
  auto* view=parentWidget();if(!view)return;
  constexpr int gap=20;
  int x=cursor.x()+gap,y=cursor.y()+gap;
  if(x+width()>view->width())x=cursor.x()-gap-width();
  if(y+height()>view->height())y=cursor.y()-gap-height();
  const QPoint at(std::clamp(x,0,std::max(0,view->width()-width())),std::clamp(y,0,std::max(0,view->height()-height())));
  if(at!=pos())move(at);
}

// The pointer ran into the boxes (they follow it a little behind): out of its way, unless they are being typed into.
void DynamicInput::enterEvent(QEnterEvent* e) {
  QWidget::enterEvent(e);
  if(!m_embedded && !typed() && !editing() && parentWidget())placeNear(parentWidget()->mapFromGlobal(QCursor::pos()));
}

bool DynamicInput::eventFilter(QObject* target,QEvent* event) {
  int index=-1;
  for(int i=0;i<count();++i)if(m_boxes[i].edit==target)index=i;
  if(index<0)return QWidget::eventFilter(target,event);
  auto& box=m_boxes[index];
  if(event->type()==QEvent::FocusIn) {
    if(m_current!=index){m_current=index;box.before=box.edit->text();}
    const auto reason=static_cast<QFocusEvent*>(event)->reason();  // clicked into: typing replaces the value
    if(reason==Qt::MouseFocusReason)QTimer::singleShot(0,box.edit,&QLineEdit::selectAll);
    restyle();
  } else if(event->type()==QEvent::FocusOut) {
    if(box.field.valued && m_current==index)m_current=-1;  // typed into again later: the first key replaces the value
    restyle();
  } else if(event->type()==QEvent::Wheel) {
    auto* wheel=static_cast<QWheelEvent*>(event);
    const int delta=wheel->angleDelta().y()?wheel->angleDelta().y():wheel->angleDelta().x();  // Shift turns the wheel sideways
    if(delta)nudge(index,delta>0?1:-1,wheel->modifiers());
    wheel->accept();return true;
  } else if(event->type()==QEvent::KeyPress || event->type()==QEvent::ShortcutOverride) {
    // The box's own keys: no window shortcut sees them (Esc would close the tool, a comma or Tab would leave the box).
    auto* key=static_cast<QKeyEvent*>(event);const bool press=event->type()==QEvent::KeyPress;
    if(key->key()==Qt::Key_Up || key->key()==Qt::Key_Down) {  // Ctrl steps by 0.1, so before the shortcut test
      if(key->modifiers()&(Qt::AltModifier|Qt::MetaModifier))return false;
      key->accept();if(press && !box.field.text)nudge(index,key->key()==Qt::Key_Up?1:-1,key->modifiers());return true;
    }
    // A prefix that switches the boxes ('@' is AltGr+Q on some layouts: Ctrl+Alt on Windows); in a text box it is a letter.
    const QString text=key->text();
    if(press && m_keyHook && !box.field.text && text.size()==1 && text.front().isPrint() && (!(key->modifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier)) || inputkeys::entryChar(text.front().unicode()))
       && m_keyHook(index,text.front())){key->accept();return true;}
    if(key->modifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))return false;
    switch(key->key()) {
      case Qt::Key_Tab:case Qt::Key_Backtab:
        key->accept();if(press)cycle(key->key()==Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier));return true;
      case Qt::Key_Comma:
        if(box.field.text)return false;
        key->accept();if(press)type(QStringLiteral(","));return true;
      case Qt::Key_Return:case Qt::Key_Enter:
        key->accept();if(press)emit committed();return true;
      case Qt::Key_Escape:
        key->accept();
        if(press)switch(inputkeys::escape(box.edit->text()!=box.before,typed())) {
          case inputkeys::Esc::RevertBox:{QSignalBlocker block(box.edit);box.edit->setText(box.before);}edited(index);break;
          case inputkeys::Esc::ClearTyped:dropTyped();break;
          case inputkeys::Esc::PassOn:giveBack();emit escaped();break;
        }
        return true;
      case Qt::Key_Backspace:
        if(!box.edit->text().isEmpty())return false;  // the box edits its text
        key->accept();if(press)emit undoPoint();return true;
      default:return false;
    }
  }
  return QWidget::eventFilter(target,event);
}

// Up/Down or the wheel: the number the box starts with (what it shows grey when nothing is typed) steps by 1, Shift 10,
// Ctrl 0.1.
void DynamicInput::nudge(int index,double steps,Qt::KeyboardModifiers modifiers) {
  if(m_boxes[index].field.text)return;
  if(m_boxes[index].field.valued)return emit stepped(index,steps,modifiers);
  auto* edit=m_boxes[index].edit;
  std::string text=(edit->text().isEmpty()?edit->placeholderText():edit->text()).toStdString();
  if(!inputkeys::nudge(text,steps,inputkeys::step(modifiers.testFlag(Qt::ShiftModifier),modifiers.testFlag(Qt::ControlModifier))))return;
  {QSignalBlocker block(edit);edit->setText(QString::fromStdString(text));}
  m_current=index;edited(index);
}

void DynamicInput::restyle() {
  const Tokens& t=theme::current();
  const bool active=editing() || typed();
  const int look=(active?1:0)|(t.dark?2:0)|(int(t.sel.rgb()&0xffff)<<2);  // a style sheet is parsed again when set: only on a change
  if(look!=m_look) {
    if((look|1)!=(m_look|1))for(auto& box:m_boxes)box.lock->setIcon(icons::icon("lock",t.sel));
    m_look=look;
    auto palette=this->palette();palette.setColor(QPalette::Window,t.bg2);setPalette(palette);
    const QString frame=m_embedded?QString("#dynamicInput { background: transparent; border: none; }")  // the host draws it
                                  :QString("#dynamicInput { background: %1; border: 1px solid %2; border-radius: 5px; }").arg(theme::css(t.bg2),theme::css(active?t.sel:t.line));
    setStyleSheet(frame+QString("#dynamicInput QLabel { color: %1; font-size: 11px; }"
                                "#dynamicInput QLineEdit { background: transparent; color: %2; border: 1px solid transparent; border-radius: 3px; padding: 0 2px; font-family: '%3'; font-size: 12px; selection-background-color: %4; }"
                                "#dynamicInput QLineEdit[current=\"true\"] { background: %4; }"
                                "#dynamicInput QLineEdit[typed=\"true\"] { font-weight: 600; }"
                                "#dynamicInput QLineEdit[locked=\"true\"] { border-color: %5; }"
                                "#dynamicInput QLineEdit[invalid=\"true\"] { border-color: %6; color: %6; }"
                                "#dynamicInput QToolButton { color: %1; background: transparent; border: 1px solid %7; border-radius: 3px; padding: 0 4px; font-size: 11px; }"
                                "#dynamicInput QToolButton:hover { color: %2; border-color: %5; }")
                         .arg(theme::css(t.fg2),theme::css(t.fg),theme::mono().family(),theme::css(t.selbg),theme::css(t.sel),theme::css(t.red),theme::css(t.line)));
  }
  for(int i=0;i<count();++i) {
    auto* edit=m_boxes[i].edit;
    if(edit->palette().color(QPalette::PlaceholderText)!=t.fg3){auto p=edit->palette();p.setColor(QPalette::PlaceholderText,t.fg3);edit->setPalette(p);}
    const bool current=i==m_current && active,typedBox=m_boxes[i].typed,invalid=typedBox && !m_boxes[i].problem.isEmpty();
    const bool locked=typedBox && !invalid && !(current && edit->hasFocus());  // typed and left (Tab, the view): it holds
    if(edit->property("current").toBool()!=current || edit->property("typed").toBool()!=typedBox || edit->property("locked").toBool()!=locked
       || edit->property("invalid").toBool()!=invalid || !edit->property("current").isValid()) {
      edit->setProperty("current",current);edit->setProperty("typed",typedBox);edit->setProperty("locked",locked);edit->setProperty("invalid",invalid);
      edit->style()->unpolish(edit);edit->style()->polish(edit);
    }
    if(m_boxes[i].lock->isVisible()!=locked){m_boxes[i].lock->setVisible(locked);fit();}
  }
}

void DynamicInput::fit() {
  // Wide enough for what is typed (or shown) and never narrower than a usual number, so the boxes do not twitch while
  // the pointer moves.
  bool resized=false;
  for(auto& box:m_boxes) {
    const QString shown=box.edit->text().isEmpty()?box.edit->placeholderText():box.edit->text();
    const int width=std::clamp(box.edit->fontMetrics().horizontalAdvance(shown+"  ")+8,box.edit->fontMetrics().horizontalAdvance("-0000.00")+8,220)+(box.lock->isVisible()?18:0);
    if(box.edit->width()!=width || box.edit->minimumWidth()!=width){box.edit->setFixedWidth(width);resized=true;}
  }
  if(resized){adjustSize();placeNear(m_cursor);if(m_embedded && parentWidget())parentWidget()->adjustSize();}
}

void DynamicInput::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  QPainterPath path;path.addRoundedRect(QRectF(rect()),5,5);setMask(QRegion(path.toFillPolygon().toPolygon()));
}
