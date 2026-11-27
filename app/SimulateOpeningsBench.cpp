// An airflow check's openings picked as loops of edges, with the air's way drawn and flipped; case in tools/bench_cases/simulate.py.
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <array>
#include <cmath>
#include <functional>
#include <set>
#include <sstream>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "GuidedSteps.hpp"
#include "MainWindow.hpp"
#include "SimulateArea.hpp"
#include "SimulateCheck.hpp"
#include "ToolPanel.hpp"
#include "Tracking.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"

namespace {
bool until(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return done();
}

// A duct adapter's wall, revolved about Z from its bore (r, z) made 2.4 mm thick outward: the 8" sleeve with a 1 mm chamfer
// inside its rim, a step in where the collar it slides over stops, a cone down to the 6" spigot, a 1 mm chamfer inside its rim.
std::string adapterBrep(double x0) {
  const std::vector<std::array<double, 2>> bore = {{99.9, 0},   {98.9, 1},    {98.9, 34},    {95.6, 34},
                                                   {95.6, 48},  {71.36, 160}, {71.36, 195}, {72.36, 196}};
  BRepBuilderAPI_MakePolygon poly;
  for (const auto& p : bore) poly.Add(gp_Pnt(x0 + p[0], 0, p[1]));
  for (auto it = bore.rbegin(); it != bore.rend(); ++it) poly.Add(gp_Pnt(x0 + (*it)[0] + 2.4, 0, (*it)[1]));
  poly.Close();
  const TopoDS_Face face = BRepBuilderAPI_MakeFace(poly.Wire(), true).Face();
  const TopoDS_Shape s = BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(x0, 0, 0), gp_Dir(0, 0, 1)), 2 * M_PI).Shape();
  std::ostringstream out;
  BRepTools::Write(s, out);
  return out.str();
}
}  // namespace

