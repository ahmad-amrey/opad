// A fillet the kernel cannot make is a refusal, never the end of the app (2026-11-26: a live-MCP fillet preview of the
// top edges of joined extrusions faulted in ChFi3d, an access violation in BRep_Tool::CurveOnSurface from StartSol, and
// the whole app went down with an unsaved document). The stand-in is generated here: on a plate, two round lobes meeting
// in a concave cusp and running into straight sides, with lower arc slots ending right on the points where the lobes
// turn into their sides (what faults), beside raised strokes outlined in quadratic B-splines (as outline fonts draw
// them; two overlap along a shared bottom, so the joined top gets very short spline edges) and a wall thinner than the
// radius.
#include <BRepCheck_Analyzer.hxx>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "check.hpp"
#include "opad/agent.hpp"
#include "opad/core.hpp"
#include "opad/geometry.hpp"
#include "opad/kernel_guard.hpp"

using namespace opad;

namespace {
struct Outline {
  json points = json::array(), entities = json::array();
  int next = 1;
  int point(double x, double y) {
    points.push_back({{"id", next}, {"x", x}, {"y", y}});
    return next++;
  }
  void line(int a, int b) { entities.push_back({{"id", 1000 + int(entities.size())}, {"type", "line"}, {"p", {a, b}}}); }
  void spline(int a, int c, int b) { entities.push_back({{"id", 1000 + int(entities.size())}, {"type", "spline"}, {"degree", 2}, {"p", {a, c, b}}}); }
  // A closed stroke: a rounded box from (x0, y0) to (x1, y1) whose corners are quadratic splines of radius r, the flat
  // bottom split into `pieces` collinear quadratic splines (outline fonts draw straight runs so too).
  void stroke(double x0, double y0, double x1, double y1, double r, int pieces) {
    const int bl = point(x0 + r, y0), br = point(x1 - r, y0);
    const int rb = point(x1, y0 + r), rt = point(x1, y1 - r), tr = point(x1 - r, y1), tl = point(x0 + r, y1);
    const int lt = point(x0, y1 - r), lb = point(x0, y0 + r);
    int last = bl;
    for (int i = 1; i <= pieces; ++i) {
      // Pieces get shorter towards the corner, the last ones a few hundredths of a millimetre.
      const double t = i == pieces ? 1.0 : 1.0 - std::pow(0.45, i);
      const int p = i == pieces ? br : point(x0 + r + (x1 - x0 - 2 * r) * t, y0);
      const double mx = (points[last - 1]["x"].get<double>() + points[p - 1]["x"].get<double>()) / 2;
      spline(last, point(mx, y0), p);
      last = p;
    }
    spline(br, point(x1, y0), rb);
    line(rb, rt);
    spline(rt, point(x1, y1), tr);
    line(tr, tl);
    spline(tl, point(x0, y1), lt);
    line(lt, lb);
    spline(lb, point(x0, y0), bl);
  }
  void arc(int centre, int from, int to) { entities.push_back({{"id", 1000 + int(entities.size())}, {"type", "arc"}, {"p", {centre, from, to}}}); }
  // Two round lobes of radius r, centres (cx -+ dx, cy), meeting in a concave cusp on x = cx above, each running into a
  // straight side tangent to it down to a point at (cx, tip).
  std::pair<double, double> left_tangent, right_tangent;  // where the lobes meet their straight sides
  void lobes(double cx, double cy, double dx, double r, double tip) {
    const double top = cy + std::sqrt(r * r - dx * dx);
    auto tangent = [&](double ox, bool left) {  // the outer tangent point from the tip to the lobe centred at (ox, cy)
      const double px = cx - ox, py = tip - cy, d = std::hypot(px, py), phi = std::atan2(py, px), a = std::acos(r / d);
      const double x1 = ox + r * std::cos(phi + a), y1 = cy + r * std::sin(phi + a), x2 = ox + r * std::cos(phi - a), y2 = cy + r * std::sin(phi - a);
      return (x1 < x2) == left ? std::make_pair(x1, y1) : std::make_pair(x2, y2);
    };
    const auto [lx, ly] = tangent(cx - dx, true);
    const auto [rx, ry] = tangent(cx + dx, false);
    const int bottom = point(cx, tip), cusp = point(cx, top), cl = point(cx - dx, cy), cr = point(cx + dx, cy);
    left_tangent = {lx, ly};
    right_tangent = {rx, ry};
    const int tl = point(lx, ly), tr = point(rx, ry);
    line(bottom, tl);
    arc(cl, cusp, tl);
    arc(cr, tr, cusp);
    line(tr, bottom);
  }
  json geometry() const { return {{"points", points}, {"entities", entities}}; }
};

json sketch(Document& doc, const std::string& name, const json& geometry) {
  return commands::run("sketch", {{"name", name}, {"plane", {{"base", "xy"}}}, {"geometry", geometry}}, &doc);
}
json extrude(Document& doc, const std::string& name, const json& sk, double height, const std::string& op, const std::string& target = {}) {
  json in = {{"profiles", json::array({{{"sketch", sk["sketch_id"]}}})}, {"distance", std::to_string(height) + " mm"}, {"operation", op}};
  if (!target.empty()) in["targets"] = json::array({target});
  return commands::run("feature", {{"kind", "extrude"}, {"name", name}, {"inputs", in}}, &doc);
}

// Plate 3 mm, strokes up to 4.5 mm (two overlapping along one bottom line), a 0.6 mm wall up to 4 mm.
std::string stand_in(Document& doc) {
  Outline plate;
  {
    const int a = plate.point(-14, -8), b = plate.point(14, -8), c = plate.point(14, 8), d = plate.point(-14, 8);
    plate.line(a, b);
    plate.line(b, c);
    plate.line(c, d);
    plate.line(d, a);
  }
  const json base = extrude(doc, "Plate", sketch(doc, "Plate", plate.geometry()), 3, "new");
  const std::string body = base["body_ids"][0];
  Outline strokes;
  strokes.stroke(-11, -4.9, -6, -1.9, 0.6, 6);
  strokes.stroke(-11.6, -4.9, -10.75, 2.5, 0.25, 3);  // overlaps the first one's bottom-left corner on the same line
  strokes.stroke(-3, -4.9, 4, -3.4, 0.7, 5);
  strokes.stroke(3.4, -4.9, 4.6, 3, 0.3, 4);
  extrude(doc, "Strokes", sketch(doc, "Strokes", strokes.geometry()), 4.5, "join", body);
  Outline wall;
  wall.stroke(6, -2, 12, -1.4, 0.2, 2);
  extrude(doc, "Wall", sketch(doc, "Wall", wall.geometry()), 4, "join", body);
  Outline lobes;
  lobes.lobes(0, 4.5, 1.6, 1.9, 0.2);
  extrude(doc, "Lobes", sketch(doc, "Lobes", lobes.geometry()), 4.5, "join", body);
  // Lower arc slots whose round ends sit on the lobes' tangent points: the blend along a lobe runs into them right where
  // its arc turns into the straight side.
  json slots = json::array();
  for (const auto& [t, side] : {std::make_pair(lobes.left_tangent, -1.0), std::make_pair(lobes.right_tangent, 1.0)}) {
    const double cx = t.first + side * 3.4, cy = t.second + 2.9, r = std::hypot(t.first - cx, t.second - cy);
    const double end = std::atan2(t.second - cy, t.first - cx), start = end + side * 1.83;  // ~105 degrees round the bottom
    const json from = {cx + r * std::cos(start), cy + r * std::sin(start)}, to = {t.first, t.second};
    slots.push_back({{"kind", "arcslot"}, {"picks", json::array({{cx, cy}, side < 0 ? from : to, side < 0 ? to : from})}, {"options", {{"width", 1.5}}}});
  }
  extrude(doc, "Slots", sketch(doc, "Slots", {{"shapes", slots}}), 4, "join", body);
  return body;
}

json at_plane(const std::string& body, double z) { return {{"body", body}, {"kind", "edge"}, {"select", {{"at_plane", {{"axis", "z"}, {"value", z}}}}}}; }
}  // namespace

