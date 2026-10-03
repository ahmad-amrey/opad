#include "ShortcutEditor.hpp"
#include "check.hpp"
#include <QApplication>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTest>

namespace {
QTreeWidgetItem* item(ShortcutEditor& dialog,const QString& id) {
  auto* tree=dialog.findChild<QTreeWidget*>("shortcutTree");
  for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->toolTip(0)==id)return *it;
  throw check::Failure{"missing shortcut row"};
}
void choose(ShortcutEditor& dialog,const QString& id,const QString& key) {
  auto* tree=dialog.findChild<QTreeWidget*>("shortcutTree");tree->setCurrentItem(item(dialog,id));
  dialog.findChild<QKeySequenceEdit*>("shortcutBinding")->setKeySequence(QKeySequence(key));
}
bool answer(const QString& button) {
  auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());if(!box)return false;
  if(button=="cancel"){box->reject();return true;}
  if(auto* b=box->findChild<QPushButton*>(button)){b->click();return true;}
  box->reject();return false;
}
void assign(ShortcutEditor& dialog,const QString& response) {
  bool seen=false;QTimer::singleShot(0,[&]{seen=answer(response);});
  dialog.findChild<QPushButton*>("shortcutAssign")->click();CHECK(seen);
}
void init(QAction& action,const QString& id,const QString& key) {
  action.setObjectName(id);action.setText(id);QSettings settings;shortcuts::initialize(&action,QKeySequence(key),settings);
}
}
TEST(prefix_and_context_conflicts) {
  CHECK(shortcuts::conflicts(QKeySequence("Ctrl+K"),QKeySequence("Ctrl+K, C")));
  CHECK(shortcuts::conflicts(QKeySequence("Ctrl+K, C"),QKeySequence("Ctrl+K")));
  CHECK(!shortcuts::conflicts({},QKeySequence("Ctrl+K")));
  CHECK(!shortcuts::conflicts(QKeySequence("G"),QKeySequence("Shift+G")));
  CHECK(!shortcuts::overlaps("sketch.dimension","inspect.distance"));
  CHECK(!shortcuts::overlaps("sketch.trim","edit.selecttouched"));
  CHECK(shortcuts::overlaps("sketch.line","view.home"));
}
TEST(migration_and_explicit_empty_bindings) {
  QSettings s;s.clear();s.setValue("shortcuts/view.ortho","O");s.setValue("shortcuts/view.top","Alt+Q");s.setValue("shortcuts/view.grid","");
  s.setValue("shortcuts/view.rollleft","");s.setValue("shortcuts/sketch.line","");
  shortcuts::migrate(s);CHECK(!s.contains("shortcuts/view.ortho"));CHECK(s.value("shortcuts/view.top").toString()=="Alt+Q");
  CHECK(!s.contains("shortcuts/view.rollleft"));CHECK(!s.contains("shortcuts/sketch.line"));
  QAction grid;init(grid,"view.grid","G");CHECK(grid.shortcut().isEmpty());
  s.setValue("shortcuts/view.ortho","O");shortcuts::migrate(s);CHECK(s.value("shortcuts/view.ortho").toString()=="O");
  s.clear();
}
TEST(annotation_shortcut_migration) {
  QSettings s;s.clear();s.setValue("shortcuts/viewDefaultsVersion",1);
  s.setValue("shortcuts/annotate.show","Shift+N");s.setValue("shortcuts/annotate.draw","");
  shortcuts::migrate(s);QAction draw,show;init(draw,"annotate.draw","Shift+N");init(show,"annotate.show","");
  CHECK(draw.shortcut()==QKeySequence("Shift+N"));CHECK(show.shortcut().isEmpty());
  s.clear();s.setValue("shortcuts/annotate.show","Alt+N");s.setValue("shortcuts/annotate.draw","Ctrl+Alt+N");
  shortcuts::migrate(s);CHECK(s.value("shortcuts/annotate.show").toString()=="Alt+N");CHECK(s.value("shortcuts/annotate.draw").toString()=="Ctrl+Alt+N");s.clear();
}
// UI-111: Properties leaves Ctrl+P (Print's) for Alt+Enter; a Ctrl+P the old editor saved goes, a key of the user's stays.
TEST(properties_shortcut_migration) {
  QSettings s;s.clear();s.setValue("shortcuts/inspect.properties","Ctrl+P");
  shortcuts::migrate(s);CHECK(!s.contains("shortcuts/inspect.properties"));CHECK(s.value("shortcuts/standardDefaultsVersion").toInt()==1);
  QAction properties;init(properties,"inspect.properties","Alt+Return");CHECK(properties.shortcut()==QKeySequence("Alt+Return"));
  s.clear();s.setValue("shortcuts/inspect.properties","Ctrl+Shift+O");shortcuts::migrate(s);
  CHECK(s.value("shortcuts/inspect.properties").toString()=="Ctrl+Shift+O");
  s.setValue("shortcuts/inspect.properties","Ctrl+P");shortcuts::migrate(s);CHECK(s.value("shortcuts/inspect.properties").toString()=="Ctrl+P");  // once only
  s.clear();
}
// Redo answers to Ctrl+Shift+Z too while it keeps its default; a key of the user's replaces both; another command on
// Ctrl+Shift+Z takes it, since Qt fires neither of two equal shortcuts.
TEST(alternate_keys) {
  QSettings s;s.clear();
  QAction redo;init(redo,"edit.redo","Ctrl+Y");
  CHECK(redo.shortcuts()==QList<QKeySequence>({QKeySequence("Ctrl+Y"),QKeySequence("Ctrl+Shift+Z")}));
  CHECK(redo.toolTip().contains("Ctrl+Shift+Z"));
  QAction other;init(other,"view.fit","F");
  shortcuts::settleAlternates({&redo,&other});CHECK(redo.shortcuts().size()==2);
  other.setShortcut(QKeySequence("Ctrl+Shift+Z"));
  shortcuts::settleAlternates({&redo,&other});CHECK(redo.shortcuts()==QList<QKeySequence>{QKeySequence("Ctrl+Y")});
  s.setValue("shortcuts/edit.redo","Ctrl+R");QAction custom;init(custom,"edit.redo","Ctrl+Y");
  CHECK(custom.shortcuts()==QList<QKeySequence>{QKeySequence("Ctrl+R")});
  s.clear();
}
TEST(editor_search_swap_reassign_cancel_and_persistence) {
  QSettings settings;settings.clear();QAction home,grid,sketch,measure;
  init(home,"view.home","H");init(grid,"view.grid","G");init(sketch,"sketch.dimension","D");init(measure,"inspect.distance","D");
  QList<QAction*> actions{&home,&grid,&sketch,&measure};
  {
    ShortcutEditor dialog(actions);
    auto* search=dialog.findChild<QLineEdit*>("shortcutSearch");search->setText("framing");
    CHECK(!item(dialog,"view.home")->isHidden());CHECK(item(dialog,"view.grid")->isHidden());search->clear();
    auto* lookup=dialog.findChild<QKeySequenceEdit*>("shortcutLookup");lookup->setKeySequence(QKeySequence("D"));
    CHECK(!item(dialog,"sketch.dimension")->isHidden());CHECK(!item(dialog,"inspect.distance")->isHidden());CHECK(item(dialog,"view.home")->isHidden());lookup->clear();
    choose(dialog,"view.home","G");assign(dialog,"cancel");CHECK(item(dialog,"view.home")->text(1)=="H");
    choose(dialog,"view.home","G");assign(dialog,"shortcutSwap");CHECK(item(dialog,"view.home")->text(1)=="G");CHECK(item(dialog,"view.grid")->text(1)=="H");
    CHECK(home.shortcut()==QKeySequence("H"));dialog.reject();CHECK(!settings.contains("shortcuts/view.home"));
  }
  {
    ShortcutEditor dialog(actions);choose(dialog,"view.home","G");assign(dialog,"shortcutReassign");dialog.accept();
    CHECK(home.shortcut()==QKeySequence("G"));CHECK(grid.shortcut().isEmpty());CHECK(settings.contains("shortcuts/view.grid"));
    CHECK(home.toolTip().contains("(G)"));
  }
  QAction restored;init(restored,"view.grid","G");CHECK(restored.shortcut().isEmpty());
  {
    ShortcutEditor dialog(actions);choose(dialog,"view.grid","G");
    bool seen=false;QTimer::singleShot(0,[&]{seen=answer("cancel");});dialog.accept();CHECK(seen);CHECK(grid.shortcut().isEmpty());
  }
  settings.clear();
}
TEST(editor_prefix_conflict_and_reserved_keys) {
  QSettings settings;settings.clear();QAction a,b;init(a,"view.home","H");init(b,"tools.commands","Ctrl+K, C");
  ShortcutEditor dialog({&a,&b});choose(dialog,"view.home","Ctrl+K");assign(dialog,"shortcutReassign");
  CHECK(item(dialog,"tools.commands")->text(1)=="Unassigned");
  choose(dialog,"view.home","Return");assign(dialog,"cancel");CHECK(item(dialog,"view.home")->text(1)==QKeySequence("Ctrl+K").toString(QKeySequence::NativeText));
  dialog.reject();CHECK(b.shortcut()==QKeySequence("Ctrl+K, C"));
}
TEST(value_keys_display_styles_and_the_sketch) {
  // UI-16: a running sketch tool types digits into its value boxes, so the display styles' 5, 6 and 7 (like the filters'
  // 1 to 4) are scoped outside the sketch, let go while one is open and taken back after; a sketch command cannot have one.
  for(const char* id:{"view.shaded","view.edges","view.wire","select.edges"})CHECK(shortcuts::scope(id)==shortcuts::OutsideSketch);
  CHECK(!shortcuts::overlaps("view.wire","sketch.line"));CHECK(shortcuts::overlaps("view.wire","view.home"));
  for(const char* key:{"5","0",".",",","-","+"})CHECK(shortcuts::typesValue(QKeySequence(key)));
  CHECK(shortcuts::typesValue(QKeySequence(QKeyCombination(Qt::KeypadModifier,Qt::Key_5))));
  for(const char* key:{"Shift+2","Ctrl+5","Alt+1","L","Esc",""})CHECK(!shortcuts::typesValue(QKeySequence(key)));
  QSettings().clear();QAction wire,line,fit;init(wire,"view.wire","7");init(line,"sketch.line","L");init(fit,"view.fit","F");
  shortcuts::suspendOutsideSketch({&wire,&line,&fit},true);
  CHECK(wire.shortcut().isEmpty() && shortcuts::binding(&wire)==QKeySequence("7"));
  CHECK(line.shortcut()==QKeySequence("L") && fit.shortcut()==QKeySequence("F"));  // the sketch's own and the everywhere ones stay
  CHECK(wire.toolTip().contains("(7)"));
  shortcuts::suspendOutsideSketch({&wire,&line,&fit},true);  // again: still held, not lost
  CHECK(shortcuts::binding(&wire)==QKeySequence("7"));
  shortcuts::bind(&wire,QKeySequence("8"));  // changed in the editor while sketching: taken back as changed
  CHECK(wire.shortcut().isEmpty() && shortcuts::binding(&wire)==QKeySequence("8"));
  shortcuts::suspendOutsideSketch({&wire,&line,&fit},false);
  CHECK(wire.shortcut()==QKeySequence("8") && shortcuts::binding(&wire)==QKeySequence("8") && !wire.property("heldShortcut").isValid());
  {
    ShortcutEditor dialog({&wire,&line,&fit});
    CHECK(item(dialog,"view.wire")->text(2)=="Outside sketch");
    choose(dialog,"sketch.line","5");assign(dialog,"cancel");  // refused: a running tool types 5
    CHECK(item(dialog,"sketch.line")->text(1)=="L");
    choose(dialog,"view.wire","5");dialog.findChild<QPushButton*>("shortcutAssign")->click();
    CHECK(item(dialog,"view.wire")->text(1)=="5");
    dialog.reject();
  }
  QSettings().clear();
}
TEST(value_keys_outside_the_sketch_are_explained) {
  // UI-122: a tool that takes values (a feature, the section, a drawing being placed) types the value keys too; the defaults
  // on them stay (no migration), the editor says when a command's key is one.
  QSettings().clear();QAction wire,fit,line;init(wire,"view.wire","7");init(fit,"view.fit","F");init(line,"sketch.line","L");
  ShortcutEditor dialog({&wire,&fit,&line});
  auto* details=dialog.findChild<QLabel*>("shortcutDetails");CHECK(details);
  const QString note="this key types into its boxes instead";
  auto* tree=dialog.findChild<QTreeWidget*>("shortcutTree");
  tree->setCurrentItem(item(dialog,"view.wire"));CHECK(details->text().contains(note));
  tree->setCurrentItem(item(dialog,"view.fit"));CHECK(!details->text().contains(note));
  choose(dialog,"view.fit","0");CHECK(details->text().contains(note));  // as a key is pressed in the binding box
  choose(dialog,"view.fit","Ctrl+0");CHECK(!details->text().contains(note));
  tree->setCurrentItem(item(dialog,"sketch.line"));CHECK(!details->text().contains(note));
  dialog.reject();QSettings().clear();
}
TEST(value_key_migration) {
  QSettings s;s.clear();s.setValue("shortcuts/sketch.line","5");s.setValue("shortcuts/sketch.circle","Alt+5");s.setValue("shortcuts/view.fit","0");s.setValue("shortcuts/sketch.trim",".");
  shortcuts::migrate(s);
  CHECK(!s.contains("shortcuts/sketch.line") && !s.contains("shortcuts/sketch.trim"));  // back to their defaults
  CHECK(s.value("shortcuts/sketch.circle").toString()=="Alt+5" && s.value("shortcuts/view.fit").toString()=="0");
  CHECK(s.value("shortcuts/inputDefaultsVersion").toInt()==1);
  s.setValue("shortcuts/sketch.line","5");shortcuts::migrate(s);CHECK(s.value("shortcuts/sketch.line").toString()=="5");  // once
  s.clear();
}
TEST(shift_digit_and_arrow_activation) {
  QWidget window;QAction flat(&window),ortho(&window),top(&window);
  flat.setShortcut(QKeySequence("Shift+2"));ortho.setShortcut(QKeySequence("Shift+3"));top.setShortcut(QKeySequence("Shift+Up"));
  window.addActions({&flat,&ortho,&top});int count=0;
  for(auto* a:{&flat,&ortho,&top})QObject::connect(a,&QAction::triggered,[&]{++count;});
  window.show();window.activateWindow();window.setFocus();QTest::qWait(50);
  QTest::keyClick(&window,Qt::Key_2,Qt::ShiftModifier);QTest::keyClick(&window,Qt::Key_3,Qt::ShiftModifier);QTest::keyClick(&window,Qt::Key_Up,Qt::ShiftModifier);CHECK_EQ(count,3);
}
TEST(shift_digit_capture_and_lookup) {
  QSettings().clear();QAction flat,ortho;init(flat,"view.2d","Shift+2");init(ortho,"view.ortho","Shift+3");
  ShortcutEditor dialog({&flat,&ortho});dialog.show();
  auto* lookup=dialog.findChild<QKeySequenceEdit*>("shortcutLookup");
  QKeyEvent press(QEvent::KeyPress,Qt::Key_At,Qt::ShiftModifier,"@");QApplication::sendEvent(lookup,&press);
  CHECK(lookup->keySequence()==QKeySequence("Shift+2"));
  CHECK(!item(dialog,"view.2d")->isHidden());CHECK(item(dialog,"view.ortho")->isHidden());
  lookup->clear();choose(dialog,"view.ortho","");
  auto* binding=dialog.findChild<QKeySequenceEdit*>("shortcutBinding");
  QKeyEvent hash(QEvent::KeyPress,Qt::Key_NumberSign,Qt::ShiftModifier,"#");QApplication::sendEvent(binding,&hash);
  CHECK(binding->keySequence()==QKeySequence("Shift+3"));
}
int main(int argc,char** argv) {
  QApplication app(argc,argv);QTemporaryDir settings;
  QCoreApplication::setOrganizationName("OPAD-tests");QCoreApplication::setApplicationName("shortcuts");
  QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
  return check::run_all(argc,argv);
}
