#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "MainWindow.hpp"
#include "ToolValues.hpp"
#include "TranslateTriad.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/util.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>

#include <cmath>
#include <optional>
#include <set>

// OPAD_BENCH_HANDLES=<prefix> (TODO 11 P2, case handles in tools/bench_cases/design.py): the value arrows and Move's triad
// the guides draw, driven through the view's mouse events on a 30 x 20 x 10 box centred on the origin:
//   Press pull: the top face clicked; the arrow stands on the face's centre, 5 mm up; pulled down past the face while the
//   button is held, the distance goes negative and the preview on screen is the box made lower; Enter commits it.
//   Fillet: two top edges clicked; the arrow sits half way along the first, out between the top and the side; pulled out,
//   the radius grows and the preview (button still held) is the rounded box with less volume; 3 typed over the view goes
//   into the arrow's box; Enter commits a 3 mm fillet.
//   Move: the box clicked; the triad stands on its middle; the X arrow pulled: X grows, the triad and the preview follow
//   while the button is held, the boxes by the pointer show the values; 1 and 4 typed go into X; Enter moves the box 14 mm.
//   Move with Rotate: a ring round Z through the box's middle, the triad on it 90 degrees round; pulled round, the angle
//   goes on in 5 degree steps and the triad and the preview travel along it while held; 4 and 5 typed go into Angle; Enter
//   turns the box 45 degrees.
//   Move with grid snapping on: the X arrow pulled 1.3 grid steps sets X to one step; with Alt held the pull is off the grid.
//   Chamfer (a bottom edge), Thicken (the top face), Construction plane (offset from XY, then from the top face) and Box: the
//   arrow on Distance, Thickness, Distance and Height is where the core's feature_handles puts it (from a face: on its middle);
//   pulled out, the value grows and, the button still held, the preview cuts more, is thicker, plans the plane at the
//   distance, is 20 x 20 x the height; Esc leaves.
// Shots: <prefix>.<step>.png (the view: arrows and triad are drawn in it) and <prefix>.<step>.box.png (the value box).
OPAD_BENCH(OPAD_BENCH_HANDLES, handles) {
  struct State {
    size_t step = 0;
    int ticks = 0, wait = 0;
    std::string box;
    double volume = 0, previewBefore = 0, pxPerMm = 0, top = 10, grid = 0, before = 0;
    QPointF press, dir, last;
    opad::Vec3 centre{0, 0, 0};
  };
  auto st = std::make_shared<State>();
  for (const auto& f : w.m_doc->scene.features)
    for (const auto& body : f.result.value("bodies", opad::json::array()))
      if (f.kind == "box") st->box = body.value("id", "");
  if (st->box.empty()) {
    trace::log("bench: handles FAIL: the document needs a box");
    QCoreApplication::exit(2);
    return true;
  }
  w.setWorkspace("design");
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  DimensionHandle* handle = design->distanceHandle();
  Viewport* view = w.m_viewport;
  MainWindow* win = &w;
  const QString prefix = value;

  auto require = [](bool ok, const std::string& why) {
    if (!ok) throw opad::Error(why);
  };
  auto pass = [](const QString& what) { trace::log("bench: handles: " + what + " PASS"); };
  auto shape = [w = &w, st] { return opad::node_world_shape(w->m_doc->doc, w->m_doc->scene, st->box); };
  auto volumeOf = [](const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
  };
  // The preview on screen: the volume of what it shows, and the inputs it was made for.
  auto previewVolume = [design, volumeOf] {
    double v = 0;
    if (const auto plan = design->readyPreview())
      for (const auto& c : plan->changed)
        if (!c.removed && c.shape && !c.shape->IsNull()) v += volumeOf(*c.shape);
    return v;
  };
  auto evaluated = [form](const char* input) {  // a length input as the feature gets it, in mm
    return opad::design::ParamTable().length(form->inputs().value(input, std::string("0")));
  };
  auto previewFor = [design, form](const char* input) {
    const opad::json in = design->readyPreviewInputs();
    return in.is_object() && in.contains(input) && in[input] == form->inputs().value(input, opad::json());
  };
  // Faces and edges of the box by where they are (ordinals change as features change it).
  auto topFace = [shape, st]() {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape(), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      BRepAdaptor_Surface s(TopoDS::Face(faces(i)));
      if (s.GetType() == GeomAbs_Plane && std::abs(s.Plane().Axis().Direction().Z()) > 0.99 && s.Plane().Location().Z() > st->top - 1e-6) return i - 1;
    }
    return -1;
  };
  auto edgesAt = [shape](double z) {  // straight edges lying at height z
    std::set<int> out;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape(), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
      BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
      if (c.GetType() == GeomAbs_Line && std::abs(c.Value(c.FirstParameter()).Z() - z) < 1e-6 && std::abs(c.Value(c.LastParameter()).Z() - z) < 1e-6) out.insert(i - 1);
    }
    return out;
  };
  auto topEdges = [edgesAt, st]() { return edgesAt(st->top); };
  auto edgeMiddle = [shape](int index) {
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape(), TopAbs_EDGE, edges);
    BRepAdaptor_Curve c(TopoDS::Edge(edges(index + 1)));
    const gp_Pnt p = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
    return opad::Vec3{p.X(), p.Y(), p.Z()};
  };
  using Want = std::function<bool(const std::string&, const opad::Ref&)>;
  auto find = [view](const Want& want, bool inside = true) -> std::optional<QPoint> {  // an edge has no inside
    int x = 0, y = 0;
    if (!view->benchPickPoint(want, x, y, inside)) return std::nullopt;
    return QPoint(x, y);
  };
  auto entity = [st](opad::Ref::Kind kind, std::set<int> among) -> Want {
    return [st, kind, among](const std::string& c, const opad::Ref& r) { return c.empty() && r.body == st->box && r.kind == kind && among.count(r.index); };
  };
  auto click = [view, require](const std::optional<QPoint>& at, const std::string& what) {
    require(at.has_value(), what + " is not found in the view");
    view->benchClickAt(at->x(), at->y());
  };
  // The pointer through the viewport's event filters (the arrow's, the triad's), in widget coordinates.
  auto mouse = [view](QEvent::Type type, const QPointF& at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, view->mapToGlobal(at), button, buttons, modifiers);
    QApplication::sendEvent(view, &e);
  };
  auto key = [view](int code, const QString& text) {
    QApplication::setActiveWindow(view->window());
    QWidget* to = QApplication::focusWidget();
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
    QApplication::sendEvent(to ? to : static_cast<QWidget*>(view), &press);
  };
  // Screen pixels per mm along a direction at a point.
  auto pixelsPerMm = [view](const opad::Vec3& at, const opad::Vec3& dir) {
    const QPointF a = view->widgetPoint(at), b = view->widgetPoint({at[0] + dir[0], at[1] + dir[1], at[2] + dir[2]});
    return std::hypot(b.x() - a.x(), b.y() - a.y());
  };
  auto start = [view, design](const char* kind) {
    view->standardView("iso");
    view->fitAll();
    design->startFeature(kind);
  };
  auto shot = [view, handle, prefix](const QString& name, bool box) {
    view->grabImage().save(prefix + "." + name + ".png");
    if (box) handle->grab().save(prefix + "." + name + ".box.png");
  };
  auto waitFor = [st](bool ok, const std::string& why) {
    if (ok) return st->wait = 0, true;
    if (++st->wait > 80) throw opad::Error(why);
    return false;
  };
  auto closeTo = [](const QPointF& a, const QPointF& b, double px) { return std::hypot(a.x() - b.x(), a.y() - b.y()) <= px; };
  auto str = [](const QPointF& p) { return "(" + std::to_string(p.x()) + ", " + std::to_string(p.y()) + ")"; };

  std::vector<std::function<bool()>> steps = {
      // ---- Press pull: the top face, then its arrow pulled down past the face.
      [=] {
        st->volume = volumeOf(shape());
        start("offset_face");
        return true;
      },
      [=] {
        const bool faces = view->selectionFilter() == Viewport::SelFilter::Face;
        const auto at = faces ? find(entity(opad::Ref::Kind::Face, {topFace()})) : std::nullopt;
        if (!waitFor(at.has_value(), faces ? "the box's top face is not found in the view" : "press pull's Faces switch the view to faces")) return false;
        click(at, "the box's top face");
        return true;
      },
      [=] {
        if (!waitFor(design->handleInput() == "distance" && previewFor("distance"), "press pull shows no arrow on Distance")) return false;
        const opad::Vec3 foot{0, 0, st->top + 5};  // the face's centre, 5 mm (the default) up
        require(closeTo(handle->arrowLine().p1(), view->widgetPoint(foot), 4), "the press pull arrow is not on the face's centre 5 mm up: " + str(handle->arrowLine().p1()) + " vs " + str(view->widgetPoint(foot)));
        pass("press pull: the arrow stands on the picked face's centre at the distance");
        st->pxPerMm = pixelsPerMm(foot, {0, 0, 1});
        const QLineF line = handle->arrowLine();
        st->dir = (line.p2() - line.p1()) / std::max(1e-9, line.length());
        st->press = line.pointAt(0.5);
        mouse(QEvent::MouseButtonPress, st->press);
        require(handle->dragging(), "a press on the press pull arrow did not grip it");
        st->last = st->press - st->dir * (8 * st->pxPerMm);  // 8 mm down: 5 -> about -3
        mouse(QEvent::MouseMove, st->last);
        require(evaluated("distance") < 0, "pulling the arrow down did not make the distance negative: " + form->valueText("distance").toStdString());
        return true;
      },
      [=] {
        if (!waitFor(previewFor("distance"), "the press pull preview did not follow the pull")) return false;
        require(handle->dragging(), "the arrow let go before the release");
        require(previewVolume() < st->volume - 1, "the preview during the pull is not the lowered box: " + std::to_string(previewVolume()) + " of " + std::to_string(st->volume));
        shot("presspull", true);
        pass("press pull: pulled below the face while held, the distance is " + form->valueText("distance") + " and the preview is the lowered box");
        mouse(QEvent::MouseButtonRelease, st->last);
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive() && !win->m_doc->scene.features.empty() && win->m_doc->scene.features.back().kind == "offset_face", "Enter did not commit the press pull")) return false;
        const double d = win->m_doc->scene.features.back().inputs.contains("distance") ? opad::design::ParamTable().length(win->m_doc->scene.features.back().inputs["distance"].get<std::string>()) : 0;
        require(d < 0 && std::abs(volumeOf(shape()) - st->volume * (10 + d) / 10) < 1e-3, "the committed press pull is not the pulled distance");
        st->top = 10 + d;
        pass("press pull: Enter commits the pulled distance");
        return true;
      },
      // ---- Fillet: two top edges, the arrow on the first pulled out.
      [=] {
        st->volume = volumeOf(shape());
        start("fillet");
        return true;
      },
      // (Found in the view on a later tick too: on a loaded machine the view's picker may not find an edge at once.)
      [=] {
        const bool edges = view->selectionFilter() == Viewport::SelFilter::Edge;
        const auto at = edges ? find(entity(opad::Ref::Kind::Edge, topEdges()), false) : std::nullopt;
        if (!waitFor(at.has_value(), edges ? "a top edge is not found in the view" : "fillet's Edges switch the view to edges")) return false;
        click(at, "a top edge");
        return true;
      },
      [=] {
        const opad::json picks = form->picks("edges");
        const bool one = picks.is_array() && picks.size() == 1;
        std::set<int> others = topEdges();
        if (one) others.erase(picks[0].value("index", -1));
        const auto at = one ? find(entity(opad::Ref::Kind::Edge, others), false) : std::nullopt;
        if (!waitFor(at.has_value(), one ? "another top edge is not found in the view" : "the edge click did not pick it")) return false;
        click(at, "another top edge");
        return true;
      },
      [=] {
        const opad::json picks = form->picks("edges");
        if (!waitFor(picks.is_array() && picks.size() == 2 && design->handleInput() == "radius" && previewFor("radius"), "fillet shows no arrow on Radius")) return false;
        // Out between the top (up) and the side the edge is on (away from the middle), 2 mm (the default) from the edge.
        const opad::Vec3 mid = edgeMiddle(picks[0].value("index", -1));
        opad::Vec3 side{std::abs(mid[0]) > 14 ? (mid[0] > 0 ? 1.0 : -1.0) : 0.0, std::abs(mid[1]) > 9 ? (mid[1] > 0 ? 1.0 : -1.0) : 0.0, 0};
        const opad::Vec3 out{side[0] / std::sqrt(2.0), side[1] / std::sqrt(2.0), 1 / std::sqrt(2.0)};
        const opad::Vec3 foot{mid[0] + out[0] * 2, mid[1] + out[1] * 2, mid[2] + out[2] * 2};
        require(closeTo(handle->arrowLine().p1(), view->widgetPoint(foot), 4), "the fillet arrow is not on the first edge, out between its faces: " + str(handle->arrowLine().p1()) + " vs " + str(view->widgetPoint(foot)));
        pass("fillet: the arrow sits half way along the first picked edge, pointing out between its two faces");
        st->previewBefore = previewVolume();
        st->pxPerMm = pixelsPerMm(foot, out);
        const QLineF line = handle->arrowLine();
        st->dir = (line.p2() - line.p1()) / std::max(1e-9, line.length());
        st->press = line.pointAt(0.5);
        mouse(QEvent::MouseButtonPress, st->press);
        require(handle->dragging(), "a press on the fillet arrow did not grip it");
        st->last = st->press + st->dir * (1.5 * st->pxPerMm);  // 1.5 mm out: 2 -> about 3.5
        mouse(QEvent::MouseMove, st->last);
        require(evaluated("radius") > 2.5, "pulling the fillet arrow out did not grow the radius: " + form->valueText("radius").toStdString());
        return true;
      },
      [=] {
        if (!waitFor(previewFor("radius"), "the fillet preview did not follow the pull")) return false;
        require(handle->dragging(), "the fillet arrow let go before the release");
        require(previewVolume() < st->previewBefore - 1e-3, "the preview during the pull is not the bigger fillet: " + std::to_string(previewVolume()) + " vs " + std::to_string(st->previewBefore));
        shot("fillet", true);
        pass("fillet: pulled out while held, the radius is " + form->valueText("radius") + " and the preview is the rounder box");
        mouse(QEvent::MouseButtonRelease, st->last);
        view->setFocus();
        key(Qt::Key_3, "3");
        return true;
      },
      [=] {
        if (!waitFor(std::abs(evaluated("radius") - 3) < 1e-9, "3 typed over the view did not go into the fillet arrow's box: " + form->valueText("radius").toStdString())) return false;
        auto* box = handle->findChild<QLineEdit*>();
        require(box && box->text().startsWith("3"), "the arrow's box does not show the typed 3");
        pass("fillet: 3 typed over the view goes into the arrow's box");
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive() && win->m_doc->scene.features.back().kind == "fillet", "Enter did not commit the fillet")) return false;
        require(opad::design::ParamTable().length(win->m_doc->scene.features.back().inputs.value("radius", std::string())) == 3, "the committed fillet is not 3 mm");
        require(!handle->isVisible(), "the arrow stayed after the commit");
        pass("fillet: Enter commits the typed radius; the arrow goes");
        return true;
      },
      // ---- Move: the box, then its X arrow pulled.
      [=] {
        start("move");
        return true;
      },
      [=] {
        if (!waitFor(view->selectionFilter() == Viewport::SelFilter::Body, "Move's Bodies switch the view to bodies")) return false;
        int x = 0, y = 0;
        require(view->benchBodyPoint(st->box, x, y), "the box is not in the view");
        view->benchClickAt(x, y);
        return true;
      },
      [=] {
        TranslateTriad* triad = design->moveTriad();
        if (!waitFor(triad && triad->shown(), "Move shows no triad on the picked box")) return false;
        const Bnd_Box box = opad::node_world_bbox(win->m_doc->doc, win->m_doc->scene, st->box);
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        st->centre = {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2};
        require(closeTo(triad->partPoint(0), view->widgetPoint(st->centre), 3), "the triad is not on the box's middle");
        require(triad->arrows().size() == 3, "the triad has no X, Y and Z arrows");
        shot("move-triad", false);
        pass("move: the triad stands on the picked body with X, Y and Z arrows");
        st->pxPerMm = pixelsPerMm(st->centre, {1, 0, 0});
        st->press = triad->partPoint(1);
        st->dir = (triad->arrowTip(1) - triad->partPoint(0));
        st->dir /= std::max(1e-9, std::hypot(st->dir.x(), st->dir.y()));
        require(triad->partAt(st->press) == 1, "the X arrow is not where the triad says");
        mouse(QEvent::MouseButtonPress, st->press);
        require(triad->dragging() == 1, "a press on the X arrow did not grip it");
        st->last = st->press + st->dir * (10 * st->pxPerMm);  // 10 mm along X
        mouse(QEvent::MouseMove, st->last);
        require(evaluated("dx") > 5 && std::abs(evaluated("dy")) < 1e-9 && std::abs(evaluated("dz")) < 1e-9, "pulling the X arrow did not set X alone: " + form->valueText("dx").toStdString());
        require(design->values()->input()->isVisible(), "the value boxes do not show during the pull");
        return true;
      },
      [=] {
        if (!waitFor(previewFor("dx"), "the move preview did not follow the pull")) return false;
        TranslateTriad* triad = design->moveTriad();
        require(triad->dragging() == 1, "the X arrow let go before the release");
        const double dx = evaluated("dx");
        require(closeTo(triad->partPoint(0), view->widgetPoint({st->centre[0] + dx, st->centre[1], st->centre[2]}), 3), "the triad did not follow the pull");
        Bnd_Box moved;
        for (const auto& c : design->readyPreview()->changed)
          if (c.shape && !c.shape->IsNull()) BRepBndLib::Add(*c.shape, moved);
        double x0, y0, z0, x1, y1, z1;
        moved.Get(x0, y0, z0, x1, y1, z1);
        require(std::abs((x0 + x1) / 2 - st->centre[0] - dx) < 0.5, "the preview during the pull is not the box moved by X");
        shot("move", false);
        design->values()->input()->shot().save(prefix + ".move.box.png");
        pass("move: pulled along X while held, X is " + form->valueText("dx") + "; the triad and the preview follow; the boxes show the values");
        mouse(QEvent::MouseButtonRelease, st->last);
        view->setFocus();
        key(Qt::Key_1, "1");
        key(Qt::Key_4, "4");
        return true;
      },
      [=] {
        if (!waitFor(std::abs(evaluated("dx") - 14) < 1e-9, "1 and 4 typed after the pull did not go into X: " + form->valueText("dx").toStdString())) return false;
        pass("move: 1 and 4 typed after pulling X go into the X box");
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive() && win->m_doc->scene.features.back().kind == "move", "Enter did not commit the move")) return false;
        const Bnd_Box box = opad::node_world_bbox(win->m_doc->doc, win->m_doc->scene, st->box);
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        require(std::abs((x0 + x1) / 2 - st->centre[0] - 14) < 0.5, "the box did not move 14 mm along X");
        require(!design->moveTriad()->shown(), "the triad stayed after the commit");
        pass("move: Enter moves the box 14 mm along X; the triad goes");
        return true;
      },
      // ---- Move with grid snapping on: the X arrow pulled 1.3 grid steps lands on one step, with Alt held off the grid.
      [=] {
        start("move");
        return true;
      },
      [=] {
        if (!waitFor(view->selectionFilter() == Viewport::SelFilter::Body, "Move's Bodies switch the view to bodies")) return false;
        int x = 0, y = 0;
        require(view->benchBodyPoint(st->box, x, y), "the box is not in the view");
        view->benchClickAt(x, y);
        return true;
      },
      [=] {
        TranslateTriad* triad = design->moveTriad();
        if (!waitFor(triad && triad->shown(), "Move shows no triad on the picked box")) return false;
        view->setGridSnap(true);
        st->grid = view->gridStep();
        require(st->grid > 0, "the view has no grid step");
        const opad::Vec3 at{st->centre[0] + 14, st->centre[1], st->centre[2]};
        st->pxPerMm = pixelsPerMm(at, {1, 0, 0});
        st->press = triad->partPoint(1);
        st->dir = (triad->arrowTip(1) - triad->partPoint(0));
        st->dir /= std::max(1e-9, std::hypot(st->dir.x(), st->dir.y()));
        mouse(QEvent::MouseButtonPress, st->press);
        require(triad->dragging() == 1, "a press on the X arrow did not grip it");
        mouse(QEvent::MouseMove, st->press + st->dir * (0.6 * st->grid * st->pxPerMm));
        st->last = st->press + st->dir * (1.3 * st->grid * st->pxPerMm);
        mouse(QEvent::MouseMove, st->last);
        require(std::abs(evaluated("dx") - st->grid) < 1e-9, "with grid snapping on, 1.3 grid steps along X did not give one step (" + std::to_string(st->grid) + " mm): " + form->valueText("dx").toStdString());
        pass("move: with grid snapping on, the X arrow pulled 1.3 grid steps sets X to one step, " + form->valueText("dx"));
        mouse(QEvent::MouseMove, st->last + st->dir * 0.5, Qt::AltModifier);
        const double free = evaluated("dx") / st->grid;
        require(std::abs(free - std::round(free)) > 0.1, "with Alt held the pull still went in grid steps: " + form->valueText("dx").toStdString());
        pass("move: with Alt held the same pull is off the grid, " + form->valueText("dx"));
        return true;
      },
      [=] {
        if (!waitFor(previewFor("dx"), "the move preview did not follow the pull with grid snapping on")) return false;
        mouse(QEvent::MouseButtonRelease, st->last);
        view->setGridSnap(false);
        design->escape();
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive(), "Esc did not leave the move")) return false;
        return true;
      },
      // ---- Move with Rotate: the ring round Z through the box's middle (14 mm out), pulled round, then 45 typed.
      [=] {
        start("move");
        return true;
      },
      [=] {
        if (!waitFor(view->selectionFilter() == Viewport::SelFilter::Body, "Move's Bodies switch the view to bodies")) return false;
        int x = 0, y = 0;
        require(view->benchBodyPoint(st->box, x, y), "the box is not in the view");
        view->benchClickAt(x, y);
        return true;
      },
      [=] {
        TranslateTriad* triad = design->moveTriad();
        if (!waitFor(triad && triad->shown(), "Move shows no triad on the picked box")) return false;
        require(!triad->ringShown(), "a ring shows with Rotate off");
        form->setValue("rotate", true);
        return true;
      },
      [=] {
        TranslateTriad* triad = design->moveTriad();
        if (!waitFor(triad->ringShown() && previewFor("angle"), "Rotate on shows no ring")) return false;
        // Turned 90 degrees (the default) about Z: the triad on the ring at (0, 14), the box's middle turned.
        require(closeTo(triad->partPoint(0), view->widgetPoint({-st->centre[1], st->centre[0] + 14, st->centre[2]}), 3), "the triad is not where the 90 degree turn takes the box's middle");
        shot("move-ring", false);
        pass("move: Rotate on shows a ring round Z through the box's middle, the triad turned on it");
        // A point on the ring a quarter turn back (the box's own place), pulled 0.6 rad further round.
        st->press = triad->ringPoint(M_PI / 2);
        require(triad->partAt(st->press) == TranslateTriad::kRing, "the ring is not where the triad says");
        mouse(QEvent::MouseButtonPress, st->press);
        require(triad->dragging() == TranslateTriad::kRing, "a press on the ring did not grip it");
        st->last = triad->ringPoint(M_PI / 2 + 0.6);
        mouse(QEvent::MouseMove, triad->ringPoint(M_PI / 2 + 0.3));
        mouse(QEvent::MouseMove, st->last);
        const double degrees = opad::design::ParamTable().angle(form->inputs().value("angle", std::string())) * 180 / M_PI;
        require(std::abs(degrees - 125) < 5.1 && std::abs(std::fmod(degrees, 5.0)) < 1e-6, "pulling round the ring did not turn the angle on in 5 degree steps: " + form->valueText("angle").toStdString());
        return true;
      },
      [=] {
        if (!waitFor(previewFor("angle"), "the turn preview did not follow the pull round the ring")) return false;
        TranslateTriad* triad = design->moveTriad();
        require(triad->dragging() == TranslateTriad::kRing, "the ring let go before the release");
        const double angle = opad::design::ParamTable().angle(form->inputs().value("angle", std::string()));
        const opad::Vec3 turned{st->centre[0] + 14, st->centre[1], st->centre[2]};  // the box's middle now
        const opad::Vec3 to{turned[0] * std::cos(angle) - turned[1] * std::sin(angle), turned[0] * std::sin(angle) + turned[1] * std::cos(angle), turned[2]};
        require(closeTo(triad->partPoint(0), view->widgetPoint(to), 3), "the triad did not travel along the ring");
        Bnd_Box moved;
        for (const auto& c : design->readyPreview()->changed)
          if (c.shape && !c.shape->IsNull()) BRepBndLib::Add(*c.shape, moved);
        double x0, y0, z0, x1, y1, z1;
        moved.Get(x0, y0, z0, x1, y1, z1);
        require(std::hypot((x0 + x1) / 2 - to[0], (y0 + y1) / 2 - to[1]) < 1, "the preview during the pull is not the box turned by the angle");
        shot("move-turn", false);
        pass("move: pulled round the ring while held, the angle is " + form->valueText("angle") + "; the triad and the preview travel along it");
        mouse(QEvent::MouseButtonRelease, st->last);
        view->setFocus();
        key(Qt::Key_4, "4");
        key(Qt::Key_5, "5");
        return true;
      },
      [=] {
        const double degrees = opad::design::ParamTable().angle(form->inputs().value("angle", std::string())) * 180 / M_PI;
        if (!waitFor(std::abs(degrees - 45) < 1e-6, "4 and 5 typed after pulling the ring did not go into Angle: " + form->valueText("angle").toStdString())) return false;
        pass("move: 4 and 5 typed after pulling the ring go into the Angle box");
        key(Qt::Key_Return, {});
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive() && win->m_doc->scene.features.back().kind == "move" && win->m_doc->scene.features.back().inputs.value("rotate", false), "Enter did not commit the turn")) return false;
        const Bnd_Box box = opad::node_world_bbox(win->m_doc->doc, win->m_doc->scene, st->box);
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const double r = st->centre[0] + 14;
        require(std::hypot((x0 + x1) / 2 - r * std::sqrt(0.5), (y0 + y1) / 2 - r * std::sqrt(0.5)) < 0.5, "the box did not turn 45 degrees about Z");
        pass("move: Enter turns the box 45 degrees about Z");
        return true;
      },
  };

  // ---- The other arrows, each pulled the same way: drawn where the core puts it (feature_handles: origin + axis x value),
  // pulled `mm` out along itself, the value grows; while the button is still held the preview is what the value makes
  // (`held` says what is wrong, empty if nothing); let go, Esc leaves the feature without changing the box.
  auto pullArrow = [=](const char* kind, std::function<void()> setup, const char* input, double mm, std::function<std::string()> held, const QString& what) {
    return std::vector<std::function<bool()>>{
        [=] {
          start(kind);
          setup();
          return true;
        },
        [=] {
          if (!waitFor(design->handleInput() == input && previewFor(input), std::string(kind) + " shows no arrow on " + input)) return false;
          const opad::json hs = opad::design::feature_handles(win->m_doc->doc, win->m_doc->scene, kind, form->inputs());
          require(hs.size() == 1 && hs[0].value("input", "") == input, std::string("the core has no ") + input + " arrow for " + kind);
          const opad::Vec3 o = hs[0].at("origin").get<opad::Vec3>(), a = hs[0].at("axis").get<opad::Vec3>();
          const double v = hs[0].at("value").get<double>();
          const opad::Vec3 foot{o[0] + a[0] * v, o[1] + a[1] * v, o[2] + a[2] * v};
          require(closeTo(handle->arrowLine().p1(), view->widgetPoint(foot), 4), std::string("the ") + kind + " arrow is not where the core puts it: " + str(handle->arrowLine().p1()) + " vs " + str(view->widgetPoint(foot)));
          st->previewBefore = previewVolume();
          st->before = evaluated(input);
          st->pxPerMm = pixelsPerMm(foot, a);
          const QLineF line = handle->arrowLine();
          st->dir = (line.p2() - line.p1()) / std::max(1e-9, line.length());
          st->press = line.pointAt(0.5);
          mouse(QEvent::MouseButtonPress, st->press);
          require(handle->dragging(), std::string("a press on the ") + kind + " arrow did not grip it");
          mouse(QEvent::MouseMove, st->press + st->dir * (mm / 2 * st->pxPerMm));
          st->last = st->press + st->dir * (mm * st->pxPerMm);
          mouse(QEvent::MouseMove, st->last);
          require(evaluated(input) > st->before + mm / 2,std::string("pulling the ") + kind + " arrow out did not grow " + input + ": " + form->valueText(input).toStdString());
          return true;
        },
        [=] {
          if (!waitFor(previewFor(input), std::string("the ") + kind + " preview did not follow the pull")) return false;
          require(handle->dragging(), std::string("the ") + kind + " arrow let go before the release");
          const std::string wrong = held();
          require(wrong.empty(), wrong);
          shot(QString(what).replace(" ", "-"), true);
          pass(what + ": the arrow is where the core puts it; pulled out while held, " + QString(input) + " is " + form->valueText(input) + " and the preview follows");
          mouse(QEvent::MouseButtonRelease, st->last);
          design->escape();
          return true;
        },
        [=] { return waitFor(!design->featureActive(), std::string("Esc did not leave ") + kind); },
    };
  };
  auto append = [&steps](std::vector<std::function<bool()>> more) { steps.insert(steps.end(), more.begin(), more.end()); };
  // Chamfer: the bottom edge whose arrow the view sees best (the box is turned 45 degrees by now); a bigger distance cuts more.
  append(pullArrow(
      "chamfer",
      [=] {
        int best = -1;
        double side = 2;
        const opad::Vec3 d = view->viewDirection();
        for (int e : edgesAt(0)) {
          const opad::json ref{{"body", st->box}, {"kind", "edge"}, {"index", e}};
          const opad::json hs = opad::design::feature_handles(win->m_doc->doc, win->m_doc->scene, "chamfer", {{"edges", opad::json::array({ref})}, {"type", "equal"}, {"distance", "2 mm"}});
          if (hs.size() != 1) continue;
          const opad::Vec3 a = hs[0].at("axis").get<opad::Vec3>();
          const double along = std::abs(a[0] * d[0] + a[1] * d[1] + a[2] * d[2]);
          if (along < side) side = along, best = e;
        }
        require(best >= 0, "the box has no bottom edge to chamfer");
        form->setPicks("edges", opad::json::array({{{"body", st->box}, {"kind", "edge"}, {"index", best}}}));
      },
      "distance", 1.5,
      [=] { return previewVolume() < st->previewBefore - 1e-3 ? std::string() : "the chamfer preview during the pull does not cut more: " + std::to_string(previewVolume()) + " vs " + std::to_string(st->previewBefore); },
      "chamfer"));
  // Thicken: the top face; a thicker skin has more volume.
  append(pullArrow(
      "thicken", [=] { form->setPicks("faces", opad::json::array({{{"body", st->box}, {"kind", "face"}, {"index", topFace()}}})); }, "thickness", 1.5,
      [=] { return previewVolume() > st->previewBefore + 1e-3 ? std::string() : "the thicken preview during the pull is not thicker: " + std::to_string(previewVolume()) + " vs " + std::to_string(st->previewBefore); },
      "thicken"));
  // Construction plane (offset from XY): the planned plane is as far up as the pulled distance.
  append(pullArrow(
      "plane", [] {}, "distance", 4,
      [=] {
        const auto plan = design->readyPreview();
        if (!plan) return std::string("the plane has no preview");
        for (auto op = plan->ops.rbegin(); op != plan->ops.rend(); ++op)
          if (op->contains("result") && (*op)["result"].contains("plane")) {
            const double z = (*op)["result"]["plane"]["origin"][2].get<double>();
            return std::abs(z - evaluated("distance")) < 1e-6 ? std::string() : "the planned plane is at " + std::to_string(z) + ", not at the pulled distance";
          }
        return std::string("the plane's preview plans no plane");
      },
      "construction plane"));
  // Construction plane from the top face: the arrow stands on the face's middle (the guide's), not on the corner the face's
  // frame starts at; the planned plane is the pulled distance above the face.
  append(pullArrow(
      "plane", [=] { form->setPicks("plane", {{"face", {{"body", st->box}, {"kind", "face"}, {"index", topFace()}}}}); }, "distance", 4,
      [=] {
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape(), TopAbs_FACE, faces);
        GProp_GProps g;
        BRepGProp::SurfaceProperties(faces(topFace() + 1), g);
        const gp_Pnt c = g.CentreOfMass();
        const double d = evaluated("distance");
        if (!closeTo(handle->arrowLine().p1(), view->widgetPoint({c.X(), c.Y(), c.Z() + d}), 4)) return "the arrow is not on the top face's middle, " + std::to_string(d) + " up";
        const auto plan = design->readyPreview();
        if (!plan) return std::string("the plane has no preview");
        for (auto op = plan->ops.rbegin(); op != plan->ops.rend(); ++op)
          if (op->contains("result") && (*op)["result"].contains("plane")) {
            const double z = (*op)["result"]["plane"]["origin"][2].get<double>();
            return std::abs(z - st->top - d) < 1e-6 ? std::string() : "the planned plane is at " + std::to_string(z) + ", not the pulled distance above the face";
          }
        return std::string("the plane's preview plans no plane");
      },
      "construction plane from a face"));
  // Box: its height arrow; the preview is 20 x 20 x the pulled height.
  append(pullArrow(
      "box", [] {}, "height", 6,
      [=] { return std::abs(previewVolume() - 400 * evaluated("height")) < 1e-3 * previewVolume() ? std::string() : "the box preview during the pull is not 20 x 20 x " + form->valueText("height").toStdString(); },
      "box"));
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
      trace::log("bench: handles: all steps PASS");
      QCoreApplication::exit(0);
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: handles FAIL at step %1: %2").arg(st->step).arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
