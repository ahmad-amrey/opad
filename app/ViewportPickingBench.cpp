// In-app regression, enabled explicitly with OPAD_BENCH_PICKING and --bench-select.
// Qt events stay within this widget; this never moves the OS cursor or drives the user's desktop.
#include "Viewport.hpp"
#include "Jobs.hpp"
#include "NavCube.hpp"
#include "CursorWrap.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
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
#include <cmath>
#include <gp_Pln.hxx>

bool Viewport::benchPicking() {
  auto require = [](bool ok, const char* message) { if (!ok) throw opad::Error(message); };
  try {
    trace::log(QStringLiteral("bench: picking synchronous regression batch begin"));
    m_view->Redraw();
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

    // A gap at the viewport center: choose the nearest projected part and its front
    // surface, independent of selection, hidden bodies and the mouse's empty location.
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
      const auto center = devicePos(QPointF(width()/2.0, height()/2.0));
      gp_Pnt picked;
      require(!navigationPoint(center, picked), "off-center test unexpectedly hits geometry at center");
      QElapsedTimer searchTimer; searchTimer.start();
      picked = orbitPoint(devicePos(QPointF(-200, -200)));
      require(std::abs(picked.X()-20) < pixelSize()*3 && std::abs(picked.Y()) < pixelSize()*3
              && std::abs(picked.Z()-30) < 1e-6, "central fallback missed closest foreground geometry");
      trace::log(QStringLiteral("bench: off-center orbit search %1 ms").arg(searchTimer.elapsed()));
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
    for (int y = 1; y < 10; ++y) for (int x = 1; x < 10; ++x) {
      gp_Pnt p;
      if (navigationPoint(devicePos(QPointF(width() * x / 10.0, height() * y / 10.0)), p)) {
        ++hits;
        orbitTarget = p;
        orbitCursor = QPoint(width() * x / 10, height() * y / 10);
      }
    }
    require(hits > 0, "navigation found no surfaces with the vertex filter active");
    trace::log(QStringLiteral("bench: picking navigation 81 rays: %1 ms, %2 hits, %3 bodies").arg(timer.elapsed()).arg(hits).arg(m_items.size()));
    const Graphic3d_Vec2i empty = devicePos(QPointF(-200, -200));
    const gp_Pnt fallback = orbitPoint(empty);
    require(fallback.Distance(centralOrbitPoint()) < 1e-7, "empty-space pivot did not use central geometry");
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
    require(perspectiveFallback.Distance(centralOrbitPoint()) < 1e-7,
        "perspective empty-space pivot did not use central geometry");
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
    m_haveTrackingAnchor=true; m_trackingAnchor=gp_Pnt(0,0,0);
    m_trackingDirection=gp_Vec(1,0,0); m_trackingHasDirection=true;
    const double reach=pixelSize()*40;
    m_trackingCursor=widgetPoint({reach,0,0}); m_trackingDirty=true;
    updateTracking();
    require(!m_trackingMarker.empty(), "extension tracking did not create a point");
    auto tracked=m_centers.at(m_trackingMarker).ref;
    require(tracked.kind==opad::Ref::Kind::Point && std::abs(tracked.point[1])<1e-8, "tracking point left its extension line");
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

    auto move = [this](const QPoint& p) {
      QMouseEvent e(QEvent::MouseMove, QPointF(p), mapToGlobal(QPointF(p)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QCoreApplication::sendEvent(this, &e);
      paintEvent(nullptr);
    };
    auto shift = [this] {
      QKeyEvent e(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
      QCoreApplication::sendEvent(this, &e);
    };
    auto click = [this](const QPoint& p) {
      QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), mapToGlobal(QPointF(p)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QMouseEvent release(QEvent::MouseButtonRelease, QPointF(p), mapToGlobal(QPointF(p)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QCoreApplication::sendEvent(this, &press);
      QCoreApplication::sendEvent(this, &release);
      paintEvent(nullptr);
    };
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
        BRepAdaptor_Curve curve(TopoDS::Edge(circle.edge));
        bool discovered = false;
        for (int sample = 1; sample < 32; ++sample) {
          gp_Pnt rim = curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * sample / 32.0);
          rim.Transform(item.ais->Transformation());
          move(widgetPoint({rim.X(), rim.Y(), rim.Z()}));
          if (m_activeCenter == ref.str()) { discovered = true; break; }
        }
        if (!discovered) continue;
        shift();
        require(m_centerLocked, "Shift did not lock the discovered center");
        shift();
        require(!m_centerLocked, "second Shift did not unlock the center");
        shift();
        if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) grabImage().save(shot + ".center.png");
        move(target);
        require(m_activeCenter == ref.str(), "locked center changed while approaching it");
        click(target);
        const auto refs = selection();
        if (refs.empty() || refs.back().str() != ref.str()) { m_centerLocked = false; continue; }
        require(refs.back().kind == opad::Ref::Kind::Center, "center was selected as an edge or body");
        picked.push_back(ref);
        trace::log(QStringLiteral("bench: picking center %1 via Qt hover / Shift / click PASS").arg(QString::fromStdString(ref.str())));
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
