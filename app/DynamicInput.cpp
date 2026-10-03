#include "DynamicInput.hpp"
#include "InputKeys.hpp"
#include "Theme.hpp"
#include <QAbstractSpinBox>
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
#include <algorithm>

DynamicInput::DynamicInput(QWidget* view) : QWidget(view) {
  setObjectName("dynamicInput");setAttribute(Qt::WA_NativeWindow);setAttribute(Qt::WA_StyledBackground);
  // A native child over OpenGL paints every pixel (holes come out black on Windows); the corners go by a mask.
  setAutoFillBackground(true);setLayoutDirection(Qt::LeftToRight);setFixedHeight(28);
  auto* row=new QHBoxLayout(this);row->setContentsMargins(8,2,4,2);row->setSpacing(4);
  connect(theme::notifier(),&theme::Notifier::changed,this,[this]{m_look=-1;restyle();});
  restyle();hide();
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
  for(int i=0;same && i<fields.size();++i)same=fields[i].key==m_boxes[i].field.key;
  if(!same) {
    const bool typedBefore=typed();
    giveBack();
    // Later: this can run inside a box's own key event (Enter placed the point, the next step has other boxes).
    for(auto& box:m_boxes){box.edit->removeEventFilter(this);box.edit->disconnect(this);box.label->hide();box.edit->hide();box.label->deleteLater();box.edit->deleteLater();}
    m_boxes.clear();m_current=-1;
    auto* row=static_cast<QHBoxLayout*>(layout());
    for(const auto& field:fields) {
      Box box;box.field=field;
      box.label=new QLabel(field.label,this);
      box.edit=new QLineEdit(this);box.edit->setObjectName("dynamicInput-"+field.key);box.edit->setFrame(false);
      box.edit->setToolTip(tr("Type a value or an expression · Tab next box · Enter uses the values · Esc undoes · ↑/↓ step it (Shift ×10, Ctrl ×0.1)"));
      box.edit->installEventFilter(this);
      row->addWidget(box.label);row->addWidget(box.edit);
      const int index=int(m_boxes.size());
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
  box.typed=!text.trimmed().isEmpty();
  if(box.field.option)emit optionEdited(box.field.key,box.typed?text.trimmed():box.optionBefore);
  restyle();fit();
  if(typed()!=m_wasTyped){m_wasTyped=typed();emit typedChanged();}
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
  if(m_current<0 || m_current>=count())makeCurrent(0,false);
  else focusBox(m_current);  // keys that still arrive over the view (the box did not get the keyboard) go on in it
  if(text==QLatin1String(",")) {
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
}

void DynamicInput::used() {
  for(auto& box:m_boxes){QSignalBlocker block(box.edit);box.edit->clear();box.typed=false;}
  m_current=-1;giveBack();restyle();fit();
  if(m_wasTyped){m_wasTyped=false;emit typedChanged();}
}

void DynamicInput::giveBack() {
  if(!editing())return;
  if(auto* view=parentWidget())view->setFocus(Qt::OtherFocusReason);
}

void DynamicInput::placeNear(const QPoint& cursor) {
  m_cursor=cursor;
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
  if(!typed() && !editing() && parentWidget())placeNear(parentWidget()->mapFromGlobal(QCursor::pos()));
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
    restyle();
  } else if(event->type()==QEvent::KeyPress || event->type()==QEvent::ShortcutOverride) {
    // The box's own keys: no window shortcut sees them (Esc would close the tool, a comma or Tab would leave the box).
    auto* key=static_cast<QKeyEvent*>(event);const bool press=event->type()==QEvent::KeyPress;
    if(key->modifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))return false;
    switch(key->key()) {
      case Qt::Key_Tab:case Qt::Key_Backtab:
        key->accept();if(press)cycle(key->key()==Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier));return true;
      case Qt::Key_Comma:
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
      case Qt::Key_Up:case Qt::Key_Down: {
        key->accept();if(!press)return true;
        std::string text=(box.edit->text().isEmpty()?box.edit->placeholderText():box.edit->text()).toStdString();
        if(inputkeys::nudge(text,key->key()==Qt::Key_Up?1:-1,inputkeys::step(key->modifiers().testFlag(Qt::ShiftModifier),false))) {
          {QSignalBlocker block(box.edit);box.edit->setText(QString::fromStdString(text));}
          m_current=index;edited(index);
        }
        return true;
      }
      default:return false;
    }
  }
  return QWidget::eventFilter(target,event);
}

void DynamicInput::restyle() {
  const Tokens& t=theme::current();
  const bool active=editing() || typed();
  const int look=(active?1:0)|(t.dark?2:0)|(int(t.sel.rgb()&0xffff)<<2);  // a style sheet is parsed again when set: only on a change
  if(look!=m_look) {
    m_look=look;
    auto palette=this->palette();palette.setColor(QPalette::Window,t.bg2);setPalette(palette);
    setStyleSheet(QString("#dynamicInput { background: %1; border: 1px solid %2; border-radius: 5px; }"
                        "#dynamicInput QLabel { color: %3; font-size: 11px; }"
                        "#dynamicInput QLineEdit { background: transparent; color: %4; border: none; border-radius: 3px; padding: 0 3px; font-family: '%5'; font-size: 12px; selection-background-color: %6; }"
                        "#dynamicInput QLineEdit[current=\"true\"] { background: %6; }"
                        "#dynamicInput QLineEdit[typed=\"true\"] { font-weight: 600; }")
                    .arg(theme::css(t.bg2),theme::css(active?t.sel:t.line),theme::css(t.fg2),theme::css(t.fg),theme::mono().family(),theme::css(t.selbg)));
  }
  for(int i=0;i<count();++i) {
    auto* edit=m_boxes[i].edit;
    if(edit->palette().color(QPalette::PlaceholderText)!=t.fg3){auto p=edit->palette();p.setColor(QPalette::PlaceholderText,t.fg3);edit->setPalette(p);}
    const bool current=i==m_current && active,typedBox=m_boxes[i].typed;
    if(edit->property("current").toBool()!=current || edit->property("typed").toBool()!=typedBox || !edit->property("current").isValid()) {
      edit->setProperty("current",current);edit->setProperty("typed",typedBox);
      edit->style()->unpolish(edit);edit->style()->polish(edit);
    }
  }
}

void DynamicInput::fit() {
  // Wide enough for what is typed (or shown) and never narrower than a usual number, so the boxes do not twitch while
  // the pointer moves.
  bool resized=false;
  for(auto& box:m_boxes) {
    const QString shown=box.edit->text().isEmpty()?box.edit->placeholderText():box.edit->text();
    const int width=std::clamp(box.edit->fontMetrics().horizontalAdvance(shown+"  ")+8,box.edit->fontMetrics().horizontalAdvance("-0000.00")+8,220);
    if(box.edit->width()!=width || box.edit->minimumWidth()!=width){box.edit->setFixedWidth(width);resized=true;}
  }
  if(resized){adjustSize();placeNear(m_cursor);}
}

void DynamicInput::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  QPainterPath path;path.addRoundedRect(QRectF(rect()),5,5);setMask(QRegion(path.toFillPolygon().toPolygon()));
}
