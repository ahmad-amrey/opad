#include "MainWindow.hpp"
#include <QCoreApplication>

// OPAD_BENCH_IP=<prefix>: the IP switches through their actions. The view cube's corner under the pointer is a corner
// view with view/cubeEdgesCorners on, the face it lies on with it off (UI-54), and back.
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
  QCoreApplication::exit(all?0:2);
  return true;
}
