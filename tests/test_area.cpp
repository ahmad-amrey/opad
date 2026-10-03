// The Area tool's measure (UI-90, core/src/measure_area.cpp): grown loops in a drawing (the cell on either side of an edge,
// the smaller one, dangling lines left out; objects cut where others meet or cross them, tangent ones told apart),
// closed objects, picked boundaries open, closed and running past each other, holes, the polygon through points, fills.
#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
#include <optional>
#include <sstream>

#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/util.hpp"

using namespace opad;

namespace {
struct Drawing {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-area-" + new_uuid());
  Document doc = Document::create();
  Scene scene;
  Drawing() {
    std::filesystem::create_directory(dir);
    std::ostringstream out;
    auto g = [&](int code, const std::string& value) { out << code << '\n' << value << '\n'; };
    auto line = [&](double x0, double y0, double x1, double y1) {
      g(0, "LINE"), g(8, "Walls"), g(10, std::to_string(x0)), g(20, std::to_string(y0)), g(11, std::to_string(x1)), g(21, std::to_string(y1));
    };
    g(0, "SECTION"), g(2, "ENTITIES");
    // A 100 x 50 room split at x = 60 into two cells (every wall meets the others at its ends), a line hanging off a
    // corner, a half disc (an arc closed by a line), an outline with a square hole in it.
    line(0, 0, 60, 0), line(60, 0, 100, 0), line(100, 0, 100, 50), line(100, 50, 60, 50), line(60, 50, 0, 50), line(0, 50, 0, 0), line(60, 0, 60, 50);
    line(100, 50, 130, 80);
    g(0, "ARC"), g(8, "Walls"), g(10, "300"), g(20, "0"), g(40, "20"), g(50, "0"), g(51, "180");
    line(280, 0, 320, 0);
    for (const auto& [x0, y0, x1, y1] : {std::array{400., 0., 440., 0.}, {440., 0., 440., 40.}, {440., 40., 400., 40.}, {400., 40., 400., 0.},
                                         {410., 10., 420., 10.}, {420., 10., 420., 20.}, {420., 20., 410., 20.}, {410., 20., 410., 10.}})
      line(x0, y0, x1, y1);
    g(0, "CIRCLE"), g(8, "Holes"), g(10, "200"), g(20, "0"), g(40, "10");
    g(0, "SOLID"), g(8, "Fill"), g(10, "0"), g(20, "100"), g(11, "10"), g(21, "100"), g(12, "0"), g(22, "110"), g(13, "10"), g(23, "110");
    g(0, "ENDSEC"), g(0, "EOF");
    write_text_file(dir / "room.dxf", out.str());
    import_file(doc, dir / "room.dxf");
    scene = resolve(doc);
  }
  ~Drawing() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
  std::string layer(const std::string& name) const {
    for (const auto& id : scene.all_bodies())
      if (scene.node(scene.node(id)->parent)->name == name) return id;
    throw Error("no layer " + name);
  }
  // The edge from (x0, y0) to (x1, y1), either way round.
  Ref edge(const std::string& layerName, double x0, double y0, double x1, double y1) const {
    const std::string body = layer(layerName);
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(node_world_shape(doc, scene, body), TopAbs_EDGE, map);
    for (int i = 1; i <= map.Extent(); ++i) {
      const BRepAdaptor_Curve c(TopoDS::Edge(map(i)));
      const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
      auto near = [](const gp_Pnt& p, double x, double y) { return std::hypot(p.X() - x, p.Y() - y) < 1e-6; };
      if ((near(a, x0, y0) && near(b, x1, y1)) || (near(a, x1, y1) && near(b, x0, y0))) {
        Ref r;
        r.body = body;
        r.kind = Ref::Kind::Edge;
        r.index = i - 1;
        return r;
      }
    }
    throw Error("no such edge");
  }
  Ref first(const std::string& layerName, Ref::Kind kind) const {
    Ref r;
    r.body = layer(layerName);
    r.kind = kind;
    r.index = 0;
    return r;
  }
  json area(const std::vector<Ref>& refs) const { return measure_area(doc, scene, refs); }
};

Ref point(double x, double y) {
  Ref r;
  r.kind = Ref::Kind::Point;
  r.point = {x, y, 0};
  return r;
}
}  // namespace

