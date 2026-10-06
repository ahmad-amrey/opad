#include "KeyGuard.hpp"
#include "KeyText.hpp"
#include "ShortcutEditor.hpp"
#include "check.hpp"
#include <QDialog>
#include <QApplication>
#include <QKeyEvent>
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
// Redo answers to its alternate too while it keeps its default (Ctrl+Shift+Z beside Ctrl+Y on Windows, Ctrl+Y beside
// Ctrl+Shift+Z where that is the standard key); a key of the user's replaces both; another command on the alternate takes
// it, since Qt fires neither of two equal shortcuts.
TEST(alternate_keys) {
  const QString redoKey=QKeySequence(QKeySequence::Redo).toString(QKeySequence::PortableText),redoAlt=shortcuts::alternates("edit.redo").value(0).toString(QKeySequence::PortableText);
  QSettings s;s.clear();
  QAction redo;init(redo,"edit.redo",redoKey);
  CHECK(redo.shortcuts()==QList<QKeySequence>({QKeySequence(redoKey),QKeySequence(redoAlt)}));
  CHECK(redo.toolTip().contains(redoAlt));
  QAction other;init(other,"view.fit","F");
  shortcuts::settleAlternates({&redo,&other});CHECK(redo.shortcuts().size()==2);
  other.setShortcut(QKeySequence(redoAlt));
  shortcuts::settleAlternates({&redo,&other});CHECK(redo.shortcuts()==QList<QKeySequence>{QKeySequence(redoKey)});
  s.setValue("shortcuts/edit.redo","Ctrl+R");QAction custom;init(custom,"edit.redo",redoKey);
  CHECK(custom.shortcuts()==QList<QKeySequence>{QKeySequence("Ctrl+R")});
  s.clear();
}
// The editor's Alternate column and field: a second key of the user's stays beside the key (shortcutAlternates/<id>),
// another command's key there is a conflict like any, taking Redo's alternate for another command clears it (saved as
// none), Restore default brings both of Redo's keys back, and an alternate left alone becomes the key.
TEST(editor_alternates) {
  const QString redoKey=QKeySequence(QKeySequence::Redo).toString(QKeySequence::PortableText),redoAlt=shortcuts::alternates("edit.redo").value(0).toString(QKeySequence::PortableText);
  QSettings settings;settings.clear();QAction redo,fit,save;
  init(redo,"edit.redo",redoKey);init(fit,"view.fit","F");init(save,"file.save","Ctrl+S");
  QList<QAction*> actions{&redo,&fit,&save};
  const auto text=[](const QString& key){return QKeySequence(key).toString(QKeySequence::NativeText);};
  {
    ShortcutEditor dialog(actions);
    auto* tree=dialog.findChild<QTreeWidget*>("shortcutTree");
    auto* alternate=dialog.findChild<QKeySequenceEdit*>("shortcutAlternate");
    CHECK(item(dialog,"edit.redo")->text(2)==text(redoAlt));
    tree->setCurrentItem(item(dialog,"view.fit"));
    alternate->setKeySequence(QKeySequence("Ctrl+Alt+F"));dialog.findChild<QPushButton*>("shortcutAssignAlternate")->click();
    CHECK(item(dialog,"view.fit")->text(2)==text("Ctrl+Alt+F"));
    alternate->setKeySequence(QKeySequence("Ctrl+S"));
    bool seen=false;QTimer::singleShot(0,[&]{seen=answer("cancel");});
    dialog.findChild<QPushButton*>("shortcutAssignAlternate")->click();CHECK(seen);
    CHECK(item(dialog,"view.fit")->text(2)==text("Ctrl+Alt+F"));
    alternate->setKeySequence(QKeySequence("Ctrl+Alt+F"));
    choose(dialog,"view.fit",redoAlt);assign(dialog,"shortcutReassign");
    CHECK(item(dialog,"edit.redo")->text(2).isEmpty()&&item(dialog,"view.fit")->text(1)==text(redoAlt));
    dialog.accept();
  }
  CHECK(fit.shortcuts()==QList<QKeySequence>({QKeySequence(redoAlt),QKeySequence("Ctrl+Alt+F")}));
  CHECK(redo.shortcuts()==QList<QKeySequence>{QKeySequence(redoKey)});
  CHECK(fit.toolTip().contains(text("Ctrl+Alt+F")));
  CHECK(settings.value("shortcutAlternates/view.fit").toString()=="Ctrl+Alt+F");
  CHECK(settings.contains("shortcutAlternates/edit.redo")&&settings.value("shortcutAlternates/edit.redo").toString().isEmpty());
  QAction fitAgain,redoAgain;init(fitAgain,"view.fit","F");init(redoAgain,"edit.redo",redoKey);  // the next start
  CHECK(fitAgain.shortcuts()==fit.shortcuts()&&redoAgain.shortcuts()==redo.shortcuts());
  {
    ShortcutEditor dialog(actions);
    choose(dialog,"view.fit","F");dialog.findChild<QPushButton*>("shortcutAssign")->click();
    dialog.findChild<QTreeWidget*>("shortcutTree")->setCurrentItem(item(dialog,"edit.redo"));
    dialog.findChild<QPushButton*>("shortcutReset")->click();
    CHECK(item(dialog,"edit.redo")->text(1)==text(redoKey)&&item(dialog,"edit.redo")->text(2)==text(redoAlt));
    dialog.findChild<QTreeWidget*>("shortcutTree")->setCurrentItem(item(dialog,"view.fit"));
    choose(dialog,"view.fit","");dialog.findChild<QPushButton*>("shortcutAssign")->click();
    dialog.accept();
  }
  CHECK(redo.shortcuts()==QList<QKeySequence>({QKeySequence(redoKey),QKeySequence(redoAlt)})&&!settings.contains("shortcutAlternates/edit.redo"));
  CHECK(fit.shortcuts()==QList<QKeySequence>{QKeySequence("Ctrl+Alt+F")}&&settings.value("shortcuts/view.fit").toString()=="Ctrl+Alt+F");
  CHECK(!settings.contains("shortcutAlternates/view.fit"));
  settings.clear();
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
    CHECK(item(dialog,"view.wire")->text(3)=="Outside sketch");  // the context after the alternate (UI-111)
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
// The highlight switches: Ctrl+/ (X-ray) was the cheat sheet's, which moved to ? (Shift+/); / is Hover highlight's. A
// Ctrl+/ the old editor saved for the sheet goes; a key the user gave another command stays theirs and the new command
// starts without one; a key of the user's for the sheet stays; once only.
TEST(highlight_keys_migration) {
  QSettings s;s.clear();s.setValue("shortcuts/help.shortcuts","Ctrl+/");
  shortcuts::migrate(s);CHECK(!s.contains("shortcuts/help.shortcuts"));CHECK(s.value("shortcuts/highlightDefaultsVersion").toInt()==1);
  CHECK(!s.contains("shortcuts/view.xrayHighlight") && !s.contains("shortcuts/view.hoverHighlight"));
  QAction sheet,xray,hover;init(sheet,"help.shortcuts","?");init(xray,"view.xrayHighlight","Ctrl+/");init(hover,"view.hoverHighlight","/");
  CHECK(sheet.shortcut()==QKeySequence("?") && xray.shortcut()==QKeySequence("Ctrl+/") && hover.shortcut()==QKeySequence("/"));
  s.clear();s.setValue("shortcuts/help.shortcuts","Ctrl+Shift+K");s.setValue("shortcuts/view.fit","/");s.setValue("shortcutAlternates/edit.redo","Ctrl+/");
  shortcuts::migrate(s);
  CHECK(s.value("shortcuts/help.shortcuts").toString()=="Ctrl+Shift+K" && s.value("shortcuts/view.fit").toString()=="/");
  CHECK(s.contains("shortcuts/view.hoverHighlight") && s.value("shortcuts/view.hoverHighlight").toString().isEmpty());
  CHECK(s.contains("shortcuts/view.xrayHighlight") && s.value("shortcuts/view.xrayHighlight").toString().isEmpty());
  QAction hover2;init(hover2,"view.hoverHighlight","/");CHECK(hover2.shortcut().isEmpty());
  s.remove("shortcuts/view.hoverHighlight");s.setValue("shortcuts/help.shortcuts","Ctrl+/");shortcuts::migrate(s);
  CHECK(!s.contains("shortcuts/view.hoverHighlight") && s.value("shortcuts/help.shortcuts").toString()=="Ctrl+/");  // once
  s.clear();
}
// / and Ctrl+/ fire in the window and ? (what Shift+/ types; Qt's key map takes the press for it) opens the sheet, while
// a text field types / and ? as text; a symbol typed with Shift is bound without it, also beside Ctrl.
TEST(highlight_keys_and_typing) {
  QSettings().clear();
  QWidget window;auto* view=new QWidget(&window);view->setFocusPolicy(Qt::StrongFocus);auto* field=new QLineEdit(&window);
  QAction hover(&window),xray(&window),sheet(&window);
  init(hover,"view.hoverHighlight","/");init(xray,"view.xrayHighlight","Ctrl+/");init(sheet,"help.shortcuts","?");
  window.addActions({&hover,&xray,&sheet});int hovers=0,xrays=0,sheets=0;
  QObject::connect(&hover,&QAction::triggered,[&]{++hovers;});QObject::connect(&xray,&QAction::triggered,[&]{++xrays;});QObject::connect(&sheet,&QAction::triggered,[&]{++sheets;});
  window.show();window.activateWindow();CHECK(QTest::qWaitForWindowActive(&window));view->setFocus();
  QTest::keyClick(view,Qt::Key_Slash);CHECK_EQ(hovers,1);
  QTest::keyClick(view,Qt::Key_Slash,Qt::ControlModifier);CHECK_EQ(xrays,1);CHECK_EQ(hovers,1);
  QTest::keyClick(view,'?');CHECK_EQ(sheets,1);
  field->setFocus();QTest::keyClick(field,'/');QTest::keyClick(field,'?');
  CHECK_EQ(field->text(),QString("/?"));CHECK_EQ(hovers,1);CHECK_EQ(sheets,1);
  QKeyEvent shiftSlash(QEvent::KeyPress,Qt::Key_Question,Qt::ShiftModifier,"?"),plain(QEvent::KeyPress,Qt::Key_Slash,Qt::NoModifier,"/"),
      ctrlSlash(QEvent::KeyPress,Qt::Key_Slash,Qt::ControlModifier);
  CHECK(keys::pressedBy(QKeySequence("?"),&shiftSlash) && !keys::pressedBy(QKeySequence("/"),&shiftSlash));
  CHECK(keys::pressedBy(QKeySequence("/"),&plain) && !keys::pressedBy(QKeySequence("Ctrl+/"),&plain) && keys::pressedBy(QKeySequence("Ctrl+/"),&ctrlSlash));
  ShortcutEditor dialog({&hover,&xray,&sheet});dialog.show();
  auto* binding=dialog.findChild<QKeySequenceEdit*>("shortcutBinding");
  QApplication::sendEvent(binding,&shiftSlash);CHECK(binding->keySequence()==QKeySequence("?"));
  binding->clear();QKeyEvent ctrlShift(QEvent::KeyPress,Qt::Key_Question,Qt::ControlModifier|Qt::ShiftModifier);QApplication::sendEvent(binding,&ctrlShift);
  CHECK(binding->keySequence()==QKeySequence("Ctrl+?"));
}
int main(int argc,char** argv) {
  QApplication app(argc,argv);QTemporaryDir settings;
  QCoreApplication::setOrganizationName("OPAD-tests");QCoreApplication::setApplicationName("shortcuts");
  QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
  return check::run_all(argc,argv);
}
