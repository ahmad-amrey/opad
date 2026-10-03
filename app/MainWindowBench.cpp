// The built-in benches (--bench-select); benches in their own files register through BenchRegistry.hpp.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "CheckPanel.hpp"
#include "FileAssociations.hpp"
#include "RecoveryManager.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

#include <Bnd_Box.hxx>

#include "Theme.hpp"
#include "Units.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace {
// Benches run in a hidden main window while the user works on the same desktop: their other top-level windows (tool
// panels, menus, message boxes) are kept off the screen, and a message box is logged and dismissed instead of waiting
// for a click that never comes.
class BenchQuiet : public QObject {
 public:
  BenchQuiet(QWidget* main, QObject* parent) : QObject(parent), m_main(main) {}
  bool eventFilter(QObject* o, QEvent* e) override {
    auto* w = qobject_cast<QWidget*>(o);
    if (!w || w == m_main || !w->isWindow()) return false;
    if (e->type() == QEvent::Polish) w->setAttribute(Qt::WA_DontShowOnScreen);
    if (e->type() == QEvent::Show)
      if (auto* box = qobject_cast<QMessageBox*>(w)) {
        trace::log(QStringLiteral("bench: message box dismissed: %1: %2").arg(box->windowTitle(), box->text()));
        QTimer::singleShot(0, box, [box] { box->done(QMessageBox::Cancel); });
      }
    return false;
  }
 private:
  QWidget* m_main;
};
}  // namespace

void MainWindow::setBenchSelect(bool on) {
  m_benchSelect = on;
  if (on) qApp->installEventFilter(new BenchQuiet(this, this));
}

