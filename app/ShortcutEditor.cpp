#include "ShortcutEditor.hpp"
#include "KeyText.hpp"
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <functional>
#include <tuple>

namespace shortcuts {
static QHash<QString,Scope>& scopes() { static QHash<QString,Scope> given; return given; }
void setScope(const QString& id,Scope scope) { scopes().insert(id,scope); }
Scope scope(const QString& id) {
  if(const auto it=scopes().constFind(id);it!=scopes().constEnd())return *it;
  if(id.startsWith("sketch."))return SketchOnly;
  if(id.startsWith("select.") || id.startsWith("annotate.") ||
      (id.startsWith("inspect.") && id!="inspect.clear") ||
      id=="edit.selecttouched" || id=="design.move" || id=="view.shaded" || id=="view.edges" || id=="view.wire" || id=="view.hidden" ||
      id=="view.hiddenEdges")return OutsideSketch;  // 5-9: digits a sketch types
  return Everywhere;
}
bool overlaps(const QString& a,const QString& b) {
  const auto x=scope(a),y=scope(b);return x==Everywhere||y==Everywhere||x==y;
}
bool conflicts(const QKeySequence& a,const QKeySequence& b) {
  return !a.isEmpty()&&!b.isEmpty()&&(a.matches(b)!=QKeySequence::NoMatch||b.matches(a)!=QKeySequence::NoMatch);
}
bool typesValue(const QKeySequence& key) {
  if(key.count()!=1 || (key[0].keyboardModifiers()&~Qt::KeypadModifier))return false;
  const int code=key[0].key();
  return (code>=Qt::Key_0 && code<=Qt::Key_9) || code==Qt::Key_Period || code==Qt::Key_Comma || code==Qt::Key_Minus || code==Qt::Key_Plus;
}
// The key and the alternate (UI-111) are held together, so the alternate comes back too.
void suspendOutsideSketch(const QList<QAction*>& actions,bool sketching) {
  for(auto* a:actions) {
    if(scope(a->objectName())!=OutsideSketch)continue;
    const QVariant held=a->property("heldShortcut");
    if(sketching && !held.isValid()){a->setProperty("heldShortcut",QVariant::fromValue(a->shortcuts()));a->setShortcuts({});}
    else if(!sketching && held.isValid()){a->setProperty("heldShortcut",QVariant());a->setShortcuts(held.value<QList<QKeySequence>>());}
  }
}
QList<QKeySequence> bindings(const QAction* a) {
  const QVariant held=a->property("heldShortcut");return held.isValid()?held.value<QList<QKeySequence>>():a->shortcuts();
}
QKeySequence binding(const QAction* a) {return bindings(a).value(0);}
void bind(QAction* a,const QList<QKeySequence>& keys) {
  if(a->property("heldShortcut").isValid())a->setProperty("heldShortcut",QVariant::fromValue(keys));else a->setShortcuts(keys);
  updateTooltip(a);
  ::keys::announce();  // help cards, the cheat sheet, lessons: key text shown anywhere follows (quiet while the editor applies)
}
void migrate(QSettings& settings) {
  if(settings.value("shortcuts/inputDefaultsVersion",0).toInt()<1) {
    // UI-16: a running sketch tool types digits, the point, a comma and the signs into its value boxes; a sketch command
    // saved on one of them could no longer be reached. Back to its default.
    settings.beginGroup("shortcuts");const auto keys=settings.childKeys();settings.endGroup();
    for(const auto& id:keys)if(scope(id)==SketchOnly && typesValue(QKeySequence(settings.value("shortcuts/"+id).toString(),QKeySequence::PortableText)))settings.remove("shortcuts/"+id);
    settings.setValue("shortcuts/inputDefaultsVersion",1);
  }
  if(settings.value("shortcuts/annotationDefaultsVersion",0).toInt()<1) {
    if(QKeySequence(settings.value("shortcuts/annotate.show").toString())==QKeySequence("Shift+N"))settings.remove("shortcuts/annotate.show");
    if(settings.contains("shortcuts/annotate.draw")&&settings.value("shortcuts/annotate.draw").toString().isEmpty())settings.remove("shortcuts/annotate.draw");
    settings.setValue("shortcuts/annotationDefaultsVersion",1);
  }
  if(settings.value("shortcuts/standardDefaultsVersion",0).toInt()<1) {
    // Properties moved from Ctrl+P (Print's everywhere) to Alt+Enter (UI-111); the old editor saved every row.
    if(QKeySequence(settings.value("shortcuts/inspect.properties").toString())==QKeySequence("Ctrl+P"))settings.remove("shortcuts/inspect.properties");
    settings.setValue("shortcuts/standardDefaultsVersion",1);
  }
  if(settings.value("shortcuts/escapeDefaultsVersion",0).toInt()<1) {
    // Esc is fixed (TODO 11 wave 3): Clear measurement is the window's whole Esc ladder (cancel a note, leave a feature,
    // step a tool back, close a panel, clear the measurement) and every footer and prompt says Esc. A saved key goes.
    settings.remove("shortcuts/inspect.clear");settings.remove("shortcutAlternates/inspect.clear");
    settings.setValue("shortcuts/escapeDefaultsVersion",1);
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
  QStringList keys;for(const auto& key:a->shortcuts())if(!key.isEmpty())keys<<key.toString(QKeySequence::NativeText);
  if(keys.isEmpty() && !binding(a).isEmpty())keys<<binding(a).toString(QKeySequence::NativeText);  // let go while a sketch is open
  if(!keys.isEmpty())text+="  ("+keys.join(" / ")+")";
  if(!a->property("shortcutHint").toString().isEmpty())text+="\n"+a->property("shortcutHint").toString();
  a->setToolTip(text);
}
QList<QKeySequence> alternates(const QString& id) {
  // Redo answers to Ctrl+Y and Ctrl+Shift+Z: its standard key is one of them (Ctrl+Y on Windows, Ctrl+Shift+Z on Linux and
  // macOS), the alternate the other. The standard key again as its alternate clashed with itself in the editor.
  if(id!="edit.redo")return {};
  const QKeySequence y("Ctrl+Y"),z("Ctrl+Shift+Z");
  return {QKeySequence(QKeySequence::Redo)==z?y:z};
}
bool fixedKey(const QAction* a) {return a->property("fixedShortcut").toBool();}
void initialize(QAction* a,const QKeySequence& key,QSettings& settings) {
  a->setProperty("defaultShortcut",key.toString(QKeySequence::PortableText));
  if(fixedKey(a)){a->setShortcuts(key.isEmpty()?QList<QKeySequence>{}:QList<QKeySequence>{key});updateTooltip(a);return;}  // no setting changes it
  const QString id=a->objectName(),path="shortcuts/"+id,second="shortcutAlternates/"+id;
  const QKeySequence first=settings.contains(path)?QKeySequence(settings.value(path).toString(),QKeySequence::PortableText):key;
  QList<QKeySequence> keys{first};
  if(settings.contains(second))keys<<QKeySequence(settings.value(second).toString(),QKeySequence::PortableText);
  else if(first==key)keys<<alternates(id);
  keys.removeAll(QKeySequence());
  a->setShortcuts(keys);
  updateTooltip(a);
}
void settleAlternates(const QList<QAction*>& actions) {
  for(QAction* a:actions) {
    QList<QKeySequence> keys=a->shortcuts();
    if(keys.size()<2)continue;
    const QKeySequence first=keys.takeFirst();
    keys.erase(std::remove_if(keys.begin(),keys.end(),[&](const QKeySequence& alternate){
      return std::any_of(actions.begin(),actions.end(),[&](QAction* other){
        return other!=a&&overlaps(a->objectName(),other->objectName())&&conflicts(alternate,other->shortcut());});
    }),keys.end());
    a->setShortcuts(QList<QKeySequence>{first}+keys);updateTooltip(a);
  }
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
    if(QStringList{"view.home","view.fit","view.fitall","view.saveview"}.contains(id) || id.startsWith("view.named"))return {T::tr("View"),T::tr("Framing")};
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
  m_tree=new QTreeWidget(this);m_tree->setObjectName("shortcutTree");m_tree->setColumnCount(4);m_tree->setHeaderLabels({tr("Command"),tr("Shortcut"),tr("Alternate"),tr("Context")});
  m_tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);for(int c=1;c<4;++c)m_tree->header()->setSectionResizeMode(c,QHeaderView::ResizeToContents);
  layout->addWidget(m_tree,1);
  QMap<QString,QTreeWidgetItem*> folders;
  QList<QAction*> fixed;  // a key no setting changes (Esc): listed with the reserved keys below
  for(auto* action:actions) {
    if(action->text().isEmpty()||action->isSeparator())continue;
    if(shortcuts::fixedKey(action)){fixed<<action;continue;}
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
    const QList<QKeySequence> keys=shortcuts::bindings(action);const QKeySequence key=keys.value(0),alternate=keys.value(1);  // also while a sketch holds them
    item->setText(3,scopeName(action->objectName()));m_entries.push_back({action,item,key,key,alternate,alternate});
  }
  auto* reservedGroup=new QTreeWidgetItem(m_tree);reservedGroup->setText(0,tr("Editing controls (reserved)"));reservedGroup->setData(0,Qt::UserRole,-1);
  // The value keys belong to every tool that takes values (UI-122): a sketch tool, a feature panel, the section, a drawing
  // being placed. The filters' and display styles' digits work while none runs.
  // Esc is Clear measurement's, fixed (TODO 11 wave 3): the window's whole Esc ladder, the sketch's step back included.
  for(QAction* action:fixed) {
    auto* item=new QTreeWidgetItem(reservedGroup);
    item->setText(0,action->objectName()=="inspect.clear"?tr("Step back, close the tool or panel, clear the measurement"):QString(action->text()).remove('&'));
    item->setText(1,shortcuts::binding(action).toString(QKeySequence::NativeText));item->setText(3,scopeName(action->objectName()));item->setData(0,Qt::UserRole,-1);item->setToolTip(0,action->objectName());
  }
  for(const auto& [text,keys,where]:QList<std::tuple<QString,QString,QString>>{{tr("Complete current input"),"Return",tr("In sketch")},
                                                                             {tr("Delete sketch selection"),"Del",tr("In sketch")},{tr("Undo the last sketch point"),"Backspace",tr("In sketch")},
                                                                             {tr("Type a value into the tool's boxes"),"0-9 . , - +",tr("In tools that take values")},{tr("Next or previous value box"),"Tab, Shift+Tab",tr("In tools that take values")}}) {
    auto* item=new QTreeWidgetItem(reservedGroup);item->setText(0,text);item->setText(1,keys);item->setText(3,where);item->setData(0,Qt::UserRole,-1);
  }
  m_tree->sortItems(0,Qt::AscendingOrder);m_tree->expandAll();
  m_details=new QLabel(this);m_details->setObjectName("shortcutDetails");m_details->setWordWrap(true);layout->addWidget(m_details);
  auto* bindings=new QGridLayout;bindings->setColumnStretch(1,1);layout->addLayout(bindings);
  bindings->addWidget(new QLabel(tr("Assign shortcut:"),this),0,0);
  m_binding=new ShortcutCapture(this);m_binding->setObjectName("shortcutBinding");bindings->addWidget(m_binding,0,1);
  auto* change=new QPushButton(tr("Assign"),this);change->setObjectName("shortcutAssign");bindings->addWidget(change,0,2);
  auto* reset=new QPushButton(tr("Restore default"),this);reset->setObjectName("shortcutReset");bindings->addWidget(reset,0,3);
  // A second key the command answers to as well (UI-111): Redo's Ctrl+Shift+Z beside Ctrl+Y.
  bindings->addWidget(new QLabel(tr("Alternate shortcut:"),this),1,0);
  m_alternate=new ShortcutCapture(this);m_alternate->setObjectName("shortcutAlternate");bindings->addWidget(m_alternate,1,1);
  auto* changeAlternate=new QPushButton(tr("Assign alternate"),this);changeAlternate->setObjectName("shortcutAssignAlternate");bindings->addWidget(changeAlternate,1,2);
  for(auto* capture:{m_binding,m_alternate}) {
    capture->setClearButtonEnabled(true);capture->setLayoutDirection(Qt::LeftToRight);capture->findChild<QLineEdit*>()->setPlaceholderText(tr("Press shortcut"));
  }
  auto* footer=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,this);layout->addWidget(footer);
  connect(m_search,&QLineEdit::textChanged,this,[this]{filter();});connect(m_lookup,&QKeySequenceEdit::keySequenceChanged,this,[this]{filter();});
  connect(clearSearch,&QPushButton::clicked,this,[this]{m_search->clear();m_lookup->clear();});
  connect(m_tree,&QTreeWidget::currentItemChanged,this,[this]{selectCurrent();});connect(m_binding,&QKeySequenceEdit::keySequenceChanged,this,[this]{describe();});
  connect(change,&QPushButton::clicked,this,[this]{const int i=current();if(i>=0)assign(i,m_binding->keySequence());});
  connect(changeAlternate,&QPushButton::clicked,this,[this]{const int i=current();if(i>=0)assign(i,m_alternate->keySequence(),1);});
  connect(reset,&QPushButton::clicked,this,[this]{
    const int i=current();
    if(i>=0&&assign(i,QKeySequence(m_entries[i].action->property("defaultShortcut").toString())))assign(i,shortcuts::alternates(m_entries[i].action->objectName()).value(0),1);
  });
  connect(footer->button(QDialogButtonBox::Apply),&QPushButton::clicked,this,&ShortcutEditor::accept);
  connect(footer,&QDialogButtonBox::rejected,this,&QDialog::reject);
  for(auto* button:findChildren<QPushButton*>())button->setAutoDefault(false);
  refresh();selectCurrent();m_search->setFocus();
}
int ShortcutEditor::current() const {
  const auto* item=m_tree->currentItem();return item?item->data(0,Qt::UserRole).toInt():-1;
}
QString ShortcutEditor::name(int i)const {return groups(m_entries[i].action).join(" / ")+" / "+m_entries[i].item->text(0);}
QString ShortcutEditor::name(const Hit& h)const {return h.slot?tr("%1 (alternate)").arg(name(h.entry)):name(h.entry);}
// Every key or alternate (the command's own other one too) that `key` would clash with in slot `slot` of command i.
QVector<ShortcutEditor::Hit> ShortcutEditor::collisions(int i,const QKeySequence& key,int slot)const {
  QVector<Hit> result;
  if(key.isEmpty())return result;  // no key clashes with nothing (most alternates): OK checks every slot of every command
  for(int n=0;n<m_entries.size();++n)
    for(int s=0;s<2;++s)  // the keys first: scope() per pair made OK take ~0.2 s
      if((n!=i||s!=slot)&&shortcuts::conflicts(key,m_entries[n].slot(s))&&shortcuts::overlaps(m_entries[i].action->objectName(),m_entries[n].action->objectName()))result.push_back({n,s});
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
      const QString text=path+" "+item->text(1)+" "+item->text(2)+" "+item->text(3)+" "+(index>=0?m_entries[index].action->objectName():QString());
      visible=true;for(const auto& word:words)visible&=text.contains(word,Qt::CaseInsensitive);
      visible&=key.isEmpty()||shortcuts::conflicts(key,assigned)||(index>=0&&shortcuts::conflicts(key,m_entries[index].alternate));
    }
    item->setHidden(!visible);if(visible&&(!words.isEmpty()||!key.isEmpty()))item->setExpanded(true);return visible;
  };
  for(int i=0;i<m_tree->topLevelItemCount();++i)visit(m_tree->topLevelItem(i),{});
}
void ShortcutEditor::selectCurrent() {
  const int i=current();
  findChild<QPushButton*>("shortcutAssign")->setEnabled(i>=0);
  findChild<QPushButton*>("shortcutReset")->setEnabled(i>=0);
  findChild<QPushButton*>("shortcutAssignAlternate")->setEnabled(i>=0);
  m_binding->setEnabled(i>=0);m_binding->setKeySequence(i>=0?m_entries[i].key:QKeySequence());
  m_alternate->setEnabled(i>=0);m_alternate->setKeySequence(i>=0?m_entries[i].alternate:QKeySequence());
  describe();
}
void ShortcutEditor::describe() {
  const int i=current();
  if(i<0){m_details->setText(tr("Select a command to edit its shortcut."));return;}
  QString standard=m_entries[i].action->property("defaultShortcut").toString().isEmpty()?tr("Unassigned"):QKeySequence(m_entries[i].action->property("defaultShortcut").toString()).toString(QKeySequence::NativeText);
  for(const auto& alternate:shortcuts::alternates(m_entries[i].action->objectName()))standard+=" / "+alternate.toString(QKeySequence::NativeText);
  QString text=name(i)+"\n"+tr("Default: %1").arg(standard);
  // A value key outside the sketch (the filters' 1-4 and the styles' 5-7 by default) acts while no tool takes values; a
  // running one types it into its boxes (UI-122). The defaults stay as they were (nothing for migrate()): said here.
  if(shortcuts::scope(m_entries[i].action->objectName())!=shortcuts::SketchOnly && shortcuts::typesValue(m_binding->keySequence()))
    text+="\n"+tr("While a tool that takes values runs (a sketch tool, a feature, the section, a drawing being placed), this key types into its boxes instead.");
  m_details->setText(text);
}
void ShortcutEditor::refresh() {
  for(int i=0;i<m_entries.size();++i) {
    auto& e=m_entries[i];e.item->setText(1,e.key.isEmpty()?tr("Unassigned"):e.key.toString(QKeySequence::NativeText));
    e.item->setText(2,e.alternate.toString(QKeySequence::NativeText));
    for(int s=0;s<2;++s) {
      const auto hits=collisions(i,e.slot(s),s);QStringList labels;for(const Hit& h:hits)labels<<name(h);
      e.item->setToolTip(1+s,hits.isEmpty()?QString():tr("Conflicts with: %1").arg(labels.join(", ")));
      e.item->setIcon(1+s,hits.isEmpty()?QIcon():style()->standardIcon(QStyle::SP_MessageBoxWarning));
    }
  }
  filter();
}
bool ShortcutEditor::assign(int i,const QKeySequence& key,int slot) {
  auto& e=m_entries[i];
  const QKeySequence standard=slot?shortcuts::alternates(e.action->objectName()).value(0):QKeySequence(e.action->property("defaultShortcut").toString());
  if(reserved(key)&&key!=(slot?e.initialAlternate:e.initial)&&key!=standard) {
    QMessageBox::warning(this,tr("Reserved shortcut"),tr("Esc, Enter, Delete and Backspace are reserved for editing controls. Choose another shortcut."));return false;
  }
  if(shortcuts::scope(e.action->objectName())==shortcuts::SketchOnly && shortcuts::typesValue(key) && key!=(slot?e.initialAlternate:e.initial)) {
    QMessageBox::warning(this,tr("Reserved shortcut"),tr("Digits, the decimal point, the comma and the signs type values into a running sketch tool. Choose another shortcut for a sketch command."));return false;
  }
  const auto hits=collisions(i,key,slot);
  if(!hits.isEmpty()) {
    QStringList labels;for(const Hit& h:hits)labels<<name(h)+" ("+scopeName(m_entries[h.entry].action->objectName())+")";
    QMessageBox dialog(QMessageBox::Warning,tr("Shortcut conflict"),tr("%1 overlaps an existing shortcut or multi-key sequence:\n%2\n\nReassign it to %3 and clear the conflicting assignments?").arg(key.toString(QKeySequence::NativeText),labels.join("\n"),name(Hit{i,slot})),QMessageBox::Cancel,this);
    dialog.setObjectName("shortcutConflict");auto* replace=dialog.addButton(tr("Reassign"),QMessageBox::AcceptRole);replace->setObjectName("shortcutReassign");
    QPushButton* swap=nullptr;
    const QKeySequence mine=e.slot(slot);
    const Hit other=hits.front();
    if(hits.size()==1&&!mine.isEmpty()&&key==m_entries[other.entry].slot(other.slot)&&!reserved(mine)) {
      auto remaining=collisions(other.entry,mine,other.slot);
      remaining.erase(std::remove_if(remaining.begin(),remaining.end(),[&](const Hit& h){return h.entry==i&&h.slot==slot;}),remaining.end());
      if(remaining.isEmpty()){swap=dialog.addButton(tr("Swap"),QMessageBox::ActionRole);swap->setObjectName("shortcutSwap");}
    }
    dialog.setDefaultButton(QMessageBox::Cancel);dialog.exec();
    if(swap&&dialog.clickedButton()==swap)m_entries[other.entry].slot(other.slot)=mine;
    else if(dialog.clickedButton()==replace)for(const Hit& h:hits)m_entries[h.entry].slot(h.slot)=QKeySequence();
    else return false;
  }
  e.slot(slot)=key;refresh();selectCurrent();return true;
}
void ShortcutEditor::accept() {
  const int i=current();
  if(i>=0&&m_binding->keySequence()!=m_entries[i].key&&!assign(i,m_binding->keySequence()))return;
  if(i>=0&&m_alternate->keySequence()!=m_entries[i].alternate&&!assign(i,m_alternate->keySequence(),1))return;
  // Resolve pre-existing conflicts too; no settings are written until the whole draft is valid.
  for(int n=0;n<m_entries.size();++n)
    for(int s=0;s<2;++s)if(!collisions(n,m_entries[n].slot(s),s).isEmpty()&&!assign(n,m_entries[n].slot(s),s))return;
  QSettings settings;
  QList<QAction*> actions;
  QSignalBlocker quiet(keys::notifier());  // one announcement once every key is set, not one per command
  for(auto& e:m_entries) {
    if(e.key.isEmpty())std::swap(e.key,e.alternate);  // an alternate alone is the key
    const QString id=e.action->objectName();
    const bool standard=e.key==QKeySequence(e.action->property("defaultShortcut").toString());
    const QList<QKeySequence> chosen=e.alternate.isEmpty()?QList<QKeySequence>{e.key}:QList<QKeySequence>{e.key,e.alternate};
    QList<QKeySequence> now=shortcuts::bindings(e.action);now.removeAll(QKeySequence());
    QList<QKeySequence> wanted=chosen;wanted.removeAll(QKeySequence());
    // Only the commands whose keys change: rebinding all of them (each QAction::changed, its tooltip) cost ~0.3 s per OK.
    if(now!=wanted)shortcuts::bind(e.action,chosen);  // held while a sketch is open: taken back when it closes
    actions<<e.action;
    // Written only where they change: every remove or set marks the file, which is then written out whole.
    auto put=[&settings](const QString& path,bool keep,const QString& value) {
      if(!keep){if(settings.contains(path))settings.remove(path);}
      else if(!settings.contains(path)||settings.value(path).toString()!=value)settings.setValue(path,value);
    };
    put("shortcuts/"+id,!standard,e.key.toString(QKeySequence::PortableText));
    // Saved only where it differs from what the key implies: the default alternate with the default key, else none.
    put("shortcutAlternates/"+id,e.alternate!=(standard?shortcuts::alternates(id).value(0):QKeySequence()),e.alternate.toString(QKeySequence::PortableText));
  }
  shortcuts::settleAlternates(actions);
  quiet.unblock();
  keys::announce();
  QDialog::accept();
}
