#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "SketchGeometryCache.hpp"
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QPaintEvent>
#include <algorithm>

// OPAD_BENCH_LARGE=<metrics.json> on a big drawing (case sketch-large in tools/bench_cases/sketch.py): its layers to a
// sketch on a worker, then the sketch opened and driven (SketchEditor::benchLarge).
OPAD_BENCH(OPAD_BENCH_LARGE, largeSketch) {
  const QString output=value;
  auto doc=std::make_shared<opad::Document>(w.m_doc->doc);
  auto scene=std::make_shared<opad::Scene>(w.m_doc->scene);
  auto sk=std::make_shared<opad::design::Sketch>();auto metrics=std::make_shared<opad::json>();
  w.m_jobs->async(MainWindow::tr("Converting drawing layers"),[doc,scene,sk,metrics](Progress p){
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
  },[&w,output,sk,metrics](bool ok,const QString& error){
    if(!ok){trace::log("bench: large sketch FAIL: "+error);QCoreApplication::exit(2);return;}
    QElapsedTimer time;time.start();w.m_design->sketch()->begin({},"Drawing benchmark",{{"base","xy"}},{},sk->to_json());
    (*metrics)["open_ms"]=time.nsecsElapsed()/1e6;
    w.m_design->sketch()->benchLarge(output,*metrics);
  });return true;
}

void SketchEditor::benchLarge(const QString& output,opad::json metrics) {
  struct Run {int step=0;size_t builds=0,loads=0;opad::json metrics,camera,edited;std::vector<double> hover,pan,frames,snap,move,gridSnap,gridMove;opad::json original;QElapsedTimer elapsed,drag;int from=0,to=0,line=0;};
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
      // Ctrl+C on it (UI-129): every point's id looked the curves up by scanning (seconds in the handler); the clip is made
      // on a worker.
      QApplication::clipboard()->clear();time.restart();
      if(!copySelection(false))throw opad::Error("copying the box selection failed");
      run->metrics["copy_all_ms"]=time.nsecsElapsed()/1e6;
      if(run->metrics["copy_all_ms"].get<double>()>200)throw opad::Error("Ctrl+C on everything held the window "+run->metrics["copy_all_ms"].dump()+" ms");
    } else if(step==39) {  // UI-27: the line tool from a point, so every snap kind is looked for on each move
      const QMimeData* mime=QApplication::clipboard()->mimeData();
      if(!mime || !mime->hasFormat(kClipMime)){--run->step;return;}  // the copy's worker
      const auto clip=opad::json::parse(mime->data(kClipMime).toStdString());
      if(clip.at("sketch").at("entities").size()!=m_sk.entities.size())throw opad::Error("the clipboard did not get every curve selected");
      trace::log(QString("bench: large sketch: Ctrl+C on a box selection of %1 curves holds the window %2 ms, the clip made on a worker PASS").arg(m_sk.entities.size()).arg(run->metrics["copy_all_ms"].get<double>()));
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
    } else if(step<72) {  // the same with grid snapping on (F9): off the points, the grid's own path and the drawn cursor
      if(step==56){m_viewport->setGridSnap(true);run->loads=m_settingsReads;}
      const auto& p=m_sk.points[(size_t(step-56)*m_sk.points.size()/16+11)%m_sk.points.size()];
      const double g=m_viewport->gridStep(),u=p.x+g*.37,v=p.y+g*.41;
      time.restart();
      sketchMove(u,v,Qt::NoModifier,false);
      run->gridMove.push_back(time.nsecsElapsed()/1e6);
      time.restart();
      snap(u,v);
      run->gridSnap.push_back(time.nsecsElapsed()/1e6);
      if(!m_drawnCursor)throw opad::Error("with grid snapping the line tool drew no cursor");
      if(m_settingsReads!=run->loads)throw opad::Error("the snap settings were read again on a mouse move (grid snapping on)");
    } else if(step==72) {
      m_viewport->setGridSnap(false);
      setTool("select");
      trace::log(QString("bench: large sketch: line tool snaps beside %1 points without reading the settings again, with and without the grid PASS").arg(m_sk.points.size()));
    } else if(step==73) {  // UI-28: a point dragged onto the point beside it is held to it, and merged into it on a worker
      const opad::design::SkEntity& e=m_sk.entities.front();const opad::design::SkPoint q=*m_sk.point(e.p.back());double best=1e300;
      for(const auto& p:m_sk.points)if(std::find(e.p.begin(),e.p.end(),p.id)==e.p.end() && std::hypot(p.x-q.x,p.y-q.y)<best){best=std::hypot(p.x-q.x,p.y-q.y);run->to=p.id;}
      run->from=q.id;run->line=e.id;const opad::design::SkPoint t=*m_sk.point(run->to);run->drag.restart();
      sketchPress(q.x,q.y,Qt::NoModifier);sketchMove(t.x+tol()*.05,t.y,Qt::NoModifier,true);
      if(m_dropPoint!=run->to)throw opad::Error("a point dragged beside another was not held to it");
      sketchRelease(t.x+tol()*.05,t.y,Qt::NoModifier);
    } else if(step==74) {
      run->metrics["drop_merge_ms"]=run->drag.elapsed();
      if(m_sk.point(run->from) || !m_sk.entity(run->line) || m_sk.entity(run->line)->p.back()!=run->to)throw opad::Error("the point dropped on another was not merged into it");
      undo();
    } else if(step==75) {
      auto restored=geometry(),expected=run->original;restored.erase("id_watermark");expected.erase("id_watermark");
      if(restored!=expected)throw opad::Error("undoing the merge did not bring the sketch back");
      trace::log(QString("bench: large sketch: a point dropped on another over %1 points merges into it on a worker, one undo step PASS").arg(m_sk.points.size()));
    } else {
      if(m_fillTimer.isActive()||m_fillJob){--run->step;return;}
      timer->stop();
      auto report=[](std::vector<double> v){std::sort(v.begin(),v.end());double sum=0;for(double x:v)sum+=x;return opad::json{{"mean_ms",sum/v.size()},{"p95_ms",v[size_t((v.size()-1)*.95)]},{"max_ms",v.back()}};};
      run->metrics["hover"]=report(run->hover);run->metrics["pan_event"]=report(run->pan);run->metrics["line_snap"]=report(run->snap);run->metrics["line_move"]=report(run->move);
      run->metrics["line_snap_grid"]=report(run->gridSnap);run->metrics["line_move_grid"]=report(run->gridMove);
      // Snapping with the line tool over a big sketch costs what is near the pointer, not the sketch's size (UI-27: 180 ms
      // a move over 30,000 segments, the apparent intersections' curves looked their points up by scanning).
      if(run->metrics["line_snap"]["p95_ms"].get<double>()>8)throw opad::Error("snapping took "+run->metrics["line_snap"].dump()+" per mouse move");
      if(run->metrics["line_snap_grid"]["p95_ms"].get<double>()>8)throw opad::Error("snapping with the grid took "+run->metrics["line_snap_grid"].dump()+" per mouse move");
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
