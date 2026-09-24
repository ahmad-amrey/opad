#include "MainWindow.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QKeySequenceEdit>

bool MainWindow::benchShortcuts() {
  const auto prefix=qEnvironmentVariable("OPAD_BENCH_SHORTCUTS");if(prefix.isEmpty())return false;
  auto phase=std::make_shared<int>(0);auto* timer=new QTimer(this);timer->setInterval(150);
  connect(timer,&QTimer::timeout,this,[this,prefix,phase,timer]{try {
    auto require=[](bool ok,const char* message){if(!ok)throw opad::Error(message);};
    auto key=[this](int code,Qt::KeyboardModifiers modifiers=Qt::NoModifier){
      QApplication::setActiveWindow(this);m_viewport->setFocus();
      QKeyEvent press(QEvent::KeyPress,code,modifiers);QApplication::sendEvent(m_viewport,&press);
      QKeyEvent release(QEvent::KeyRelease,code,modifiers);QApplication::sendEvent(m_viewport,&release);
    };
    switch((*phase)++) {
      case 0: {
        for(auto* a:m_actions)for(auto* b:m_actions)if(a!=b&&shortcuts::overlaps(a->objectName(),b->objectName())&&shortcuts::conflicts(a->shortcut(),b->shortcut()))
          throw opad::Error("default shortcut conflict: "+a->objectName().toStdString()+" / "+b->objectName().toStdString());
        require(action("view.2d")->shortcut()==QKeySequence("Shift+2"),"2D shortcut");
        require(action("view.ortho")->shortcut()==QKeySequence("Shift+3"),"projection shortcut");
        require(!m_viewport->twoDimensional(),"drawing load enabled 2D mode");
        key(Qt::Key_2,Qt::ShiftModifier);require(m_viewport->twoDimensional(),"Shift+2 did not enable 2D");
        key(Qt::Key_2,Qt::ShiftModifier);require(!m_viewport->twoDimensional(),"Shift+2 did not disable 2D");
        const bool ortho=m_viewport->isOrthographic();key(Qt::Key_3,Qt::ShiftModifier);require(m_viewport->isOrthographic()!=ortho,"Shift+3 did not toggle projection");key(Qt::Key_3,Qt::ShiftModifier);
        const bool grid=action("view.grid")->isChecked();key(Qt::Key_G);require(action("view.grid")->isChecked()!=grid,"G did not toggle grid");
        for(const auto& pair:QList<QPair<QString,int>>{{"top",Qt::Key_Up},{"bottom",Qt::Key_Down},{"left",Qt::Key_Left},{"right",Qt::Key_Right},{"front",Qt::Key_PageUp},{"back",Qt::Key_PageDown},{"iso",Qt::Key_H}}) {
          bool triggered=false;auto connection=connect(action("view."+pair.first),&QAction::triggered,this,[&]{triggered=true;});key(pair.second,Qt::ShiftModifier);disconnect(connection);require(triggered,"orientation shortcut did not activate");
        }
        m_design->sketch()->begin({},"Shortcut benchmark",{{"base","xy"}},{},opad::design::Sketch().to_json());break;
      }
      case 1: {
        require(!action("inspect.distance")->isEnabled(),"measurement binding still active in sketch");
        key(Qt::Key_D);require(m_design->sketch()->tool()=="dimension","sketch D shortcut");
        key(Qt::Key_T);require(m_design->sketch()->tool()=="trim","sketch T shortcut");
        key(Qt::Key_R);require(m_design->sketch()->tool()=="rect","sketch R shortcut");
        key(Qt::Key_A);require(m_design->sketch()->tool()=="arc3","sketch A shortcut");
        key(Qt::Key_L);require(m_design->sketch()->tool()=="line","sketch L shortcut");
        action("sketch.line")->setShortcut(QKeySequence("Alt+L"));key(Qt::Key_C);key(Qt::Key_L);require(m_design->sketch()->tool()=="circle","old sketch binding was still hard-coded");
        key(Qt::Key_L,Qt::AltModifier);require(m_design->sketch()->tool()=="line","custom sketch binding failed");action("sketch.line")->setShortcut(QKeySequence("L"));
        bool iso=false;auto connection=connect(action("view.iso"),&QAction::triggered,this,[&]{iso=true;});key(Qt::Key_H,Qt::ShiftModifier);disconnect(connection);require(iso,"modified view shortcut swallowed by sketch");
        key(Qt::Key_Escape);require(m_design->sketch()->tool()=="select","Escape editing control failed");
        bool construction=false;auto constructionConnection=connect(action("sketch.construction"),&QAction::triggered,this,[&]{construction=true;});key(Qt::Key_X);disconnect(constructionConnection);require(construction,"sketch construction shortcut failed");
        m_design->sketch()->end();break;
      }
      default: {
        timer->stop();ShortcutEditor dialog(m_actions,this);dialog.show();dialog.grab().save(prefix+".png");
        dialog.findChild<QLineEdit*>("shortcutSearch")->setText("view");dialog.grab().save(prefix+".view.png");
        dialog.findChild<QLineEdit*>("shortcutSearch")->clear();dialog.findChild<QKeySequenceEdit*>("shortcutLookup")->setKeySequence(QKeySequence("D"));dialog.grab().save(prefix+".lookup.png");
        trace::log("bench: view shortcuts, mode toggles, sketch bindings and shortcut editor PASS");QCoreApplication::exit(0);break;
      }
    }
  }catch(const std::exception& e){timer->stop();trace::log(QString("bench: shortcuts FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});
  timer->start();return true;
}
