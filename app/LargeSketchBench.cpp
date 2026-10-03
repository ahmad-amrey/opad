#include "MainWindow.hpp"
#include "DesignController.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "SketchGeometryCache.hpp"
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QApplication>
#include <QPaintEvent>
#include <algorithm>

bool MainWindow::benchLargeSketch() {
  const auto output=qEnvironmentVariable("OPAD_BENCH_LARGE");
  if(output.isEmpty())return false;
  auto doc=std::make_shared<opad::Document>(m_doc->doc);
  auto scene=std::make_shared<opad::Scene>(m_doc->scene);
  auto sk=std::make_shared<opad::design::Sketch>();auto metrics=std::make_shared<opad::json>();
  m_jobs->async(tr("Converting drawing layers"),[doc,scene,sk,metrics](Progress p){
    std::vector<opad::design::DrawingLayer> layers;
    for(const auto& id:scene->all_bodies())if(scene->node(id)->representation=="drawing2d")layers.push_back({id,false});
    QElapsedTimer time;time.start();*sk=opad::design::drawing_sketch(*doc,*scene,layers,{},.01,[p]{return p.cancelled();});
    (*metrics)["conversion_ms"]=time.nsecsElapsed()/1e6;(*metrics)["entities"]=sk->entities.size();(*metrics)["points"]=sk->points.size();
    trace::log("large benchmark: converted "+QString::fromStdString(metrics->dump()));
    // UI-29: O(n log n), was minutes for 100k (every id and point lookup scanned the sketch).
    const double perEntity=(*metrics)["conversion_ms"].get<double>()/std::max<size_t>(1,sk->entities.size());
    if(sk->entities.size()<10000 || perEntity>0.25)throw opad::Error(QString("drawing to sketch took %1 ms per curve over %2 curves").arg(perEntity).arg(sk->entities.size()).toStdString());
    trace::log(QString("bench: large sketch: %1 curves converted in %2 ms PASS").arg(sk->entities.size()).arg((*metrics)["conversion_ms"].get<double>()));
    time.restart();auto solved=opad::design::solve(*sk);(*metrics)["solve_ms"]=time.nsecsElapsed()/1e6;
    if(!solved.converged)throw opad::Error("large drawing solve failed");
  },[this,output,sk,metrics](bool ok,const QString& error){
    if(!ok){trace::log("bench: large sketch FAIL: "+error);QCoreApplication::exit(2);return;}
    QElapsedTimer time;time.start();m_design->sketch()->begin({},"Drawing benchmark",{{"base","xy"}},{},sk->to_json());
    (*metrics)["open_ms"]=time.nsecsElapsed()/1e6;
    m_design->sketch()->benchLarge(output,*metrics);
  });return true;
}

