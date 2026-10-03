// The Area tool's measure (UI-90, core/src/measure_area.cpp): grown loops in a drawing (the cell on either side of an edge,
// the smaller one, dangling lines left out), closed objects, picked boundaries open and closed, holes, the polygon
// through points, fills.
#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <filesystem>
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

CHECK_MAIN()
