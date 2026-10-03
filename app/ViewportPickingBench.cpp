#include <QSignalBlocker>
#include <QSettings>
#include <QEventLoop>
// In-app regression, enabled explicitly with OPAD_BENCH_PICKING and --bench-select.
// Qt events stay within this widget; this never moves the OS cursor or drives the user's desktop.
#include "Viewport.hpp"
#include <AIS_RubberBand.hxx>
#include "Jobs.hpp"
#include "NavCube.hpp"
#include "CursorWrap.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBndLib.hxx>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QWheelEvent>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <BRep_Tool.hxx>
#include <SelectMgr_SensitiveEntity.hxx>
#include <cmath>
#include <gp_Pln.hxx>
#include <Graphic3d_StructureManager.hxx>
#include <QAction>

bool Viewport::benchPicking() {
  auto require = [](bool ok, const char* message) { if (!ok) throw opad::Error(message); };
  try {
    if(qEnvironmentVariableIsSet("OPAD_BENCH_HOVER_FADE")) {
      QSignalBlocker blocked(this);
      myViewAnimation->Stop();ResetViewInput();myUI.Reset();myGL.Reset();
      m_ctx->ClearSelected(false);m_ctx->ClearDetected(false);m_ctx->EraseAll(false);m_ctx->Deactivate();
      setGrid(false);
      const auto box=BRepPrimAPI_MakeBox(gp_Pnt(-10,-10,0),20,20,10).Shape();
      BRepMesh_IncrementalMesh mesh(box,.1);Bnd_Box bounds;BRepBndLib::Add(box,bounds);
      const auto prs=BodyPrs::build(box,bounds);
      Handle(BodyShape) first=new BodyShape(box,prs),second=new BodyShape(box,prs);
      gp_Trsf placement;placement.SetTranslation(gp_Vec(-25,0,0));first->SetLocalTransformation(placement);
      placement.SetTranslation(gp_Vec(25,0,0));second->SetLocalTransformation(placement);
      m_nodeOf[first.get()]="hover-a";m_nodeOf[second.get()]="hover-b";
      for(const auto& body:{first,second}){body->SetColor(Quantity_NOC_GRAY50);m_ctx->Display(body,1,0,false);m_ctx->Load(body,-1);}
      m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
      m_view->SetProj(V3d_XposYnegZpos);m_view->FitAll(.2,false);m_view->ZFitAll();
      m_view->Redraw();
      auto frame=[&]{handleViewRedraw(m_ctx,m_view);};
      auto structures=[&]{Graphic3d_MapOfStructure displayed;m_viewer->StructureManager()->DisplayedStructures(displayed);return displayed.Extent();};
      const Graphic3d_Vec2i empty(-200,-200);
      auto move=[&](const Graphic3d_Vec2i& point){m_ctx->MoveTo(point.x(),point.y(),m_view,false);frame();};
      auto target=[&](const Handle(BodyShape)& body,int mode){
        for(const auto& entity:body->Selection(mode)->Entities()) {
          const auto p=entity->BaseSensitive()->CenterOfGeometry().Transformed(body->Transformation());
          const auto pixel=devicePos(widgetPoint({p.X(),p.Y(),p.Z()}));
          m_ctx->MoveTo(pixel.x(),pixel.y(),m_view,false);
          if(m_ctx->HasDetected() && m_ctx->DetectedInteractive()==body)return pixel;
        }
        throw opad::Error("hover fade fixture has no pickable target");
      };
      auto expire=[&]{
        QEventLoop loop;QTimer::singleShot(600,&loop,&QEventLoop::quit);loop.exec();frame();
        require(m_hoverAge.isValid() && m_hoverAge.elapsed()>=500,"hover timeout restarted without context change");
      };
      for(int mode=0;mode<4;++mode) {
        m_filter=SelFilter(mode);m_ctx->Deactivate();
        const int selectionMode=AIS_Shape::SelectionMode(mode==0?TopAbs_SHAPE:mode==1?TopAbs_FACE:mode==2?TopAbs_EDGE:TopAbs_VERTEX);
        for(const auto& body:{first,second})m_ctx->Activate(body,selectionMode);
        const auto a=target(first,selectionMode),b=target(second,selectionMode);
        setHoverFade(true,.1);move(empty);const auto plain=structures();
        move(a);require(structures()>plain,"fresh object has no hover presentation");
        const auto owner=m_ctx->DetectedOwner();expire();
        require(structures()==plain,"expired hover presentation remains displayed");
        require(m_ctx->HasDetected() && m_ctx->DetectedOwner()==owner,"fading removed the pickable owner");
        // Force OCCT to regenerate the same owner's highlight and exercise the
        // actual pre-render hook, without a timer turn available to hide a flash.
        for(int repeat=0;repeat<3;++repeat) {
          move(empty);move(a);
          require(structures()==plain,"returning to a faded object flashes a highlight in the rendered frame");
        }
        move(b);require(structures()>plain,"different object did not reset hover");
        move(a);require(structures()>plain && m_hoverAge.elapsed()<100,"return after another object did not reset hover");
        expire();
        // Clicking a panel also changes context; no mouse movement is needed.
        QWidget panel(window());
        QMouseEvent click(QEvent::MouseButtonPress,QPointF(1,1),QPointF(1,1),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QCoreApplication::sendEvent(&panel,&click);frame();
        require(structures()>plain && m_hoverAge.elapsed()<100,"click did not restore stationary hover");
        expire();
        const auto scale=m_view->Camera()->Scale();m_view->Camera()->SetScale(scale*.98);frame();
        require(structures()>plain && m_hoverAge.elapsed()<100,"camera change did not restore hover");
        m_view->Camera()->SetScale(scale);frame();
        expire();
        if(mode==0) {
          auto* action=window()->findChild<QAction*>("view.grid");require(action,"grid action missing");
          action->trigger();action->trigger();frame();
          require(structures()>plain && m_hoverAge.elapsed()<100,"window action did not restore hover");
          expire();
        }
        setPickAccumulate(!m_pickAccumulate,true);frame();
        require(structures()>plain && m_hoverAge.elapsed()<100,"tool picking change did not restore hover");
        expire();
        setHoverFade(false,.1);frame();
        require(structures()>plain,"disabling fading did not restore hover");
        setHoverFade(true,.1);frame();expire();
        m_ctx->SelectDetected(AIS_SelectionScheme_Replace);
        require(m_ctx->NbSelected()==1,"faded object is not selectable");
        frame();const auto selected=structures();
        move(empty);move(a);
        require(m_ctx->NbSelected()==1 && structures()==selected,"hover suppression altered selection highlighting");
        m_ctx->ClearSelected(false);m_ctx->ClearDetected(false);frame();
        trace::log(QStringLiteral("bench: hover fade mode %1: no flash / other object / click / camera / tool / disabled / selectable PASS").arg(mode));
      }
      return true;
    }
    CursorWarpGate gate; gate.begin({0,200},{1917,200});
    require(!gate.accept({0,201}) && !gate.accept({1,202}), "queued edge events should not reapply cursor warp");
    require(gate.accept({1915,201}) && gate.accept({1910,202}), "warp destination should resume continuous drag");
    trace::log(QStringLiteral("bench: picking synchronous regression batch begin"));
    m_view->Redraw();
    if(const QString shots=qEnvironmentVariable("OPAD_BENCH_HIGHLIGHTS");!shots.isEmpty()) {
      QSignalBlocker blocked(this);const auto id=benchHeaviest();auto ais=m_items.at(id).ais;
      fitNodes({id});m_view->SetProj(V3d_XposYnegZpos);m_view->Redraw();
      for(auto& [node,item]:m_items) m_ctx->Deactivate(item.ais);
      for(int mode=0;mode<4;++mode) {
        m_ctx->ClearSelected(false);applySelectionLayers();refreshSubHighlight();m_filter=SelFilter(mode);activateSelection(ais);m_view->Redraw();
        const auto type=mode==0?TopAbs_SHAPE:mode==1?TopAbs_FACE:mode==2?TopAbs_EDGE:TopAbs_VERTEX;
        const auto selection=ais->Selection(AIS_Shape::SelectionMode(type));bool hit=false;Graphic3d_Vec2i pixel;
        for(const auto& entity:selection->Entities()) {
          auto point=entity->BaseSensitive()->CenterOfGeometry().Transformed(ais->Transformation());
          pixel=devicePos(widgetPoint({point.X(),point.Y(),point.Z()}));m_ctx->MoveTo(pixel.x(),pixel.y(),m_view,false);
          if(m_ctx->HasDetected() && m_ctx->DetectedInteractive()==ais) {hit=true;break;}
        }
        require(hit,"highlight screenshot could not find target");m_ctx->SelectDetected(AIS_SelectionScheme_Replace);OnSelectionChanged(m_ctx,m_view);m_ctx->ClearDetected(false);
        if(mode==0) require(m_bodyGlows.count(ais.get()),"selected body has no glow overlay");
        if(mode==1) require(!m_subHl.IsNull() && !m_subHl->m_triangles.empty() && !m_subHl->m_segments.empty(),"selected face is missing fill or glow border");
        require(grabImage().save(shots+QString::number(mode)+".selected.png"),"selection image failed");
        m_ctx->MoveTo(pixel.x(),pixel.y(),m_view,false);m_view->RedrawImmediate();
        require(grabImage().save(shots+QString::number(mode)+".hover.png"),"hover image failed");
      }
      m_ctx->ClearSelected(false);applySelectionLayers();refreshSubHighlight();
      require(m_bodyGlows.empty() && m_subHl.IsNull(),"selection glow survived clearing selection");
      trace::log(QStringLiteral("bench: body / face / edge / vertex white glow PASS"));return true;
    }
    // Two overlapping instances of one mesh: nearest triangle wins, with instance transforms.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(-10, -10, 0), 20, 20, 10).Shape();
    BRepMesh_IncrementalMesh mesh(box, 0.1);
    Bnd_Box bounds;
    BRepBndLib::Add(box, bounds);
    const auto prs = BodyPrs::build(box, bounds);
    Handle(SelectMgr_ViewerSelector) selector = new SelectMgr_ViewerSelector();
    selector->SetPickClosest(true);
    Handle(SelectMgr_SelectionManager) manager = new SelectMgr_SelectionManager(selector);
    Handle(NavigationShape) nearShape = new NavigationShape(prs->navigation), farShape = new NavigationShape(prs->navigation);
    gp_Trsf placement;
    placement.SetTranslation(gp_Vec(0, 0, 20));
    nearShape->SetLocalTransformation(placement);
    manager->Load(farShape); manager->Activate(farShape);
    manager->Load(nearShape); manager->Activate(nearShape);
    selector->Pick(gp_Ax1(gp_Pnt(0, 0, 100), gp_Dir(0, 0, -1)), m_view);
    require(selector->NbPicked() == 2, "navigation did not hit both box instances");
    require(std::abs(selector->PickedPoint(1).Z() - 30) < 1e-6, "navigation did not choose the nearest surface");
    Handle(Graphic3d_ClipPlane) clip = new Graphic3d_ClipPlane(gp_Pln(gp_Pnt(0, 0, 15), gp_Dir(0, 0, -1)));
    m_view->AddClipPlane(clip);
    selector->Pick(gp_Ax1(gp_Pnt(0, 0, 100), gp_Dir(0, 0, -1)), m_view);
    const bool clipped = selector->NbPicked() == 1 && std::abs(selector->PickedPoint(1).Z() - 10) < 1e-6;
    m_view->RemoveClipPlane(clip);
    require(clipped, "navigation picked clipped geometry");
    manager->Deactivate(nearShape);
    selector->Pick(gp_Ax1(gp_Pnt(0, 0, 100), gp_Dir(0, 0, -1)), m_view);
    require(selector->NbPicked() == 1 && std::abs(selector->PickedPoint(1).Z() - 10) < 1e-6, "navigation ignored deactivation");
    trace::log(QStringLiteral("bench: picking nearest surface / transformed instances PASS"));

    // A gap at the viewport center: choose the nearest projected part and its front surface, independent of selection
    // and hidden bodies. Over empty space the orbit follows the pointer (TODO 10 A7); the view cube keeps the centre.
    {
      const Handle(Graphic3d_Camera) camera = new Graphic3d_Camera(*m_view->Camera());
      auto items = std::move(m_items);
      auto nodes = std::move(m_navNodes);
      const auto oldSelector = m_navSelector;
      const auto oldManager = m_navSelection;
      m_items.clear(); m_navNodes.clear();
      m_navSelector = selector; m_navSelection = manager;
      selector->SetPixelTolerance(1);
      selector->SetDepthTolerance(SelectMgr_TypeOfDepthTolerance_Uniform, 0.0);
      placement.SetTranslation(gp_Vec(30, 0, 20)); nearShape->SetLocalTransformation(placement);
      placement.SetTranslation(gp_Vec(30, 0, 0)); farShape->SetLocalTransformation(placement);
      manager->Update(nearShape, true); manager->Update(farShape, true);
      manager->Activate(nearShape);
      Handle(AIS_Shape) nearAis = new AIS_Shape(box), farAis = new AIS_Shape(box);
      nearAis->SetLocalTransformation(nearShape->Transformation());
      farAis->SetLocalTransformation(farShape->Transformation());
      m_ctx->Display(nearAis, false); m_ctx->Display(farAis, false);
      m_items["near"].ais = nearAis; m_items["far"].ais = farAis;
      m_navNodes[nearShape.get()] = "near"; m_navNodes[farShape.get()] = "far";
      m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
      m_view->Camera()->SetEyeAndCenter(gp_Pnt(0, 0, 100), gp_Pnt(0, 0, 0));
      m_view->Camera()->SetUp(gp::DY()); m_view->Camera()->SetScale(100);
      m_view->Redraw();
      {
        QSignalBlocker blockedSignals(this);
        m_nodeOf[nearAis.get()]="near";m_nodeOf[farAis.get()]="far";
        m_ctx->Load(nearAis,-1);m_ctx->Load(farAis,-1);m_ctx->Activate(nearAis,0);m_ctx->Activate(farAis,0);
        const bool oldThrough=m_selectThrough;
        auto rectangle=[&](double x0,double x1,bool through,AIS_SelectionScheme scheme=AIS_SelectionScheme_Replace) {
          QElapsedTimer duration;duration.start();
          qint64 first=-1;
          m_selectThrough=through;
          UpdateRubberBand(devicePos(widgetPoint({x0,11,0})),devicePos(widgetPoint({x1,-11,0})));
          myGL.Selection=myUI.Selection;myGL.Selection.Scheme=scheme;myGL.Selection.ToApplyTool=true;
          handleSelectionPoly(m_ctx,m_view);
          QElapsedTimer wait;wait.start();while(m_boxJob && wait.elapsed()<15000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents,10);
            if(first<0 && m_ctx->NbSelected()) first=duration.elapsed();
          }
          if(!through && scheme==AIS_SelectionScheme_Replace) {
            trace::log(QString("bench: box first feedback=%1 ms, completion=%2 ms").arg(first).arg(duration.elapsed()));
            require(first>=0 && first<200,"box feedback exceeded 200 ms");
          }
          require(!m_boxJob,"visible box selection timed out");
        };
        rectangle(30,41,true);require(!m_ctx->IsSelected(nearAis),"enclosing box selected a partially covered body");
        rectangle(41,30,true);require(m_ctx->IsSelected(nearAis) && m_ctx->IsSelected(farAis),"crossing box missed overlapping bodies");
        m_ctx->ClearSelected(false);applySelectionLayers();
        rectangle(19,41,false);require(m_ctx->IsSelected(nearAis) && !m_ctx->IsSelected(farAis),"visible-only box selected an occluded body");
        placement.SetTranslation(gp_Vec(31,0,0));farAis->SetLocalTransformation(placement);farShape->SetLocalTransformation(placement);
        m_ctx->Redisplay(farAis,false);m_ctx->RecomputeSelectionOnly(farAis);manager->Update(farShape,true);m_view->Redraw();
        m_ctx->ClearSelected(false);applySelectionLayers();
        rectangle(19,42,false);require(m_ctx->IsSelected(nearAis) && m_ctx->IsSelected(farAis),"visible box missed a narrow exposed part");
        rectangle(19,42,false,AIS_SelectionScheme_XOR);require(!m_ctx->IsSelected(nearAis) && !m_ctx->IsSelected(farAis),"progressive XOR toggled an owner twice");
        placement.SetTranslation(gp_Vec(30,0,0));farAis->SetLocalTransformation(placement);farShape->SetLocalTransformation(placement);
        manager->Update(farShape,true);
        m_selectThrough=oldThrough;
        m_ctx->ClearSelected(false);m_ctx->Deactivate(nearAis);m_ctx->Deactivate(farAis);
        m_nodeOf.erase(nearAis.get());m_nodeOf.erase(farAis.get());
        myUI.Reset();myGL.Reset();myUI.Selection.Points.Clear();myGL.Selection.Points.Clear();
        m_ctx->Remove(myRubberBand,false);myRubberBand->ClearPoints();
        trace::log(QStringLiteral("bench: directional / occluded box selection PASS"));
      }
      const auto center = devicePos(QPointF(width()/2.0, height()/2.0));
      gp_Pnt picked;
      require(!navigationPoint(center, picked), "off-center test unexpectedly hits geometry at center");
      QElapsedTimer searchTimer; searchTimer.start();
      picked = orbitPoint(center);  // the pointer over the empty centre: the part nearest to it
      require(std::abs(picked.X()-20) < pixelSize()*3 && std::abs(picked.Y()) < pixelSize()*3
              && std::abs(picked.Z()-30) < 1e-6, "empty-space pivot missed the foreground part nearest the pointer");
      trace::log(QStringLiteral("bench: off-center orbit search %1 ms").arg(searchTimer.elapsed()));
      picked = orbitPoint(devicePos(widgetPoint({15, 25, 30})));  // above and left of the part: its nearest corner
      require(std::abs(picked.X()-20) < pixelSize()*3 && std::abs(picked.Y()-10) < pixelSize()*3
              && std::abs(picked.Z()-30) < 1e-6, "empty-space pivot did not follow the pointer");
      require(std::abs(centralOrbitPoint().Y()) < pixelSize()*3, "the view cube's pivot left the centre");
      m_ctx->Erase(nearAis, false);
      require(std::abs(centralOrbitPoint().Z()-10) < 1e-6, "central fallback picked a hidden part");
      m_ctx->Display(nearAis, false);
      m_view->AddClipPlane(clip);
      require(std::abs(centralOrbitPoint().Z()-10) < 1e-6, "central fallback picked clipped geometry");
      m_view->RemoveClipPlane(clip);
      Handle(Graphic3d_ClipPlane) allClipped = new Graphic3d_ClipPlane(gp_Pln(gp_Pnt(0,0,-5), gp_Dir(0,0,-1)));
      m_view->AddClipPlane(allClipped);
      require(centralOrbitPoint().Distance(m_view->Camera()->Center()) < 1e-6,
              "fully clipped scene did not retain camera focus");
      m_view->RemoveClipPlane(allClipped);
      for (auto projection : {Graphic3d_Camera::Projection_Orthographic, Graphic3d_Camera::Projection_Perspective}) {
        m_view->Camera()->SetProjectionType(projection);
        m_view->Redraw();
        picked = centralOrbitPoint();
        const gp_Pnt position = m_view->Camera()->ConvertWorld2View(picked);
        const gp_Dir startDirection = m_view->Camera()->Direction();
        focusCube();
        m_ctx->MoveTo(center.x(), center.y(), m_view, false);
        m_cube->StartAnimation(new AIS_ViewCubeOwner(m_cube.get(), V3d_Xpos));
        for (double time : {0.25, 0.5}) {
          myViewAnimation->Update(time);
          require(position.Distance(m_view->Camera()->ConvertWorld2View(picked)) < 1e-6,
                  "cube animation drifted away from its off-center orbit point");
        }
        require(!startDirection.IsEqual(m_view->Camera()->Direction(), 1e-6), "cube animation did not rotate");
        myViewAnimation->Stop();
        m_view->Camera()->SetEyeAndCenter(gp_Pnt(0, 0, 100), gp_Pnt(0, 0, 0));
        m_view->Camera()->SetUp(gp::DY());
      }
      m_ctx->Erase(nearAis, false); m_ctx->Erase(farAis, false);
      require(centralOrbitPoint().Distance(m_view->Camera()->Center()) < 1e-6,
              "empty scene did not retain camera focus");
      m_ctx->Remove(nearAis, false); m_ctx->Remove(farAis, false);
      m_items = std::move(items); m_navNodes = std::move(nodes);
      m_navSelector = oldSelector; m_navSelection = oldManager;
      m_view->SetCamera(camera); m_view->Redraw();
      trace::log(QStringLiteral("bench: central orbit gap / hidden / clipping / cube animation PASS"));
    }

    // Drawing interiors use the pointer's plane intersection, not the outline or scene center.
    {
      const Handle(Graphic3d_Camera) camera=new Graphic3d_Camera(*m_view->Camera());
      const std::string id="bench-drawing-orbit";
      const auto edge=BRepBuilderAPI_MakeEdge(gp_Pnt(2000,2000,0),gp_Pnt(2040,2030,0)).Shape();
      Bnd_Box bounds;BRepBndLib::Add(edge,bounds);auto drawing=BodyPrs::build(edge,bounds);
      Handle(AIS_Shape) ais=new BodyShape(edge,drawing);m_ctx->Display(ais,0,-1,false);
      m_items[id].ais=ais;m_items[id].key=id;m_prs[id]=drawing;
      opad::Node node;node.id=id;node.representation="drawing2d";m_doc->scene.nodes[id]=node;
      m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
      m_view->Camera()->SetEyeAndCenter(gp_Pnt(2020,2015,100),gp_Pnt(2020,2015,0));m_view->Camera()->SetUp(gp::DY());m_view->Camera()->SetScale(100);
      for(const gp_Pnt at:{gp_Pnt(2008,2022,0),gp_Pnt(2030,2006,0)}) {
        const QPointF pointer=widgetPoint({at.X(),at.Y(),at.Z()});bool found=false;
        const auto pivot=drawingOrbitPoint(&pointer,&found);
        require(found && pivot.Distance(at)<pixelSize()*2,"drawing orbit did not follow the pointer inside its bounds");
      }
      m_ctx->Remove(ais,false);m_items.erase(id);m_prs.erase(id);m_doc->scene.nodes.erase(id);m_view->SetCamera(camera);
      trace::log("bench: pointer-priority drawing orbit PASS");
    }

    // Pixel regression for coincident surfaces: all interior samples must use
    // one stable material, at both a normal and a grazing viewing angle.
    {
      const Handle(Graphic3d_Camera) camera=new Graphic3d_Camera(*m_view->Camera());
      const int quality=m_renderQuality;
      auto items=std::move(m_items); m_items.clear();
      std::vector<Handle(AIS_Shape)> visible;
      for(const auto& [id,item]:items) if(m_ctx->IsDisplayed(item.ais)) { visible.push_back(item.ais); m_ctx->Erase(item.ais,false); }
      Handle(AIS_Shape) red=new BodyShape(box,prs),green=new BodyShape(box,prs);
      Graphic3d_MaterialAspect matte(Graphic3d_NameOfMaterial_Plastified); matte.SetSpecularColor(Quantity_NOC_BLACK);
      red->SetMaterial(matte); green->SetMaterial(matte);
      red->SetColor(Quantity_Color(1,0,0,Quantity_TOC_RGB)); green->SetColor(Quantity_Color(0,1,0,Quantity_TOC_RGB));
      m_ctx->Display(red,1,-1,false); m_ctx->Display(green,1,-1,false);
      m_items["a"].ais=red; m_items["b"].ais=green; updateDepthBias(); setRenderQuality(0);
      m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
      for (int level : {0,1,2}) { setRenderQuality(level);
      trace::log(QString("bench: render mode %1 uses %2").arg(level).arg(m_view->RenderingParams().Method==Graphic3d_RM_RAYTRACING?"ray tracing":"rasterization"));
      for(const gp_Pnt eye:{gp_Pnt(0,0,100),gp_Pnt(0,-100,35)}) {
        m_view->Camera()->SetEyeAndCenter(eye,gp_Pnt(0,0,10)); m_view->Camera()->SetUp(gp::DY()); m_view->Camera()->SetScale(60);
        m_view->Redraw(); const QImage frame=grabImage();
        for(int y=-6;y<=6;y+=3) for(int x=-6;x<=6;x+=3) {
          const QPoint at=widgetPoint({double(x),double(y),10});
          const QColor pixel=frame.pixelColor(qRound(at.x()*double(frame.width())/width()),qRound(at.y()*double(frame.height())/height()));
          if (!(pixel.red()>80 && pixel.red()>pixel.green()*2)) { frame.save(QString("build/todo2-depth-%1.png").arg(level)); trace::log(QString("depth level %1 pixel %2,%3,%4").arg(level).arg(pixel.red()).arg(pixel.green()).arg(pixel.blue())); }
          require(pixel.red()>80 && pixel.red()>pixel.green()*2,"coincident faces have mixed or unstable depth ordering");
        }
      }
      }
      m_ctx->Remove(red,false); m_ctx->Remove(green,false); m_items=std::move(items);
      for(const auto& shape:visible) { m_ctx->Display(shape,false); activateSelection(shape); }
      setRenderQuality(quality); m_view->SetCamera(camera); m_view->Redraw();
      trace::log(QStringLiteral("bench: coincident surface pixel regression PASS"));
    }

    {
      const auto camera=new Graphic3d_Camera(*m_view->Camera());
      auto items=std::move(m_items); auto sketches=std::move(m_sketchWires); m_items.clear(); m_sketchWires.clear();
      const auto line=BRepBuilderAPI_MakeEdge(gp_Pnt(500,-10,0),gp_Pnt(500,10,0)).Shape(); Bnd_Box bounds; BRepBndLib::Add(line,bounds);
      auto drawing=BodyPrs::build(line,bounds); Handle(AIS_Shape) ais=new BodyShape(line,drawing); m_ctx->Display(ais,0,-1,false);
      m_prs["bench-drawing"]=drawing; m_items["bench-drawing"].ais=ais; m_items["bench-drawing"].key="bench-drawing";
      m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic); m_view->Camera()->SetEyeAndCenter(gp_Pnt(0,0,100),gp_Pnt(0,0,0)); m_view->Camera()->SetUp(gp::DY()); m_view->Redraw();
      require(centralOrbitPoint().Distance(gp_Pnt(500,0,0))<pixelSize()*2,"drawing-only orbit missed the drawing");
      const bool grid=m_grid; setGrid(true); const Bnd_Box drawn=benchGridBox();
      require(!drawn.IsVoid() && !drawn.IsOut(gp_Pnt(500,-10,0)) && !drawn.IsOut(gp_Pnt(500,10,0)),"grid did not cover the scene bounds");
      m_items.clear();m_sketchWires["bench-sketch"]={ais,drawing,"bench"};
      require(centralOrbitPoint().Distance(gp_Pnt(500,0,0))<pixelSize()*2,"sketch-only orbit missed the sketch");
      m_ctx->Remove(ais,false);m_prs.erase("bench-drawing");m_items=std::move(items);m_sketchWires=std::move(sketches);setGrid(grid);m_view->SetCamera(camera);m_view->Redraw();
      trace::log(QStringLiteral("bench: drawing/sketch orbit fallback and grid extent PASS"));
    }

    // Exercise Qt's trackpad event path, including the virtual drag that drives OCCT's existing gestures.
    const Handle(Graphic3d_Camera) beforeTrackpad = new Graphic3d_Camera(*m_view->Camera());
    const QPointF gesturePoint(width() * 0.5, height() * 0.5);
    QPointingDevice touchpad("bench trackpad", 1, QInputDevice::DeviceType::TouchPad,
                            QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 2, 0);
    auto scroll = [&](QPoint pixels, Qt::KeyboardModifiers modifiers, Qt::ScrollPhase phase) {
      QWheelEvent event(gesturePoint, mapToGlobal(gesturePoint), pixels, {}, Qt::NoButton,
                        modifiers, phase, true, Qt::MouseEventNotSynthesized, &touchpad);
      QCoreApplication::sendEvent(this, &event);
      paintEvent(nullptr);
    };
    const opad::Vec3 centerWorld{beforeTrackpad->Center().X(), beforeTrackpad->Center().Y(), beforeTrackpad->Center().Z()};
    const QPoint beforePan = widgetPoint(centerWorld);
    scroll({}, Qt::NoModifier, Qt::ScrollBegin);
    scroll({40, 20}, Qt::NoModifier, Qt::ScrollUpdate);
    scroll({}, Qt::NoModifier, Qt::ScrollEnd);
    const QPoint panDelta = widgetPoint(centerWorld) - beforePan;
    require(std::abs(panDelta.x() - 40) <= 3 && std::abs(panDelta.y() - 20) <= 3,
            "trackpad two-finger pan did not follow the scroll delta");
    m_view->SetCamera(new Graphic3d_Camera(*beforeTrackpad));
    m_view->Redraw();
    scroll({}, Qt::ShiftModifier, Qt::ScrollBegin);
    scroll({40, 20}, Qt::ShiftModifier, Qt::ScrollUpdate);
    scroll({}, Qt::ShiftModifier, Qt::ScrollEnd);
    require(!beforeTrackpad->Direction().IsEqual(m_view->Camera()->Direction(), 1e-6),
            "Shift plus trackpad scroll did not orbit");
    m_view->SetCamera(new Graphic3d_Camera(*beforeTrackpad));
    m_view->Redraw();
    QNativeGestureEvent pinch(Qt::ZoomNativeGesture, &touchpad, 2, gesturePoint,
                              mapTo(window(), gesturePoint), mapToGlobal(gesturePoint), 0.2, {});
    QCoreApplication::sendEvent(this, &pinch);
    paintEvent(nullptr);
    require(std::abs(m_view->Camera()->Scale() / beforeTrackpad->Scale() - 1.0 / 1.2) < 0.03,
            "trackpad pinch did not zoom");
    const double afterPinchIn = m_view->Camera()->Scale();
    QNativeGestureEvent pinchOut(Qt::ZoomNativeGesture, &touchpad, 2, gesturePoint,
                                 mapTo(window(), gesturePoint), mapToGlobal(gesturePoint), -0.2, {});
    QCoreApplication::sendEvent(this, &pinchOut);
    paintEvent(nullptr);
    require(m_view->Camera()->Scale() > afterPinchIn, "trackpad pinch-in did not zoom out");
    QNativeGestureEvent pinchEnd(Qt::EndNativeGesture, &touchpad, 0, gesturePoint,
                                 mapTo(window(), gesturePoint), mapToGlobal(gesturePoint), 0.0, {});
    QCoreApplication::sendEvent(this, &pinchEnd);
    m_view->SetCamera(new Graphic3d_Camera(*beforeTrackpad));
    m_view->Redraw();
    scroll({0, 15}, Qt::ControlModifier, Qt::ScrollUpdate);  // Windows touchpads can report pinch as Ctrl+wheel
    require(m_view->Camera()->Scale() < beforeTrackpad->Scale(), "Ctrl plus trackpad scroll did not zoom");
    m_view->SetCamera(new Graphic3d_Camera(*beforeTrackpad));
    m_view->Redraw();
    QPinchGesture fallbackPinch;
    fallbackPinch.setScaleFactor(1.2);
    fallbackPinch.setChangeFlags(QPinchGesture::ScaleFactorChanged);
    fallbackPinch.setHotSpot(mapToGlobal(gesturePoint));
    QGestureEvent fallbackEvent({&fallbackPinch});
    event(&fallbackEvent);  // macOS does not grab this fallback gesture; exercise the handler directly
    paintEvent(nullptr);
    require(std::abs(m_view->Camera()->Scale() / beforeTrackpad->Scale() - 1.0 / 1.2) < 0.03,
            "Qt pinch gesture fallback did not zoom");
    m_view->SetCamera(new Graphic3d_Camera(*beforeTrackpad));
    m_view->Redraw();
    QPointingDevice mouse("bench mouse", 2, QInputDevice::DeviceType::Mouse,
                          QPointingDevice::PointerType::Generic, QInputDevice::Capability::Position, 1, 3);
    QWheelEvent mouseWheel(gesturePoint, mapToGlobal(gesturePoint), {}, {0, 120}, Qt::NoButton,
                           Qt::NoModifier, Qt::NoScrollPhase, false, Qt::MouseEventNotSynthesized, &mouse);
    QCoreApplication::sendEvent(this, &mouseWheel);
    paintEvent(nullptr);
    require(m_view->Camera()->Scale() < beforeTrackpad->Scale(), "mouse wheel no longer zooms");
    m_view->SetCamera(new Graphic3d_Camera(*beforeTrackpad));
    m_view->Redraw();
    require(selection().empty(), "trackpad navigation selected an object");
    trace::log(QStringLiteral("bench: picking trackpad pan / Shift orbit / pinch / mouse wheel PASS"));

    // Exercise the real scene selector before fitting one part for center discovery.
    QElapsedTimer timer;
    timer.start();
    int hits = 0;
    gp_Pnt orbitTarget;
    QPoint orbitCursor;
    const Graphic3d_Vec2i empty = devicePos(QPointF(-200, -200));
    double nearestHit = 1e300;  // screen distance from the empty-space pointer to the nearest sampled surface
    for (int y = 1; y < 10; ++y) for (int x = 1; x < 10; ++x) {
      gp_Pnt p;
      const Graphic3d_Vec2i ray = devicePos(QPointF(width() * x / 10.0, height() * y / 10.0));
      if (navigationPoint(ray, p)) {
        ++hits;
        orbitTarget = p;
        orbitCursor = QPoint(width() * x / 10, height() * y / 10);
        nearestHit = std::min(nearestHit, std::hypot(double(ray.x() - empty.x()), double(ray.y() - empty.y())));
      }
    }
    auto nearPointer = [&](const gp_Pnt& pivot) {  // TODO 10 A7: empty space pivots on the geometry nearest the pointer
      Standard_Integer px = 0, py = 0;
      m_view->Convert(pivot.X(), pivot.Y(), pivot.Z(), px, py);
      return std::hypot(double(px - empty.x()), double(py - empty.y())) <= nearestHit + 3;
    };
    require(hits > 0, "navigation found no surfaces with the vertex filter active");
    trace::log(QStringLiteral("bench: picking navigation 81 rays: %1 ms, %2 hits, %3 bodies").arg(timer.elapsed()).arg(hits).arg(m_items.size()));
    const gp_Pnt fallback = orbitPoint(empty);
    require(nearPointer(fallback), "empty-space pivot is not the geometry nearest the pointer");
    gp_Pnt directTarget;
    require(navigationPoint(devicePos(orbitCursor), directTarget)
            && orbitPoint(devicePos(orbitCursor)).Distance(directTarget) < 1e-7, "surface under cursor lost orbit priority");
    const double scale = m_view->Camera()->Scale();
    const gp_Dir direction = m_view->Camera()->Direction();
    focusCube();
    require(std::abs(m_view->Camera()->Scale() - scale) < 1e-7 && direction.IsEqual(m_view->Camera()->Direction(), 1e-7), "cube focus moved or zoomed the image");
    trace::log(QStringLiteral("bench: picking empty-space and cube focus PASS"));

    // A direct press on the cube must use that press location even when the last hover was over a body.
#if defined(__APPLE__)
    require(std::abs(viewScale().x() - devicePixelRatioF()) < 0.01,
            "Cocoa viewport size does not match its Retina backing pixels");
#endif
    m_ctx->MoveTo(devicePos(orbitCursor).x(), devicePos(orbitCursor).y(), m_view, Standard_False);
    const QPoint cubePoint(width() - 100, 68);
    auto cubeMouse = [this](QEvent::Type type, QPoint p, Qt::MouseButton button, Qt::MouseButtons buttons) {
      QMouseEvent event(type, QPointF(p), mapToGlobal(QPointF(p)), button, buttons, Qt::NoModifier);
      QCoreApplication::sendEvent(this, &event);
    };
    const Handle(Graphic3d_Camera) beforeCubeDrag = new Graphic3d_Camera(*m_view->Camera());
    cubeMouse(QEvent::MouseButtonPress, cubePoint, Qt::LeftButton, Qt::LeftButton);
    require(m_cubeGesture, "direct cube press did not start cube orbit");
    cubeMouse(QEvent::MouseMove, cubePoint + QPoint(60, 30), Qt::NoButton, Qt::LeftButton);
    paintEvent(nullptr);
    cubeMouse(QEvent::MouseButtonRelease, cubePoint + QPoint(60, 30), Qt::LeftButton, Qt::NoButton);
    paintEvent(nullptr);
    require(!m_cubeGesture, "cube drag did not finish");
    require(!beforeCubeDrag->Direction().IsEqual(m_view->Camera()->Direction(), 1e-6),
            "cube drag did not orbit");
    m_view->SetCamera(new Graphic3d_Camera(*beforeCubeDrag));
    m_view->Redraw();
    trace::log(QStringLiteral("bench: picking direct cube press and drag PASS"));

    Handle(Graphic3d_Camera) savedCamera = new Graphic3d_Camera(*m_view->Camera());
    m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Perspective);
    const gp_Pnt perspectiveFallback = orbitPoint(empty);
    require(nearPointer(perspectiveFallback), "perspective empty-space pivot is not the geometry nearest the pointer");
    const gp_Pnt eyeBefore = m_view->Camera()->Eye();
    const gp_Dir dirBefore = m_view->Camera()->Direction();
    const double fovBefore = m_view->Camera()->FOVy();
    focusCube();
    require(eyeBefore.Distance(m_view->Camera()->Eye()) < 1e-7 && dirBefore.IsEqual(m_view->Camera()->Direction(), 1e-7)
        && std::abs(fovBefore - m_view->Camera()->FOVy()) < 1e-7, "perspective cube focus moved the image");
    m_view->SetCamera(new Graphic3d_Camera(*savedCamera));
    const NavPreset oldPreset = m_preset;
    setNavPreset(NavPreset::SolidWorks);
    auto orbitEvent = [this](QEvent::Type type, QPoint p, Qt::MouseButton button, Qt::MouseButtons buttons) {
      QMouseEvent event(type, QPointF(p), mapToGlobal(QPointF(p)), button, buttons, Qt::NoModifier);
      QCoreApplication::sendEvent(this, &event);
      paintEvent(nullptr);
    };
    orbitEvent(QEvent::MouseMove, orbitCursor, Qt::NoButton, Qt::NoButton);
    orbitEvent(QEvent::MouseButtonPress, orbitCursor, Qt::MiddleButton, Qt::MiddleButton);
    orbitEvent(QEvent::MouseMove, orbitCursor + QPoint(50, 30), Qt::NoButton, Qt::MiddleButton);
    orbitEvent(QEvent::MouseMove, orbitCursor + QPoint(100, 60), Qt::NoButton, Qt::MiddleButton);
    orbitEvent(QEvent::MouseButtonRelease, orbitCursor + QPoint(100, 60), Qt::MiddleButton, Qt::NoButton);
    const QPoint afterOrbit = widgetPoint({orbitTarget.X(), orbitTarget.Y(), orbitTarget.Z()});
    require((afterOrbit - orbitCursor).manhattanLength() <= 3, "orbit did not retain the surface beneath its starting cursor");
    require(!direction.IsEqual(m_view->Camera()->Direction(), 1e-6), "orbit gesture did not rotate");
    m_view->SetCamera(new Graphic3d_Camera(*savedCamera));
    setNavPreset(oldPreset);
    trace::log(QStringLiteral("bench: picking perspective / clipping / Qt orbit pivot PASS"));