TEST(an_edge_grows_into_the_cell_it_bounds) {
  Drawing d;
  json r = d.area({d.edge("Walls", 0, 0, 60, 0)});  // an outer wall: its one bounded side
  CHECK(r["closed"].get<bool>() && r.value("grown", false));
  CHECK_NEAR(r["value"].get<double>(), 3000, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 220, 1e-6);
  CHECK_EQ(r["edges"].get<int>(), 4);
  CHECK(!r["boundary"].empty() && r["boundary"][0].size() >= 4);
  const json c = r["center"];
  CHECK_NEAR(c[0].get<double>(), 30, 1e-6);
  CHECK_NEAR(c[1].get<double>(), 25, 1e-6);
  r = d.area({d.edge("Walls", 60, 0, 60, 50)});  // the dividing wall: cells on both sides, the smaller one
  CHECK_NEAR(r["value"].get<double>(), 2000, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 180, 1e-6);
  r = d.area({d.edge("Walls", 100, 50, 130, 80)});  // hangs off a corner: encloses nothing
  CHECK(!r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 0, 1e-12);
  r = d.area({d.first("Walls", Ref::Kind::Edge)});  // whichever edge comes first: a loop or nothing, never an error
  CHECK(r.contains("closed"));
}

TEST(an_arc_closes_with_its_chord_and_a_circle_alone) {
  Drawing d;
  Ref arc;
  for (int i = 0;; ++i) {  // the arc: the edge whose middle is at (300, 20)
    arc = d.first("Walls", Ref::Kind::Edge);
    arc.index = i;
    const BRepAdaptor_Curve c(TopoDS::Edge(subshape(node_world_shape(d.doc, d.scene, arc.body), Ref::Kind::Edge, i)));
    if (c.GetType() == GeomAbs_Circle) break;
  }
  json r = d.area({arc});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), M_PI * 400 / 2, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), M_PI * 20 + 40, 1e-6);
  r = d.area({d.first("Holes", Ref::Kind::Edge)});
  CHECK(r["closed"].get<bool>() && !r.value("grown", false));
  CHECK_NEAR(r["value"].get<double>(), M_PI * 100, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), M_PI * 20, 1e-6);
}

TEST(picked_boundaries_open_closed_and_with_holes) {
  Drawing d;
  json r = d.area({d.edge("Walls", 60, 0, 100, 0), d.edge("Walls", 100, 0, 100, 50), d.edge("Walls", 100, 50, 60, 50)});
  CHECK(!r["closed"].get<bool>());
  CHECK_EQ(r["open_ends"].get<int>(), 2);
  CHECK_EQ(r["ends"].size(), 2u);
  CHECK_NEAR(r["perimeter"].get<double>(), 130, 1e-6);
  r = d.area({d.edge("Walls", 60, 0, 100, 0), d.edge("Walls", 100, 0, 100, 50), d.edge("Walls", 100, 50, 60, 50), d.edge("Walls", 60, 50, 60, 0)});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 2000, 1e-6);
  CHECK_EQ(r["loops"].get<int>(), 1);
  std::vector<Ref> outline = {d.edge("Walls", 400, 0, 440, 0), d.edge("Walls", 440, 0, 440, 40), d.edge("Walls", 440, 40, 400, 40), d.edge("Walls", 400, 40, 400, 0)};
  CHECK_NEAR(d.area(outline)["value"].get<double>(), 1600, 1e-6);
  for (const auto& e : {d.edge("Walls", 410, 10, 420, 10), d.edge("Walls", 420, 10, 420, 20), d.edge("Walls", 420, 20, 410, 20), d.edge("Walls", 410, 20, 410, 10)}) outline.push_back(e);
  r = d.area(outline);  // the inner square is a hole
  CHECK_NEAR(r["value"].get<double>(), 1500, 1e-6);
  CHECK_EQ(r["loops"].get<int>(), 2);
  CHECK_EQ(r["holes"].get<int>(), 1);
  CHECK_NEAR(r["perimeter"].get<double>(), 200, 1e-6);
}

TEST(points_fills_and_what_is_refused) {
  Drawing d;
  json r = d.area({point(0, 0), point(100, 0), point(100, 50)});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 2500, 1e-9);
  CHECK_NEAR(r["perimeter"].get<double>(), 150 + std::hypot(100, 50), 1e-9);
  r = d.area({point(0, 0), point(100, 0)});
  CHECK(!r["closed"].get<bool>());
  r = d.area({d.first("Fill", Ref::Kind::Face)});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 100, 1e-9);
  CHECK_NEAR(r["perimeter"].get<double>(), 40, 1e-9);
  CHECK_EQ(r["normal"].size(), 3u);
  CHECK_THROWS(d.area({d.first("Fill", Ref::Kind::Face), d.edge("Walls", 0, 0, 60, 0)}));
  Ref body;
  body.body = d.layer("Walls");
  CHECK_THROWS(d.area({body}));
  CHECK_THROWS(d.area({}));
}