// The call that took the app down: every top edge at the lobes' and the slots' heights, R0.5. The kernel faults on the
// lobes (OPAD_NO_KERNEL_GUARD=1 shows it: this test then crashes); under the guard it is a refusal naming edges.
json fillet_tops(const std::string& body, const std::string& radius) {
  return {{"kind", "fillet"}, {"name", "Soft tops"}, {"inputs", {{"edges", json::array({at_plane(body, 4.5), at_plane(body, 4)})}, {"radius", radius}}}};
}

std::string refusal(Document& doc, const json& args) {
  try {
    commands::run("feature", args, &doc);
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

TEST(fillet_that_faults_the_kernel_is_refused) {
  Document doc = Document::create();
  const std::string body = stand_in(doc);
  const Scene before = resolve(doc);
  CHECK(BRepCheck_Analyzer(node_world_shape(doc, before, body)).IsValid());
  const unsigned long faults = kernel_faults();
  const std::string message = refusal(doc, fillet_tops(body, "0.5 mm"));
  std::printf("fillet: %s (kernel faults %lu)\n", message.empty() ? "made" : message.c_str(), kernel_faults() - faults);
  CHECK(message.find("R0.5 is too large for these edges") != std::string::npos);
  CHECK(message.find("it does not fit at edges") != std::string::npos);  // named, though there are too many to try alone
  // Nothing changed, and the document goes on: a fillet that fits is made.
  CHECK_EQ(resolve(doc).node(body)->body_key, before.node(body)->body_key);
  const json bottom = {{"body", body}, {"kind", "edge"}, {"select", {{"at_plane", {{"axis", "z"}, {"value", 0}}}}}};
  commands::run("feature", {{"kind", "fillet"}, {"inputs", {{"edges", json::array({bottom})}, {"radius", "0.5 mm"}}}}, &doc);
  const Scene after = resolve(doc);
  CHECK(after.node(body)->body_key != before.node(body)->body_key);
  CHECK(BRepCheck_Analyzer(node_world_shape(doc, after, body)).IsValid());
}

// The live preview computes on a worker thread: the same refusal there, and this thread is untouched.
TEST(fillet_that_faults_the_kernel_is_refused_on_a_worker) {
  Document doc = Document::create();
  const std::string body = stand_in(doc);
  std::string message;
  std::thread worker([&] { message = refusal(doc, fillet_tops(body, "0.5 mm")); });
  worker.join();
  CHECK(message.find("R0.5 is too large for these edges") != std::string::npos);
  CHECK_EQ(*kernel_guarded([] { return 1; }), 1);
}

CHECK_MAIN()