// Drafting locks the camera and produces exact world-space point references on extension guides.
    const Handle(Graphic3d_Camera) before2d = new Graphic3d_Camera(*m_view->Camera());
    setTwoDimensional(true);
    const auto planar = m_view->Camera()->Direction();
    require(std::max({std::abs(planar.X()),std::abs(planar.Y()),std::abs(planar.Z())}) > 1.0-1e-10,
            "2D entry did not snap to a principal plane");
    require(!m_ctx->IsDisplayed(m_cube) && isOrthographic(), "2D cube/projection state is wrong");
    m_view->SetProj(V3d_Zpos);
    m_view->Redraw();
    if (qEnvironmentVariableIsSet("OPAD_BENCH_ORBIT_PERF")) {
      const auto camera = new Graphic3d_Camera(*m_view->Camera());
      const gp_Vec side = gp_Vec(camera->Direction()).Crossed(gp_Vec(camera->Up()));
      for (double offset : {0.0, 0.4, 0.8, 1.2}) {
        m_view->SetCamera(new Graphic3d_Camera(*camera));
        const gp_Vec shift = side * (camera->Scale() * offset);
        m_view->Camera()->SetEyeAndCenter(camera->Eye().Translated(shift), camera->Center().Translated(shift));
        m_view->Redraw();
        QElapsedTimer timer; timer.start();
        const gp_Pnt pivot = centralOrbitPoint();
        trace::log(QStringLiteral("bench: orbit performance offset=%1 bodies=%2 time=%3 ms pivot=%4,%5,%6")
                   .arg(offset).arg(m_items.size()).arg(timer.nsecsElapsed()/1e6,0,'f',3).arg(pivot.X()).arg(pivot.Y()).arg(pivot.Z()));
      }
      m_view->SetCamera(camera);
      return true;
    }
    m_ctx->ClearDetected(false);
    // Above the model (the view looks down), so nothing hides the guides (UI-31).
    const double lift=fitBounds().CornerMax().Z()+1, reach=pixelSize()*40;
    setPickAccumulate(false);  // the Distance tool runs: off, no tool takes points
    m_trackingAnchors={{gp_Pnt(0,0,lift),gp_Vec(1,0,0),true}};
    m_trackingCursor=widgetPoint({reach,0,lift}); m_trackingDirty=true;
    updateTracking();
    require(m_trackingMarker.empty() && m_trackingAnchors.empty(), "2D mode tracked with no tool taking points");
    setPickAccumulate(true,true);
    m_trackingAnchors={{gp_Pnt(0,0,lift),gp_Vec(1,0,0),true}};
    m_trackingDirty=true; updateTracking();
    require(!m_trackingMarker.empty(), "extension tracking did not create a point");
    auto tracked=m_centers.at(m_trackingMarker).ref;
    require(tracked.kind==opad::Ref::Kind::Point && std::abs(tracked.point[1])<1e-8, "tracking point left its extension line");
    QMouseEvent extensionPress(QEvent::MouseButtonPress,m_trackingCursor,mapToGlobal(m_trackingCursor),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent extensionRelease(QEvent::MouseButtonRelease,m_trackingCursor,mapToGlobal(m_trackingCursor),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(this,&extensionPress); QCoreApplication::sendEvent(this,&extensionRelease);
    require(!selection().empty() && selection().back().kind==opad::Ref::Kind::Point,"extension was not selectable in measurement mode");
    m_ctx->ClearSelected(false);
    m_trackingAnchors={{gp_Pnt(0,reach,lift),gp_Vec(1,0,0),true},{gp_Pnt(reach,0,lift),gp_Vec(0,1,0),true}};
    m_trackingCursor=widgetPoint({reach,reach,lift}); m_trackingDirty=true; updateTracking();
    require(!m_trackingCandidates.empty() && m_trackingCandidates.front().intersection,
            "two extensions did not produce a composite intersection");
    require(m_trackingCandidates.front().point.Distance(gp_Pnt(reach,reach,lift))<1e-7,"incorrect intersection");
    opad::Ref candidateCenter; candidateCenter.kind=opad::Ref::Kind::Point; candidateCenter.point={0,0,0};
    centerMarker(candidateCenter,gp_Pnt(0,0,0)); m_activeCenter=candidateCenter.str(); m_inferenceChoice=0;
    QKeyEvent down(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier),up(QEvent::KeyRelease,Qt::Key_Shift,Qt::NoModifier);
    inferenceKey(&down); require(!m_centerLocked && m_trackingLocked,"Shift must only lock tracking/extension");
    inferenceKey(&up);
    inferenceKey(&down); require(m_trackingLocked && !m_centerLocked,"Shift did not lock selected inference");
    inferenceKey(&up); m_activeCenter.clear(); clearTracking();
    m_trackingAnchors={{gp_Pnt(0,reach,lift),gp_Vec(1,0,0),true},{gp_Pnt(reach,0,lift+reach),gp_Vec(0,1,0),true}};
    m_trackingDirty=true; updateTracking();
    for(const auto& c:m_trackingCandidates) require(!c.intersection,"skew 3D lines produced a false intersection");
    clearTracking(); setPickAccumulate(false);
    const gp_Dir flatDirection=m_view->Camera()->Direction();
    trackpadScroll(QPointF(width()/2,height()/2),QPointF(20,10),true);
    FlushViewEvents(m_ctx,m_view,true);finishTrackpadScroll();
    require(flatDirection.IsEqual(m_view->Camera()->Direction(),1e-8), "2D mode allowed trackpad orbit");
    clearCenters(); setTwoDimensional(false);
    require(before2d->Direction().IsEqual(m_view->Camera()->Direction(),1e-10)
            && before2d->Eye().Distance(m_view->Camera()->Eye())<1e-8
            && before2d->ProjectionType()==m_view->Camera()->ProjectionType(), "3D camera was not restored");
    cubeMouse(QEvent::MouseButtonPress, cubePoint, Qt::LeftButton, Qt::LeftButton);
    require(m_cubeGesture, "cube inactive after leaving 2D");
    cubeMouse(QEvent::MouseMove, cubePoint + QPoint(40,20), Qt::NoButton, Qt::LeftButton);
    paintEvent(nullptr);
    cubeMouse(QEvent::MouseButtonRelease, cubePoint + QPoint(40,20), Qt::LeftButton, Qt::NoButton);
    paintEvent(nullptr);
    require(!before2d->Direction().IsEqual(m_view->Camera()->Direction(),1e-6), "3D orbit not restored");
    standardView("top");
    const auto topDirection=m_view->Camera()->Direction();
    trackpadScroll(QPointF(width()/2,height()/2),QPointF(30,40),true);
    FlushViewEvents(m_ctx,m_view,true);finishTrackpadScroll();
    require(!topDirection.IsEqual(m_view->Camera()->Direction(),1e-6),"top view locks 3D orbit");
    // Cursor arithmetic is tested without moving the user's OS pointer.
    const QRect screen(-1920,0,1920,1080);
    const QPoint edge(-1,400), wrapped=wrappedCursor(edge,screen);
    require(wrapped==QPoint(-1918,400), "display edge wrap target is wrong");
    const QPoint offset=edge-wrapped;
    require(wrapped+offset==edge && wrapped+QPoint(5,0)+offset==edge+QPoint(5,0), "warp introduced motion jump");
    m_view->SetCamera(new Graphic3d_Camera(*savedCamera));
    trace::log(QStringLiteral("bench: 2D orbit lock / extension point PASS"));

    const int previousQuality = m_renderQuality;
    for (int level = 0; level < 3; ++level) {
      setRenderQuality(level);
      m_view->Redraw();
      const QString shots = QString::fromLocal8Bit(qgetenv("OPAD_BENCH_QUALITY"));
      if (!shots.isEmpty()) require(grabImage().save(shots + QString::number(level) + ".png"), "render preset screenshot failed");
    }
    setRenderQuality(previousQuality);
    trace::log(QStringLiteral("bench: three rendering presets PASS"));

    {
      const std::vector<QRect> screens={QRect(-1920,0,1920,1080),QRect(0,0,2560,1440)};
      require(wrappedDesktopCursor(QPoint(-1,500),screens)==QPoint(-1,500),"cursor warped at an internal monitor seam");
      require(wrappedDesktopCursor(QPoint(2559,500),screens)==QPoint(-1918,500),"cursor did not wrap across full desktop");
      require(wrappedDesktopCursor(QPoint(2559,1300),screens)==QPoint(2,1300),"offset monitor wrap landed outside display");
      auto beforeHome=new Graphic3d_Camera(*m_view->Camera());
      home();
      require(m_view->Camera()->Scale()>0,"Home produced an invalid scale");
      for(const auto& [id,item]:m_items) {
        Bnd_Box box; item.ais->BoundingBox(box); if(box.IsVoid()) continue;
        const auto lo=box.CornerMin(),hi=box.CornerMax();
        for(int i=0;i<8;++i) { double x,y; m_view->Project(i&1?hi.X():lo.X(),i&2?hi.Y():lo.Y(),i&4?hi.Z():lo.Z(),x,y);
          double w,h; m_view->Size(w,h); require(std::abs(x)<=w*.51 && std::abs(y)<=h*.51,"Home clipped scene bounds"); }
      }
      m_view->SetCamera(beforeHome);
      trace::log(QStringLiteral("bench: desktop topology / fitted Home PASS"));
    }
    auto move = [this](const QPoint& p,Qt::KeyboardModifiers mods=Qt::NoModifier) {
      QMouseEvent e(QEvent::MouseMove, QPointF(p), mapToGlobal(QPointF(p)), Qt::NoButton, Qt::NoButton, mods);
      QCoreApplication::sendEvent(this, &e);
      paintEvent(nullptr);
    };
    auto shift = [this](bool down) {
      QKeyEvent e(down ? QEvent::KeyPress : QEvent::KeyRelease, Qt::Key_Shift, down ? Qt::ShiftModifier : Qt::NoModifier);
      inferenceKey(&e);
    };
    auto click = [this](const QPoint& p,Qt::KeyboardModifiers mods=Qt::ControlModifier) {
      QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), mapToGlobal(QPointF(p)), Qt::LeftButton, Qt::LeftButton, mods);
      QMouseEvent release(QEvent::MouseButtonRelease, QPointF(p), mapToGlobal(QPointF(p)), Qt::LeftButton, Qt::NoButton, mods);
      QCoreApplication::sendEvent(this, &press);
      QCoreApplication::sendEvent(this, &release);
      paintEvent(nullptr);
    };
    if(m_selJob) m_selJob->cancel();
    m_ctx->ClearSelected(false);clearCenters();
    setPickAccumulate(true, true);
    std::vector<opad::Ref> picked;
    for (const auto& [id, item] : m_items) {
      auto p = m_prs.find(item.key);
      if (p == m_prs.end() || p->second->circles.size() < 2) continue;
      fitNodes({id});
      m_view->Redraw();
      for (const auto& [index, circle] : p->second->circles) {
        const gp_Pnt center = circle.center.Transformed(item.ais->Transformation());
        if (!picked.empty() && m_centers.at(picked.front().str()).point.Distance(center) < 1e-6) continue;
        const QPoint target = widgetPoint({center.X(), center.Y(), center.Z()});
        if (!rect().adjusted(12, 12, -12, -12).contains(target)) continue;
        opad::Ref ref; ref.body = id; ref.kind = opad::Ref::Kind::Center; ref.index = index;
        std::vector<gp_Pnt> rimSamples;
        if(circle.edge.ShapeType()==TopAbs_EDGE) {
          BRepAdaptor_Curve curve(TopoDS::Edge(circle.edge));
          for(int sample=1;sample<32;++sample) rimSamples.push_back(curve.Value(curve.FirstParameter()+(curve.LastParameter()-curve.FirstParameter())*sample/32.0));
        } else for(TopExp_Explorer edge(circle.edge,TopAbs_EDGE);edge.More();edge.Next()) {
          BRepAdaptor_Curve curve(TopoDS::Edge(edge.Current()));
          // Mesh rim endpoints deliberately compete with selectable mesh vertices.
          rimSamples.push_back(curve.Value(curve.FirstParameter()));
        }
        bool discovered = false; QPoint rimTarget;
        int tested=0;
        for (gp_Pnt rim : rimSamples) {
          if(circle.segments && ++tested>16) break;
          rim.Transform(item.ais->Transformation());
          move(widgetPoint({rim.X(), rim.Y(), rim.Z()}),Qt::ControlModifier);
          if (m_activeCenter == ref.str()) { rimTarget=widgetPoint({rim.X(),rim.Y(),rim.Z()}); discovered = true; break; }
        }
        if (!discovered) continue;
        if(picked.empty()) {
          TopExp_Explorer vertex(circle.edge,TopAbs_VERTEX);require(vertex.More(),"circle has no rim vertex");
          auto at=BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current())).Transformed(item.ais->Transformation());
          const auto vertexTarget=widgetPoint({at.X(),at.Y(),at.Z()});
          move(vertexTarget);click(vertexTarget,Qt::NoModifier);
          require(!selection().empty() && selection().back().kind==opad::Ref::Kind::Vertex,"plain circle rim click must select a vertex");
          m_ctx->ClearSelected(false);refreshSubHighlight();
          move(vertexTarget,Qt::ControlModifier);click(vertexTarget);
          require(!selection().empty() && selection().back().kind==opad::Ref::Kind::Center,"Ctrl-click on rim vertex must select a center");
          m_ctx->ClearSelected(false);clearCenters();move(rimTarget,Qt::ControlModifier);
          trace::log(QStringLiteral("bench: plain rim vertex / Ctrl circle center PASS"));
          click(rimTarget); picked.push_back(ref); require(!selection().empty() && selection().back().str()==ref.str(),"rim click did not select its center"); continue; }
        shift(true);
        require(!m_centerLocked, "Shift must not lock circle centers");
        shift(false);
        click(rimTarget);
        const auto refs = selection();
        if (refs.empty() || refs.back().str() != ref.str()) { m_centerLocked = false; continue; }
        require(refs.back().kind == opad::Ref::Kind::Center, "center was selected as an edge or body");
        picked.push_back(ref);
        trace::log(QStringLiteral("bench: picking center %1 via Qt rim click PASS").arg(QString::fromStdString(ref.str())));
        if (picked.size() == 2) break;
      }
      if (picked.size() == 2) break;
      if (!picked.empty()) break;  // preserve the first part's view for a useful failure image
    }
    require(picked.size() == 2 && selection().size() == 2, "could not pick two distinct circle centers");
    // Restoration uses the same stable source refs, including saved measurement picks.
    selectRefs(picked);
    require(selection().size() == 2 && selection()[0].str() == picked[0].str(), "center refs did not restore");
    trace::log(QStringLiteral("bench: picking center restoration PASS"));
    {
      QSignalBlocker blocked(this);
      const auto filter=m_filter;setSelectionFilter(SelFilter::Body);setHoverFade(true,.1);
      QEventLoop switchLoop;QTimer::singleShot(100,&switchLoop,&QEventLoop::quit);switchLoop.exec();m_view->Redraw();
      const auto cursor=devicePos(orbitCursor);m_ctx->MoveTo(cursor.x(),cursor.y(),m_view,false);trackHoverFade();
      if(m_ctx->HasDetected() && m_nodeOf.count(m_ctx->DetectedInteractive().get())) {
        const auto owner=m_ctx->DetectedOwner();QEventLoop loop;QTimer::singleShot(550,&loop,&QEventLoop::quit);loop.exec();
        updateHoverFade();require(m_ctx->HasDetected() && m_ctx->DetectedOwner()==owner,"hover fade disabled picking");
        const auto age=m_hoverAge.elapsed();m_ctx->MoveTo(cursor.x(),cursor.y(),m_view,false);trackHoverFade();require(m_hoverAge.elapsed()>=age,"same object movement restarted highlight");
        m_ctx->SelectDetected(AIS_SelectionScheme_Replace);require(m_ctx->NbSelected()>0,"faded object was not clickable");clearSelection();
      } else throw opad::Error("hover regression has no detected body");
      setHoverFade(true,5);setSelectionFilter(filter);trace::log("bench: hover fade keeps detection, selection and same-object timeout PASS");
      QEventLoop restoreLoop;QTimer::singleShot(100,&restoreLoop,&QEventLoop::quit);restoreLoop.exec();selectRefs(picked);
    }
    OnSelectionChanged(m_ctx,m_view);
    trace::log(QStringLiteral("bench: picking synchronous regression batch end"));
    return true;
  } catch (const std::exception& e) {
    trace::log(QStringLiteral("bench: picking FAIL: %1").arg(QString::fromUtf8(e.what())));
    return false;
  } catch (const Standard_Failure& e) {
    trace::log(QStringLiteral("bench: picking OCCT FAIL: %1").arg(QString::fromUtf8(e.GetMessageString())));
    return false;
  }
}
