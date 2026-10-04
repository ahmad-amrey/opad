#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "HelpClip.hpp"
#include "I18n.hpp"
#include "MainWindow.hpp"
#include "PrimitivePlacer.hpp"
#include "ToolValues.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/util.hpp"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>

#include <cmath>

// OPAD_BENCH_PRIMITIVES=<prefix> (TODO 11 P1, case primitive-place in tools/bench_cases/design.py): new primitives placed in
// the view as their guides show, through the view's own mouse and key events, on a document with a 30 x 20 x 10 box at
// x -55..-25 (the origin stays clear):
//   Cone: no preview before the click and the Plane box reads "Click in the view"; the pointer over XY shows the marker
//   where it meets the plane; a click writes Plane XY and Position X/Y; two moves of the pointer give two base diameters
//   and the preview (planned while the pointer moves) follows each, the boxes beside the pointer show it; a click fixes it;
//   the height arrow stands on the base's middle; pulled up, the preview while the button is held is the taller cone;
//   Tab twice reaches Top diameter and 4 typed goes there; Enter commits what was shown (the op's inputs, the body).
//   Box, grid snapping on: the marker and the click go to grid nodes; Esc from the footprint goes back to placing (no
//   preview, the Plane box asks again); the footprint snaps to the grid (Centred on: twice the pointer's offset); Esc from
//   the height goes back to the footprint (no arrow); 1 and 5 typed after the second click go into the height's box; Enter
//   commits 4 x 2 grid steps x 15 mm at the clicked node.
//   Sphere: pressed on the box's top face, dragged and let go: the face resolved, the diameter is the drag; centred on the
//   face (half of it below); Enter commits.
//   Torus: the click snaps to the plane's origin; the ring diameter (through the tube's middle) from the pointer, a click;
//   the section from the pointer's distance to the ring, a click; centred on the plane; Enter commits.
//   Coil: placed by a click; 1 and 8 typed while sizing hold as the pointer moves on; the click fixes them; Enter commits.
//   Box with Centred off: the click is a corner and the box runs towards the pointer; Esc twice leaves.
//   Cylinder: Enter before any click adds the panel's defaults at the XY origin (the keyboard's way).
// Along the way the panel's guide loops the clip's step for the stage (placing, sizing, then the arrow and Enter).
// Shots: <prefix>.<step>.png (the view: marker, outline, preview and arrow are drawn in it), <prefix>.panel.png.
OPAD_BENCH(OPAD_BENCH_PRIMITIVES, primitives) {
  struct State {
    size_t step = 0, features = 0;
    int ticks = 0, wait = 0;
    std::string block;
    double grid = 0, size1 = 0, size2 = 0, height = 0;
    QPointF press, dir, last;
    opad::Vec3 centre{0, 0, 0};
  };
  auto st = std::make_shared<State>();
  for (const auto& f : w.m_doc->scene.features)
    for (const auto& body : f.result.value("bodies", opad::json::array()))
      if (f.kind == "box") st->block = body.value("id", "");
  if (st->block.empty()) {
    trace::log("bench: primitives FAIL: the document needs a box");
    QCoreApplication::exit(2);
    return true;
  }
  w.setWorkspace("design");
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  PrimitivePlacer* placer = design->placer();
  DimensionHandle* handle = design->distanceHandle();
  Viewport* view = w.m_viewport;
  MainWindow* win = &w;
  const QString prefix = value;
  using Stage = PrimitivePlacer::Stage;

  auto require = [](bool ok, const std::string& why) {
    if (!ok) throw opad::Error(why);
  };
  auto pass = [](const QString& what) { trace::log("bench: primitives: " + what + " PASS"); };
  auto mm = [form](const char* input) { return opad::design::ParamTable().length(form->inputs().value(input, std::string("0"))); };
  auto stored = [win](const char* input) { return opad::design::ParamTable().length(win->m_doc->scene.features.back().inputs.value(input, std::string("0"))); };
  // The preview on screen was planned for what the panel holds now.
  auto previewFor = [design, form](std::initializer_list<const char*> inputs) {
    const opad::json in = design->readyPreviewInputs();
    if (!in.is_object() || !design->readyPreview() || design->readyPreview()->changed.empty()) return false;
    for (const char* name : inputs)
      if (!in.contains(name) || in[name] != form->inputs().value(name, opad::json())) return false;
    return true;
  };
  auto previewBox = [design] {
    Bnd_Box box;
    if (const auto plan = design->readyPreview())
      for (const auto& c : plan->changed)
        if (!c.removed && c.shape && !c.shape->IsNull()) BRepBndLib::AddOptimal(*c.shape, box, Standard_False, Standard_False);
    return box;
  };
  auto extent = [](const Bnd_Box& b, int axis) {
    if (b.IsVoid()) return 0.0;
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return axis == 0 ? x1 - x0 : axis == 1 ? y1 - y0 : z1 - z0;
  };
  auto low = [](const Bnd_Box& b, int axis) {
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return axis == 0 ? x0 : axis == 1 ? y0 : z0;
  };
  auto made = [win] {  // the last feature's body, where it is
    Bnd_Box box;
    for (const auto& body : win->m_doc->scene.features.back().result.value("bodies", opad::json::array()))
      BRepBndLib::AddOptimal(opad::node_world_shape(win->m_doc->doc, win->m_doc->scene, body.value("id", "")), box, Standard_False, Standard_False);
    return box;
  };
  // The pointer through the viewport's event filters (the placer's, the arrow's), in widget coordinates.
  auto mouse = [view](QEvent::Type type, const QPointF& at, Qt::MouseButtons held = Qt::NoButton) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseMove ? held : type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, view->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(view, &e);
  };
  auto at = [view](const opad::Vec3& p) { return QPointF(view->widgetPoint(p)); };
  // A move as a user's: a frame's hover has detected what is under the point before the placer reads it (the hover reads
  // the last frame's detection; a hidden window draws no frames, so the bench detects for it).
  auto hoverAt = [view, mouse](const QPointF& p) {
    view->benchHover(p);
    mouse(QEvent::MouseMove, p);
  };
  auto moveTo = [hoverAt, at](const opad::Vec3& p) { hoverAt(at(p)); };
  auto clickAt = [mouse, at](const opad::Vec3& p) {
    mouse(QEvent::MouseMove, at(p));
    mouse(QEvent::MouseButtonPress, at(p));
    mouse(QEvent::MouseButtonRelease, at(p));
  };
  auto key = [view](int code, const QString& text) {
    QApplication::setActiveWindow(view->window());
    QWidget* to = QApplication::focusWidget();
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
    QApplication::sendEvent(to ? to : static_cast<QWidget*>(view), &press);
  };
  auto shot = [view, prefix](const QString& name) { view->grabImage().save(prefix + "." + name + ".png"); };
  auto waitFor = [st](bool ok, const std::string& why) {
    if (ok) return st->wait = 0, true;
    if (++st->wait > 80) throw opad::Error(why);
    return false;
  };
  auto about = [](double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; };
  auto str = [](double v) { return std::to_string(v); };
  // An iso view over the box and the origin's neighbourhood; a frame drawn so the picker sees what the camera does.
  auto start = [view, design, st, win](const char* kind) {
    view->standardView("iso");
    Bnd_Box box;
    box.Update(-60, -30, 0, 40, 30, 25);
    view->fitBox(box);
    view->grabImage();
    st->features = win->m_doc->scene.features.size();
    design->startFeature(kind);
  };
  auto committed = [win, design, st](const char* kind) {
    return !design->featureActive() && win->m_doc->scene.features.size() == st->features + 1 && win->m_doc->scene.features.back().kind == kind && !win->m_doc->designBusy;
  };
  auto pullStep = [view] { return DimensionHandle::pullStep(view->pixelSize()); };
  // The panel's guide loops the clip's steps for the stage the tool waits in (clips::guideRange of the stage, of 3 or 4).
  auto guideAt = [form, placer](const char* clip) {
    return form->guide()->view()->range() == clips::guideRange(clip, placer->guideStep(), placer->guideCount());
  };
  // A point of XY in front of the origin (x > 0, y < 0 seen from the iso corner): the pointer meets the XY square there and
  // no other origin plane on its way (the XZ and YZ squares stand behind it).
  auto front = [view] {
    const double half = std::max(10.0, view->pixelSize() * 70);  // the origin planes' half size (DesignController::quickCandidates)
    return opad::Vec3{0.6 * half, -0.6 * half, 0};
  };
  auto undo = [win] { win->m_doc->undo(); };  // each primitive is taken back: the next one has the view to itself

  std::vector<std::function<bool()>> steps = {
      // ---- Cone on XY, by the pointer.
      [=] {
        start("cone");
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Place && !view->cameraMoving(), "the cone does not wait for a plane")) return false;
        view->grabImage();
        require(view->previewBodyCount() == 0 && !handle->isVisible(), "a preview or an arrow shows before the plane is clicked");
        const opad::Vec3 p = front();
        moveTo(p);
        opad::Vec3 marker;
        require(placer->markerShown(&marker), "the pointer over XY shows no marker");
        require(about(marker[0], p[0], pullStep()) && about(marker[1], p[1], pullStep()) && std::abs(marker[2]) < 1e-9, "the marker is not where the pointer meets XY: " + str(marker[0]) + ", " + str(marker[1]));
        require(view->hoveredCandidate() == opad::json{{"base", "xy"}}.dump(), "XY is not the hovered plane: " + view->hoveredCandidate());
        require(placer->guideStep() == 0 && guideAt("design.cone") && form->guide()->view()->range() == qMakePair(0, 0), "the guide does not loop the clip's placing step");
        shot("cone-place");
        win->m_featurePanel->grab().save(prefix + ".panel.png");
        pass("cone: nothing previewed before the click; the pointer over XY shows the marker where it meets the plane");
        clickAt(p);
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Size, "the click on XY did not place the cone")) return false;
        require(form->picks("plane") == opad::json{{"base", "xy"}}, "the cone's plane is not XY: " + form->picks("plane").dump());
        require(about(mm("x"), front()[0], pullStep()) && about(mm("y"), front()[1], pullStep()), "Position X/Y are not the click: " + form->valueText("x").toStdString() + ", " + form->valueText("y").toStdString());
        st->centre = {mm("x"), mm("y"), 0};
        require(guideAt("design.cone") && form->guide()->view()->range() == qMakePair(1, 1), "the guide does not loop the clip's sizing step");
        pass("cone: the click writes Plane XY and Position " + form->valueText("x") + ", " + form->valueText("y") + "; the guide goes on to the sizing step");
        moveTo({st->centre[0] + 8, st->centre[1], 0});
        require(about(mm("diameter"), 16, 2 * pullStep()), "the pointer 8 mm off did not make the base 16 mm: " + form->valueText("diameter").toStdString());
        return true;
      },
      [=] {
        if (!waitFor(previewFor({"diameter"}), "the preview did not follow the first move")) return false;
        st->size1 = extent(previewBox(), 0);
        require(about(st->size1, mm("diameter"), 0.1), "the preview's base is not the diameter shown: " + str(st->size1));
        DynamicInput* boxes = design->values()->input();
        require(placer->outlineShown(), "the base is not outlined on the plane while the pointer sizes it");
        require(win->m_promptText == placer->prompt() && placer->prompt().startsWith(form->input("diameter") ? i18n::t("Base diameter") : QString()),
                "the status bar's prompt is not the sizing step's: " + win->m_promptText.toStdString());
        require(boxes && boxes->isVisible() && boxes->count() == 1 && boxes->key(0) == "diameter" && boxes->box(0)->placeholderText() == form->valueText("diameter"),
                "the box beside the pointer does not show the base diameter");
        moveTo({st->centre[0] + 12, st->centre[1], 0});
        return true;
      },
      [=] {
        if (!waitFor(previewFor({"diameter"}), "the preview did not follow the second move")) return false;
        st->size2 = extent(previewBox(), 0);
        require(std::abs(st->size2 - st->size1) > 5 && about(st->size2, mm("diameter"), 0.1), "the preview's base did not change between the moves: " + str(st->size1) + " then " + str(st->size2));
        shot("cone-size");
        design->values()->input()->shot().save(prefix + ".cone-size.box.png");
        win->grab().save(prefix + ".cone-size.window.png");  // the panel, the guide and the prompt (the 3D view is blank there)
        pass("cone: the pointer sizes the base; the preview follows it (" + QString::number(st->size1) + " then " + QString::number(st->size2) + " mm) and the box beside it shows the diameter");
        clickAt({st->centre[0] + 12, st->centre[1], 0});
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Height && design->handleInput() == "height" && previewFor({"diameter", "height"}), "the second click shows no height arrow")) return false;
        const double h = mm("height");
        require(about(QLineF(handle->arrowLine().p1(), at({st->centre[0], st->centre[1], h})).length(), 0, 4), "the height arrow is not on the base's middle at the height");
        require(!design->values()->input()->isVisible(), "the boxes beside the pointer stayed after the click");
        require(guideAt("design.cone") && form->guide()->view()->range() == qMakePair(2, 3), "the guide does not loop the clip's arrow and Enter steps");
        // Pulled up 6 mm along itself, the button held.
        const QLineF line = handle->arrowLine();
        st->dir = (line.p2() - line.p1()) / std::max(1e-9, line.length());
        const double px = QLineF(at({st->centre[0], st->centre[1], h}), at({st->centre[0], st->centre[1], h + 1})).length();
        st->press = line.pointAt(0.5);
        st->height = h;
        mouse(QEvent::MouseButtonPress, st->press);
        require(handle->dragging(), "a press on the height arrow did not grip it");
        mouse(QEvent::MouseMove, st->press + st->dir * (3 * px), Qt::LeftButton);
        st->last = st->press + st->dir * (6 * px);
        mouse(QEvent::MouseMove, st->last, Qt::LeftButton);
        require(mm("height") > h + 4, "pulling the arrow up did not raise the height: " + form->valueText("height").toStdString());
        return true;
      },
      [=] {
        if (!waitFor(previewFor({"height"}), "the cone preview did not follow the pull")) return false;
        require(handle->dragging(), "the arrow let go before the release");
        require(about(extent(previewBox(), 2), mm("height"), 0.1), "the preview during the pull is not the cone at the pulled height");
        shot("cone-height");
        pass("cone: the second click fixes the base; the height arrow pulled up while held makes the preview " + form->valueText("height") + " tall");
        mouse(QEvent::MouseButtonRelease, st->last);
        view->setFocus();
        key(Qt::Key_Tab, "\t");
        key(Qt::Key_Tab, "\t");
        key(Qt::Key_4, "4");
        return true;
      },
      [=] {
        if (!waitFor(about(mm("top_diameter"), 4, 1e-9), "Tab twice and 4 typed did not set the top diameter: " + form->valueText("top_diameter").toStdString())) return false;
        pass("cone: Tab from the height's box reaches Top diameter; 4 typed goes there");
        st->centre[2] = mm("height");  // kept: what the panel shows now
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(committed("cone"), "Enter did not commit the cone")) return false;
        const auto& f = win->m_doc->scene.features.back();
        require(f.inputs.value("plane", opad::json()) == opad::json{{"base", "xy"}} && about(stored("x"), st->centre[0], 1e-6) && about(stored("y"), st->centre[1], 1e-6) &&
                    about(stored("diameter"), st->size2, 1e-6) && about(stored("height"), st->centre[2], 1e-6) && about(stored("top_diameter"), 4, 1e-9),
                "the cone's inputs are not the values shown: " + f.inputs.dump());
        const Bnd_Box b = made();
        require(about(extent(b, 0), st->size2, 0.1) && about(extent(b, 2), st->centre[2], 0.1) && about(low(b, 2), 0, 0.05), "the cone body is not the one shown");
        require(!handle->isVisible() && placer->stage() == Stage::Off, "the arrow or the placing stayed after the commit");
        pass("cone: Enter commits the values shown");
        undo();
        return true;
      },
      // ---- Box with grid snapping: Esc steps back from the footprint and from the height.
      [=] {
        view->setGridSnap(true);
        st->grid = view->gridStep();
        require(st->grid > 0, "the view has no grid step");
        start("box");
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Place && !view->cameraMoving(), "the box does not wait for a plane")) return false;
        view->grabImage();
        const double g = st->grid;
        moveTo({2.3 * g, -1.2 * g, 0});
        opad::Vec3 marker;
        require(placer->markerShown(&marker) && about(marker[0], 2 * g, 1e-6) && about(marker[1], -g, 1e-6), "with grid snapping on the marker is not on the grid node");
        clickAt({2.3 * g, -1.2 * g, 0});
        require(placer->stage() == Stage::Size && about(mm("x"), 2 * g, 1e-6) && about(mm("y"), -g, 1e-6), "the click did not place the box on the grid node");
        moveTo({3.6 * g, -2.1 * g, 0});
        return true;
      },
      [=] {
        if (!waitFor(previewFor({"length", "width"}), "the box preview did not follow the pointer")) return false;
        pass("box: with grid snapping on the marker and the click go to the grid node; the pointer sizes the base");
        design->escape();
        require(placer->stage() == Stage::Place && view->previewBodyCount() == 0, "Esc from the footprint did not go back to placing");
        return true;
      },
      [=] {
        const double g = st->grid;
        moveTo({1.1 * g, -0.9 * g, 0});
        require(placer->markerShown(), "back in placing, the pointer shows no marker");
        pass("box: Esc from the footprint goes back to placing: no preview, the marker again");
        clickAt({1.1 * g, -0.9 * g, 0});
        require(placer->stage() == Stage::Size && about(mm("x"), g, 1e-6) && about(mm("y"), -g, 1e-6), "the second placing click did not go to the node: stage " + std::to_string(int(placer->stage())) + ", " + form->valueText("x").toStdString() + ", " + form->valueText("y").toStdString() + ", active " + form->activeInput().toStdString());
        moveTo({2.6 * g, -2.1 * g, 0});  // the node (3g, -2g): 2g and g from the centre
        require(about(mm("length"), 4 * g, 1e-6) && about(mm("width"), 2 * g, 1e-6), "the footprint is not twice the pointer's grid offset: " + form->valueText("length").toStdString() + " x " + form->valueText("width").toStdString());
        clickAt({2.6 * g, -2.1 * g, 0});
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Height && design->handleInput() == "height", "the box shows no height arrow")) return false;
        design->escape();
        require(placer->stage() == Stage::Size && !handle->isVisible(), "Esc from the height did not go back to the footprint");
        const double g = st->grid;
        moveTo({3.4 * g, -2.4 * g, 0});  // the same node again
        clickAt({3.4 * g, -2.4 * g, 0});
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Height && design->handleInput() == "height" && previewFor({"length", "width", "height"}), "the box shows no height arrow again")) return false;
        pass("box: Esc from the height goes back to the footprint; the arrow goes, a click brings it back");
        view->setFocus();
        key(Qt::Key_1, "1");
        key(Qt::Key_5, "5");
        return true;
      },
      [=] {
        if (!waitFor(about(mm("height"), 15, 1e-9), "1 and 5 typed did not go into the height: " + form->valueText("height").toStdString())) return false;
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(committed("box"), "Enter did not commit the box")) return false;
        const double g = st->grid;
        require(about(stored("length"), 4 * g, 1e-6) && about(stored("width"), 2 * g, 1e-6) && about(stored("height"), 15, 1e-9) && about(stored("x"), g, 1e-6) && about(stored("y"), -g, 1e-6),
                "the box's inputs are not the values shown: " + win->m_doc->scene.features.back().inputs.dump());
        const Bnd_Box b = made();
        require(about(low(b, 0), g - 2 * g, 0.05) && about(extent(b, 0), 4 * g, 0.05) && about(extent(b, 2), 15, 0.05), "the box body is not the one shown");
        view->setGridSnap(false);
        pass("box: the typed height and Enter commit " + QString::number(4 * g) + " x " + QString::number(2 * g) + " x 15 mm at the node");
        undo();
        return true;
      },
      // ---- Sphere on the block's top face: pressed, dragged, let go.
      [=] {
        start("sphere");
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Place && !view->cameraMoving(), "the sphere does not wait for a plane")) return false;
        view->grabImage();
        opad::Vec3 marker;
        hoverAt(at({-25, -10, 10}) + QPointF(-5, -3));  // a few pixels inside the top face's front corner
        require(placer->markerShown(&marker) && about(marker[0], -25, 1e-6) && about(marker[1], -10, 1e-6) && about(marker[2], 10, 1e-6), "the pointer near the face's corner does not snap to it");
        pass("sphere: over the block's top face the marker snaps to its corner");
        moveTo({-40, 0, 10});
        require(placer->markerShown(&marker) && about(marker[2], 10, 1e-6), "the pointer over the block's top face shows no marker on it");
        st->press = at({-40, 0, 10});
        mouse(QEvent::MouseButtonPress, st->press);
        mouse(QEvent::MouseMove, at({-37, 0, 10}), Qt::LeftButton);
        st->last = at({-33, 0, 10});
        mouse(QEvent::MouseMove, st->last, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, st->last);
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Done && previewFor({"diameter"}), "pressing on the face, dragging and letting go did not size the sphere")) return false;
        const opad::json plane = form->picks("plane");
        require(plane.is_object() && plane.contains("face"), "the sphere's plane is not the face: " + plane.dump());
        require(about(mm("diameter"), 14, 1), "the drag of 7 mm did not make a 14 mm sphere: " + form->valueText("diameter").toStdString());
        const Bnd_Box b = previewBox();
        require(about(low(b, 2) + extent(b, 2) / 2, 10, 0.05), "the sphere preview is not centred on the face");
        shot("sphere");
        pass("sphere: pressed on the block's top face, dragged 7 mm and let go: the face's plane, a " + form->valueText("diameter") + " sphere centred on it");
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(committed("sphere"), "Enter did not commit the sphere")) return false;
        const Bnd_Box b = made();
        require(about(low(b, 2), 10 - stored("diameter") / 2, 0.05) && about(low(b, 0) + extent(b, 0) / 2, -40, 0.6), "the sphere body is not centred on the face where it was pressed");
        pass("sphere: Enter commits it centred on the face");
        undo();
        return true;
      },
      // ---- Torus: the origin snapped, the ring, then the section.
      [=] {
        start("torus");
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Place && !view->cameraMoving(), "the torus does not wait for a plane")) return false;
        view->grabImage();
        opad::Vec3 marker;
        hoverAt(at({0, 0, 0}) + QPointF(4, -3));  // a few pixels off: the hovered plane's origin takes it
        require(placer->markerShown(&marker) && marker == opad::Vec3{0, 0, 0}, "the pointer a few pixels off the origin does not snap to it");
        pass("torus: the pointer a few pixels off the origin snaps to it");
        const opad::Vec3 p = front();
        clickAt(p);
        require(placer->stage() == Stage::Size && form->picks("plane") == opad::json{{"base", "xy"}}, "the torus was not placed on XY");
        st->centre = {mm("x"), mm("y"), 0};
        moveTo({st->centre[0] + 12, st->centre[1], 0});
        require(about(mm("diameter"), 24, 2 * pullStep()), "the ring diameter is not twice the pointer's distance: " + form->valueText("diameter").toStdString());
        clickAt({st->centre[0] + 12, st->centre[1], 0});
        require(placer->stage() == Stage::Section, "the ring's click does not go on to the section");
        moveTo({st->centre[0] + 15, st->centre[1], 0});
        return true;
      },
      [=] {
        if (!waitFor(previewFor({"diameter", "section"}), "the torus preview did not follow the section")) return false;
        require(about(mm("section"), 6, 2 * pullStep()), "the section is not twice the pointer's distance to the ring: " + form->valueText("section").toStdString());
        const Bnd_Box b = previewBox();
        require(about(low(b, 2), -mm("section") / 2, 0.05) && about(extent(b, 0), mm("diameter") + mm("section"), 0.1), "the torus preview is not centred on the plane, the ring through the tube's middle");
        shot("torus");
        pass("torus: the ring through the tube's middle is " + form->valueText("diameter") + ", the section " + form->valueText("section") + ", centred on the plane");
        clickAt({st->centre[0] + 15, st->centre[1], 0});
        require(placer->stage() == Stage::Done, "the section's click does not end the placing");
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(committed("torus"), "Enter did not commit the torus")) return false;
        const Bnd_Box b = made();
        require(about(extent(b, 0), stored("diameter") + stored("section"), 0.1) && about(low(b, 2), -stored("section") / 2, 0.05), "the torus body is not the one shown");
        pass("torus: Enter commits it");
        undo();
        return true;
      },
      // ---- Coil: placed, 1 and 8 typed while sizing: the diameter holds whatever the pointer does; a click, Enter.
      [=] {
        start("coil");
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Place && !view->cameraMoving(), "the coil does not wait for a plane")) return false;
        view->grabImage();
        clickAt(front());
        require(placer->stage() == Stage::Size, "the click did not place the coil");
        st->centre = {mm("x"), mm("y"), 0};
        moveTo({st->centre[0] + 5, st->centre[1], 0});
        require(about(mm("diameter"), 10, 2 * pullStep()), "the coil diameter is not twice the pointer's distance: " + form->valueText("diameter").toStdString());
        view->setFocus();
        key(Qt::Key_1, "1");
        key(Qt::Key_8, "8");
        return true;
      },
      [=] {
        if (!waitFor(about(mm("diameter"), 18, 1e-9), "1 and 8 typed while sizing did not go into the coil diameter: " + form->valueText("diameter").toStdString())) return false;
        moveTo({st->centre[0] + 3, st->centre[1], 0});
        require(about(mm("diameter"), 18, 1e-9), "the pointer moved the typed coil diameter: " + form->valueText("diameter").toStdString());
        clickAt({st->centre[0] + 3, st->centre[1], 0});
        require(placer->stage() == Stage::Done && about(mm("diameter"), 18, 1e-9), "the click did not fix the typed diameter");
        pass("coil: placed by a click; 18 typed while sizing holds as the pointer moves and the click fixes it");
        return true;
      },
      [=] {
        if (!waitFor(previewFor({"diameter"}), "the coil shows no preview")) return false;
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(committed("coil"), "Enter did not commit the coil")) return false;
        require(about(stored("diameter"), 18, 1e-9), "the committed coil is not 18 mm across");
        pass("coil: Enter commits it");
        undo();
        return true;
      },
      // ---- Box with Centred off: the click is a corner, the box runs towards the pointer (here back and left of it).
      [=] {
        start("box");
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Place && !view->cameraMoving(), "the box does not wait for a plane")) return false;
        view->grabImage();
        form->setValue("centered", false);
        clickAt(front());
        st->centre = {mm("x"), mm("y"), 0};
        moveTo({st->centre[0] - 10, st->centre[1] + 6, 0});
        require(about(mm("length"), 10, 2 * pullStep()) && about(mm("width"), 6, 2 * pullStep()), "with Centred off the sizes are not the pointer's offset");
        require(about(mm("x"), st->centre[0] - mm("length"), 1e-6) && about(mm("y"), st->centre[1], 1e-6), "with Centred off the box does not run from the corner towards the pointer");
        pass("box: with Centred off the click is a corner and the box runs towards the pointer");
        design->escape();
        design->escape();
        return true;
      },
      [=] { return waitFor(!design->featureActive(), "Esc twice did not leave the box"); },
      // ---- Cylinder: Enter before any click (the keyboard's way): the panel's defaults at the origin.
      [=] {
        start("cylinder");
        return true;
      },
      [=] {
        if (!waitFor(placer->stage() == Stage::Place, "the cylinder does not wait for a plane")) return false;
        view->setFocus();
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(committed("cylinder"), "Enter before any click did not commit the cylinder")) return false;
        require(stored("x") == 0 && stored("y") == 0 && stored("diameter") == 20 && stored("height") == 20, "Enter before any click did not add the defaults at the origin");
        pass("cylinder: Enter before any click adds the panel's defaults at the XY origin");
        return true;
      },
  };
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, steps, timer] {
    try {
      if (++st->ticks > 3000) throw opad::Error("timed out at step " + std::to_string(st->step));
      if (w.m_doc->loading || w.m_doc->designBusy || w.m_jobs->busy() || w.m_viewport->cameraMoving()) return;
      if (st->step < steps.size()) {
        if (steps[st->step]()) ++st->step;
        return;
      }
      timer->stop();
      trace::log("bench: primitives: all steps PASS");
      QCoreApplication::exit(0);
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: primitives FAIL at step %1: %2").arg(st->step).arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
