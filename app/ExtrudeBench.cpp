#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "Theme.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/util.hpp"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <QApplication>
#include <QKeyEvent>
#include <QTimer>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <cmath>
#include <optional>
#include <set>

// OPAD_BENCH_EXTRUDE=<prefix> (case extrude in tools/bench_cases/design.py): the extrusion's automatic operation, its preview
// colours and To face / To all, through the panel and clicks in the view on a 30 x 20 x 10 box (x -15..15, y -10..10, z
// 0..10) with a sketch "Low" on XY under it (a 10 x 8 rectangle about the origin):
//   Extrude starts with Operation Automatic; the region clicked in the view is the profile and Profiles stays active (nothing
//   else is missing). 4 mm up from the box's bottom goes into it: "Automatic: Cut", the preview's tool drawn in the cut red
//   through the box (Topmost). Flipped, the extrusion leaves the box from the face it touches: "Automatic: Join", no red.
//   Operation New chosen: it sticks when the flip goes back into the box (no red tool); Automatic again: Cut.
//   Extent To face: Up to face takes the clicks at once; the box's top face clicked is the target, not a second profile, and
//   Profiles does not become active again; the red tool ends at the top face (z 10).
//   Extent All: the red tool ends where the box ends (z 10), not past the model; Enter commits a cut written as "cut" that
//   takes 10 x 8 x 10 out of the box.
// Then with the slab tilted about Y beside it (top face z = 35.59 - 0.5 (x - 60)) and the sketch "Side" under that (x 55..65):
//   Start from Face takes the clicks at once; the slab's top face clicked: Start at Nearest contact starts flat at z 33.09,
//   Farthest contact at 38.09, Follow face from the face (10 mm on from it everywhere, 1000 mm3); Sketch on face with a 2 mm
//   offset extrudes the profile projected onto the face's plane (894.4 mm3); Enter commits the derived sketch, its
//   construction plane and the extrusion from it, and one Undo takes all three back.
//   Extent To face up to the slab's top face: End at Nearest contact ends flat at z 33.09, Farthest contact at 38.09.
// Shots: <prefix>.cut.png, .join.png, .to-face.png, .all.png, .from-face.png, .sketch-on-face.png, .end-at.png (the view)
// and <prefix>.panel.png (the feature panel).
OPAD_BENCH(OPAD_BENCH_EXTRUDE, extrude) {
  struct State {
    size_t step = 0;
    int ticks = 0, wait = 0;
    std::string box, slab;  // the box, the tilted slab
    std::string low, side;  // the sketches under them
    int top = -1;           // the box's face facing +Z
    double volume = 0;
    size_t sketches = 0, features = 0;
  };
  auto st = std::make_shared<State>();
  for (const auto& f : w.m_doc->scene.features)
    for (const auto& body : f.result.value("bodies", opad::json::array()))
      if (f.kind == "box") (st->box.empty() ? st->box : st->slab) = body.value("id", "");
  for (const auto& sk : w.m_doc->scene.sketches) (sk.name == "Side" ? st->side : st->low) = sk.id;
  if (st->box.empty() || st->slab.empty() || st->low.empty() || st->side.empty()) {
    trace::log("bench: extrude FAIL: the document needs a box, a tilted slab and the sketches Low and Side");
    QCoreApplication::exit(2);
    return true;
  }
  auto boxShape = [w = &w, st] { return opad::node_world_shape(w->m_doc->doc, w->m_doc->scene, st->box); };
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(boxShape(), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const opad::json d = opad::describe_entity(faces(i));
      if (d.contains("normal") && d["normal"][2].get<double>() > 0.99) st->top = i - 1;
    }
  }
  w.setWorkspace("design");
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  Viewport* view = w.m_viewport;
  const QString prefix = value;
  MainWindow* win = &w;

  using Want = std::function<bool(const std::string&, const opad::Ref&)>;
  auto require = [](bool ok, const std::string& why) {
    if (!ok) throw opad::Error(why);
  };
  auto pass = [](const QString& what) { trace::log("bench: extrude: " + what + " PASS"); };
  auto find = [view](const Want& want, bool inside = true) -> std::optional<QPoint> {
    int x = 0, y = 0;
    if (!view->benchPickPoint(want, x, y, inside)) return std::nullopt;
    return QPoint(x, y);
  };
  auto click = [view, require](const std::optional<QPoint>& at, const std::string& what) {
    require(at.has_value(), what + " is not found in the view");
    view->benchClickAt(at->x(), at->y());
  };
  auto count = [form](const char* input) {
    const opad::json p = form->picks(input);
    return p.is_array() ? int(p.size()) : p.is_null() ? 0 : 1;
  };
  auto shot = [view, prefix](const char* name) { view->grabImage().save(prefix + "." + name + ".png"); };
  auto waitFor = [st](bool ok, const std::string& why) {
    if (ok) return st->wait = 0, true;
    if (++st->wait > 80) throw opad::Error(why);
    return false;
  };
  // The preview on screen is for what the panel holds now.
  auto ready = [design, form] {
    if (design->previewing() || !design->readyPreview()) return false;
    opad::json shown = design->readyPreviewInputs(), now = form->inputs();
    return shown == now;
  };
  // The planned tool of the preview: its operation and how high it reaches.
  auto tool = [design]() -> std::pair<std::string, double> {
    const auto plan = design->readyPreview();
    if (!plan) return {"", 0};
    for (const auto& t : plan->tools)
      if (t.shape && !t.shape->IsNull()) {
        Bnd_Box b;
        BRepBndLib::AddOptimal(*t.shape, b, Standard_False, Standard_False);
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        return {t.operation, z1};
      }
    return {"", 0};
  };
  // Whether the view draws a preview part in the cut colour through the model.
  auto redTool = [view] {
    for (const auto& [colour, xray] : view->previewLooks())
      if (xray && colour == theme::current().previewCut) return true;
    return false;
  };
  auto volume = [w = &w] {
    double v = 0;
    for (const auto& id : w->m_doc->scene.all_bodies()) {
      GProp_GProps g;
      BRepGProp::VolumeProperties(opad::node_world_shape(w->m_doc->doc, w->m_doc->scene, id), g);
      v += g.Mass();
    }
    return v;
  };
  auto topFace = [st](const std::string& c, const opad::Ref& r) { return c.empty() && r.body == st->box && r.kind == opad::Ref::Kind::Face && r.index == st->top; };
  // The slab's tilted top face, by its normal (ordinals may change).
  auto slabTop = [win, st](const std::string& c, const opad::Ref& r) {
    if (!c.empty() || r.body != st->slab || r.kind != opad::Ref::Kind::Face) return false;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(opad::node_world_shape(win->m_doc->doc, win->m_doc->scene, st->slab), TopAbs_FACE, faces);
    if (r.index < 0 || r.index >= faces.Extent()) return false;
    const opad::json d = opad::describe_entity(faces(r.index + 1));
    return d.contains("normal") && d["normal"][2].get<double>() > 0.8 && d["normal"][0].get<double>() > 0.3;
  };
  auto region = [](const std::string& sketch) {
    return [sketch](const std::string& c, const opad::Ref&) { return c.find("\"at\"") != std::string::npos && c.find(sketch) != std::string::npos; };
  };
  // The planned tool's volume and how low and high it reaches.
  struct Span {
    double volume = 0, low = 0, high = 0;
  };
  auto span = [design]() {
    Span out;
    const auto plan = design->readyPreview();
    if (!plan) return out;
    for (const auto& t : plan->tools)
      if (t.shape && !t.shape->IsNull()) {
        GProp_GProps g;
        BRepGProp::VolumeProperties(*t.shape, g);
        out.volume = g.Mass();
        Bnd_Box b;
        BRepBndLib::AddOptimal(*t.shape, b, Standard_False, Standard_False);
        double x0, y0, x1, y1;
        b.Get(x0, y0, out.low, x1, y1, out.high);
      }
    return out;
  };
  auto close = [](double a, double b) { return std::abs(a - b) < 2e-3; };
  auto num = [](double v) { return std::to_string(v); };
  const double nearest = 35.5902 - 2.5, farthest = 35.5902 + 2.5;  // the slab's top over x 55..65

  std::vector<std::function<bool()>> steps = {
      [=] {
        st->volume = volume();
        view->standardView("iso");
        view->fitAll();
        design->startFeature("extrude");
        require(form->inputs().value("operation", "") == "auto" && form->choiceText("operation") == "Automatic", "Extrude does not start with Operation Automatic: " + form->choiceText("operation").toStdString());
        require(form->activeInput() == "profiles", "Extrude starts on Profiles");
        return true;
      },
      [=] {
        const auto at = find(region(st->low));
        if (!waitFor(at.has_value(), "the sketch's region is not shown in the view")) return false;
        click(at, "the sketch's region");
        return true;
      },
      [=] {
        if (!waitFor(count("profiles") == 1, "the region click did not pick the profile")) return false;
        require(form->activeInput() == "profiles", "Profiles stays active while nothing else is missing");
        pass("Extrude starts with Operation Automatic; the region clicked is the profile, Profiles stays active");
        form->setValue("distance", "4 mm");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview of the extrusion into the box")) return false;
        require(form->suggestion("operation") == "cut" && form->choiceText("operation") == "Automatic: Cut", "into the box is not an automatic cut: " + form->choiceText("operation").toStdString());
        require(tool().first == "cut" && std::abs(tool().second - 4) < 1e-3, "the preview's tool is not the 4 mm cut");
        require(redTool(), "the cut's tool is not drawn in the cut colour through the box");
        shot("cut");
        if (auto* panel = form->window()) panel->grab().save(prefix + ".panel.png");
        pass("into the box: Automatic: Cut, the tool drawn red through the box");
        form->setValue("flip", true);
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview of the flipped extrusion")) return false;
        require(form->suggestion("operation") == "join" && form->choiceText("operation") == "Automatic: Join", "away from the face it touches is not an automatic join: " + form->choiceText("operation").toStdString());
        require(!redTool(), "a join shows a red tool");
        shot("join");
        pass("flipped, away from the box's bottom: Automatic: Join, nothing red");
        form->setValue("operation", "new");
        form->setValue("flip", false);
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview after choosing New")) return false;
        require(form->inputs().value("operation", "") == "new" && tool().first == "new", "the chosen New did not stick: " + tool().first);
        require(!redTool(), "a new body shows a red tool");
        pass("Operation New chosen sticks when the extrusion goes back into the box");
        form->setValue("operation", "auto");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview after choosing Automatic again")) return false;
        require(form->suggestion("operation") == "cut" && redTool(), "Automatic again is not a cut");
        pass("Automatic chosen again: Cut");
        form->setValue("extent", "to_face");
        require(form->activeInput() == "extent_face", "Extent To face did not make Up to face the active input: " + form->activeInput().toStdString());
        pass("Extent To face: Up to face takes the clicks at once");
        return true;
      },
      [=] {
        const auto at = view->selectionFilter() == Viewport::SelFilter::Face ? find(topFace) : std::nullopt;
        if (!waitFor(at.has_value(), "the box's top face is not found in the view")) return false;
        click(at, "the box's top face");
        return true;
      },
      [=] {
        if (!waitFor(count("extent_face") == 1, "the top face click did not fill Up to face")) return false;
        require(count("profiles") == 1, "the face clicked for Up to face became a profile");
        require(form->activeInput() != "profiles", "Profiles became active again after the to-face pick");
        pass("the face clicked is the target, not a profile; the clicks do not go back to Profiles");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview up to the face")) return false;
        require(tool().first == "cut" && std::abs(tool().second - 10) < 1e-3, "the to-face tool does not end at the top face: " + std::to_string(tool().second));
        require(redTool(), "the to-face cut is not drawn red");
        shot("to-face");
        pass("To face: the red tool ends at the picked face");
        form->setValue("extent", "all");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview through all")) return false;
        require(tool().first == "cut" && std::abs(tool().second - 10) < 1e-3, "the To all tool does not end where the box ends: " + std::to_string(tool().second));
        require(redTool(), "the To all cut is not drawn red");
        shot("all");
        pass("To all: the red tool ends at the last body in the way");
        QApplication::setActiveWindow(view->window());
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(view, &enter);
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive(), "Enter did not commit the extrusion")) return false;
        const opad::Feature& f = win->m_doc->scene.features.back();
        require(f.kind == "extrude" && f.error.empty() && f.inputs.value("operation", "") == "cut" && f.inputs.value("extent", "") == "all", "the committed extrusion is not a cut through all: " + f.inputs.dump());
        require(std::abs(st->volume - volume() - 800) < 1e-3, "the cut did not take 10 x 8 x 10 out: " + std::to_string(st->volume - volume()));
        pass("Enter commits the automatic cut through all, written as \"cut\"");
        return true;
      },
      // ---- Start from a tilted face.
      [=] {
        view->standardView("iso");
        view->fitAll();
        design->startFeature("extrude");
        return true;
      },
      [=] {
        const auto at = find(region(st->side));
        if (!waitFor(at.has_value(), "the Side sketch's region is not shown in the view")) return false;
        click(at, "the Side sketch's region");
        return true;
      },
      [=] {
        if (!waitFor(count("profiles") == 1, "the Side region click did not pick the profile")) return false;
        form->setValue("start", "face");
        require(form->activeInput() == "start_face", "Start from Face did not make Start face the active input: " + form->activeInput().toStdString());
        pass("Start from Face: Start face takes the clicks at once");
        return true;
      },
      [=] {
        const auto at = view->selectionFilter() == Viewport::SelFilter::Face ? find(slabTop) : std::nullopt;
        if (!waitFor(at.has_value(), "the slab's top face is not found in the view")) return false;
        click(at, "the slab's top face");
        return true;
      },
      [=] {
        if (!waitFor(count("start_face") == 1, "the slab click did not fill Start face")) return false;
        require(count("profiles") == 1 && form->activeInput() != "profiles", "the start face click went to Profiles");
        form->setValues({{"distance", "10 mm"}, {"start_shape", "nearest_contact"}});
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview from the nearest contact")) return false;
        require(close(span().low, nearest), "Nearest contact does not start at z " + num(nearest) + ": " + num(span().low));
        pass("Start at Nearest contact: flat where the extrusion first meets the tilted face");
        form->setValue("start_shape", "farthest_contact");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview from the farthest contact")) return false;
        require(close(span().low, farthest) && close(span().high, farthest + 10), "Farthest contact does not start at z " + num(farthest) + ": " + num(span().low));
        pass("Start at Farthest contact: flat where all of the profile has met the face");
        form->setValue("start_shape", "follow_face");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview following the face")) return false;
        const Span s = span();
        require(close(s.low, nearest) && close(s.high, farthest + 10) && std::abs(s.volume - 1000) < 1e-2, "Follow face is not the profile swept 10 mm on from the face: " + num(s.low) + ".." + num(s.high) + ", " + num(s.volume));
        shot("from-face");
        pass("Start at Follow face: 10 mm on from the tilted face everywhere");
        form->setValues({{"start_shape", "sketch_on_face"}, {"face_offset", "2 mm"}});
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview of the profile sketched on the face")) return false;
        require(std::abs(span().volume - 1000 / std::sqrt(1.25)) < 1e-2, "Sketch on face does not extrude the projected profile: " + num(span().volume));
        shot("sketch-on-face");
        pass("Start at Sketch on face: the profile projected onto the face's plane, 2 mm off it, extruded along its normal");
        st->sketches = win->m_doc->scene.sketches.size();
        st->features = win->m_doc->scene.features.size();
        QApplication::setActiveWindow(view->window());
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(view, &enter);
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive(), "Enter did not commit the extrusion sketched on the face")) return false;
        const auto& scene = win->m_doc->scene;
        require(scene.sketches.size() == st->sketches + 1 && scene.sketches.back().name.find("(derived)") != std::string::npos, "no derived sketch was made");
        require(scene.features.size() == st->features + 2 && scene.features[scene.features.size() - 2].kind == "plane", "no construction plane before the extrusion");
        const opad::Feature& f = scene.features.back();
        require(f.kind == "extrude" && f.error.empty() && f.inputs.value("start", "") == "profile", "the extrusion does not start from the derived sketch: " + f.inputs.dump());
        pass("Enter commits the derived sketch, its construction plane and the extrusion from it");
        win->m_doc->undo();
        return true;
      },
      [=] {
        const auto& scene = win->m_doc->scene;
        if (!waitFor(scene.sketches.size() == st->sketches && scene.features.size() == st->features, "one Undo did not take the derived sketch, the plane and the extrusion back")) return false;
        pass("one Undo takes all three back");
        view->standardView("iso");
        view->fitAll();
        design->startFeature("extrude");
        return true;
      },
      // ---- End at, up to the tilted face.
      [=] {
        const auto at = find(region(st->side));
        if (!waitFor(at.has_value(), "the Side sketch's region is not shown in the view")) return false;
        click(at, "the Side sketch's region");
        return true;
      },
      [=] {
        if (!waitFor(count("profiles") == 1, "the Side region click did not pick the profile")) return false;
        form->setValue("extent", "to_face");
        require(form->activeInput() == "extent_face", "To face did not make Up to face active");
        return true;
      },
      [=] {
        const auto at = view->selectionFilter() == Viewport::SelFilter::Face ? find(slabTop) : std::nullopt;
        if (!waitFor(at.has_value(), "the slab's top face is not found in the view")) return false;
        click(at, "the slab's top face");
        return true;
      },
      [=] {
        if (!waitFor(count("extent_face") == 1, "the slab click did not fill Up to face")) return false;
        form->setValue("extent_end", "nearest_contact");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview ending at the nearest contact")) return false;
        require(close(span().high, nearest) && close(span().low, 0), "End at Nearest contact does not end at z " + num(nearest) + ": " + num(span().high));
        pass("End at Nearest contact: flat where the extrusion first touches the tilted face");
        form->setValue("extent_end", "farthest_contact");
        return true;
      },
      [=] {
        if (!waitFor(ready(), "no preview ending at the farthest contact")) return false;
        require(close(span().high, farthest), "End at Farthest contact does not end at z " + num(farthest) + ": " + num(span().high));
        shot("end-at");
        pass("End at Farthest contact: flat where all of it has reached the face");
        emit form->cancelled();
        return true;
      },
      [=] { return waitFor(!design->featureActive(), "Cancel did not close the extrusion"); },
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
      trace::log("bench: extrude: all steps PASS");
      QCoreApplication::exit(0);
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: extrude FAIL at step %1: %2").arg(st->step).arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
