#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/util.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <optional>
#include <set>

// OPAD_BENCH_PICKROUTING=<prefix> (TODO 11 P3, case pick-routing in tools/bench_cases/design.py): clicks in the view reach the
// input the guide shows, through the viewport's own mouse handlers on a box (x -55..-25), a cylinder (x 32..48) and a sketch
// "Ring" on XZ (x 15..22, z 0..10), each feature from its start:
//   Revolve: the ring's region, then the Z axis in the view while Profiles is still the active input: Axis is Z, a preview.
//   Mirror: the box, then the YZ origin plane in the view (no plane picker): Mirror plane is YZ, a preview.
//   Combine: the first target pick moves on to Tool bodies; the cylinder becomes the tool.
//   Draft: one side face, then the XY plane in the view: Neutral plane is XY; opening the Neutral plane box with that one
//   face picked does not take the face as the plane.
//   Circular pattern: the cylinder, then the Y axis, then the Z axis in the view.
//   Construction axis: a flat face cannot be picked, the cylinder's round face can (no filter change): the axis follows.
//   Construction plane: a click on the box's top face, then on the YZ origin plane, fills From plane.
//   New sketch: the XY origin plane clicked in the view, then Enter in the origin stage: the sketch is open.
// Shots: <prefix>.<step>.png (the view; hidden windows draw their frames for grabImage) and <prefix>.sketch-origin.png (the
// plane panel).
OPAD_BENCH(OPAD_BENCH_PICKROUTING, pickrouting) {
  struct State {
    size_t step = 0;
    int ticks = 0, wait = 0, settle = 0;
    std::string box, cylinder;
    std::set<int> sideFaces, topFaces;  // the box's faces upright / facing +Z, by ordinal
  };
  auto st = std::make_shared<State>();
  for (const auto& f : w.m_doc->scene.features)
    for (const auto& body : f.result.value("bodies", opad::json::array())) {
      if (f.kind == "box") st->box = body.value("id", "");
      if (f.kind == "cylinder") st->cylinder = body.value("id", "");
    }
  if (st->box.empty() || st->cylinder.empty() || w.m_doc->scene.sketches.empty()) {
    trace::log("bench: pick routing FAIL: the document needs a box, a cylinder and a sketch");
    QCoreApplication::exit(2);
    return true;
  }
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(opad::node_world_shape(w.m_doc->doc, w.m_doc->scene, st->box), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const opad::json d = opad::describe_entity(faces(i));
      if (!d.contains("normal")) continue;
      const double z = d["normal"][2].get<double>();
      if (std::abs(z) < 0.1) st->sideFaces.insert(i - 1);
      if (z > 0.99) st->topFaces.insert(i - 1);
    }
  }
  w.setWorkspace("design");
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  Viewport* view = w.m_viewport;
  const QString prefix = value;

  using Want = std::function<bool(const std::string&, const opad::Ref&)>;
  auto require = [](bool ok, const std::string& why) {
    if (!ok) throw opad::Error(why);
  };
  auto pass = [](const QString& what) { trace::log("bench: pick routing: " + what + " PASS"); };
  auto find = [view](const Want& want, bool inside = true) -> std::optional<QPoint> {
    int x = 0, y = 0;
    if (!view->benchPickPoint(want, x, y, inside)) return std::nullopt;
    return QPoint(x, y);
  };
  auto candidate = [](const std::string& id) -> Want { return [id](const std::string& c, const opad::Ref&) { return c == id; }; };
  auto body = [](const std::string& id) -> Want { return [id](const std::string& c, const opad::Ref& r) { return c.empty() && r.body == id; }; };
  auto face = [](const std::string& id, std::set<int> among) -> Want {
    return [id, among](const std::string& c, const opad::Ref& r) { return c.empty() && r.body == id && r.kind == opad::Ref::Kind::Face && (among.empty() || among.count(r.index)); };
  };
  auto click = [view, require](const std::optional<QPoint>& at, const std::string& what) {
    require(at.has_value(), what + " is not found in the view");
    view->benchClickAt(at->x(), at->y());
  };
  auto count = [form](const char* input) {
    const opad::json p = form->picks(input);
    return p.is_array() ? int(p.size()) : p.is_null() ? 0 : 1;
  };
  auto start = [view, design](const char* kind) {
    view->standardView("iso");
    view->fitAll();
    design->startFeature(kind);
  };
  auto shot = [view, prefix](const char* name) { view->grabImage().save(prefix + "." + name + ".png"); };
  const std::string xy = R"({"base":"xy"})", yz = R"({"base":"yz"})";

  // Each step runs once the jobs are idle; true moves on, false waits for the next tick (`waitFor` bounds that).
  auto waitFor = [st](bool ok, const std::string& why) {
    if (ok) return st->wait = 0, true;
    if (++st->wait > 60) throw opad::Error(why);
    return false;
  };
  std::vector<std::function<bool()>> steps = {
      // ---- Revolve: the profile, then the Z axis in the view.
      [=] { start("revolve"); require(form->activeInput() == "profiles", "revolve starts on Profiles"); return true; },
      [=] { click(find([](const std::string& c, const opad::Ref&) { return c.find("\"at\"") != std::string::npos; }), "the ring's region"); return true; },
      [=] {
        if (!waitFor(count("profiles") == 1, "the region click did not pick the profile")) return false;
        require(form->activeInput() == "profiles", "Profiles stays active after its first pick");
        click(find(candidate(R"({"base":"z"})"), false), "the Z axis (shown while Profiles is active)");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("axis") == opad::json::parse(R"({"base":"z"})"), "a click on the Z axis did not fill Axis: " + form->picks("axis").dump())) return false;
        require(form->activeInput() == "profiles" && count("profiles") == 1, "the axis click left the profile picked");
        return true;
      },
      [=] {
        if (!waitFor(view->previewBodyCount() > 0, "revolve preview")) return false;
        shot("revolve");
        pass("revolve: the region, then the Z axis in the view fill Profiles and Axis; the preview shows");
        design->escape();
        return true;
      },
      // ---- Mirror: the box, then the YZ origin plane in the view, no plane picker.
      [=] { start("mirror"); require(form->activeInput() == "bodies", "mirror starts on Bodies"); return true; },
      [=] { click(find(body(st->box)), "the box"); return true; },
      [=] {
        if (!waitFor(count("bodies") == 1, "the box click did not pick it")) return false;
        click(find(candidate(yz)), "the YZ origin plane (shown once a body is picked)");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("plane") == opad::json::parse(yz), "a click on the YZ plane did not fill Mirror plane: " + form->picks("plane").dump())) return false;
        require(!design->pickingPlane(), "the plane click went through the plane picker");
        if (!waitFor(view->previewBodyCount() > 0, "mirror preview")) return false;
        shot("mirror");
        pass("mirror: the box, then the YZ plane in the view fill Bodies and Mirror plane; the preview shows");
        design->escape();
        return true;
      },
      // ---- Combine: target, then tools.
      [=] { start("combine"); require(form->activeInput() == "target", "combine starts on Target bodies"); return true; },
      [=] { click(find(body(st->box)), "the box"); return true; },
      [=] {
        if (!waitFor(count("target") == 1, "the box click did not pick the target")) return false;
        require(form->activeInput() == "tools", "the first target pick did not move on to Tool bodies (active: " + form->activeInput().toStdString() + ")");
        pass("combine: the first target pick moves on to Tool bodies");
        click(find(body(st->cylinder)), "the cylinder");
        return true;
      },
      [=] {
        if (!waitFor(count("tools") == 1, "the cylinder click did not pick a tool")) return false;
        require(count("target") == 1 && form->picks("tools")[0].value("body", "") == st->cylinder && form->picks("target")[0].value("body", "") == st->box, "target box, tool cylinder");
        if (!waitFor(view->previewBodyCount() > 0, "combine preview")) return false;
        pass("combine: two clicks give one target and one tool; the preview shows");
        design->escape();
        return true;
      },
      // ---- Draft: one side face, then the XY plane; the Neutral plane box does not take that face.
      [=] { start("draft"); require(form->activeInput() == "faces", "draft starts on Faces"); return true; },
      [=] {
        if (!waitFor(view->selectionFilter() == Viewport::SelFilter::Face, "draft's Faces switch the view to faces")) return false;
        click(find(face(st->box, st->sideFaces)), "a side face of the box");
        return true;
      },
      [=] {
        if (!waitFor(count("faces") == 1, "the face click did not pick it")) return false;
        click(find(candidate(xy)), "the XY origin plane (shown once a face is picked)");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("plane") == opad::json::parse(xy), "a click on the XY plane did not fill Neutral plane: " + form->picks("plane").dump())) return false;
        require(form->activeInput() == "faces" && count("faces") == 1, "the plane click left the face picked");
        if (!waitFor(view->previewBodyCount() > 0, "draft preview")) return false;
        shot("draft");
        pass("draft: one face, then the XY plane in the view fill Faces and Neutral plane; the preview shows");
        form->activate("plane");  // the Neutral plane box clicked: the plane picker opens
        return true;
      },
      [=] {
        if (!waitFor(design->pickingPlane() && design->planePicker()->active(), "the Neutral plane box did not open the plane picker")) return false;
        if (++st->settle < 12) return false;  // a second: a face adopted by the picker would have been resolved by now
        require(form->picks("plane") == opad::json::parse(xy) && !design->planePicker()->positioning(), "opening Neutral plane took the picked face: " + form->picks("plane").dump());
        pass("draft: opening Neutral plane with one face picked does not take that face as the plane");
        design->escape();  // the picker
        return true;
      },
      [=] {
        if (!waitFor(!design->pickingPlane(), "Esc did not leave the plane picker")) return false;
        design->escape();  // the feature
        return true;
      },
      // ---- Circular pattern: the cylinder, then the Y axis, then the Z axis.
      [=] { start("pattern_circ"); return true; },
      [=] { click(find(body(st->cylinder)), "the cylinder"); return true; },
      [=] {
        if (!waitFor(count("bodies") == 1, "the cylinder click did not pick it")) return false;
        click(find(candidate(R"({"base":"y"})"), false), "the Y axis (shown once a body is picked)");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("axis") == opad::json::parse(R"({"base":"y"})"), "a click on the Y axis did not fill Axis: " + form->picks("axis").dump())) return false;
        click(find(candidate(R"({"base":"z"})"), false), "the Z axis");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("axis") == opad::json::parse(R"({"base":"z"})"), "a click on the Z axis did not fill Axis: " + form->picks("axis").dump())) return false;
        require(count("bodies") == 1 && form->activeInput() == "bodies", "the axis clicks left the body picked");
        if (!waitFor(view->previewBodyCount() > 0, "circular pattern preview")) return false;
        shot("pattern-circ");
        pass("circular pattern: the cylinder, then the Y and the Z axis in the view fill Bodies and Axis; the preview shows");
        design->escape();
        return true;
      },
      // ---- Construction axis: round faces without a filter change, flat ones not.
      [=] { start("axis"); require(form->activeInput() == "edge", "the construction axis starts on Edge or round face"); return true; },
      [=] {
        if (!waitFor(view->selectionFilter() == Viewport::SelFilter::Edge && view->roundFacesPickable(), "the axis input did not make round faces pickable")) return false;
        require(!find(face(st->box, {})), "a flat face of the box can be picked for an axis");
        click(find(face(st->cylinder, {})), "the cylinder's round face");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("edge").contains("face"), "the round face click did not fill the axis input: " + form->picks("edge").dump())) return false;
        if (!waitFor(view->previewBodyCount() > 0, "construction axis preview")) return false;
        shot("axis");
        pass("construction axis: a round face is picked under the edge filter (flat faces are not); the axis preview shows");
        design->escape();
        return true;
      },
      // ---- Construction plane: From plane by a click, with no input active.
      [=] { start("plane"); require(form->activeInput().isEmpty(), "the construction plane starts with no input active"); return true; },
      [=] {
        if (!waitFor(view->selectionFilter() == Viewport::SelFilter::Face, "the construction plane did not make faces pickable")) return false;
        click(find(face(st->box, st->topFaces)), "the box's top face");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("plane").contains("face"), "the face click did not fill From plane: " + form->picks("plane").dump())) return false;
        require(form->picks("plane")["face"].contains("hint"), "the face was not resolved (no hint)");
        if (!waitFor(view->previewBodyCount() > 0, "construction plane preview")) return false;
        shot("plane");
        pass("construction plane: a click on a planar face fills From plane; the preview shows");
        click(find(candidate(yz)), "the YZ origin plane");
        return true;
      },
      [=] {
        if (!waitFor(form->picks("plane") == opad::json::parse(yz), "a click on the YZ plane did not fill From plane: " + form->picks("plane").dump())) return false;
        pass("construction plane: a click on the YZ origin plane fills From plane");
        design->escape();
        return true;
      },
      // ---- New sketch: the XY origin plane in the view, then Enter.
      [=] {
        view->standardView("iso");
        view->fitAll();
        design->startSketch();
        require(design->pickingPlane(), "New sketch did not ask for the plane");
        return true;
      },
      [=] {
        const auto at = find(candidate(xy));
        if (!waitFor(at.has_value(), "the XY origin plane is not in the view while New sketch asks for a plane")) return false;
        shot("sketch-planes");
        click(at, "the XY origin plane");
        return true;
      },
      [=] {
        if (!waitFor(design->planePicker()->positioning(), "the XY plane click did not reach the origin stage")) return false;
        design->planePanel()->grab().save(prefix + ".sketch-origin.png");
        QApplication::setActiveWindow(view->window());
        view->setFocus();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(view, &enter);
        return true;
      },
      [=] {
        if (!waitFor(design->sketchActive(), "Enter in the origin stage did not open the sketch")) return false;
        pass("new sketch: the XY plane clicked in the view, then Enter: the sketch is open");
        design->cancelSketch();
        return true;
      },
      // ---- The same with a value typed into Plane X: Enter there is OK with that origin.
      [=] {
        if (!waitFor(!design->sketchActive(), "the sketch did not close")) return false;
        design->startSketch();
        return true;
      },
      [=] {
        const auto at = find(candidate(xy));
        if (!waitFor(at.has_value(), "the XY origin plane is not in the view the second time")) return false;
        click(at, "the XY origin plane");
        return true;
      },
      [=] {
        if (!waitFor(design->planePicker()->positioning(), "the XY plane click did not reach the origin stage")) return false;
        const auto boxes = design->planePanel()->findChildren<QLineEdit*>();
        require(boxes.size() >= 2, "Plane X and Plane Y boxes");
        QLineEdit* x = boxes.front();
        x->setFocus();
        x->selectAll();
        for (const QString& key : {QString("5")}) {
          QKeyEvent press(QEvent::KeyPress, Qt::Key_5, Qt::NoModifier, key);
          QApplication::sendEvent(x, &press);
        }
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(x, &enter);
        return true;
      },
      [=] {
        if (!waitFor(design->sketchActive(), "Enter in Plane X did not open the sketch")) return false;
        const auto origin = design->sketch()->frame().origin;
        require(std::abs(origin[0] - 5) < 1e-9 && std::abs(origin[1]) < 1e-9, "the sketch origin is not the typed X 5 mm: " + std::to_string(origin[0]) + ", " + std::to_string(origin[1]));
        pass("new sketch: 5 typed into Plane X, then Enter there: the sketch is open with its origin at X 5 mm");
        design->cancelSketch();
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
      trace::log("bench: pick routing: all steps PASS");
      QCoreApplication::exit(0);
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: pick routing FAIL at step %1: %2").arg(st->step).arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