// Objects that meet in the middle of others (T), cross them (X), touch a circle (a slot's tangent lines), run past their
// corners, and an ellipse cut by lines.
struct Crossings {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-area-x-" + new_uuid());
  Document doc = Document::create();
  Scene scene;
  std::string body;
  Crossings() {
    std::filesystem::create_directory(dir);
    std::ostringstream out;
    auto g = [&](int code, const std::string& value) { out << code << '\n' << value << '\n'; };
    auto line = [&](double x0, double y0, double x1, double y1) {
      g(0, "LINE"), g(8, "Plan"), g(10, std::to_string(x0)), g(20, std::to_string(y0)), g(11, std::to_string(x1)), g(21, std::to_string(y1));
    };
    g(0, "SECTION"), g(2, "ENTITIES");
    // A 100 x 50 room of four walls with a wall from the bottom one to the top one at x = 60 (T at both ends).
    line(0, 0, 100, 0), line(100, 0, 100, 50), line(100, 50, 0, 50), line(0, 50, 0, 0), line(60, 0, 60, 50);
    // A slot: a circle with two lines leaving its top and bottom tangentially, closed by a line at x = 350.
    g(0, "CIRCLE"), g(8, "Plan"), g(10, "300"), g(20, "0"), g(40, "10");
    line(300, 10, 350, 10), line(300, -10, 350, -10), line(350, -10, 350, 10);
    // A # of four lines around a 20 x 20 cell.
    line(500, 10, 560, 10), line(500, 30, 560, 30), line(520, 0, 520, 40), line(540, 0, 540, 40);
    // A 50 x 40 outline whose corners overshoot by 5.
    line(595, 0, 655, 0), line(650, -5, 650, 45), line(655, 40, 595, 40), line(600, 45, 600, -5);
    // An ellipse (semi-axes 40 and 20) cut along its major axis by a line ending on it and crossed by a longer one.
    g(0, "ELLIPSE"), g(8, "Plan"), g(10, "900"), g(20, "0"), g(11, "40"), g(21, "0"), g(40, "0.5"), g(41, "0"), g(42, std::to_string(2 * M_PI));
    line(860, 0, 940, 0), line(900, -30, 900, 30);
    // Three lines that miss a common point by a hair: a speck of a triangle with 0.003 legs.
    line(1990, 0, 2010, 0), line(2000, -10, 2000, 10), line(1990, 10.003, 2010, -9.997);
    // A 1000 x 1000 square whose bottom has a 1 mm piece: its cell is far bigger than the first window around it.
    line(3000, 0, 3500, 0), line(3500, 0, 3501, 0), line(3501, 0, 4000, 0), line(4000, 0, 4000, 1000), line(4000, 1000, 3000, 1000), line(3000, 1000, 3000, 0);
    // A 200 x 100 outline whose left side has a 1 mm piece with small circles on both its ends (as a board outline's
    // vertex marks): in the first window the walls leading on are cut off, and must not be taken for dead ends.
    line(5000, 0, 5200, 0), line(5200, 0, 5200, 100), line(5200, 100, 5000, 100), line(5000, 100, 5000, 99), line(5000, 99, 5000, 0);
    g(0, "CIRCLE"), g(8, "Plan"), g(10, "5000"), g(20, "99"), g(40, "0.3");
    g(0, "CIRCLE"), g(8, "Plan"), g(10, "5000"), g(20, "100"), g(40, "0.3");
    g(0, "ENDSEC"), g(0, "EOF");
    write_text_file(dir / "plan.dxf", out.str());
    import_file(doc, dir / "plan.dxf");
    scene = resolve(doc);
    body = scene.all_bodies().at(0);
  }
  ~Crossings() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
  Ref edge(double x0, double y0, double x1, double y1) const {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(node_world_shape(doc, scene, body), TopAbs_EDGE, map);
    for (int i = 1; i <= map.Extent(); ++i) {
      const BRepAdaptor_Curve c(TopoDS::Edge(map(i)));
      const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
      auto at = [](const gp_Pnt& p, double x, double y) { return std::hypot(p.X() - x, p.Y() - y) < 1e-6; };
      if ((at(a, x0, y0) && at(b, x1, y1)) || (at(a, x1, y1) && at(b, x0, y0))) {
        Ref r;
        r.body = body;
        r.kind = Ref::Kind::Edge;
        r.index = i - 1;
        return r;
      }
    }
    throw Error("no such edge");
  }
  json area(const std::vector<Ref>& refs, std::optional<Vec3> clicked = std::nullopt) const { return measure_area(doc, scene, refs, {}, clicked); }
};

