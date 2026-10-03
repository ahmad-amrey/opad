#include "ShortcutEditor.hpp"
#include <QDialogButtonBox>
#include <QHash>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <functional>

namespace shortcuts {
static QHash<QString,Scope>& scopes() { static QHash<QString,Scope> given; return given; }
void setScope(const QString& id,Scope scope) { scopes().insert(id,scope); }
Scope scope(const QString& id) {
  if(const auto it=scopes().constFind(id);it!=scopes().constEnd())return *it;
  if(id.startsWith("sketch."))return SketchOnly;
  if(id.startsWith("select.") || id.startsWith("annotate.") ||
      (id.startsWith("inspect.") && id!="inspect.clear") ||
      id=="edit.selecttouched" || id=="design.move")return OutsideSketch;
  return Everywhere;
}
bool overlaps(const QString& a,const QString& b) {
  const auto x=scope(a),y=scope(b);return x==Everywhere||y==Everywhere||x==y;
}
bool conflicts(const QKeySequence& a,const QKeySequence& b) {
  return !a.isEmpty()&&!b.isEmpty()&&(a.matches(b)!=QKeySequence::NoMatch||b.matches(a)!=QKeySequence::NoMatch);
}
void migrate(QSettings& settings) {
  if(settings.value("shortcuts/annotationDefaultsVersion",0).toInt()<1) {
    if(QKeySequence(settings.value("shortcuts/annotate.show").toString())==QKeySequence("Shift+N"))settings.remove("shortcuts/annotate.show");
    if(settings.contains("shortcuts/annotate.draw")&&settings.value("shortcuts/annotate.draw").toString().isEmpty())settings.remove("shortcuts/annotate.draw");
    settings.setValue("shortcuts/annotationDefaultsVersion",1);
  }
  if(settings.value("shortcuts/viewDefaultsVersion",0).toInt()>=1)return;
  // The old editor saved every row, including untouched defaults. Keep actual custom bindings.
  const QList<QPair<QString,QString>> old{{"view.top","Ctrl+Alt+1"},{"view.front","Ctrl+Alt+2"},
    {"view.right","Ctrl+Alt+3"},{"view.iso","Ctrl+Alt+4"},{"view.bottom","Ctrl+Alt+5"},
    {"view.back","Ctrl+Alt+6"},{"view.left","Ctrl+Alt+7"},{"view.2d","Ctrl+Alt+D"},
    {"view.ortho","O"},{"view.gridSettings","S"},{"view.alignPlane","Ctrl+Alt+0"}};
  for(const auto& [id,key]:old)if(QKeySequence(settings.value("shortcuts/"+id).toString())==QKeySequence(key))settings.remove("shortcuts/"+id);
  // Sketch letters were previously hard-coded even when the old editor displayed empty rows.
  for(const auto& id:{"line","rect","circle","arc3","dimension","trim","construction"}) {
    const QString path="shortcuts/sketch."+QString(id);
    if(settings.contains(path)&&settings.value(path).toString().isEmpty())settings.remove(path);
  }
  for(const auto& id:{"view.rollleft","view.rollright"}) {
    const QString path="shortcuts/"+QString(id);
    if(settings.contains(path)&&settings.value(path).toString().isEmpty())settings.remove(path);
  }
  settings.setValue("shortcuts/viewDefaultsVersion",1);
}
void updateTooltip(QAction* a) {
  QString text=a->text();text.remove('&');
  if(!a->shortcut().isEmpty())text+="  ("+a->shortcut().toString(QKeySequence::NativeText)+")";
  if(!a->property("shortcutHint").toString().isEmpty())text+="\n"+a->property("shortcutHint").toString();
  a->setToolTip(text);
}
void initialize(QAction* a,const QKeySequence& key,QSettings& settings) {
  a->setProperty("defaultShortcut",key.toString(QKeySequence::PortableText));
  const QString path="shortcuts/"+a->objectName();
  a->setShortcut(settings.contains(path)?QKeySequence(settings.value(path).toString(),QKeySequence::PortableText):key);
  updateTooltip(a);
}
}