// OPAD_BENCH_FLOWOPENINGS=<prefix> on an empty document: the fitting check on a duct adapter (a sleeve with a chamfered rim,
// a step in, a cone, a chamfered spigot) beside another duct left out of the check. OPAD proposes each opening as the loop
// of edges round the bore (in past the chamfers), on the adapter only; the core plans it as a wedge with its step; an
// arrow is drawn through each opening (clickable in the view). A click on the spigot rim's inner edge takes that loop in
// place of the proposed outlet (one edge, not added to it). A click on the inlet's arrow flips the air's way (in and out
// swapped, planned again the other way); the Flip button flips it back. Shots: <prefix>.ends.png, .view.png, .flipped.png.
OPAD_BENCH(OPAD_BENCH_FLOWOPENINGS, flowopenings) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: flow openings: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  const QString prefix = value;
  AppDocument* doc = w.m_doc;
  Simulate* area = w.findChild<Simulate*>();
  Viewport* view = w.m_viewport;
  if (!area || !doc->hasDocument || !view) {
    require(false, "an empty document, the Simulate area and the view");
    return QCoreApplication::exit(2), true;
  }
  auto run = [doc](const std::string& command, const opad::json& args) {
    until([doc] { return !doc->designBusy && !doc->snapshotBusy(); }, 10000);
    return doc->run(command, args);
  };
  auto shot = [&prefix](QWidget* widget, const QString& name) {
    if (widget && !prefix.isEmpty() && prefix != "1") widget->grab().save(prefix + "." + name + ".png");
  };
  try {
    run("import_brep", {{"brep", adapterBrep(0)}, {"name", "Duct adapter"}});
    run("import_brep", {{"brep", adapterBrep(400)}, {"name", "Spare duct"}});
    std::string adapter, spare;
    for (const auto& id : doc->scene.all_bodies())
      (doc->scene.node(id)->name == "Duct adapter" ? adapter : spare) = id;
    require(!adapter.empty() && !spare.empty(), "the adapter and a spare duct beside it");
    // The edges by what they are: circles at a height of a radius.
    auto circle = [&](const std::string& body, double z, double r) {
      TopTools_IndexedMapOfShape edges;
      TopExp::MapShapes(opad::node_world_shape(doc->doc, doc->scene, body), TopAbs_EDGE, edges);
      for (int i = 1; i <= edges.Extent(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
        if (c.GetType() == GeomAbs_Circle && std::fabs(c.Circle().Location().Z() - z) < 1e-6 && std::fabs(c.Circle().Radius() - r) < 1e-6) return i - 1;
      }
      return -1;
    };
    const int bore8 = circle(adapter, 1, 98.9), bore6 = circle(adapter, 195, 71.36), rim6 = circle(adapter, 196, 72.36);
    require(bore8 >= 0 && bore6 >= 0 && rim6 >= 0, "the bores' and the spigot rim's circles");
    view->standardView("iso");
    view->fitAll();
    opad::Ref part;
    part.body = adapter;
    area->services().select({part});  // the check takes the part selected
    area->openCheck("fitting");
    GuidedCheck* check = area->check();
    require(check->check() == "fitting" && check->listed("bodies").size() == 1 && check->listed("bodies")[0].body == adapter, "the fitting check on the adapter alone");
    require(until([check] { return check->proposed("inlet") && check->proposed("outlet"); }, 20000), "OPAD proposes the openings (a worker)");
    require(until([check] { return !check->listed("inlet").empty() && !check->listed("outlet").empty(); }, 5000), "a new check takes them at once");
    auto edgesOf = [](const std::vector<opad::Ref>& refs) {
      std::set<int> out;
      for (const auto& r : refs)
        if (r.kind == opad::Ref::Kind::Edge) out.insert(r.index);
      return out;
    };
    auto onAdapter = [&adapter](const std::vector<opad::Ref>& refs) {
      return !refs.empty() && std::all_of(refs.begin(), refs.end(), [&](const opad::Ref& r) { return r.body == adapter; });
    };
    require(edgesOf(check->listed("inlet")) == std::set<int>{bore8} && edgesOf(check->listed("outlet")) == std::set<int>{bore6},
            "each opening is the loop round the bore, in past the chamfer at its rim (the wider in)");
    require(onAdapter(check->listed("inlet")) && onAdapter(check->listed("outlet")), "both on the adapter: the duct left out of the check is never proposed from");
    check->setValue("flow", 350);
    check->setValue("quick", 1);
    check->flush();
    check->frame()->setCurrent(check->stepNames().indexOf("ends"));
    require(until([check] { return check->plan().is_object() && check->plan().contains("inlet"); }, 20000), "the core plans it");
    opad::json plan = check->plan();
    require(plan.value("mesh", "") == "axisymmetric" && plan.value("steps", 0) == 1,
            "round with a step: solved as a wedge whose blocks step with it (" + QString::fromStdString(plan.dump().substr(0, 160)) + ")");
    require(std::fabs(plan["inlet"].value("diameter_mm", 0.0) - 197.8) < 0.01 && std::fabs(plan["outlet"].value("diameter_mm", 0.0) - 142.72) < 0.01,
            "the openings are the bores, 197.8 and 142.72 across");
    require(plan.value("seconds_estimate", 0.0) > 0, "the plan says about how long it takes");
    require(until([area] { return area->checkArrowsShown() == 2 && area->fanGlyphsShown() == 2; }, 5000),
            "an outline and an arrow through each opening, the arrows clickable in the view");
    shot(area->checkPanel(), "ends");
    if (!prefix.isEmpty() && prefix != "1") view->grabImage().save(prefix + ".view.png");

    // Picking the outlet again: one click on the spigot rim's inner edge, its loop in place of the proposal.
    auto* pick = check->findChild<QPushButton*>("checkPick_outlet");
    require(pick != nullptr, "the outlet's Pick in the view");
    if (pick) pick->click();
    require(check->picking(), "picking the outlet");
    // Close on the spigot's end from above (its rim's hole 1 mm outside the bore's end there).
    view->standardView("top");
    Bnd_Box spigot;
    spigot.Update(-80, -80, 190, 80, 80, 197);
    view->fitBox(spigot);
    int x = 0, y = 0;
    const bool seen = until([&] {
      return view->benchPickPoint([&](const std::string& c, const opad::Ref& r) { return c.empty() && r.body == adapter && r.kind == opad::Ref::Kind::Edge && r.index == rim6; }, x, y,
                                  false);
    }, 8000);
    require(seen, "the spigot rim's inner edge is found in the view");
    if (seen) view->benchClickAt(x, y);
    require(until([&] { return !check->picking() && edgesOf(check->listed("outlet")) == std::set<int>{rim6}; }, 10000),
            "one click takes that loop in place of the proposed one (not added to it), and picking stops");
    check->flush();
    require(until([check] { return std::fabs(check->plan().value("outlet", opad::json::object()).value("diameter_mm", 0.0) - 144.72) < 0.01; }, 20000),
            "planned again with the rim's hole as the outlet, 144.72 across");

    view->standardView("iso");
    view->fitAll();
    // The air the other way: a click on the inlet's arrow.
    const std::vector<opad::Ref> in0 = check->listed("inlet"), out0 = check->listed("outlet");
    const double axis0 = check->plan().value("axis", opad::Vec3{0, 0, 0})[2];
    const bool arrow = until([&] { return view->benchPickPoint([](const std::string& c, const opad::Ref&) { return c == "flow:inlet"; }, x, y, false); }, 8000);
    require(arrow, "the inlet's arrow is found in the view");
    if (arrow) view->benchClickAt(x, y);
    require(until([&] { return edgesOf(check->listed("inlet")) == edgesOf(out0) && edgesOf(check->listed("outlet")) == edgesOf(in0); }, 5000),
            "a click on an arrow flips the air's way: in where it came out");
    check->flush();
    require(until([&] { return check->plan().value("axis", opad::Vec3{0, 0, 0})[2] * axis0 < 0; }, 20000), "planned again the other way");
    require(area->checkArrowsShown() == 2, "the arrows are drawn the new way");
    shot(area->checkPanel(), "flipped");
    if (auto* flip = check->findChild<QPushButton*>("checkFlip")) flip->click();
    require(edgesOf(check->listed("inlet")) == edgesOf(in0) && edgesOf(check->listed("outlet")) == edgesOf(out0), "the Flip button flips it back");
    check->frame()->setCurrent(check->stepNames().indexOf("part"));
    require(until([area] { return area->checkArrowsShown() == 0; }, 5000), "another step: the arrows go");
  } catch (const std::exception& e) {
    require(false, QString("no step throws: %1").arg(QString::fromStdString(e.what())));
  }
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