void SketchEditor::benchLarge(const QString& output,opad::json metrics) {
  struct Run {int step=0;size_t builds=0,loads=0;opad::json metrics,camera,edited;std::vector<double> hover,pan,frames,snap,move;opad::json original;QElapsedTimer elapsed,drag;};
  auto run=std::make_shared<Run>();run->metrics=std::move(metrics);run->original=geometry();
  run->elapsed.start();m_viewport->setNavPreset(Viewport::NavPreset::Fusion);
  auto* timer=new QTimer(this);timer->setInterval(30);
  connect(timer,&QTimer::timeout,this,[this,run,timer,output]{try{
    if(run->elapsed.elapsed()>120000)throw opad::Error("large sketch benchmark timed out");
    if(busy())return;
    const int step=run->step++;
    QElapsedTimer time;time.start();
    if(step==0){run->builds=m_geometry->builds;run->metrics["cache_ready_ms"]=run->elapsed.elapsed();}
    if(step<16) {
      const auto& p=m_sk.points[(size_t(step)*m_sk.points.size()/16)%m_sk.points.size()];
      sketchMove(p.x+tol()*.3,p.y+tol()*.2,Qt::NoModifier,false);
      if(m_hover.kind==Hit::None)throw opad::Error("spatial query missed a visible sketch point");
      run->hover.push_back(time.nsecsElapsed()/1e6);
    } else if(step<32) {
      const QPointF point(m_viewport->width()/2+(step-16)*3,m_viewport->height()/2);
      const QPointF global=m_viewport->mapToGlobal(point.toPoint());
      // Hidden benchmark windows need explicit paints to flush OCCT's queued gestures.
      auto paint=[this]{QPaintEvent event(m_viewport->rect());QApplication::sendEvent(m_viewport,&event);};
      if(step==16){run->camera=m_viewport->cameraJson();QMouseEvent press(QEvent::MouseButtonPress,point,global,Qt::MiddleButton,Qt::MiddleButton,Qt::NoModifier);QApplication::sendEvent(m_viewport,&press);paint();}
      time.restart();
      QMouseEvent move(QEvent::MouseMove,point,global,Qt::NoButton,Qt::MiddleButton,Qt::NoModifier);QApplication::sendEvent(m_viewport,&move);
      run->pan.push_back(time.nsecsElapsed()/1e6);
      time.restart();paint();run->frames.push_back(time.nsecsElapsed()/1e6);
      if(step==31){QMouseEvent release(QEvent::MouseButtonRelease,point,global,Qt::MiddleButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(m_viewport,&release);}
    } else if(step==32) {
      if(m_fillTimer.isActive()||m_fillJob){--run->step;return;}
      if(geometry()!=run->original)throw opad::Error("navigation changed sketch geometry");
      if(m_viewport->cameraJson()["target"]==run->camera["target"])throw opad::Error("benchmark pan did not move camera");
      run->metrics["interaction_curve_rebuilds"]=m_geometry->builds-run->builds;
      if(m_geometry->builds!=run->builds)throw opad::Error("hover or navigation rebuilt curve geometry");
      run->metrics["profile_triangles"]=m_fill.size()/3;
    } else if(step==33) {
      const auto p=m_sk.points.front();const double delta=tol()*4;run->drag.start();
      sketchPress(p.x,p.y,Qt::NoModifier);sketchMove(p.x+delta,p.y+delta,Qt::NoModifier,true);sketchRelease(p.x+delta,p.y+delta,Qt::NoModifier);
    } else if(step==34) {
      run->metrics["drag_ready_ms"]=run->drag.elapsed();run->edited=geometry();
      if(run->edited==run->original)throw opad::Error("large sketch drag did not edit geometry");
      undo();
    } else if(step==35) {
      auto restored=geometry(),expected=run->original;restored.erase("id_watermark");expected.erase("id_watermark");
      if(restored!=expected)throw opad::Error("large sketch drag undo changed IDs or geometry");redo();
    } else if(step==36) {
      if(geometry()!=run->edited)throw opad::Error("large sketch drag redo changed geometry");undo();
    } else if(step==37) {
      auto camera=m_viewport->cameraJson();camera["scale"]=camera["scale"].get<double>()*.5;m_viewport->setCameraJson(camera);rebuild();
    } else if(step==38) {
      const auto p=m_sk.points.front();if(hitTest(p.x,p.y).kind==Hit::None)throw opad::Error("zoom invalidated sketch picking");
      double x0=1e300,y0=1e300,x1=-1e300,y1=-1e300;
      for(const auto& pt:m_sk.points){x0=std::min(x0,pt.x);y0=std::min(y0,pt.y);x1=std::max(x1,pt.x);y1=std::max(y1,pt.y);}
      const double margin=tol()*20;m_sel.clear();
      sketchPress(x0-margin,y0-margin,Qt::NoModifier);sketchMove(x1+margin,y1+margin,Qt::NoModifier,true);sketchRelease(x1+margin,y1+margin,Qt::NoModifier);
      run->metrics["select_all_box_ms"]=time.nsecsElapsed()/1e6;
      if(m_sel.size()<m_sk.entities.size())throw opad::Error("window selection missed sketch entities");
      // The prompt and the panel count what the box selected (UI-25): by index, not a scan per item (it took 5.7 s).
      if(run->metrics["select_all_box_ms"].get<double>()>2000)throw opad::Error("selecting everything took "+run->metrics["select_all_box_ms"].dump()+" ms");
    } else if(step==39) {  // UI-27: the line tool from a point, so every snap kind is looked for on each move
      m_sel.clear();setTool("line");
      const auto& p=m_sk.points[m_sk.points.size()/2];sketchPress(p.x,p.y,Qt::NoModifier);
      run->loads=m_settingsReads;
    } else if(step<56) {
      if(m_chain.empty())throw opad::Error("the line tool did not start on the point clicked");
      const auto& p=m_sk.points[(size_t(step-40)*m_sk.points.size()/16+7)%m_sk.points.size()];
      time.restart();
      sketchMove(p.x+tol()*.3,p.y+tol()*.2,Qt::NoModifier,false);
      run->move.push_back(time.nsecsElapsed()/1e6);
      time.restart();
      snap(p.x+tol()*.3,p.y+tol()*.2);  // the snapping alone (the move also redraws and places the value boxes)
      run->snap.push_back(time.nsecsElapsed()/1e6);
      if(m_cursor.kind!=Snap::Kind::Point || m_cursor.point!=p.id)throw opad::Error("the pointer beside a point did not snap to it");
      if(m_settingsReads!=run->loads)throw opad::Error("the snap settings were read again on a mouse move");
    } else if(step==56) {
      setTool("select");
      trace::log(QString("bench: large sketch: line tool snaps beside %1 points without reading the settings again PASS").arg(m_sk.points.size()));
    } else {
      if(m_fillTimer.isActive()||m_fillJob){--run->step;return;}
      timer->stop();
      auto report=[](std::vector<double> v){std::sort(v.begin(),v.end());double sum=0;for(double x:v)sum+=x;return opad::json{{"mean_ms",sum/v.size()},{"p95_ms",v[size_t((v.size()-1)*.95)]},{"max_ms",v.back()}};};
      run->metrics["hover"]=report(run->hover);run->metrics["pan_event"]=report(run->pan);run->metrics["line_snap"]=report(run->snap);run->metrics["line_move"]=report(run->move);
      // Snapping with the line tool over a big sketch costs what is near the pointer, not the sketch's size (UI-27: 180 ms
      // a move over 30,000 segments, the apparent intersections' curves looked their points up by scanning).
      if(run->metrics["line_snap"]["p95_ms"].get<double>()>8)throw opad::Error("snapping took "+run->metrics["line_snap"].dump()+" per mouse move");
      run->metrics["pan_frame"]=report(run->frames);
      auto restored=geometry(),expected=run->original;restored.erase("id_watermark");expected.erase("id_watermark");
      if(restored!=expected)throw opad::Error("selection changed sketch geometry");
      m_viewport->grabImage().save(output+".png");
      opad::write_text_file(output.toStdString(),run->metrics.dump(2));
      trace::log("bench: large sketch: hover, pan, drag, undo and box selection PASS: "+QString::fromStdString(run->metrics.dump()));
      end();QCoreApplication::exit(0);
    }
  }catch(const std::exception& e){timer->stop();trace::log(QString("bench: large sketch FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});
  timer->start();
}