namespace {
// Qt reports Shift+2 as Key_At on layouts such as US English. Preserve the
// physical digit shortcut, including its Shift modifier, in both editor fields.
class ShortcutCapture : public QKeySequenceEdit {
 public:
  using QKeySequenceEdit::QKeySequenceEdit;
 protected:
  void keyPressEvent(QKeyEvent* event) override {
    if(event->modifiers().testFlag(Qt::ShiftModifier)) {
      const QString symbols=")!@#$%^&*(";
      const int digit=symbols.indexOf(QChar(event->key()));
      if(digit>=0) {
        QKeyEvent normalized(event->type(),Qt::Key_0+digit,event->modifiers(),event->nativeScanCode(),event->nativeVirtualKey(),event->nativeModifiers(),QString::number(digit),event->isAutoRepeat(),event->count());
        QKeySequenceEdit::keyPressEvent(&normalized);emit keySequenceChanged(keySequence());return;
      }
    }
    QKeySequenceEdit::keyPressEvent(event);
  }
};
QStringList groups(const QAction* action) {
  using T=ShortcutEditor;
  const QString id=action->objectName();
  if(id.startsWith("view.")) {
    if(QStringList{"view.top","view.bottom","view.left","view.right","view.front","view.back","view.iso","view.alignPlane","view.rollleft","view.rollright"}.contains(id))return {T::tr("View"),T::tr("Orientation")};
    if(QStringList{"view.home","view.fit","view.fitall","view.saveview"}.contains(id))return {T::tr("View"),T::tr("Framing")};
    if(id.contains("grid",Qt::CaseInsensitive)||id=="view.extensions"||id=="view.tracking")return {T::tr("View"),T::tr("Grid and snapping")};
    return {T::tr("View"),T::tr("Display")};
  }
  if(id.startsWith("sketch.c."))return {T::tr("Sketch"),T::tr("Constraints")};
  if(id.startsWith("sketch."))return {T::tr("Sketch"),T::tr("Tools and editing")};
  if(id.startsWith("design."))return {T::tr("Design")};
  if(id.startsWith("file."))return {T::tr("File")};
  if(id.startsWith("edit."))return {T::tr("Edit")};
  if(id.startsWith("annotate."))return {T::tr("Annotations")};
  if(id.startsWith("inspect."))return {T::tr("Inspect")};
  if(id.startsWith("select."))return {T::tr("Selection")};
  if(id.startsWith("panel.")||id.startsWith("workspace."))return {T::tr("Panels and workspaces")};
  if(id.startsWith("nav."))return {T::tr("Navigation")};
  if(id.startsWith("tools."))return {T::tr("Settings and commands")};
  // An area's commands: the group of their record (Commands.hpp), as the palette shows it.
  if(const QString group=action->property("commandGroup").toString();!id.startsWith("help.")&&!group.isEmpty())return {group};
  return {T::tr("Help")};
}
QString scopeName(const QString& id) {
  switch(shortcuts::scope(id)) {
    case shortcuts::SketchOnly:return ShortcutEditor::tr("In sketch");
    case shortcuts::OutsideSketch:return ShortcutEditor::tr("Outside sketch");
    default:return ShortcutEditor::tr("All workspaces");
  }
}
bool reserved(const QKeySequence& key) {
  if(key.isEmpty())return false;
  const auto first=key[0];
  return first.keyboardModifiers()==Qt::NoModifier && (first.key()==Qt::Key_Escape||first.key()==Qt::Key_Return||first.key()==Qt::Key_Enter||first.key()==Qt::Key_Delete||first.key()==Qt::Key_Backspace);
}
}