// --bench-select: select every root once the load has settled, log how long the selection takes, quit.
void MainWindow::runBench() {
  if(bench::run(*this))return;  // the benches registered from their own files (OPAD_BENCH) first
  if(const auto mode=qEnvironmentVariable("OPAD_BENCH_RECOVERY");!mode.isEmpty()){m_recovery->bench(mode);return;}
  if(benchViewer())return;
  if(benchIp())return;
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_FILETYPES");!shot.isEmpty()){  // the dialog as drawn, nothing registered
    auto* dialog=new FileTypesDialog(this);dialog->show();
    QTimer::singleShot(300,this,[dialog,shot]{const bool saved=dialog->grab().save(shot);dialog->deleteLater();trace::log(QString("bench: file types dialog %1").arg(saved?"PASS":"FAIL"));QCoreApplication::exit(saved?0:2);});
    return;
  }
  // Read-only render regression: retain imported geometry and dump it before
  // the general selection benchmark hides/edits its leaf.
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_MESH_SHOT");!shot.isEmpty()) {
    m_viewport->standardView(qEnvironmentVariable("OPAD_BENCH_VIEW","iso"));
    m_viewport->fitAll();
    QTimer::singleShot(500,this,[this,shot] {
      const bool saved=m_viewport->grabImage().save(shot);
      trace::log(QString("bench: mesh render %1").arg(saved?"PASS":"FAIL"));
      QCoreApplication::exit(saved?0:2);
    });
    return;
  }
  // OPAD_BENCH_SHOWROOT=<prefix>: show hidden roots after the load; the status bar (every second, <prefix>-<n>.png)
  // must report the bodies streaming in until they are all displayed.
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_SHOWROOT");!shot.isEmpty()){
    std::vector<std::string> hidden;for(const auto& root:m_doc->scene.roots)if(const auto* n=m_doc->scene.node(root);n && !n->visible)hidden.push_back(root);
    for(const auto& id:hidden)m_doc->run("appearance",opad::json{{"target",id},{"visible",true}});
    auto count=std::make_shared<int>(0),reported=std::make_shared<int>(0);auto* timer=new QTimer(this);auto clock=std::make_shared<QElapsedTimer>();clock->start();
    connect(timer,&QTimer::timeout,this,[this,shot,timer,count,reported,clock]{
      if(m_displayJob)++*reported;
      statusBar()->grab().save(QString("%1-%2.png").arg(shot).arg(++*count));
      if((!m_displayJob && m_meshRemaining==0 && *count>2) || *count>=60){
        timer->stop();trace::log(QString("bench: show root: %1 hidden roots shown, displayed after %2 ms, progress reported in %3 of %4 grabs").arg(m_doc->scene.roots.size()).arg(clock->elapsed()).arg(*reported).arg(*count));
        QCoreApplication::exit(*reported>0?0:2);
      }
    });
    timer->start(1000);return;
  }
  // OPAD_BENCH_TWOD=<png>: 2D mode shows its card; an orbit press in 2D mode gives the hint; the card turns it off.
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_TWOD");!shot.isEmpty()){
    action("view.2d")->setChecked(true);
    QTimer::singleShot(300,this,[this,shot]{
      auto* card=m_chips->findChild<QLabel*>(QString(),Qt::FindDirectChildrenOnly);bool shown=false;
      for(auto* label:m_chips->findChildren<QLabel*>())if(label->text()==tr("2D mode"))card=label,shown=label->isVisibleTo(m_chips);
      m_chips->grab().save(shot);
      const QPointF at(m_viewport->width()/2.0,m_viewport->height()/2.0);
      QMouseEvent press(QEvent::MouseButtonPress,at,m_viewport->mapToGlobal(at),Qt::MiddleButton,Qt::MiddleButton,Qt::ShiftModifier);
      QCoreApplication::sendEvent(m_viewport,&press);
      const bool hinted=QToolTip::text().contains("2D mode");
      QMouseEvent release(QEvent::MouseButtonRelease,at,m_viewport->mapToGlobal(at),Qt::MiddleButton,Qt::NoButton,Qt::ShiftModifier);
      QCoreApplication::sendEvent(m_viewport,&release);
      QMouseEvent click(QEvent::MouseButtonRelease,QPointF(3,3),card->mapToGlobal(QPointF(3,3)),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
      QCoreApplication::sendEvent(card,&click);
      const bool left=!action("view.2d")->isChecked() && !card->isVisibleTo(m_chips);
      trace::log(QString("bench: 2D mode card %1, orbit hint %2, card leaves 2D mode %3 %4").arg(shown?"shown":"MISSING",hinted?"shown":"MISSING",left?"yes":"NO",shown&&hinted&&left?"PASS":"FAIL"));
      QCoreApplication::exit(shown&&hinted&&left?0:2);
    });
    return;
  }
  if(qEnvironmentVariableIsSet("OPAD_BENCH_LEAVE")){const bool ok=m_viewport->benchLeave();QCoreApplication::exit(ok?0:2);return;}
  // OPAD_BENCH_FIT=model|origin [OPAD_BENCH_FITSHOT=<prefix>]: with the grid on, the load's own fit, F (nothing selected)
  // and Shift+F frame the model as fitting its own bodies does; the grid as drawn holds the model, around its footprint
  // (model) or the origin, and in 2D mode around the view, which stays on the model. OCCT's box-less FitAll takes the
  // centre of any infinite structure over 500 m across (the grid): a model 600 m out was framed with (0,0,0), 9x small.
  if(const QString fit=qEnvironmentVariable("OPAD_BENCH_FIT");!fit.isEmpty()){
    const bool gridAtLoad=action("view.grid")->isChecked();const auto loaded=m_viewport->cameraJson();
    action("view.grid")->setChecked(true);
    QTimer::singleShot(300,this,[this,fit,gridAtLoad,loaded]{
      m_viewport->fitNodes(m_doc->scene.roots);
      const auto reference=m_viewport->cameraJson();
      const double model=reference.value("scale",0.0);
      bool ok=model>0;
      auto check=[&](const QString& what,const opad::json& camera){
        double off=0;
        for(int i=0;i<3;++i)off+=std::pow(camera["target"][i].get<double>()-reference["target"][i].get<double>(),2);
        off=std::sqrt(off);
        const double scale=camera.value("scale",0.0);
        const bool good=std::abs(scale/model-1)<0.05 && off<0.05*model;
        ok=ok&&good;
        trace::log(QString("bench: fit %1: view %2 mm (model %3 mm), centre %4 mm off %5").arg(what).arg(scale,0,'f',0).arg(model,0,'f',0).arg(off,0,'f',0).arg(good?"PASS":"FAIL"));
      };
      if(gridAtLoad)check("on load",loaded);
      for(const char* id:{"view.fit","view.fitall"}){action(id)->trigger();check(id,m_viewport->cameraJson());}
      Bnd_Box box;for(const auto& body:m_doc->scene.all_bodies())box.Add(opad::node_world_bbox(m_doc->doc,m_doc->scene,body));
      double x0=0,y0=0,z0=0,x1=0,y1=0,z1=0;if(!box.IsVoid())box.Get(x0,y0,z0,x1,y1,z1);
      const double size=std::max(x1-x0,y1-y0);
      const QString shot=qEnvironmentVariable("OPAD_BENCH_FITSHOT");
      // Where OCCT draws the grid: its structure's box.
      auto grid=[&](bool flat){
        if(!shot.isEmpty())m_viewport->grabImage().save(shot+(flat?".2d.png":".3d.png"));
        const Bnd_Box drawn=m_viewport->benchGridBox();
        double g0=0,h0=0,gz0=0,g1=0,h1=0,gz1=0;if(!drawn.IsVoid())drawn.Get(g0,h0,gz0,g1,h1,gz1);
        const double gx=(g0+g1)/2,gy=(h0+h1)/2,half=std::max(g1-g0,h1-h0)/2;
        const auto target=m_viewport->cameraJson()["target"];const double tx=target[0].get<double>(),ty=target[1].get<double>();
        bool good=!drawn.IsVoid() && !box.IsVoid();
        if(flat)good=good && g0<=tx && tx<=g1 && h0<=ty && ty<=h1 && std::hypot(tx-(x0+x1)/2,ty-(y0+y1)/2)<0.05*size;  // still on the model
        else good=good && x0>=g0 && x1<=g1 && y0>=h0 && y1<=h1 && (fit=="origin"?std::abs(gx)<1 && std::abs(gy)<1:std::abs(gx-(x0+x1)/2)<=size/2 && std::abs(gy-(y0+y1)/2)<=size/2 && half<=size);
        ok=ok&&good;
        trace::log(QString("bench: fit grid%1: drawn around (%2, %3), %4 mm each way, model %5 mm wide around (%6, %7), expected around the %8, view at (%9, %10) %11").arg(flat?" in 2D mode":"").arg(gx,0,'f',0).arg(gy,0,'f',0).arg(half,0,'f',0).arg(size,0,'f',0).arg((x0+x1)/2,0,'f',0).arg((y0+y1)/2,0,'f',0).arg(flat?"view":fit).arg(tx,0,'f',0).arg(ty,0,'f',0).arg(good?"PASS":"FAIL"));
      };
      grid(false);
      action("view.2d")->setChecked(true);
      grid(true);
      QCoreApplication::exit(ok?0:2);
    });
    return;
  }
  if(benchShortcuts())return;
  if(benchDrawingImport())return;
  if(benchTodo9())return;
  if(benchAnnotateLarge())return;
  if(qEnvironmentVariableIsSet("OPAD_BENCH_INSTANCES")) {
    if(m_doc->scene.all_bodies().empty()){QCoreApplication::exit(2);return;}
    const auto source=m_doc->scene.all_bodies().front(),copy=opad::new_uuid();
    m_doc->doc.append({{"op","import"},{"nodes",opad::json::array({{{"type","body"},{"id",copy},{"key",m_doc->scene.node(source)->body_key},{"name","Linked copy"}}})}});m_doc->refresh();
    const auto count=m_doc->doc.ops.size();browseInstances(source);
    auto* panel=findChild<ToolPanel*>("instanceBrowser");bool valid=panel && m_viewport->isolatedNodes()==std::vector<std::string>{source};
    if(panel){panel->findChild<QPushButton*>("nextInstance")->click();valid=valid&&m_viewport->isolatedNodes()!=std::vector<std::string>{source};panel->findChild<QPushButton*>("previousInstance")->click();valid=valid&&m_viewport->isolatedNodes()==std::vector<std::string>{source};
      // Looking around keeps browsing (TODO 10 A5): Fit and Home leave the panel and the isolation alone.
      action("view.fit")->trigger();action("view.home")->trigger();valid=valid&&panel->isVisible()&&m_viewport->isolatedNodes()==std::vector<std::string>{source};
      if(!valid)trace::log("bench: Fit or Home ended instance browsing");
      panel->hide();valid=valid&&!m_viewport->isIsolated()&&m_doc->doc.ops.size()==count;}
    trace::log(valid?"bench: instance next/previous, isolation restoration and no document edits PASS":"bench: instance browser FAIL");QCoreApplication::exit(valid?0:2);return;
  }
  if(benchLargeSketch())return;
  if(benchTodo5())return;
  if(const auto mode=qEnvironmentVariable("OPAD_BENCH_NAVIGATION");!mode.isEmpty()) {
    if(mode=="write") {
      action("view.grid")->setChecked(true);action("view.ortho")->setChecked(false);action("view.wire")->trigger();
      auto camera=m_viewport->cameraJson();camera["target"]={10,20,30};camera["eye"]={100,120,130};camera["up"]={0,0,1};camera["scale"]=175;camera["fov_deg"]=47;
      m_viewport->setCameraJson(camera);saveLastView();m_settings.sync();
      trace::log("bench: saved view preferences PASS");QCoreApplication::exit(0);return;
    }
    const auto camera=m_viewport->cameraJson();
    if(!action("view.grid")->isChecked() || m_viewport->isOrthographic() || m_viewport->style()!=Viewport::Style::Wireframe || std::abs(camera["target"][0].get<double>()-10)>1e-6 || std::abs(camera["target"][1].get<double>()-20)>1e-6 || std::abs(camera["target"][2].get<double>()-30)>1e-6 || std::abs(camera["scale"].get<double>()-175)>1e-6 || std::abs(camera["fov_deg"].get<double>()-47)>1e-6) {
      trace::log("bench: restored view preferences FAIL: "+QString::fromStdString(camera.dump()));QCoreApplication::exit(2);return;
    }
    trace::log("bench: restored view preferences across launches PASS");
    auto once=std::make_shared<QMetaObject::Connection>();
    *once=connect(m_viewport,&Viewport::filterApplied,this,[this,once] {
      disconnect(*once);
      QTimer::singleShot(0,this,[this] {
        m_design->planePicker()->choose({{"base","yz"}});
        QTimer::singleShot(700,this,[this] {
          const auto camera=m_viewport->cameraJson();
          const double dx=camera["eye"][0].get<double>()-camera["target"][0].get<double>();
          const double dy=camera["eye"][1].get<double>()-camera["target"][1].get<double>();
          const double dz=camera["eye"][2].get<double>()-camera["target"][2].get<double>();
          if(m_design->pickingPlane() || dx<=0 || std::abs(dy)>1e-6 || std::abs(dz)>1e-6) {trace::log("bench: plane alignment FAIL: "+QString::fromStdString(camera.dump()));QCoreApplication::exit(2);return;}
          m_doc->newDocument();m_viewport->home();const auto empty=m_viewport->cameraJson();
          const double extent=m_settings.value("view/gridExtent",100.0).toDouble();
          bool covered=true;
          for(double x:{-extent,extent}) for(double y:{-extent,extent}) covered=covered && m_viewport->rect().contains(m_viewport->widgetPoint({x,y,0}));
          trace::log(covered?"bench: plane alignment and empty Home grid coverage PASS":"bench: empty Home grid coverage FAIL: "+QString::fromStdString(empty.dump()));QCoreApplication::exit(covered?0:2);
        });
      });
    });
    action("view.alignPlane")->trigger();return;
  }

  if(qEnvironmentVariableIsSet("OPAD_BENCH_REVIEW")) {
    m_doc->newDocument();
    const auto id=m_doc->run("append",{{"op",{{"op","measurement"},{"kind","distance"},{"refs",{"point/0,0,0","point/3,0,0"}},{"result",{{"value",3},{"unit","mm"}}}}}})["appended"][0].get<std::string>();
    const auto cards=m_annotations->findChildren<NoteCard*>();
    if(cards.size()!=1 || !cards[0]->note().measurement || !cards[0]->note().value.contains("3")) {QCoreApplication::exit(2);return;}
    bool removed=false;
    for(auto* button:cards[0]->findChildren<QPushButton*>()) if(button->text()==tr("Remove")) {removed=true;button->click();break;}
    if(!removed) {QCoreApplication::exit(2);return;}
    if(!m_doc->scene.measurements.empty()) {QCoreApplication::exit(2);return;}
    restoreOp(id); if(m_doc->scene.measurements.size()!=1) {QCoreApplication::exit(2);return;}
    showOpGitLog(id,{});
    auto* unsaved=findChild<QDialog*>("opGitLog");
    if(!unsaved || !unsaved->property("finished").toBool()) {QCoreApplication::exit(2);return;}
    unsaved->setObjectName("closedGitLog");unsaved->close();
    showOpGitLog(id,qEnvironmentVariable("OPAD_BENCH_REVIEW"));
    QPointer<QDialog> log=findChild<QDialog*>("opGitLog");
    auto* poll=new QTimer(this);poll->setInterval(50);
    connect(poll,&QTimer::timeout,this,[=] {
      if(!log || !log->property("finished").toBool()) return;
      const auto text=log->findChild<QPlainTextEdit*>()->toPlainText();
      trace::log("bench: review measurement remove/restore and Git log PASS: "+text);
      QCoreApplication::exit(text.isEmpty()?2:0);
    });poll->start();return;
  }

  m_benchSelect = false;
  if (const auto next=qEnvironmentVariable("OPAD_BENCH_IMPORT_NEXT"); !next.isEmpty()) {
    connect(m_doc,&AppDocument::loadFinished,this,[this](bool ok,const QString& error) {
      if (!ok) { trace::log(error); QCoreApplication::exit(2); return; }
      QTimer::singleShot(3500,this,[this] {
        const bool imported=m_doc->scene.all_bodies().size()>=2;
        m_doc->newDocument();
        const bool clean=!m_design->sketchActive() && m_lastMeasure.is_null() && m_doc->scene.all_bodies().empty();
        trace::log(QString("bench: sequential import %1; document reset %2").arg(imported).arg(clean));
        QCoreApplication::exit(imported && clean?0:2);
      });
    });
    m_doc->startImport(next); return;
  }

  if(qEnvironmentVariableIsSet("OPAD_BENCH_EXPORT_DIALOG")) { exportDialog(); QCoreApplication::exit(0); return; }
  if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD")) { drawingToSketch(); return; }
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_STATUS");!shot.isEmpty()) {
    QTimer::singleShot(700,this,[this,shot] {
      const bool dark=theme::current().dark;
      const bool a=action("view.extensions")->isChecked(),b=action("view.tracking")->isChecked(),c=action("view.gridSnap")->isChecked();
      action("view.extensions")->setChecked(true); action("view.tracking")->setChecked(false); action("view.gridSnap")->setChecked(true);
      theme::apply(true); statusBar()->grab().save(shot+".dark.png");
      theme::apply(false); statusBar()->grab().save(shot+".light.png");
      action("view.extensions")->setChecked(a); action("view.tracking")->setChecked(b); action("view.gridSnap")->setChecked(c); theme::apply(dark);
      QCoreApplication::exit(action("file.import")->text().contains("STEP")?2:0);
    }); return;
  }
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_SCENE");!shot.isEmpty()) {
    // OPAD_BENCH_CAMERA=<camera json> frames a chosen spot (zoomed-in tessellation checks) instead of top + fit.
    // OPAD_BENCH_ZOOM=<factor> zooms the fitted OPAD_BENCH_VIEW (default top) into its centre instead.
    const auto camera=opad::json::parse(qEnvironmentVariable("OPAD_BENCH_CAMERA").toStdString(),nullptr,false);
    const double zoom=qEnvironmentVariable("OPAD_BENCH_ZOOM","0").toDouble();
    if(!camera.is_object())m_viewport->standardView(qEnvironmentVariable("OPAD_BENCH_VIEW","top"));
    QTimer::singleShot(700,this,[this,shot,camera,zoom] {
      const bool refine=camera.is_object() || zoom>0;
      if(camera.is_object())m_viewport->setCameraJson(camera);else m_viewport->fitAll();
      if(zoom>0){auto fitted=m_viewport->cameraJson();fitted["scale"]=fitted.value("scale",1.0)/zoom;m_viewport->setCameraJson(fitted);}
      if(refine && !qEnvironmentVariableIsSet("OPAD_BENCH_NO_REFINE"))m_viewport->requestRefinement();
      QTimer::singleShot(refine?qEnvironmentVariable("OPAD_BENCH_WAIT","3000").toInt():0,this,[this,shot]{QCoreApplication::exit(m_viewport->grabImage().save(shot)?0:2);});
    });
    return;
  }
  if (qEnvironmentVariableIsSet("OPAD_BENCH_PICKING")) {
    auto once = std::make_shared<QMetaObject::Connection>();
    *once = connect(m_viewport, &Viewport::filterApplied, this, [this, once] {
      disconnect(*once);
      QTimer::singleShot(500, this, [this] {
        startTool("distance");
        if (!m_viewport->benchPicking()) return QCoreApplication::exit(2);
        if (qEnvironmentVariableIsSet("OPAD_BENCH_ORBIT_PERF") || qEnvironmentVariableIsSet("OPAD_BENCH_HOVER_FADE")) return QCoreApplication::exit(0);
        QTimer::singleShot(1500, this, [this] {
          const bool ok = m_toolPicks.size() == 2 && m_lastMeasure.value("kind", "") == "distance";
          trace::log(QStringLiteral("bench: picking guided distance %1: %2").arg(ok ? "PASS" : "FAIL", QString::fromStdString(m_lastMeasure.dump())));
          if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) {
            m_viewport->grabImage().save(shot + ".viewport.png");
            m_toolPanel->grab().save(shot + ".panel.png");
            m_prompt->grab().save(shot + ".prompt.png");
          }
          if (!ok) return QCoreApplication::exit(2);
          const auto center = m_toolPicks.front();
          cancelTool();
          m_viewport->selectRefs({center});
          startTool("radius");
          QTimer::singleShot(1500, this, [this] {
            const bool radiusOk = m_viewport->selectionFilter() == Viewport::SelFilter::Vertex
                && m_lastMeasure.value("kind", "") == "radius" && m_lastMeasure.contains("diameter");
            trace::log(QStringLiteral("bench: picking guided radius %1: %2").arg(radiusOk ? "PASS" : "FAIL", QString::fromStdString(m_lastMeasure.dump())));
            if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) {
              m_viewport->grabImage().save(shot + ".radius.png");
              m_toolPanel->grab().save(shot + ".radius-panel.png");
            }
            if (!radiusOk) return QCoreApplication::exit(2);
            const auto saved=m_doc->scene.measurements.size();m_pinAction->trigger();
            const bool pinned=m_doc->scene.measurements.size()==saved+1;
            trace::log(QString("bench: pin measurement %1").arg(pinned?"PASS":"FAIL"));
            if(!pinned) return QCoreApplication::exit(2);
            // Route Escape from a child of the floating measurement window.
            QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
            QCoreApplication::sendEvent(m_toolSteps,&escape);
            if(const QString shot=qEnvironmentVariable("OPAD_BENCH_UISHOT");!shot.isEmpty()) {
              m_browserOverlay->setAutoHide(true);
              QTimer::singleShot(190,this,[this,shot] { m_browserOverlay->grab().save(shot+".browser.png"); });
            }
            QTimer::singleShot(250, this, [this] {
              const bool cleared = m_viewport->selection().empty();
              trace::log(QStringLiteral("bench: picking Esc clears centers %1").arg(cleared ? "PASS" : "FAIL"));
              m_doc->newDocument();
              const bool clean=m_doc->scene.measurements.empty() && m_lastMeasure.is_null() && !m_design->sketchActive();
              trace::log(QString("bench: transient measurement reset %1").arg(clean?"PASS":"FAIL"));
              QCoreApplication::exit(cleared && clean ? 0 : 2);
            });
          });
        });
      });
    });
    m_viewport->setSelectionFilter(Viewport::SelFilter::Vertex);
    return;
  }
  // Deterministic presentation smoke check: a saved measurement JSON and output prefix.
  if (const QString source = qEnvironmentVariable("OPAD_BENCH_MEASUREMENT"); !source.isEmpty()) {
    const opad::json result = opad::json::parse(opad::read_text_file(source.toStdString()));
    startTool(QString::fromStdString(result.value("kind", "distance")));
    QTimer::singleShot(1000, this, [this, result] {
      if (const QString view = qEnvironmentVariable("OPAD_BENCH_VIEW"); !view.isEmpty()) {
        if (view == "perspective") m_viewport->setOrthographic(false);
        else m_viewport->standardView(view);
      }
      if (qEnvironmentVariable("OPAD_BENCH_THEME") == "light") {
        theme::apply(false);
        m_viewport->setTokens(theme::current());
      }
      m_lastMeasure = result;
      m_viewport->showMeasurement(result);
      refreshToolUi();
      QTimer::singleShot(300, this, [this] {
        const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT");
        m_viewport->grabImage().save(shot + ".viewport.png");
        m_toolPanel->grab().save(shot + ".panel.png");
        QCoreApplication::quit();
      });
    });
    return;
  }
  // OPAD_BENCH_CHECK=interference|print (TODO 10 B13, B17): run the check through its panel, click the first finding,
  // grab the panel (OPAD_BENCH_UISHOT: <shot>.check.png), log PASS when there are findings and the click showed them.
  if (const QString kind = qEnvironmentVariable("OPAD_BENCH_CHECK"); !kind.isEmpty()) {
    startCheck(kind == "print");
    auto ticks = std::make_shared<int>(0);
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this, kind, ticks, timer] {
      if (++*ticks > 300) { timer->stop(); trace::log("bench: check timed out FAIL"); QCoreApplication::exit(2); return; }
      if (m_checkJob) return;
      timer->stop();
      const int findings = m_checks->findingCount();
      if (findings > 0) m_checks->activate(0);
      QTimer::singleShot(2500, this, [this, kind, findings] {
        if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) m_toolPanel->grab().save(shot + ".check.png");
        const bool shown = !m_viewport->selection().empty();
        trace::log(QStringLiteral("bench: %1 check, %2 findings, finding shown %3 %4").arg(kind).arg(findings).arg(shown).arg(findings > 0 && shown ? "PASS" : "FAIL"));
        QCoreApplication::exit(findings > 0 && shown ? 0 : 2);
      });
    });
    timer->start();
    return;
  }
  // OPAD_BENCH_DESIGN=<png>: sketch + extrude through the design controller, dump the frame, quit.
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_DESIGN"); !shot.isEmpty()) {
    setWorkspace("design");
    m_design->bench();
    QTimer::singleShot(qEnvironmentVariableIsSet("OPAD_BENCH_RULE") ? 13000 : 9000, this, [this, shot] {
      trace::log(QStringLiteral("bench: design: %1 bodies, %2 features, %3 unresolved").arg(m_doc->scene.all_bodies().size()).arg(m_doc->scene.features.size()).arg(m_doc->scene.unresolved.size()));
      m_viewport->benchDesignShot(shot);
      if (const QByteArray ui = qgetenv("OPAD_BENCH_UISHOT"); !ui.isEmpty()) grab().save(QString::fromLocal8Bit(ui));
      guarded([this] { m_doc->save(); });  // the round trip through the file, and no "unsaved changes" question on the way out
      QTimer::singleShot(500, qApp, &QCoreApplication::quit);
    });
    return;
  }
  // OPAD_BENCH_DISTANCE=<n>: time body-to-body distance between the n bodies with the most faces (every pair),
  // on a worker like the tool does. The worst case for measure_distance; compare values with opad-cli measure.
  if (const int n = qEnvironmentVariableIntValue("OPAD_BENCH_DISTANCE"); n > 1) {
    const std::vector<std::string> bodies = m_doc->scene.all_bodies();
    m_jobs->async(tr("Measuring %1").arg(tr("Distance")), [this, bodies, n](Progress) {
      std::vector<std::pair<int, std::string>> heavy;  // counting faces walks each body: on the worker
      for (const auto& b : bodies) {
        try {
          heavy.push_back({opad::subshape_count(opad::node_world_shape(m_doc->doc, m_doc->scene, b), opad::Ref::Kind::Face), b});
        } catch (const std::exception&) {
        }
      }
      std::sort(heavy.rbegin(), heavy.rend());
      heavy.resize(std::min<size_t>(heavy.size(), static_cast<size_t>(n)));
      for (size_t i = 0; i < heavy.size(); ++i)
        for (size_t k = i + 1; k < heavy.size(); ++k) {
          opad::Ref a, b;
          a.body = heavy[i].second;
          b.body = heavy[k].second;
          QElapsedTimer clock;
          clock.start();
          QString out;
          try {
            out = QString::number(opad::measure_distance(m_doc->doc, m_doc->scene, a, b)["value"].get<double>(), 'g', 15);
          } catch (const std::exception& e) {
            out = QString::fromUtf8(e.what());
          }
          trace::log(QStringLiteral("bench: distance %1 (%2 faces) <-> %3 (%4 faces) = %5 in %6 ms").arg(QString::fromStdString(a.body)).arg(heavy[i].first).arg(QString::fromStdString(b.body)).arg(heavy[k].first).arg(out).arg(clock.elapsed()));
        }
    }, [](bool, const QString&) { QCoreApplication::quit(); });
    return;
  }
  // OPAD_BENCH_SECTION=<png>: section on (Z through the model's middle), then hover and drag the plane outline's
  // handle strip through synthetic mouse events on the viewport widget: logs the hovered side and the plane's
  // origin before and after the drag, dumps the frame with the handle hovered, quits.
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_SECTION"); !shot.isEmpty()) {
    m_viewport->fitAll();
    QTimer::singleShot(1500, this, [this] { action("inspect.section")->setChecked(true); });
    QTimer::singleShot(2500, this, [this, shot] {
      auto vec = [](const opad::Vec3& v) { return QStringLiteral("(%1 %2 %3)").arg(v[0], 0, 'f', 2).arg(v[1], 0, 'f', 2).arg(v[2], 0, 'f', 2); };
      auto mouse = [this](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent e(type, at, m_viewport->mapToGlobal(at), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(m_viewport, &e);
      };
      QPointF at;
      if (!m_viewport->benchSectionHandle(at)) {
        trace::log(QStringLiteral("bench: section: no plane outline (section %1)").arg(m_viewport->sectionEnabled()));
        QCoreApplication::quit();
        return;
      }
      mouse(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
      const opad::Vec3 before = m_section->origin();
      trace::log(QStringLiteral("bench: section: handle at %1,%2 hover side %3 cursor %4 origin %5").arg(at.x()).arg(at.y()).arg(m_viewport->sectionHover()).arg(m_viewport->cursor().shape()).arg(vec(before)));
      QTimer::singleShot(300, this, [this, shot, at, mouse, vec, before] {
        m_viewport->grabImage().save(shot);  // hovered: outline + arrow
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step <= 4; ++step) mouse(QEvent::MouseMove, at + QPointF(0, -20.0 * step), Qt::NoButton, Qt::LeftButton);
        const opad::Vec3 dragged = m_section->origin();
        mouse(QEvent::MouseButtonRelease, at + QPointF(0, -80), Qt::LeftButton, Qt::NoButton);
        const opad::Vec3 after = m_section->origin();
        const double moved = std::sqrt((after[0] - before[0]) * (after[0] - before[0]) + (after[1] - before[1]) * (after[1] - before[1]) + (after[2] - before[2]) * (after[2] - before[2]));
        trace::log(QStringLiteral("bench: section: dragged 80 px up: origin %1 -> %2 (moved %3 mm, released %4, selected %5, hover side %6)")
                       .arg(vec(before), vec(after)).arg(moved, 0, 'f', 2).arg(dragged == after).arg(m_viewport->selection().size()).arg(m_viewport->sectionHover()));
        QTimer::singleShot(300, this, [this, shot] {
          m_viewport->grabImage().save(shot.left(shot.size() - 4) + ".dragged.png");
          QCoreApplication::quit();
        });
      });
    });
    return;
  }
  // OPAD_BENCH_TOOL=<distance|angle|radius|bbox>[,faces]: walk a guided tool without a mouse. Start it, click twice
  // through the view controller, log the tool's state after each, dump the prompt bar and the panel next to
  // OPAD_BENCH_UISHOT, step back with Esc, quit.
  if (const QStringList spec = qEnvironmentVariable("OPAD_BENCH_TOOL").split(',', Qt::SkipEmptyParts); !spec.isEmpty()) {
    auto state = [this](const char* when) {
      trace::log(QStringLiteral("bench: tool '%1' %2: %3 picks, result %4").arg(m_tool.id, when).arg(m_toolPicks.size()).arg(QString::fromStdString(m_lastMeasure.dump()).left(240)));
    };
    m_viewport->fitAll();
    const QStringList second = qEnvironmentVariable("OPAD_BENCH_CLICK2", "0.38,0.62").split(',');  // where the second pick goes, as view fractions
    auto walk = [this, spec, state, second] {
    QTimer::singleShot(1500, this, [this, spec, state] { startTool(spec[0]); state("started"); });
    QTimer::singleShot(2300, this, [this, state] { m_viewport->benchClick(0.5, 0.5); QTimer::singleShot(700, this, [state] { state("after click 1"); }); });
    QTimer::singleShot(4500, this, [this, state, second] { m_viewport->benchClick(second.value(0).toDouble(), second.value(1).toDouble()); QTimer::singleShot(700, this, [state] { state("after click 2"); }); });
    QTimer::singleShot(14000, this, [this, state] {
      state("settled");
      if (const QString ui = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !ui.isEmpty()) {
        m_prompt->grab().save(ui + ".prompt.png");
        m_toolPanel->grab().save(ui + ".panel.png");
        m_viewport->grabImage().save(ui + ".viewport.png");
      }
      toolEscape();
    });
    QTimer::singleShot(14800, this, [this, state] { state("after Esc"); toolEscape(); toolEscape(); state("after Esc x3"); });
    QTimer::singleShot(15500, qApp, &QCoreApplication::quit);
    };
    // On a big model the filter switch is a sliced job: picking before it has reached every body hits nothing.
    if (spec.size() > 1) {
      auto once = std::make_shared<QMetaObject::Connection>();
      *once = connect(m_viewport, &Viewport::filterApplied, this, [once, walk] { disconnect(*once); walk(); });
      action("select." + spec[1])->trigger();
    } else {
      walk();
    }
    return;
  }
  const std::vector<std::string> roots = m_doc->scene.roots;
  auto t = std::make_shared<QElapsedTimer>();
  t->start();
  trace::log(QStringLiteral("bench: selecting %1 roots (%2 bodies)").arg(roots.size()).arg(m_doc->scene.all_bodies().size()));
  auto conn = std::make_shared<QMetaObject::Connection>();
  *conn = connect(m_viewport, &Viewport::selectionApplied, this, [this, t, conn] {
    disconnect(*conn);
    trace::log(QStringLiteral("bench: selection applied after %1 ms (%2 refs)").arg(t->elapsed()).arg(m_viewport->selection().size()));
    trace::log(QStringLiteral("bench: camera before fit %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
    m_viewport->fitSelection();
    trace::log(QStringLiteral("bench: camera after fitSelection %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
    m_viewport->fitAll();
    m_viewport->fitNodes(m_doc->scene.bodies_under(m_doc->scene.roots.front()).size() > 1 ? std::vector<std::string>{m_doc->scene.bodies_under(m_doc->scene.roots.front()).front()} : m_doc->scene.roots);
    trace::log(QStringLiteral("bench: camera after fitNodes(first body) %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
    // Phase 2: the deepest leaf, selected from the browser as a user would, then Fit selection.
    std::string leaf;
    size_t depth = 0;
    for (const auto& b : m_doc->scene.all_bodies()) {
      const size_t d = m_doc->scene.path_to(b).size();
      if (d > depth) { depth = d; leaf = b; }
    }
    auto conn2 = std::make_shared<QMetaObject::Connection>();
    *conn2 = connect(m_viewport, &Viewport::selectionApplied, this, [this, conn2, leaf] {
      disconnect(*conn2);
      trace::log(QStringLiteral("bench: leaf %1 selected (%2 refs)").arg(m_doc->nodeName(leaf)).arg(m_viewport->selection().size()));
      m_viewport->fitAll();
      trace::log(QStringLiteral("bench: camera after fitAll %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
      m_viewport->fitSelection();
      trace::log(QStringLiteral("bench: camera after fitSelection(leaf) %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
      m_viewport->benchPick();
      if (!qEnvironmentVariableIsSet("OPAD_BENCH_FILTER")) m_section->beginPick();  // then a face pick must set the section plane (logged as "section from face")
      // Undo/redo: hide the leaf, undo back to the saved state (clean again), redo.
      const size_t n0 = m_doc->doc.ops.size();
      m_doc->run("appearance", opad::json{{"target", leaf}, {"visible", false}});
      const bool d1 = m_doc->doc.dirty, u1 = m_doc->canUndo();
      m_doc->undo();
      const bool d2 = m_doc->doc.dirty;
      const size_t n2 = m_doc->doc.ops.size();
      m_doc->redo();
      trace::log(QStringLiteral("bench: undo/redo: after hide dirty=%1 canUndo=%2; after undo dirty=%3 ops %4->%5; after redo dirty=%6 ops %7 canRedo=%8").arg(d1).arg(u1).arg(d2).arg(n0).arg(n2).arg(m_doc->doc.dirty).arg(m_doc->doc.ops.size()).arg(m_doc->canRedo()));
      QTimer::singleShot(1500, this, [this] { m_viewport->fitAll(); m_viewport->benchPick(); });  // the board: a planar face at the centre
      if (const QByteArray shot = qgetenv("OPAD_BENCH_SHOT"); !shot.isEmpty()) m_viewport->benchShot(QString::fromLocal8Bit(shot));
      // The widget side of the window (ribbon, docks, splitters; the native viewport comes out blank): a UI check
      // that needs no mouse or keyboard driving. OPAD_BENCH_WORKSPACE=1 (Design) or =<id> switches to that workspace first.
      if (const QString ws = qEnvironmentVariable("OPAD_BENCH_WORKSPACE"); !ws.isEmpty()) setWorkspace(ws == "1" ? QString("design") : ws);
      if (const QByteArray ui = qgetenv("OPAD_BENCH_UISHOT"); !ui.isEmpty())
        QTimer::singleShot(300, this, [this, ui] { grab().save(QString::fromLocal8Bit(ui)); });  // after the layout has settled
      // OPAD_BENCH_FILTER=face|edge|vertex: switch the selection mode, time it, then pick a sub-shape.
      if (const QByteArray filter = qgetenv("OPAD_BENCH_FILTER"); !filter.isEmpty()) {
        QTimer::singleShot(3000, this, [this, filter] {
          auto ft = std::make_shared<QElapsedTimer>();
          ft->start();
          connect(m_viewport, &Viewport::filterApplied, this, [this, ft, filter] {
            trace::log(QStringLiteral("bench: filter %1 applied after %2 ms").arg(QString::fromLatin1(filter)).arg(ft->elapsed()));
            m_viewport->fitNodes({m_viewport->benchHeaviest()});
            m_viewport->benchPick();
            const auto refs = m_viewport->selection();
            trace::log(QStringLiteral("bench: sub-shape pick: %1 refs, kind %2 index %3").arg(refs.size()).arg(refs.empty() ? -1 : static_cast<int>(refs.front().kind)).arg(refs.empty() ? -1 : refs.front().index));
            if (const QByteArray shot = qgetenv("OPAD_BENCH_SUBSHOT"); !shot.isEmpty()) m_viewport->benchSubShot(QString::fromLocal8Bit(shot));
            if (!qEnvironmentVariableIsSet("OPAD_BENCH_BAND")) {
              QTimer::singleShot(1000, qApp, &QCoreApplication::quit);
              return;
            }
            // OPAD_BENCH_BAND: rubber band over everything in this mode, then clear it; the watchdog logs stalls.
            QTimer::singleShot(500, this, [this] {
              auto ht = std::make_shared<QElapsedTimer>();
              ht->start();
              connect(m_viewport, &Viewport::subHighlightApplied, this, [ht] { trace::log(QStringLiteral("bench: band highlight shown after %1 ms").arg(ht->elapsed())); });
              m_viewport->benchBand();
              QTimer::singleShot(6000, this, [this] {
                if (const QByteArray shot = qgetenv("OPAD_BENCH_BANDSHOT"); !shot.isEmpty()) m_viewport->grabImage().save(QString::fromLocal8Bit(shot));
                auto ct = std::make_shared<QElapsedTimer>();
                ct->start();
                connect(m_viewport, &Viewport::selectionApplied, this, [ct] {
                  trace::log(QStringLiteral("bench: band cleared after %1 ms").arg(ct->elapsed()));
                  QTimer::singleShot(1500, qApp, &QCoreApplication::quit);
                });
                m_viewport->clearSelection();
              });
            });
          });
          m_viewport->setSelectionFilter(filter == "edge" ? Viewport::SelFilter::Edge : filter == "vertex" ? Viewport::SelFilter::Vertex : Viewport::SelFilter::Face);
        });
        return;
      }
      QTimer::singleShot(4000, qApp, &QCoreApplication::quit);
    });
    m_browser->setSelectedIds({leaf});
    onBrowserSelection({leaf});
  });
  onBrowserSelection(roots);
}

// OPAD_BENCH_VIEWER=<out.opad>, opened on a file other than .opad: viewer mode shows itself, keeps view changes out of
// "unsaved", refuses edits (the question is dismissed here), becomes editable without drawing anything again, saves, and
// steps on to the next file in the folder.
bool MainWindow::benchViewer() {
  static bool ran = false;  // the next file it opens would start it again
  const QString out = qEnvironmentVariable("OPAD_BENCH_VIEWER");
  if (out.isEmpty()) return false;
  if (std::exchange(ran, true)) return true;
  auto fail = [](const QString& why) { trace::log("bench: viewer FAIL: " + why); QCoreApplication::exit(2); };
  if (!m_doc->browse || m_doc->scene.all_bodies().empty()) { fail("not in viewer mode"); return true; }
  bool card = false;
  for (auto* label : m_chips->findChildren<QLabel*>()) card = card || (label->text() == tr("Viewer · read-only") && label->isVisibleTo(m_chips));
  if (!card || !windowTitle().contains("Viewer")) { fail("no viewer card or title: " + windowTitle()); return true; }
  const std::string body = m_doc->scene.all_bodies().front();
  m_doc->run("appearance", opad::json{{"target", body}, {"visible", false}});
  if (m_doc->isDirty()) { fail("hiding a body made the viewed file unsaved"); return true; }
  const size_t ops = m_doc->doc.ops.size();
  action("edit.rename")->trigger();  // asks to save first; the bench dismisses the question
  bool refused = false;
  try { m_doc->run("rename", opad::json{{"target", body}, {"name", "x"}}); } catch (const std::exception&) { refused = true; }
  if (!refused || !m_doc->browse || m_doc->doc.ops.size() != ops) { fail("an edit went through in viewer mode"); return true; }
  setDocumentUnit("in");  // viewer mode: shown in inches for the session, the file untouched (UI-123)
  if (units::sessionUnit() != "in" || units::current().length != "in" || m_doc->doc.ops.size() != ops) { fail("inches in viewer mode"); return true; }
  QTimer::singleShot(500, this, [this, out, fail, body] {
    const int shown = m_viewport->displayedCount();
    auto remeshed = std::make_shared<int>(0);
    auto watch = connect(m_viewport, &Viewport::meshingProgress, this, [remeshed](int remaining) { if (remaining > 0) ++*remeshed; });
    makeEditable(out, [this, out, fail, body, shown, remeshed, watch] {
      disconnect(watch);
      // An editable document shows its own unit: the viewer session's inches end with viewer mode.
      if (!units::sessionUnit().empty() || units::current().length != m_doc->scene.units) return fail("the viewer's inches outlived viewer mode");
      const bool editable = !m_doc->browse && !m_doc->doc.has_live_bodies() && !m_doc->node(body)->visible;
      QTimer::singleShot(800, this, [this, out, fail, editable, shown, remeshed] {
        bool reloads = false;
        try { reloads = opad::resolve(opad::Document::load(std::filesystem::path(out.toStdU16String()))).all_bodies().size() == m_doc->scene.all_bodies().size(); } catch (...) {}
        trace::log(QString("bench: viewer editable %1, saved and reloaded %2, kept %3 of %4 bodies on screen, re-meshed %5 times")
                       .arg(editable).arg(reloads).arg(m_viewport->displayedCount()).arg(shown).arg(*remeshed));
        if (!editable || !reloads || m_viewport->displayedCount() != shown || *remeshed > 0) return fail("conversion");
        // Another file of the bench folder opens over this one (viewer mode again).
        const QString before = m_doc->path();
        QString other;
        for (const QFileInfo& file : QFileInfo(before).absoluteDir().entryInfoList({"*.step", "*.svg"}, QDir::Files))
          if (file.completeBaseName() != QFileInfo(before).completeBaseName()) other = file.absoluteFilePath();
        if (other.isEmpty()) return fail("no other file in the bench folder");
        openPath(other);
        QTimer::singleShot(3000, this, [this, before, fail] {
          if (m_doc->loading || QFileInfo(m_doc->browse ? m_doc->viewing : m_doc->path()) == QFileInfo(before)) return fail("another file did not open");
          trace::log("bench: viewer card, view changes not unsaved, edits refused, editable in place, saved, another file PASS");
          QCoreApplication::exit(0);
        });
      });
    });
  });
  return true;
}
