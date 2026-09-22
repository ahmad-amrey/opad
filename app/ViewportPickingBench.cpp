// In-app regression, enabled explicitly with OPAD_BENCH_PICKING and --bench-select.
// Qt events stay within this widget; this never moves the OS cursor or drives the user's desktop.
#include "Viewport.hpp"
#include "Jobs.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBndLib.hxx>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
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
    const QPoint projection = widgetPoint({fallback.X(), fallback.Y(), fallback.Z()});
    require((projection - QPoint(-200, -200)).manhattanLength() <= 2, "empty-space pivot is not under the cursor");
    const double scale = m_view->Camera()->Scale();
    const gp_Dir direction = m_view->Camera()->Direction();
    focusCube();
    require(std::abs(m_view->Camera()->Scale() - scale) < 1e-7 && direction.IsEqual(m_view->Camera()->Direction(), 1e-7), "cube focus moved or zoomed the image");
    trace::log(QStringLiteral("bench: picking empty-space and cube focus PASS"));

    Handle(Graphic3d_Camera) savedCamera = new Graphic3d_Camera(*m_view->Camera());
    m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Perspective);
    const gp_Pnt perspectiveFallback = orbitPoint(empty);
    require((widgetPoint({perspectiveFallback.X(), perspectiveFallback.Y(), perspectiveFallback.Z()}) - QPoint(-200, -200)).manhattanLength() <= 2,
        "perspective empty-space pivot is not under the cursor");
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