ShortcutEditor::ShortcutEditor(const QList<QAction*>& actions,QWidget* parent):QDialog(parent) {
  setWindowTitle(tr("Keyboard shortcuts"));resize(860,660);
  auto* layout=new QVBoxLayout(this);
  m_search=new QLineEdit(this);m_search->setObjectName("shortcutSearch");m_search->setPlaceholderText(tr("Search commands, categories or shortcuts…"));m_search->setClearButtonEnabled(true);layout->addWidget(m_search);
  auto* lookupRow=new QHBoxLayout;
  lookupRow->addWidget(new QLabel(tr("Find shortcut:"),this));
  m_lookup=new ShortcutCapture(this);m_lookup->setObjectName("shortcutLookup");m_lookup->setClearButtonEnabled(true);m_lookup->setLayoutDirection(Qt::LeftToRight);m_lookup->findChild<QLineEdit*>()->setPlaceholderText(tr("Press shortcut"));lookupRow->addWidget(m_lookup,1);
  auto* clearSearch=new QPushButton(tr("Clear filters"),this);lookupRow->addWidget(clearSearch);layout->addLayout(lookupRow);
  auto* hint=new QLabel(tr("Type a command name or press a shortcut above to find its assignments. Select a command below to change it."),this);hint->setWordWrap(true);layout->addWidget(hint);
  m_tree=new QTreeWidget(this);m_tree->setObjectName("shortcutTree");m_tree->setColumnCount(3);m_tree->setHeaderLabels({tr("Command"),tr("Shortcut"),tr("Context")});
  m_tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);m_tree->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);m_tree->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
  layout->addWidget(m_tree,1);
  QMap<QString,QTreeWidgetItem*> folders;
  for(auto* action:actions) {
    if(action->text().isEmpty()||action->isSeparator())continue;
    QTreeWidgetItem* parentItem=nullptr;QString path;
    for(const auto& part:groups(action)) {
      path+="/"+part;
      if(!folders.contains(path)) {
        auto* folder=parentItem?new QTreeWidgetItem(parentItem):new QTreeWidgetItem(m_tree);
        folder->setText(0,part);folder->setData(0,Qt::UserRole,-1);folders[path]=folder;
      }
      parentItem=folders[path];
    }
    auto* item=new QTreeWidgetItem(parentItem);item->setText(0,QString(action->text()).remove('&'));item->setToolTip(0,action->objectName());item->setData(0,Qt::UserRole,m_entries.size());
    item->setText(2,scopeName(action->objectName()));m_entries.push_back({action,item,action->shortcut(),action->shortcut()});
  }
  auto* reservedGroup=new QTreeWidgetItem(m_tree);reservedGroup->setText(0,tr("Editing controls (reserved)"));reservedGroup->setData(0,Qt::UserRole,-1);
  for(const auto& pair:QList<QPair<QString,QString>>{{tr("Cancel or step back"),"Esc"},{tr("Complete current input"),"Return"},{tr("Delete sketch selection"),"Del"},{tr("Delete sketch selection"),"Backspace"}}) {
    auto* item=new QTreeWidgetItem(reservedGroup);item->setText(0,pair.first);item->setText(1,pair.second);item->setText(2,tr("In sketch"));item->setData(0,Qt::UserRole,-1);
  }
  m_tree->sortItems(0,Qt::AscendingOrder);m_tree->expandAll();
  m_details=new QLabel(this);m_details->setWordWrap(true);layout->addWidget(m_details);
  auto* bindingRow=new QHBoxLayout;
  bindingRow->addWidget(new QLabel(tr("Assign shortcut:"),this));
  m_binding=new ShortcutCapture(this);m_binding->setObjectName("shortcutBinding");m_binding->setClearButtonEnabled(true);m_binding->setLayoutDirection(Qt::LeftToRight);m_binding->findChild<QLineEdit*>()->setPlaceholderText(tr("Press shortcut"));bindingRow->addWidget(m_binding,1);
  auto* change=new QPushButton(tr("Assign"),this);change->setObjectName("shortcutAssign");bindingRow->addWidget(change);
  auto* reset=new QPushButton(tr("Restore default"),this);reset->setObjectName("shortcutReset");bindingRow->addWidget(reset);layout->addLayout(bindingRow);
  auto* footer=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,this);layout->addWidget(footer);
  connect(m_search,&QLineEdit::textChanged,this,[this]{filter();});connect(m_lookup,&QKeySequenceEdit::keySequenceChanged,this,[this]{filter();});
  connect(clearSearch,&QPushButton::clicked,this,[this]{m_search->clear();m_lookup->clear();});
  connect(m_tree,&QTreeWidget::currentItemChanged,this,[this]{selectCurrent();});
  connect(change,&QPushButton::clicked,this,[this]{const int i=current();if(i>=0)assign(i,m_binding->keySequence());});
  connect(reset,&QPushButton::clicked,this,[this]{const int i=current();if(i>=0)assign(i,QKeySequence(m_entries[i].action->property("defaultShortcut").toString()));});
  connect(footer->button(QDialogButtonBox::Apply),&QPushButton::clicked,this,&ShortcutEditor::accept);
  connect(footer,&QDialogButtonBox::rejected,this,&QDialog::reject);
  for(auto* button:findChildren<QPushButton*>())button->setAutoDefault(false);
  refresh();selectCurrent();m_search->setFocus();
}
int ShortcutEditor::current() const {
  const auto* item=m_tree->currentItem();return item?item->data(0,Qt::UserRole).toInt():-1;
}
QString ShortcutEditor::name(int i)const {return groups(m_entries[i].action).join(" / ")+" / "+m_entries[i].item->text(0);}
QVector<int> ShortcutEditor::collisions(int i,const QKeySequence& key)const {
  QVector<int> result;
  for(int n=0;n<m_entries.size();++n)if(n!=i&&shortcuts::overlaps(m_entries[i].action->objectName(),m_entries[n].action->objectName())&&shortcuts::conflicts(key,m_entries[n].key))result.push_back(n);
  return result;
}
void ShortcutEditor::filter() {
  const auto words=m_search->text().simplified().split(' ',Qt::SkipEmptyParts);const auto key=m_lookup->keySequence();
  std::function<bool(QTreeWidgetItem*,QString)> visit=[&](QTreeWidgetItem* item,QString path) {
    path+=" "+item->text(0);bool visible=false;
    if(item->childCount())for(int i=0;i<item->childCount();++i)visible=visit(item->child(i),path)||visible;
    else {
      const int index=item->data(0,Qt::UserRole).toInt();
      const auto assigned=index>=0?m_entries[index].key:QKeySequence(item->text(1));
      const QString text=path+" "+item->text(1)+" "+item->text(2)+" "+(index>=0?m_entries[index].action->objectName():QString());
      visible=true;for(const auto& word:words)visible&=text.contains(word,Qt::CaseInsensitive);
      visible&=key.isEmpty()||shortcuts::conflicts(key,assigned);
    }
    item->setHidden(!visible);if(visible&&(!words.isEmpty()||!key.isEmpty()))item->setExpanded(true);return visible;
  };
  for(int i=0;i<m_tree->topLevelItemCount();++i)visit(m_tree->topLevelItem(i),{});
}
void ShortcutEditor::selectCurrent() {
  const int i=current();
  findChild<QPushButton*>("shortcutAssign")->setEnabled(i>=0);
  findChild<QPushButton*>("shortcutReset")->setEnabled(i>=0);
  m_binding->setEnabled(i>=0);m_binding->setKeySequence(i>=0?m_entries[i].key:QKeySequence());
  m_details->setText(i>=0?name(i)+"\n"+tr("Default: %1").arg(m_entries[i].action->property("defaultShortcut").toString().isEmpty()?tr("Unassigned"):QKeySequence(m_entries[i].action->property("defaultShortcut").toString()).toString(QKeySequence::NativeText)):tr("Select a command to edit its shortcut."));
}
void ShortcutEditor::refresh() {
  for(int i=0;i<m_entries.size();++i) {
    auto& e=m_entries[i];e.item->setText(1,e.key.isEmpty()?tr("Unassigned"):e.key.toString(QKeySequence::NativeText));
    const auto hits=collisions(i,e.key);QStringList labels;for(int n:hits)labels<<name(n);
    e.item->setToolTip(1,hits.isEmpty()?QString():tr("Conflicts with: %1").arg(labels.join(", ")));
    e.item->setIcon(1,hits.isEmpty()?QIcon():style()->standardIcon(QStyle::SP_MessageBoxWarning));
  }
  filter();
}
bool ShortcutEditor::assign(int i,const QKeySequence& key) {
  auto& e=m_entries[i];
  if(reserved(key)&&key!=e.initial&&key!=QKeySequence(e.action->property("defaultShortcut").toString())) {
    QMessageBox::warning(this,tr("Reserved shortcut"),tr("Esc, Enter, Delete and Backspace are reserved for editing controls. Choose another shortcut."));return false;
  }
  const auto hits=collisions(i,key);
  if(!hits.isEmpty()) {
    QStringList labels;for(int n:hits)labels<<name(n)+" ("+scopeName(m_entries[n].action->objectName())+")";
    QMessageBox dialog(QMessageBox::Warning,tr("Shortcut conflict"),tr("%1 overlaps an existing shortcut or multi-key sequence:\n%2\n\nReassign it to %3 and clear the conflicting assignments?").arg(key.toString(QKeySequence::NativeText),labels.join("\n"),name(i)),QMessageBox::Cancel,this);
    dialog.setObjectName("shortcutConflict");auto* replace=dialog.addButton(tr("Reassign"),QMessageBox::AcceptRole);replace->setObjectName("shortcutReassign");
    QPushButton* swap=nullptr;
    if(hits.size()==1&&!e.key.isEmpty()&&key==m_entries[hits[0]].key&&!reserved(e.key)) {
      auto remaining=collisions(hits[0],e.key);remaining.removeAll(i);
      if(remaining.isEmpty()){swap=dialog.addButton(tr("Swap"),QMessageBox::ActionRole);swap->setObjectName("shortcutSwap");}
    }
    dialog.setDefaultButton(QMessageBox::Cancel);dialog.exec();
    if(swap&&dialog.clickedButton()==swap)m_entries[hits[0]].key=e.key;
    else if(dialog.clickedButton()==replace)for(int n:hits)m_entries[n].key=QKeySequence();
    else return false;
  }
  e.key=key;refresh();selectCurrent();return true;
}
void ShortcutEditor::accept() {
  const int i=current();if(i>=0&&m_binding->keySequence()!=m_entries[i].key&&!assign(i,m_binding->keySequence()))return;
  // Resolve pre-existing conflicts too; no settings are written until the whole draft is valid.
  for(int n=0;n<m_entries.size();++n)if(!collisions(n,m_entries[n].key).isEmpty()&&!assign(n,m_entries[n].key))return;
  QSettings settings;
  for(const auto& e:m_entries) {
    e.action->setShortcut(e.key);shortcuts::updateTooltip(e.action);
    const QString path="shortcuts/"+e.action->objectName();
    if(e.key==QKeySequence(e.action->property("defaultShortcut").toString()))settings.remove(path);
    else settings.setValue(path,e.key.toString(QKeySequence::PortableText));
  }
  QDialog::accept();
}
