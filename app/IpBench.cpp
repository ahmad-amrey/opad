#include "MainWindow.hpp"
#include <QApplication>
#include <QDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include "Legal.hpp"
#include "opad/drawing_io.hpp"

// OPAD_BENCH_IP=<prefix>: the IP switches through their actions. The view cube's corner under the pointer is a corner
// view with view/cubeEdgesCorners on, the face it lies on with it off (UI-54), and back. The ODA File Converter is off
// by default and turns on only through its terms box (UI-14; <prefix>.oda-terms.png). Help shows the third-party notices
// (UI-13; <prefix>.notices.png), About Qt and an About box that names the licences.
bool MainWindow::benchIp() {
  const QString prefix=qEnvironmentVariable("OPAD_BENCH_IP");if(prefix.isEmpty())return false;
  bool all=true;
  auto report=[&](const QString& what,bool ok){all=all&&ok;trace::log(QString("bench: ip: %1 %2").arg(what,ok?"PASS":"FAIL"));};
  m_viewport->standardView("iso");  // the front-right-top corner lies at the cube's centre on screen
  QAction* cube=action("view.cubeEdgesCorners");
  const QString on=m_viewport->benchCubePart(0,-5),face=m_viewport->benchCubePart(0,-36);
  report(QString("cube default full (%1), corner under the pointer: %2, top face: %3").arg(cube->isChecked()).arg(on,face),cube->isChecked() && on=="corner" && face=="side");
  cube->trigger();
  const QString off=m_viewport->benchCubePart(0,-5);
  report(QString("cube faces only: corner area picks %1, setting %2").arg(off,m_settings.value("view/cubeEdgesCorners").toString()),off=="side" && !m_settings.value("view/cubeEdgesCorners").toBool());
  cube->trigger();
  const QString back=m_viewport->benchCubePart(0,-5);
  report(QString("cube full again: %1").arg(back),back=="corner" && m_settings.value("view/cubeEdgesCorners").toBool());

  QAction* oda=findChild<QAction*>("files.useOda");
  // Answered on the first turn of the box's loop, before the bench's own dismissal (BenchQuiet: Cancel).
  auto answer=[this,prefix](bool accept){QTimer::singleShot(0,this,[this,accept,prefix]{
    auto* box=findChild<QMessageBox*>("odaTerms");if(!box)return;
    if(accept)box->grab().save(prefix+".oda-terms.png");
    for(auto* b:box->buttons())if(box->buttonRole(b)==(accept?QMessageBox::AcceptRole:QMessageBox::RejectRole))return b->click();});};
  const bool odaSet=m_settings.value("files/useOda",false).toBool();
  report(QString("ODA converter off by default (action %1, core %2, setting %3)").arg(oda && oda->isChecked()).arg(opad::use_oda()).arg(odaSet),oda && !oda->isChecked() && !opad::use_oda() && !odaSet);
  if(oda){
    answer(false);oda->trigger();
    report("ODA terms box cancelled: stays off",!oda->isChecked() && !opad::use_oda() && !m_settings.value("files/useOda",false).toBool());
    answer(true);oda->trigger();
    report("ODA terms accepted: on",oda->isChecked() && opad::use_oda() && m_settings.value("files/useOda",false).toBool());
    oda->trigger();
    report("ODA off again without asking",!oda->isChecked() && !opad::use_oda() && !m_settings.value("files/useOda",false).toBool());
  }
  // Presets are named after what they mimic, never as the product (trademarks).
  QStringList presets;bool styled=true;
  for(const char* id:{"nav.fusion","nav.solidworks","nav.onshape","nav.blender"}){const QString text=action(id)->text();presets<<text;styled=styled&&text.endsWith("-style");}
  report("navigation presets: "+presets.join(", "),styled && presets[1]=="Navigation: SOLIDWORKS-style");

  // Help > Third-party licences (UI-13): the notices compiled in from the link libraries, in a dialog with About Qt.
  QMenu* help=nullptr;
  for(QAction* a:menuBar()->actions())if(a->menu() && a->menu()->actions().contains(action("help.licenses")))help=a->menu();
  report("Help menu: Third-party licences, About Qt, About",help && help->actions().contains(action("help.aboutqt")) && help->actions().contains(action("help.about")));
  action("help.licenses")->trigger();
  auto* notices=findChild<QDialog*>("thirdPartyNotices");
  auto* text=notices?notices->findChild<QPlainTextEdit*>("noticesText"):nullptr;
  const QString body=text?text->toPlainText():QString();
  if(notices)notices->grab().save(prefix+".notices.png");
  report(QString("third-party licences: %1 characters").arg(body.size()),body.startsWith("OPAD ") && body.contains("MIT licence") && body.contains("Trademarks") &&
         (!body.contains("packages.msys2.org") || (body.contains("* opencascade ") && body.contains("* qt6-") && body.contains("Licence texts"))));
  if(notices)notices->close();
  const QString about=legal::aboutText();
  report("About: licences and trademarks, no 'STEP viewer'",!about.contains("STEP viewer") && about.contains("MIT licence") && about.contains("Third-party licences") && about.contains("not affiliated"));
  action("help.about")->trigger();  // message boxes: dismissed by the bench (BenchQuiet), logged
  action("help.aboutqt")->trigger();
  QCoreApplication::exit(all?0:2);
  return true;
}
