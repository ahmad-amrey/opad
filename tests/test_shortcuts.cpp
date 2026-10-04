#include "KeyGuard.hpp"
#include "ShortcutEditor.hpp"
#include "check.hpp"
#include <QDialog>
#include <QApplication>
#include <QKeySequenceEdit>
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
  CHECK(shortcuts::scope("view.hidden")==shortcuts::OutsideSketch);CHECK(shortcuts::scope("view.hiddenEdges")==shortcuts::OutsideSketch);
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
// UI-09: one-key shortcuts are held back for a moment after a modal dialog closed, while an inline editor is open its
// keys go there, and between hold() and release() they are kept for the editor that opens; other keys pass.
TEST(key_guard_holds_one_key_shortcuts) {
  QKeyEvent v(QEvent::KeyPress,Qt::Key_V,Qt::NoModifier,"v"),shiftV(QEvent::KeyPress,Qt::Key_V,Qt::ShiftModifier,"V"),ctrlV(QEvent::KeyPress,Qt::Key_V,Qt::ControlModifier),
      del(QEvent::KeyPress,Qt::Key_Delete,Qt::NoModifier),escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier),f2(QEvent::KeyPress,Qt::Key_F2,Qt::NoModifier);
  CHECK(KeyGuard::oneKey(&v) && KeyGuard::oneKey(&shiftV) && KeyGuard::oneKey(&del));
  CHECK(!KeyGuard::oneKey(&ctrlV) && !KeyGuard::oneKey(&escape) && !KeyGuard::oneKey(&f2));
  QWidget window;auto* target=new QWidget(&window);target->setFocusPolicy(Qt::StrongFocus);
  QAction hide(&window),undo(&window);hide.setShortcut(QKeySequence("V"));undo.setShortcut(QKeySequence("Ctrl+Z"));window.addActions({&hide,&undo});
  int hidden=0,undone=0;QObject::connect(&hide,&QAction::triggered,[&]{++hidden;});QObject::connect(&undo,&QAction::triggered,[&]{++undone;});
  QLineEdit* editor=nullptr;
  KeyGuard guard([&]()->QWidget*{return editor;});qApp->installEventFilter(&guard);
  window.show();window.activateWindow();CHECK(QTest::qWaitForWindowActive(&window));target->setFocus();
  QTest::keyClick(target,Qt::Key_V);CHECK_EQ(hidden,1);CHECK(!guard.quiet());
  {QDialog dialog(&window);QTimer::singleShot(0,&dialog,&QDialog::accept);dialog.exec();}
  window.activateWindow();CHECK(QTest::qWaitForWindowActive(&window));target->setFocus();
  CHECK(guard.quiet());
  QTest::keyClick(target,Qt::Key_V);QTest::keyClick(target,Qt::Key_Z,Qt::ControlModifier);
  CHECK_EQ(hidden,1);CHECK_EQ(undone,1);  // V held back, Ctrl+Z not
  QTest::qWait(KeyGuard::kQuietMs+50);
  QTest::keyClick(target,Qt::Key_V);CHECK_EQ(hidden,2);
  editor=new QLineEdit(&window);editor->show();  // open, but the keyboard is elsewhere
  QTest::keyClick(target,Qt::Key_V);
  CHECK_EQ(hidden,2);CHECK_EQ(editor->text(),QString("v"));CHECK(QApplication::focusWidget()==editor);  // it took the keyboard
  QTest::keyClick(QApplication::focusWidget(),Qt::Key_N);CHECK_EQ(editor->text(),QString("vn"));
  editor->setText("kept");editor->hide();editor=nullptr;  // closed: V is the shortcut again
  QTest::keyClick(target,Qt::Key_V);CHECK_EQ(hidden,3);
  // Held while a command waits to resume (Rename after Edit unsaved copy): kept, then typed into the editor it opened.
  guard.hold();guard.hold();
  QTest::keyClick(target,Qt::Key_V);QTest::keyClick(target,Qt::Key_A);QTest::keyClick(target,Qt::Key_Z,Qt::ControlModifier);
  CHECK_EQ(hidden,3);CHECK_EQ(undone,2);  // V kept, Ctrl+Z not
  auto* opened=new QLineEdit(&window);opened->setText("Body");opened->selectAll();opened->show();editor=opened;
  guard.release();CHECK(guard.holding());CHECK_EQ(opened->text(),QString("Body"));  // nested: the outer release delivers
  guard.release();CHECK(!guard.holding());CHECK_EQ(opened->text(),QString("va"));CHECK(QApplication::focusWidget()==opened);
  opened->hide();editor=nullptr;target->setFocus();
  guard.hold();QTest::keyClick(target,Qt::Key_V);guard.release();  // no editor opened: dropped, not run
  CHECK_EQ(hidden,3);
  QTest::keyClick(target,Qt::Key_V);CHECK_EQ(hidden,4);
  guard.release();CHECK(!guard.holding());  // one too many: nothing
  // The resumed command runs unheld: a dialog it opens takes its keys.
  guard.hold();QString asked;
  guard.release([&]{
    QDialog dialog(&window);auto* field=new QLineEdit(&dialog);
    QTimer::singleShot(0,&dialog,[&]{QTest::qWaitForWindowActive(&dialog);field->setFocus();QTest::keyClick(field,Qt::Key_V);asked=field->text();dialog.accept();});
    dialog.exec();
  });
  CHECK_EQ(asked,QString("v"));CHECK_EQ(hidden,4);
  qApp->removeEventFilter(&guard);
}
int main(int argc,char** argv) {
  QApplication app(argc,argv);QTemporaryDir settings;
  QCoreApplication::setOrganizationName("OPAD-tests");QCoreApplication::setApplicationName("shortcuts");
  QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
  return check::run_all(argc,argv);
}