TEST(cells_between_objects_that_meet_in_the_middle) {
  Crossings d;
  const Ref bottom = d.edge(0, 0, 100, 0);
  json r = d.area({bottom}, Vec3{30, 0, 0});  // the wall under the left room: cut at the T, the part clicked
  CHECK(r["closed"].get<bool>() && r.value("grown", false));
  CHECK_NEAR(r["value"].get<double>(), 3000, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 220, 1e-6);
  CHECK_EQ(r["edges"].get<int>(), 4);
  CHECK_NEAR(d.area({bottom}, Vec3{80, 0.5, 0})["value"].get<double>(), 2000, 1e-6);  // near the right room's part
  CHECK_NEAR(d.area({bottom})["value"].get<double>(), 3000, 1e-6);                  // no click: the part at its middle
  CHECK_NEAR(d.area({d.edge(60, 0, 60, 50)})["value"].get<double>(), 2000, 1e-6);    // the wall with a T at each end
}

TEST(tangent_objects_and_crossings) {
  Crossings d;
  // The slot's end: past the lines' tangent points the trace goes on along the half of the circle that bends towards the
  // cell (curvature tells the tangent directions apart), not around the other half.
  json r = d.area({d.edge(350, -10, 350, 10)});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 1000 - 50 * M_PI, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 120 + 10 * M_PI, 1e-6);
  CHECK_NEAR(d.area({d.edge(300, 10, 350, 10)}, Vec3{320, 10, 0})["value"].get<double>(), 1000 - 50 * M_PI, 1e-6);
  // One line of the #: the cell its middle part bounds.
  r = d.area({d.edge(500, 10, 560, 10)}, Vec3{530, 10, 0});
  CHECK_NEAR(r["value"].get<double>(), 400, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 80, 1e-6);
  CHECK(!d.area({d.edge(500, 10, 560, 10)}, Vec3{505, 10, 0})["closed"].get<bool>());  // its loose end bounds nothing
  // The ellipse: cut where the lines end on it and cross it (refined on the curve), a quarter of it.
  r = d.area({d.edge(860, 0, 940, 0)}, Vec3{880, 0, 0});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 200 * M_PI, 1e-4);
}

TEST(specks_are_not_areas_and_big_cells_are_found_far_away) {
  Crossings d;
  CHECK(!d.area({d.edge(1990, 0, 2010, 0)}, Vec3{2000.0015, 0, 0})["closed"].get<bool>());  // the speck above, nothing below
  json r = d.area({d.edge(3500, 0, 3501, 0)});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 1e6, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 4000, 1e-6);
  CHECK_EQ(r["edges"].get<int>(), 6);
  r = d.area({d.edge(5000, 100, 5000, 99)});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 20000 - 0.09 * M_PI * 3 / 4, 1e-6);  // less a half and a quarter of the circles
}

TEST(picked_objects_that_run_past_each_other) {
  Crossings d;
  json r = d.area({d.edge(500, 10, 560, 10), d.edge(500, 30, 560, 30), d.edge(520, 0, 520, 40), d.edge(540, 0, 540, 40)});
  CHECK(r["closed"].get<bool>() && r.value("trimmed", false));
  CHECK_NEAR(r["value"].get<double>(), 400, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 80, 1e-6);
  r = d.area({d.edge(595, 0, 655, 0), d.edge(650, -5, 650, 45), d.edge(655, 40, 595, 40), d.edge(600, 45, 600, -5)});
  CHECK(r["closed"].get<bool>());
  CHECK_NEAR(r["value"].get<double>(), 2000, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 180, 1e-6);
  r = d.area({d.edge(595, 0, 655, 0), d.edge(650, -5, 650, 45), d.edge(655, 40, 595, 40)});  // three sides: still open
  CHECK(!r["closed"].get<bool>());
  CHECK_EQ(r["open_ends"].get<int>(), 6);
}

CHECK_MAIN()
