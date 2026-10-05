// What each feature kind asks for (the spec table the UI builds its forms from) and how it is computed.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BOPAlgo_Alerts.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_RectangularTrimmedSurface.hxx>
#include <GeomLib.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>

#include "engine.hpp"
#include "opad/design/sketch_reference.hpp"
#include "opad/checks.hpp"
#include "opad/geometry.hpp"

namespace opad::design {

// ---------------------------------------------------------------- specs
namespace {

InputSpec in(const char* name, const char* label, const char* type, json def = nullptr, const char* show_if = "", bool optional = false) {
  InputSpec s;
  s.name = name;
  s.label = label;
  s.type = type;
  s.def = std::move(def);
  s.show_if = show_if;
  s.optional = optional;
  if (optional) s.min_count = 0;
  return s;
}

InputSpec choice(const char* name, const char* label, std::vector<std::string> choices, const char* show_if = "") {
  InputSpec s = in(name, label, "choice", choices.front(), show_if);
  s.choices = std::move(choices);
  return s;
}

InputSpec pick(const char* name, const char* label, const char* type, int min_count, int max_count, const char* show_if = "") {
  InputSpec s = in(name, label, type, nullptr, show_if, min_count == 0);
  s.min_count = min_count;
  s.max_count = max_count;
  return s;
}

// A selection whose first pick hands over to the next input, as Fusion's Combine goes from the target to the tools (TODO 11
// P3); more can still be picked after clicking its box again.
InputSpec advancing(InputSpec s) {
  s.advance = true;
  return s;
}

// A face input that also takes an origin or construction plane (offered in the view beside the faces).
InputSpec with_planes(InputSpec s) {
  s.planes = true;
  return s;
}

void with_operation(std::vector<InputSpec>& v, const char* first = "new") {
  std::vector<std::string> ops = {"new", "join", "cut", "intersect"};
  std::rotate(ops.begin(), std::find(ops.begin(), ops.end(), first), ops.end());
  if (std::string(first) == "join") ops = {"join", "new", "cut", "intersect"};
  if (std::string(first) == "cut") ops = {"cut", "join", "new", "intersect"};
  v.push_back(choice("operation", "Operation", ops));
  v.push_back(pick("targets", "Bodies to change", "bodies", 0, 0, "operation=join|cut|intersect"));
}

std::vector<FeatureSpec> build_specs() {
  std::vector<FeatureSpec> v;
  auto add = [&](const char* kind, const char* label, const char* icon, const char* group, const char* hint, std::vector<InputSpec> inputs, const char* operation = nullptr) {
    if (operation) with_operation(inputs, operation);
    v.push_back({kind, label, icon, group, std::move(inputs), hint});
  };
  const json xy = {{"base", "xy"}};
  auto placed = [&](std::vector<InputSpec> rest) {
    std::vector<InputSpec> a = {in("plane", "Plane", "plane", xy), in("x", "Position X", "length", "0 mm"), in("y", "Position Y", "length", "0 mm")};
    a.insert(a.end(), rest.begin(), rest.end());
    return a;
  };
  add("box", "Box", "box", "create", "A box on a plane at the position: length along the plane's X, width along its Y, height along its normal (a negative height grows the other way). Centred in X and Y only.",
      placed({in("length", "Length", "length", "20 mm"), in("width", "Width", "length", "20 mm"), in("height", "Height", "length", "20 mm"), in("centered", "Centred on the position", "bool", true)}), "new");
  add("cylinder", "Cylinder", "cylinder", "create", "A cylinder standing on a plane at the position; its axis is the plane's normal (a negative height grows the other way).",
      placed({in("diameter", "Diameter", "length", "20 mm"), in("height", "Height", "length", "20 mm")}), "new");
  add("sphere", "Sphere", "sphere", "create", "A sphere centred on a plane.", placed({in("diameter", "Diameter", "length", "20 mm")}), "new");
  add("cone", "Cone", "cone", "create", "A cone or a truncated cone standing on a plane at the position; its axis is the plane's normal.",
      placed({in("diameter", "Base diameter", "length", "20 mm"), in("top_diameter", "Top diameter", "length", "0 mm"), in("height", "Height", "length", "20 mm")}), "new");
  add("torus", "Torus", "torus", "create", "A ring centred on a plane: the ring diameter runs through the middle of the tube.",
      placed({in("diameter", "Ring diameter", "length", "40 mm"), in("section", "Section diameter", "length", "10 mm")}), "new");
  // The profiles hand over to the next input still empty (Up to face, Start face), so a face clicked for those is not taken
  // as one more profile; more profiles: the Profiles box again.
  add("extrude", "Extrude", "extrude", "create", "Pull sketch profiles or planar faces along their normal. Symmetric splits the distance in half on each side; a start offset moves the start along the sketch normal.",
      {advancing(pick("profiles", "Profiles", "profiles", 1, 0)),choice("start", "Start from", {"profile", "offset", "face"}),
       in("start_offset", "Start offset", "length", "0 mm", "start=offset"),pick("start_face", "Start face", "faces", 1, 1, "start=face"),
       // A start face that is curved or tilted to the axis: the start follows it, or is flat at its nearest or farthest
       // contact with the extrusion, or the profiles go onto its plane as a derived sketch (Sketch on face).
       choice("start_shape", "Start at", {"follow_face", "nearest_contact", "farthest_contact", "sketch_on_face"}, "start=face"),
       in("face_offset", "Offset from face", "length", "0 mm", "start=face"),
       choice("direction", "Direction", {"one", "symmetric", "two"}), choice("extent", "Extent", {"distance", "all", "to_face", "to_body"}),
       with_planes(pick("extent_face", "Up to face", "faces", 1, 1, "extent=to_face")), pick("extent_body", "Up to body", "bodies", 1, 1, "extent=to_body"),
       in("extend", "Extend the face", "bool", true, "extent=to_face"), in("extent_offset", "End offset", "length", "0 mm", "extent=to_face|to_body"),
       // A target that is curved or tilted to the axis: the end follows it, or is flat where the extrusion first touches it
       // or where all of it has reached it.
       choice("extent_end", "End at", {"follow_face", "nearest_contact", "farthest_contact"}, "extent=to_face|to_body"),
       in("distance", "Distance", "length", "10 mm", "extent=distance"), in("distance2", "Distance, other side", "length", "10 mm", "direction=two"),
       in("taper", "Taper angle", "angle", "0 deg", "extent=distance"), in("flip", "Flip direction", "bool", false)},
      "new");
  // "auto": Fusion's automatic operation, decided from the bodies around (extrude_operation) and written into the op as the
  // operation it was taken as. Not the default of the command (agents and scripts keep "new"); the panel starts with it.
  for (auto& i : v.back().inputs)
    if (i.name == "operation") i.choices.push_back("auto");
  add("revolve", "Revolve", "revolve", "create", "Turn sketch profiles about an axis.",
      {pick("profiles", "Profiles", "profiles", 1, 0), in("axis", "Axis", "axis"), in("angle", "Angle", "angle", "360 deg"), in("symmetric", "Symmetric", "bool", false)}, "new");
  add("sweep", "Sweep", "sweep", "create", "Move a profile along a path.", {pick("profiles", "Profile", "profiles", 1, 1), in("path", "Path", "path")}, "new");
  add("loft", "Loft", "loft", "create", "Blend through two or more profiles, in the order they are picked.",
      {pick("profiles", "Profiles", "profiles", 2, 0), in("ruled", "Straight sides (ruled)", "bool", false)}, "new");
  add("pipe", "Pipe", "pipe", "create", "A round section along a path.",
      {in("path", "Path", "path"), in("diameter", "Diameter", "length", "10 mm"), in("hollow", "Hollow", "bool", false), in("thickness", "Wall thickness", "length", "1 mm", "hollow=true")}, "new");
  add("coil", "Coil", "coil", "create", "A round or square section wound about the plane's normal at the position: springs, threads.",
      placed({in("diameter", "Coil diameter", "length", "20 mm"), in("pitch", "Pitch", "length", "5 mm"), in("turns", "Turns", "number", "5"),
              choice("section", "Section", {"circle", "square"}), in("size", "Section size", "length", "2 mm"), in("left", "Left-handed", "bool", false)}),
      "new");
  add("thicken", "Thicken", "thicken", "create", "Give faces a thickness: a solid skin over the picked faces.",
      {pick("faces", "Faces", "faces", 1, 0), in("thickness", "Thickness", "length", "2 mm"), in("flip", "Other side", "bool", false)}, "new");
  add("hole", "Hole", "hole", "create", "Drill at sketch points, into the material behind the sketch.",
      {pick("points", "Sketch points", "points", 1, 0), choice("type", "Type", {"simple", "counterbore", "countersink"}), in("diameter", "Diameter", "length", "5 mm"),
       choice("extent", "Extent", {"distance", "all"}), in("depth", "Depth", "length", "10 mm", "extent=distance"), choice("tip", "Bottom", {"flat", "angled"}, "extent=distance"),
       in("tip_angle", "Tip angle", "angle", "118 deg", "tip=angled"), in("cb_diameter", "Counterbore diameter", "length", "9 mm", "type=counterbore"),
       in("cb_depth", "Counterbore depth", "length", "3 mm", "type=counterbore"), in("cs_diameter", "Countersink diameter", "length", "10 mm", "type=countersink"),
       in("cs_angle", "Countersink angle", "angle", "90 deg", "type=countersink"), in("flip", "Flip direction", "bool", false), pick("targets", "Bodies to drill", "bodies", 0, 0)});
  add("fillet", "Fillet", "fillet", "modify", "Round edges.", {pick("edges", "Edges", "edges", 1, 0), in("radius", "Radius", "length", "2 mm")});
  add("chamfer", "Chamfer", "chamfer", "modify", "Bevel edges.",
      {pick("edges", "Edges", "edges", 1, 0), choice("type", "Type", {"equal", "two"}), in("distance", "Distance", "length", "2 mm"), in("distance2", "Second distance", "length", "2 mm", "type=two")});
  add("shell", "Shell", "shell", "modify", "Hollow a body; the picked faces are removed and become openings.",
      {pick("faces", "Faces to remove", "faces", 0, 0), pick("bodies", "Bodies to hollow (no opening)", "bodies", 0, 0), in("thickness", "Thickness", "length", "1 mm"),
       choice("direction", "Direction", {"inside", "outside"})});
  add("draft", "Draft", "draft", "modify", "Tilt faces about a neutral plane, for moulds.",
      {pick("faces", "Faces", "faces", 1, 0), in("plane", "Neutral plane", "plane"), in("angle", "Angle", "angle", "3 deg")});
  add("offset_face", "Press pull", "presspull", "modify", "Push planar faces in or pull them out.", {pick("faces", "Faces", "faces", 1, 0), in("distance", "Distance", "length", "5 mm")});
  // TODO 11 UI-97: deleting a detail of an imported or dumb body is an op in the timeline.
  add("remove_faces", "Remove faces", "removeFaces", "modify",
      "Delete faces and close the gap by extending the faces around them (holes, fillets, chamfers, bosses, imported details).", {pick("faces", "Faces", "faces", 1, 0)});
  add("scale", "Scale", "scale", "modify", "Resize bodies uniformly.", {pick("bodies", "Bodies", "bodies", 1, 0), in("factor", "Factor", "number", "2"), choice("about", "About", {"origin", "centre"})});
  add("combine", "Combine", "combine", "combine", "Join, cut or intersect bodies.",
      {advancing(pick("target", "Target bodies", "bodies", 1, 0)), pick("tools", "Tool bodies", "bodies", 1, 0), choice("operation", "Operation", {"join", "cut", "intersect"}), in("keep_tools", "Keep tools", "bool", false)});
  add("split", "Split body", "split", "combine", "Cut bodies in two along a plane.", {pick("bodies", "Bodies", "bodies", 1, 0), in("plane", "Splitting plane", "plane")});
  add("mirror", "Mirror", "mirror", "pattern", "Mirrored copies of bodies.", {pick("bodies", "Bodies", "bodies", 1, 0), in("plane", "Mirror plane", "plane")}, "new");
  add("pattern_rect", "Rectangular pattern", "patternRect", "pattern", "Copies of bodies in rows and columns.",
      {pick("bodies", "Bodies", "bodies", 1, 0), in("axis", "Direction", "axis", json{{"base", "x"}}), in("count", "Count", "count", "3"), in("spacing", "Spacing", "length", "20 mm"),
       in("second", "Second direction", "bool", false), in("axis2", "Direction 2", "axis", json{{"base", "y"}}, "second=true"), in("count2", "Count 2", "count", "2", "second=true"),
       in("spacing2", "Spacing 2", "length", "20 mm", "second=true")},
      "new");
  add("pattern_circ", "Circular pattern", "patternCirc", "pattern", "Copies of bodies about an axis.",
      {pick("bodies", "Bodies", "bodies", 1, 0), in("axis", "Axis", "axis", json{{"base", "z"}}), in("count", "Count", "count", "6"), in("angle", "Total angle", "angle", "360 deg")}, "new");
  add("move", "Move / copy", "move", "body", "Move bodies by distances and an optional turn; the values can be expressions.",
      {pick("bodies", "Bodies", "bodies", 1, 0), in("dx", "X", "length", "0 mm"), in("dy", "Y", "length", "0 mm"), in("dz", "Z", "length", "0 mm"), in("rotate", "Rotate", "bool", false),
       in("axis", "Axis", "axis", json{{"base", "z"}}, "rotate=true"), in("angle", "Angle", "angle", "90 deg", "rotate=true"), in("copy", "Make a copy", "bool", false)});
  add("remove", "Remove", "delete", "body", "Take bodies out of the design from here on (history is kept).", {pick("bodies", "Bodies", "bodies", 1, 0)});
  add("plane", "Construction plane", "plane", "construct", "A plane to sketch on or to mirror, split and draft about.",
      {choice("mode", "Type", {"offset", "angle", "midplane", "three_points", "point_normal"}), in("plane", "From plane", "plane", xy, "mode=offset|angle|midplane"),
       in("distance", "Distance", "length", "10 mm", "mode=offset"), in("axis", "About axis", "axis", json{{"base", "x"}}, "mode=angle"), in("angle", "Angle", "angle", "45 deg", "mode=angle"),
       in("plane2", "Second plane", "plane", nullptr, "mode=midplane"), pick("points", "Three points", "points", 3, 3, "mode=three_points"),
       pick("point", "Through point", "points", 1, 1, "mode=point_normal"), in("normal", "Normal along", "axis", nullptr, "mode=point_normal")});
  add("axis", "Construction axis", "axis", "construct", "An axis to revolve or pattern about.",
      {choice("mode", "Type", {"edge", "two_points", "normal"}), in("edge", "Edge or round face", "axis", nullptr, "mode=edge"), pick("points", "Two points", "points", 2, 2, "mode=two_points"),
       in("plane", "Plane", "plane", xy, "mode=normal"), pick("point", "Through point", "points", 1, 1, "mode=normal")});
  // Gap log #10: a check kept in the timeline, run again whenever its bodies change.
  add("interference", "Interference check", "interference", "construct",
      "Bodies that overlap, or come closer than the clearance, stored with the design and checked again whenever they change. With Fail on, a finding is an error.",
      {pick("bodies", "Bodies (all solids if none)", "bodies", 0, 0), in("clearance", "Clearance", "length", "0 mm"), choice("fail_on", "Fail on", {"nothing", "interference", "clearance"})});
  // TODO 11 P1: the primitives are placed in the view by a click and sized by the pointer, as their guides show.
  for (auto& s : v) {
    if (s.kind == "box") s.footprint = "rect";
    if (s.kind == "cylinder" || s.kind == "cone" || s.kind == "sphere" || s.kind == "coil") s.footprint = "round";
    if (s.kind == "torus") s.footprint = "ring";
  }
  return v;
}

}  // namespace

const std::vector<FeatureSpec>& feature_specs() {
  static const std::vector<FeatureSpec> specs = build_specs();
  return specs;
}

const FeatureSpec* feature_spec(const std::string& kind) {
  for (const auto& s : feature_specs())
    if (s.kind == kind) return &s;
  return nullptr;
}

json feature_specs_json() {
  json out = json::array();
  for (const auto& s : feature_specs()) {
    json inputs = json::array();
    for (const auto& i : s.inputs) {
      json j = {{"name", i.name}, {"label", i.label}, {"type", i.type}};
      if (!i.def.is_null()) j["default"] = i.def;
      if (!i.choices.empty()) j["choices"] = i.choices;
      if (!i.show_if.empty()) j["show_if"] = i.show_if;
      if (i.optional) j["optional"] = true;
      if (i.type == "bodies" || i.type == "faces" || i.type == "edges" || i.type == "profiles" || i.type == "points") {
        j["min"] = i.min_count;
        if (i.max_count) j["max"] = i.max_count;
      }
      inputs.push_back(j);
    }
    out.push_back({{"kind", s.kind}, {"label", s.label}, {"group", s.group}, {"hint", s.hint}, {"inputs", inputs}});
  }
  return out;
}

// ---------------------------------------------------------------- geometry helpers
namespace {

gp_Pnt pnt(const Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }
gp_Vec vec(const Vec3& v) { return gp_Vec(v[0], v[1], v[2]); }

std::vector<TopoDS_Shape> solids_of(const TopoDS_Shape& s) {
  std::vector<TopoDS_Shape> out;
  for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) out.push_back(ex.Current());
  return out;
}

// A body's shape: its one solid, or a compound of several separate ones (three screws, the letters of a word). A body
// may hold several solids: joining material that does not touch it, or a cut that parts it, keeps them in one body.
TopoDS_Shape bundle(const std::vector<TopoDS_Shape>& solids) {
  if (solids.size() == 1) return solids.front();
  TopoDS_Compound c;
  BRep_Builder b;
  b.MakeCompound(c);
  for (const auto& s : solids) b.Add(c, s);
  return c;
}

double volume_of(const TopoDS_Shape& s) {
  GProp_GProps g;
  BRepGProp::VolumeProperties(s, g);
  return g.Mass();
}

TopoDS_Shape compound_of(const std::vector<TopoDS_Shape>& shapes) {
  if (shapes.size() == 1) return shapes.front();
  TopoDS_Compound c;
  BRep_Builder b;
  b.MakeCompound(c);
  for (const auto& s : shapes) b.Add(c, s);
  return c;
}

TopoDS_Shape healed(const TopoDS_Shape& s) {
  if (s.IsNull()) return s;
  if (BRepCheck_Analyzer(s).IsValid()) return s;
  ShapeFix_Shape fix(s);
  fix.Perform();
  return fix.Shape();
}

// A solid with negative volume is inside out (revolving the other way round, mirrored input).
TopoDS_Shape outward(const TopoDS_Shape& s) {
  if (s.ShapeType() == TopAbs_SOLID && volume_of(s) < 0) return s.Reversed();
  return s;
}

enum class BoolOp { Fuse, Cut, Common };

TopoDS_Shape boolean(BoolOp op, const TopoDS_Shape& a, const TopoDS_Shape& b) {
  TopTools_ListOfShape args, tools;
  args.Append(a);
  // A compound's parts go in as separate tools: OCCT merges overlapping tools, but parts that overlap inside one
  // compound argument made the cut a no-op (four counterbores 8.9 mm apart: "the cut does not touch any body").
  if (b.ShapeType() == TopAbs_COMPOUND)
    for (TopoDS_Iterator it(b); it.More(); it.Next()) tools.Append(it.Value());
  if (tools.IsEmpty()) tools.Append(b);
  auto run = [&](BRepAlgoAPI_BooleanOperation& algo, double fuzzy) -> std::optional<TopoDS_Shape> {
    algo.SetArguments(args);
    algo.SetTools(tools);
    algo.SetFuzzyValue(fuzzy);
    algo.SetRunParallel(Standard_True);
    algo.SetNonDestructive(Standard_True);
    algo.Build();
    if (!algo.IsDone() || algo.HasErrors()) return std::nullopt;
    algo.SimplifyResult(Standard_True, Standard_True);  // coplanar faces left by the cut are merged, like any CAD user expects
    return algo.Shape();
  };
  // Shapes that only touch or nearly coincide (a tool flush with a body's face, an extrusion ending on one, a tangent
  // round face) can defeat the kernel at the usual tolerance: tried again with a coarser one before giving up, rather than
  // handing the user the kernel's own message.
  for (const double fuzzy : {1e-6, 1e-5, 1e-4}) {
    try {
      std::optional<TopoDS_Shape> r;
      if (op == BoolOp::Fuse) { BRepAlgoAPI_Fuse f; r = run(f, fuzzy); }
      else if (op == BoolOp::Cut) { BRepAlgoAPI_Cut c; r = run(c, fuzzy); }
      else { BRepAlgoAPI_Common c; r = run(c, fuzzy); }
      if (r && !r->IsNull()) return *r;
    } catch (const Standard_Failure&) {
    }
  }
  throw Error("the boolean operation failed");
}

Bnd_Box box_of(const TopoDS_Shape& s) {
  Bnd_Box b;
  BRepBndLib::Add(s, b, Standard_False);
  return b;
}

// Body references -> distinct node ids, in pick order.
std::vector<std::string> body_ids(const Ctx& ctx, const json& refs) {
  std::vector<std::string> out;
  if (!refs.is_array()) return out;
  for (const auto& r : refs) {
    const std::string id = Ref::from_json(r).body;
    const Node* n = ctx.scene.node(id);
    if (!n) throw Error("a referenced body no longer exists");
    const std::vector<std::string> bodies = n->kind == Node::Kind::Body ? std::vector<std::string>{id} : ctx.scene.bodies_under(id);
    for (const auto& b : bodies)
      if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
  }
  return out;
}

// Sub-shape references grouped by the body they belong to, keeping first-pick order of the bodies.
std::vector<std::pair<std::string, std::vector<TopoDS_Shape>>> by_body(const Ctx& ctx, const json& refs, TopAbs_ShapeEnum type, const char* what) {
  std::vector<std::pair<std::string, std::vector<TopoDS_Shape>>> out;
  for (const ResolvedRef& r : ctx.resolve_all(refs)) {
    if (r.sub.ShapeType() != type) throw Error(std::string("pick ") + what);
    auto it = std::find_if(out.begin(), out.end(), [&](const auto& p) { return p.first == r.node; });
    if (it == out.end()) { out.push_back({r.node, {}}); it = out.end() - 1; }
    it->second.push_back(r.sub);
  }
  if (out.empty()) throw Error(std::string("pick ") + what);
  return out;
}

// The sub-shape of `body` that is the same TShape as `sub` (references resolve on a located copy).
TopoDS_Shape same_in(const TopoDS_Shape& body, const TopoDS_Shape& sub) {
  for (TopExp_Explorer ex(body, sub.ShapeType()); ex.More(); ex.Next())
    if (ex.Current().IsSame(sub)) return ex.Current();
  // A placed body (a node with a transform) is located anew by every lookup: the same entity then carries an equal
  // location that is another object, which IsSame does not accept. Match the TShape and the placement's value.
  const gp_Trsf want = sub.Location().Transformation();
  auto same_place = [&](const gp_Trsf& t) {
    for (int r = 1; r <= 3; ++r)
      for (int c = 1; c <= 4; ++c)
        if (std::fabs(t.Value(r, c) - want.Value(r, c)) > 1e-12) return false;
    return true;
  };
  for (TopExp_Explorer ex(body, sub.ShapeType()); ex.More(); ex.Next())
    if (ex.Current().IsPartner(sub) && same_place(ex.Current().Location().Transformation())) return ex.Current();
  throw Error("a referenced entity is not part of its body any more");
}

struct Profiles {
  std::vector<TopoDS_Face> faces;
  gp_Dir normal;
};

Profiles resolve_profiles(const Ctx& ctx, const json& refs) {
  Profiles out;
  if (!refs.is_array() || refs.empty()) throw Error("pick at least one profile");
  std::map<std::string, std::pair<Frame, std::vector<Region>>> cache;
  bool first = true;
  for (const auto& r : refs) {
    gp_Dir n;
    if (r.contains("sketch")) {
      const std::string id = r["sketch"].get<std::string>();
      auto it = cache.find(id);
      if (it == cache.end()) {
        Frame frame;
        const Sketch sk = ctx.sketch(id, &frame);
        it = cache.emplace(id, std::make_pair(frame, sketch_regions(sk, frame))).first;
        if (it->second.second.empty()) throw Error("the sketch has no closed profile");
      }
      const Frame& frame = it->second.first;
      const std::vector<Region>& regions = it->second.second;
      n = gp_Dir(vec(frame.normal()));
      if (r.value("all", false) || (!r.contains("at") && !r.contains("boundary"))) {
        for (const auto& g : regions) out.faces.push_back(g.face);
      } else {
        int i=-1;
        if(r.contains("boundary")) {
          // The region whose loops are these signed entity ids, as sketch_details lists them; in any order, and
          // without signs if that still names one region. A boundary alone used to take every region (gap log #8).
          auto boundary=r.at("boundary").get<std::vector<int>>();std::sort(boundary.begin(),boundary.end());
          auto unsigned_ids=[](std::vector<int> ids){for(int& id:ids)id=std::abs(id);std::sort(ids.begin(),ids.end());return ids;};
          for(const bool exact:{true,false}) {
            for(size_t k=0;k<regions.size();++k)if(exact?regions[k].boundary==boundary:unsigned_ids(regions[k].boundary)==unsigned_ids(boundary)) {
              if(i>=0)throw Error("a picked profile has become ambiguous; pick it again");
              i=int(k);
            }
            if(i>=0)break;
          }
          if(i<0 && !r.contains("at")) {  // a pick from the panel (with at) keeps the message below
            std::string listed;for(const auto& g:regions)listed+=(listed.empty()?"":", ")+json(g.boundary).dump();
            throw Error("no region of the sketch has the boundary "+json(boundary).dump()+"; its regions have "+listed);
          }
        } else i=region_at(regions,frame,r["at"][0].get<double>(),r["at"][1].get<double>());
        if (i < 0) throw Error("a picked profile no longer exists in its sketch");
        out.faces.push_back(regions[static_cast<size_t>(i)].face);
      }
    } else {
      const ResolvedRef f = ctx.resolve(r);
      if (f.sub.ShapeType() != TopAbs_FACE) throw Error("a profile must be a sketch region or a planar face");
      BRepAdaptor_Surface surf(TopoDS::Face(f.sub));
      if (surf.GetType() != GeomAbs_Plane) throw Error("only planar faces can be used as profiles");
      n = surf.Plane().Axis().Direction();
      if (!surf.Plane().Position().Direct()) n.Reverse();
      if (f.sub.Orientation() == TopAbs_REVERSED) n.Reverse();
      out.faces.push_back(TopoDS::Face(BRepBuilderAPI_Copy(f.sub).Shape()));
    }
    if (first) out.normal = n;
    first = false;
  }
  return out;
}

struct Points {
  std::vector<gp_Pnt> at;
  gp_Dir normal{0, 0, 1};
  bool from_sketch = false;
};

// A point in space written out, in any of the forms references take elsewhere: "point/x,y,z", {"point": [x, y, z]}
// (without a sketch) or [x, y, z].
std::optional<gp_Pnt> free_point(const json& r) {
  auto triple = [](const json& v) -> std::optional<gp_Pnt> {
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) return std::nullopt;
    return gp_Pnt(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
  };
  if (r.is_string() && r.get<std::string>().rfind("point/", 0) == 0) return pnt(Ref::parse(r.get<std::string>()).point);
  if (r.is_object() && !r.contains("sketch") && !r.contains("body") && r.contains("point")) return triple(r["point"]);
  return triple(r);
}

Points resolve_points(const Ctx& ctx, const json& refs) {
  Points out;
  if (!refs.is_array()) return out;
  for (const auto& r : refs) {
    if (r.contains("sketch")) {
      Frame frame;
      const Sketch sk = ctx.sketch(r["sketch"].get<std::string>(), &frame);
      const SkPoint* p = sk.point(r.value("point", 0));
      if (!p) throw Error("a picked sketch point no longer exists");
      out.at.push_back(pnt(frame.to_world(p->x, p->y)));
      out.normal = gp_Dir(vec(frame.normal()));
      out.from_sketch = true;
    } else if (r.is_object() && r.contains("kind") && r["kind"] == "point") {
      const Ref p = Ref::from_json(r);
      out.at.push_back(pnt(p.point));
    } else if (auto free = free_point(r)) {  // "point/x,y,z", {"point": [x, y, z]} or [x, y, z] (gap log #14)
      out.at.push_back(*free);
    } else {
      const ResolvedRef v = ctx.resolve(r);
      if (v.sub.ShapeType() != TopAbs_VERTEX) throw Error("pick sketch points or vertices");
      out.at.push_back(BRep_Tool::Pnt(TopoDS::Vertex(v.sub)));
    }
  }
  return out;
}

TopoDS_Wire resolve_path(const Ctx& ctx, const json& path) {
  if (path.is_object() && path.contains("sketch")) {
    Frame frame;
    const Sketch sk = ctx.sketch(path["sketch"].get<std::string>(), &frame);
    std::vector<int> ids;
    if (path.contains("entities"))
      for (const auto& e : path["entities"]) ids.push_back(e.get<int>());
    return sketch_wire(sk, frame, ids);
  }
  // Edges of bodies, chained.
  const json& refs = path.is_object() && path.contains("edges") ? path["edges"] : path;
  BRepBuilderAPI_MakeWire mw;
  TopTools_ListOfShape edges;
  for (const ResolvedRef& r : ctx.resolve_all(refs)) {
    if (r.sub.ShapeType() != TopAbs_EDGE) throw Error("the path must be a sketch or edges");
    edges.Append(r.sub);
  }
  if (edges.IsEmpty()) throw Error("pick a path");
  mw.Add(edges);
  if (!mw.IsDone()) throw Error("the path must be one connected chain of edges");
  return mw.Wire();
}

// Bodies an automatic operation or automatic targets may change: solid, not part of a linked file (read-only), not missing,
// not locked (UI-37), and when the feature is made in a component (Ctx::component) only those under it, as Fusion's active
// component; of them those whose box meets `nearby` (void: all).
std::vector<std::string> automatic_bodies(const Ctx& ctx, const Bnd_Box& nearby) {
  std::set<std::string> under;
  if (!ctx.component.empty())
    for (const auto& b : ctx.scene.bodies_under(ctx.component)) under.insert(b);
  std::vector<std::string> out;
  for (const auto& id : ctx.scene.all_bodies()) {
    const Node* n = ctx.scene.node(id);
    if (!n || n->representation != "solid" || n->linked || (n->body_missing && !ctx.fresh.count(n->body_key))) continue;
    if (ctx.scene.effectively_locked(id)) continue;
    if (!ctx.component.empty() && !under.count(id)) continue;
    if (nearby.IsVoid() || !box_of(ctx.node_shape(id)).IsOut(nearby)) out.push_back(id);
  }
  return out;
}

// The bodies an operation's automatic choice looks at: the named targets, else automatic_bodies near the tool.
std::vector<TopoDS_Shape> operation_bodies(const Ctx& ctx, const json& inputs, const TopoDS_Shape& tool) {
  Bnd_Box nearby = box_of(tool);
  nearby.Enlarge(1e-4);
  std::vector<std::string> ids = body_ids(ctx, inputs.value("targets", json::array()));
  if (ids.empty()) ids = automatic_bodies(ctx, nearby);
  std::vector<TopoDS_Shape> out;
  for (const auto& id : ids) out.push_back(ctx.node_shape(id));
  return out;
}

bool touches_any(const std::vector<TopoDS_Shape>& bodies, const TopoDS_Shape& tool) {
  for (const auto& b : bodies) {
    BRepExtrema_DistShapeShape d(b, tool, Extrema_ExtFlag_MIN);
    if (d.IsDone() && d.NbSolution() > 0 && d.Value() < 1e-6) return true;
  }
  return false;
}

// An "auto" operation of a feature that does not decide it itself (extrude_operation): Cut when most of the tool lies in the
// bodies, Join when it overlaps or touches one, else New body.
std::string tool_operation(const Ctx& ctx, const json& inputs, const TopoDS_Shape& tool) {
  const std::vector<TopoDS_Shape> bodies = operation_bodies(ctx, inputs, tool);
  if (bodies.empty()) return "new";
  const double whole = std::fabs(volume_of(tool));
  double inside = 0;
  for (const auto& b : bodies) {
    ctx.check_cancel();
    const TopoDS_Shape common = boolean(BoolOp::Common, b, tool);
    if (!solids_of(common).empty()) inside += std::fabs(volume_of(common));
  }
  if (whole > 0 && inside > 0.55 * whole) return "cut";
  return touches_any(bodies, tool) ? "join" : "new";
}

// The shared ending of every feature that makes material: new body, or join / cut / intersect with others. "auto" takes
// what out.operation says (decided by the feature), else tool_operation decides. Out::tool and Out::operation record it.
void apply_operation(const Ctx& ctx, const json& inputs, const TopoDS_Shape& tool_in, Out& out) {
  const TopoDS_Shape tool = healed(tool_in);
  if (solids_of(tool).empty()) throw Error("the operation produced no solid");
  std::string op = inputs.value("operation", "new");
  if (op == "auto") op = !out.operation.empty() ? out.operation : tool_operation(ctx, inputs, tool);
  if (op != "new" && op != "join" && op != "cut" && op != "intersect") throw Error("unknown operation \"" + op + "\": new, join, cut, intersect or auto");
  out.operation = op;
  out.tool = tool;
  if (op == "new") {
    for (const auto& s : solids_of(tool)) out.bodies.push_back({"", outward(s)});
    return;
  }
  std::vector<std::string> targets = body_ids(ctx, inputs.value("targets", json::array()));
  const bool automatic = targets.empty();
  if (automatic) targets = automatic_bodies(ctx, box_of(tool));
  if (op == "join") {
    TopoDS_Shape acc = tool;
    std::string owner;
    for (const auto& id : targets) {
      ctx.check_cancel();
      const TopoDS_Shape body = ctx.node_shape(id);
      const TopoDS_Shape fused = boolean(BoolOp::Fuse, body, acc);
      // Bodies picked by the automatic search join only where the material touches them; named targets take it
      // anyway and hold the separate pieces as one body of several solids.
      if (automatic && solids_of(fused).size() >= solids_of(body).size() + solids_of(acc).size()) continue;
      acc = fused;
      out.used_targets.push_back(id);
      if (owner.empty()) owner = id;
      else out.removed.push_back(id);  // joined bodies become one
    }
    const auto pieces = solids_of(acc);
    if (owner.empty()) {
      for (const auto& s : pieces) out.bodies.push_back({"", outward(s)});
      return;
    }
    out.bodies.push_back({owner, bundle(pieces)});
    return;
  }
  bool any = false;
  for (const auto& id : targets) {
    ctx.check_cancel();
    const TopoDS_Shape body = ctx.node_shape(id);
    const TopoDS_Shape r = boolean(op == "cut" ? BoolOp::Cut : BoolOp::Common, body, tool);
    const auto pieces = solids_of(r);
    const double before = volume_of(body), after = pieces.empty() ? 0.0 : volume_of(r);
    if (op == "cut" && std::fabs(before - after) <= 1e-9 * std::max(1.0, std::fabs(before))) continue;  // the tool misses this body
    if (op == "intersect" && pieces.empty() && automatic) continue;
    any = true;
    out.used_targets.push_back(id);
    if (pieces.empty()) {
      out.removed.push_back(id);
      continue;
    }
    out.bodies.push_back({id, bundle(pieces)});  // parted by the cut: the pieces stay one body
  }
  if (!any) throw Error(op == "cut" ? "the cut does not touch any body" : "nothing intersects");
}

gp_Trsf placement(const Frame& f, double x, double y) {
  gp_Trsf t;
  t.SetTransformation(gp_Ax3(pnt(f.to_world(x, y)), gp_Dir(vec(f.normal())), gp_Dir(vec(f.x))), gp_Ax3());
  return t;
}

TopoDS_Shape moved(const TopoDS_Shape& s, const gp_Trsf& t) { return BRepBuilderAPI_Transform(s, t, Standard_True).Shape(); }

TopoDS_Face big_plane_face(const Frame& f, double half) {
  return BRepBuilderAPI_MakeFace(frame_plane(f), -half, half, -half, half).Face();
}

double scene_reach(const Ctx& ctx, const TopoDS_Shape& extra) {
  Bnd_Box box = box_of(extra);
  for (const auto& id : ctx.scene.all_bodies()) {
    const Node* n = ctx.scene.node(id);
    if (n && (!n->body_missing || ctx.fresh.count(n->body_key))) box.Add(box_of(ctx.node_shape(id)));
  }
  if (box.IsVoid()) return 1000.0;
  return std::sqrt(box.SquareExtent()) * 2.0 + 10.0;
}

// ---------------------------------------------------------------- features
double extrusion_start(const Ctx& ctx,const json& in,const gp_Vec& normal,const gp_Pnt& center) {
  const auto mode=in.value("start","profile");
  if(mode=="profile")return 0;
  if(mode=="offset")return ctx.length(in,"start_offset");
  if(mode!="face")throw Error("unknown extrusion start mode");
  const auto& refs=in.at("start_face");if(!refs.is_array()||refs.size()!=1)throw Error("pick one planar start face");
  const auto frame=ctx.plane({{"face",refs.front()}});const gp_Vec other(vec(frame.normal()));
  if(std::abs(std::abs(other.Dot(normal))-1)>1e-7)throw Error("the start face must be perpendicular to the extrusion axis");
  return gp_Vec(center,pnt(frame.origin)).Dot(normal)+(in.contains("face_offset")?ctx.length(in,"face_offset"):0.0);
}

// Points inside a planar face, spread over it (a grid over its parameter box, a finer one for a thin face): where an
// extrusion looks for material ahead and from where it is sure to start inside its own solid (a ring's centre is not).
std::vector<gp_Pnt> face_samples(const TopoDS_Face& face) {
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  BRepTopAdaptor_FClass2d inside(face, 1e-7);
  BRepAdaptor_Surface surface(face);
  std::vector<gp_Pnt> out;
  for (int grid : {5, 17}) {
    for (int i = 0; i < grid; ++i)
      for (int j = 0; j < grid; ++j) {
        const gp_Pnt2d uv(u0 + (u1 - u0) * (i + 0.5) / grid, v0 + (v1 - v0) * (j + 0.5) / grid);
        if (inside.Perform(uv) == TopAbs_IN) out.push_back(surface.Value(uv.X(), uv.Y()));
      }
    if (!out.empty()) break;
  }
  if (out.empty()) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(face, g);
    out.push_back(g.CentreOfMass());
  }
  return out;
}

// Classifies points against a set of solids, each loaded once.
struct Inside {
  std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> solids;
  explicit Inside(const std::vector<TopoDS_Shape>& shapes) {
    for (const auto& s : shapes)
      for (const auto& solid : solids_of(s)) solids.push_back(std::make_unique<BRepClass3d_SolidClassifier>(solid));
  }
  bool operator()(const gp_Pnt& p) const {
    for (const auto& c : solids) {
      c->Perform(p, 1e-7);
      if (c->State() == TopAbs_IN) return true;
    }
    return false;
  }
};

// The operation an extrusion takes by itself (Fusion's automatic operation): Cut when it starts into material (points spread
// over the profiles, just ahead of them on each side the extrusion goes, are mostly inside a body), Join when the extrusion
// overlaps or touches a body, else New body. The bodies are the named targets, else those automatic targets may take (in the
// component the feature is made in). Works the same for every extent and direction: the sides it goes are read off the tool.
std::string extrude_operation(const Ctx& ctx, const json& in, const Profiles& prof, const TopoDS_Shape& tool, const std::vector<double>& starts) {
  const std::vector<TopoDS_Shape> bodies = operation_bodies(ctx, in, tool);
  if (bodies.empty()) return "new";
  const Bnd_Box tb = box_of(tool);
  const double eps = std::clamp(std::sqrt(tb.SquareExtent()) * 1e-3, 1e-5, 0.05);
  gp_Vec n(prof.normal);
  if (in.value("flip", false)) n.Reverse();
  const Inside in_tool({tool}), in_material(bodies);
  int ahead = 0, into = 0;
  for (size_t i = 0; i < prof.faces.size(); ++i) {
    ctx.check_cancel();
    const TopoDS_Face& face = prof.faces[i];
    const double start = i < starts.size() ? starts[i] : 0.0;
    for (const gp_Pnt& on : face_samples(face)) {
      const gp_Pnt p = on.Translated(n * start);
      for (const double side : {1.0, -1.0}) {
        const gp_Pnt q = p.Translated(n * (side * eps));
        if (!in_tool(q)) continue;  // the extrusion does not go this way
        ++ahead;
        if (in_material(q)) ++into;
      }
    }
  }
  if (ahead > 0 && into * 2 > ahead) return "cut";
  return touches_any(bodies, tool) ? "join" : "new";
}

// How far the material inside `path` reaches along `dir` beyond the plane through `from` (normal `dir`): the farthest
// point of the bodies' parts within the path. Negative: no body lies in it.
double material_reach(const Ctx& ctx, const std::vector<TopoDS_Shape>& bodies, const TopoDS_Shape& path, const gp_Pnt& from, const gp_Dir& dir) {
  const Bnd_Box pb = box_of(path);
  gp_Trsf local;
  local.SetTransformation(gp_Ax3(from, dir));  // world -> a frame whose Z is the extrusion's direction
  auto top = [&local](const TopoDS_Shape& s) {
    Bnd_Box box;
    BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(s, local, Standard_True).Shape(), box, Standard_False, Standard_False);
    if (box.IsVoid()) return -1.0;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    return z1;
  };
  double farthest = -1;
  for (const auto& b : bodies) {
    ctx.check_cancel();
    if (box_of(b).IsOut(pb)) continue;
    try {
      const TopoDS_Shape within = boolean(BoolOp::Common, b, path);
      if (solids_of(within).empty()) continue;
      farthest = std::max(farthest, top(within));
    } catch (const Standard_Failure&) {  // the kernel cannot intersect them (faces that only touch): the whole body counts
      farthest = std::max(farthest, top(b));
    } catch (const Error&) {
      farthest = std::max(farthest, top(b));
    }
  }
  return farthest;
}

// To all (Fusion's All): through every body in the way, ending where the last of them ends along the extrusion, on each side
// when it goes two ways (symmetric: the farther of the two both ways), instead of running on for twice the model's size: a
// cut goes through all of them, a join fills up to the farthest material. The bodies are the named targets of a join, cut or
// intersect, else every body automatic targets may take. Refused when nothing is in the way.
std::pair<double, double> through_all(const Ctx& ctx, const json& in, const Profiles& prof, const gp_Vec& n, double reach, const std::string& direction,
                                      const std::vector<double>& starts) {
  std::vector<std::string> ids;
  if (in.value("operation", "new") != "new") ids = body_ids(ctx, in.value("targets", json::array()));
  if (ids.empty()) ids = automatic_bodies(ctx, Bnd_Box());
  std::vector<TopoDS_Shape> bodies;
  for (const auto& id : ids) bodies.push_back(ctx.node_shape(id));
  const bool both = direction != "one";
  double ahead = -1, behind = -1;
  for (size_t i = 0; i < prof.faces.size(); ++i) {
    ctx.check_cancel();
    const TopoDS_Face& face = prof.faces[i];
    GProp_GProps g;
    BRepGProp::SurfaceProperties(face, g);
    const double start = starts.at(i);
    gp_Trsf to_start;
    to_start.SetTranslation(n * start);
    const TopoDS_Shape base = moved(face, to_start);
    const gp_Pnt from = g.CentreOfMass().Translated(n * start);
    ahead = std::max(ahead, material_reach(ctx, bodies, BRepPrimAPI_MakePrism(base, n * reach, Standard_True).Shape(), from, gp_Dir(n)));
    if (both) behind = std::max(behind, material_reach(ctx, bodies, BRepPrimAPI_MakePrism(base, -n * reach, Standard_True).Shape(), from, gp_Dir(-n)));
  }
  if (ahead <= 1e-7 && behind <= 1e-7)
    throw Error(both ? "nothing lies in the extrusion's way on either side: To all goes through the bodies in the way; extrude to a distance instead"
                     : "nothing lies ahead of the profile: To all goes through the bodies in the way; flip the direction or extrude to a distance");
  ahead = std::max(ahead, 0.0);
  behind = std::max(behind, 0.0);
  if (direction == "symmetric") ahead = behind = std::max(ahead, behind);
  return {ahead, behind};
}

// A face's surface carried on past its edges (Fusion's Extend faces, for To face): round directions all the way round,
// straight ones (a cylinder's length, an extrusion's, a cone's sides away from its apex) `by` beyond the face, a B-spline
// patch lengthened by `by` on each open side. The face itself when its surface cannot be extended.
TopoDS_Face extended_face(const TopoDS_Face& face, double by) {
  Handle(Geom_Surface) surface = BRep_Tool::Surface(face);  // with the face's placement
  if (surface.IsNull()) return face;
  for (Handle(Geom_RectangularTrimmedSurface) t = Handle(Geom_RectangularTrimmedSurface)::DownCast(surface); !t.IsNull();
       t = Handle(Geom_RectangularTrimmedSurface)::DownCast(surface))
    surface = t->BasisSurface();
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  double U0 = 0, U1 = 0, V0 = 0, V1 = 0;
  try {
    if (Handle(Geom_BSplineSurface) spline = Handle(Geom_BSplineSurface)::DownCast(surface); !spline.IsNull()) {
      Handle(Geom_BoundedSurface) longer = Handle(Geom_BoundedSurface)::DownCast(spline->Copy());
      for (const bool inU : {true, false})
        for (const bool after : {true, false})
          if (!(inU ? longer->IsUPeriodic() : longer->IsVPeriodic())) GeomLib::ExtendSurfByLength(longer, by, 1, inU, after);
      surface = longer;
      surface->Bounds(U0, U1, V0, V1);
    } else {
      surface->Bounds(U0, U1, V0, V1);
      auto range = [by](bool periodic, double period, double lo, double hi, double a, double b) {
        if (periodic) return std::make_pair(a, a + period);
        return std::make_pair(std::max(lo, a - by), std::min(hi, b + by));
      };
      const auto u = range(surface->IsUPeriodic(), surface->IsUPeriodic() ? surface->UPeriod() : 0.0, U0, U1, u0, u1);
      auto v = range(surface->IsVPeriodic(), surface->IsVPeriodic() ? surface->VPeriod() : 0.0, V0, V1, v0, v1);
      if (Handle(Geom_ConicalSurface) cone = Handle(Geom_ConicalSurface)::DownCast(surface); !cone.IsNull()) {
        const double apex = -cone->RefRadius() / std::sin(cone->SemiAngle());  // the cone's sides stop short of its apex
        const double gap = std::max(1e-6, (v1 - v0) * 1e-3);
        if (v0 >= apex) v.first = std::max(v.first, apex + gap);
        else v.second = std::min(v.second, apex - gap);
      }
      U0 = u.first, U1 = u.second, V0 = v.first, V1 = v.second;
    }
    BRepBuilderAPI_MakeFace make(surface, U0, U1, V0, V1, 1e-7);
    if (make.IsDone()) return make.Face();
  } catch (const Standard_Failure&) {
  }
  return face;
}

// What an extrusion goes up to (TODO 10 B8, and what Fusion's To object takes): a plane (an origin or construction plane,
// a planar face's plane when Extend is on, the plane through a vertex or a sketch point parallel to the profile), a face
// (curved, or planar with Extend off) or a body.
struct UpTo {
  std::optional<gp_Pln> plane;
  TopoDS_Shape shape;
  bool body = false;
  bool extended = false;  // `shape` is a face carried on past its edges
  TopoDS_Shape wide;      // a face: its surface carried on (tried when the face itself does not cover the profile)
};

// The targets to try in turn: the face itself, then (when there is one) its surface carried on past its edges.
std::vector<UpTo> tries_of(const UpTo& target) {
  std::vector<UpTo> out;
  out.reserve(2);
  out.push_back(target);
  if (!target.wide.IsNull()) {
    out.push_back(target);
    out.back().shape = target.wide;
    out.back().wide.Nullify();
    out.back().extended = true;
  }
  return out;
}

// Heights along `dir` above the plane through `from` square to it: the lowest and the highest point of a shape.
std::pair<double, double> heights(const TopoDS_Shape& s, const gp_Pnt& from, const gp_Dir& dir) {
  gp_Trsf local;
  local.SetTransformation(gp_Ax3(from, dir));
  Bnd_Box b;
  BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(s, local, Standard_True).Shape(), b, Standard_False, Standard_False);
  if (b.IsVoid()) return {0.0, 0.0};
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return {z0, z1};
}

// A solid split by a target surface (a plane: a square of it larger than the model) into its pieces; tried at coarser
// tolerances as boolean() is. Empty when the kernel cannot.
std::vector<TopoDS_Shape> split_by(const TopoDS_Shape& solid, const UpTo& surface, double reach) {
  const TopoDS_Shape tool = surface.plane ? TopoDS_Shape(BRepBuilderAPI_MakeFace(*surface.plane, -4 * reach, 4 * reach, -4 * reach, 4 * reach).Face()) : surface.shape;
  for (const double fuzzy : {1e-6, 1e-5, 1e-4}) {
    try {
      BRepAlgoAPI_Splitter split;
      TopTools_ListOfShape arguments, tools;
      arguments.Append(solid);
      tools.Append(tool);
      split.SetArguments(arguments);
      split.SetTools(tools);
      split.SetFuzzyValue(fuzzy);
      split.SetNonDestructive(Standard_True);
      split.Build();
      if (split.IsDone() && !split.HasErrors()) return solids_of(split.Shape());
    } catch (const Standard_Failure&) {
    }
  }
  return {};
}

UpTo up_to_target(const Ctx& ctx, const json& in, const gp_Dir& normal, double reach) {
  UpTo t;
  if (in.value("extent", "") == "to_body") {
    const auto bodies = ctx.resolve_all(in.value("extent_body", json()));
    if (bodies.size() != 1) throw Error("pick the body the extrusion goes up to");
    t.shape = ctx.node_shape(bodies[0].node);
    t.body = true;
    return t;
  }
  const json refs = in.value("extent_face", json());
  const json one = refs.is_array() ? (refs.size() == 1 ? refs[0] : json()) : refs;
  if (one.is_null()) throw Error("pick the face the extrusion goes up to");
  if (one.is_object() && !one.contains("body") && (one.contains("base") || one.contains("feature") || one.contains("face") || one.contains("normal"))) {
    const Frame f = ctx.plane(one);  // an origin plane, a construction plane, a face's plane
    t.plane = gp_Pln(pnt(f.origin), gp_Dir(vec(f.normal())));
    return t;
  }
  if (one.is_object() && one.contains("sketch") && one.contains("point")) {
    t.plane = gp_Pln(resolve_points(ctx, json::array({one})).at.at(0), normal);
    return t;
  }
  if (const auto free = free_point(one)) {
    t.plane = gp_Pln(*free, normal);
    return t;
  }
  const ResolvedRef r = ctx.resolve(one);
  if (r.sub.ShapeType() == TopAbs_VERTEX) {
    t.plane = gp_Pln(BRep_Tool::Pnt(TopoDS::Vertex(r.sub)), normal);
    return t;
  }
  if (r.sub.ShapeType() != TopAbs_FACE) throw Error("the extrusion goes up to a face, a plane, a vertex or a body");
  const TopoDS_Face face = TopoDS::Face(r.sub);
  const bool extend = in.value("extend", true);
  BRepAdaptor_Surface surface(face);
  if (surface.GetType() == GeomAbs_Plane && extend) {
    t.plane = surface.Plane();
    return t;
  }
  t.shape = face;
  if (extend) t.wide = extended_face(face, reach);
  return t;
}

// The extrusion of `base` along `dir` cut back to end at the target moved `offset` along `dir`: the piece that starts at the
// profile, which must stop short of the far end everywhere. Null, with why, when it does not get there this way. A plane
// parallel to the profile ends a plain prism at its distance (no Boolean at all); a tilted plane keeps the half-space on the
// profile's side; a face splits the long prism; a body is cut out of it. What the kernel cannot do (shapes that only touch
// or run along each other) is said in words, never as the kernel's own message.
TopoDS_Shape trim_to(const UpTo& target, const TopoDS_Shape& base, const gp_Dir& dir, double reach, double offset, const std::string& end, std::string& why) {
  const TopoDS_Face base_face = TopoDS::Face(TopExp_Explorer(base, TopAbs_FACE).Current());
  const gp_Pnt inside = face_samples(base_face).front();
  double eps = 1e-3;
  gp_Trsf shift;
  shift.SetTranslation(gp_Vec(dir) * offset);
  try {
    TopoDS_Shape trimmed;
    if (target.plane) {
      const gp_Pln plane = target.plane->Translated(gp_Vec(dir) * offset);
      const gp_Vec normal(plane.Axis().Direction());
      const double along = gp_Vec(dir).Dot(normal);
      if (std::fabs(along) < 1e-9) {
        why = "the extrusion runs parallel to that plane and never reaches it";
        return {};
      }
      // Where the plane is along the extrusion from each corner of the profile: behind it or on it, this way does not get
      // there; ahead of some corners and behind others, it cuts through the profile.
      double nearest = 1e300, farthest = -1e300;
      for (TopExp_Explorer v(base, TopAbs_VERTEX); v.More(); v.Next()) {
        const double at = gp_Vec(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())), plane.Location()).Dot(normal) / along;
        nearest = std::min(nearest, at), farthest = std::max(farthest, at);
      }
      const double at = gp_Vec(inside, plane.Location()).Dot(normal) / along;
      nearest = std::min(nearest, at), farthest = std::max(farthest, at);
      if (farthest <= 1e-7) {
        if (nearest > -1e-7) why = "the profile lies on that plane";
        return {};
      }
      if (nearest < -1e-7) {
        why = "that plane cuts through the profile, so the extrusion would end on both sides of it; extrude to a distance or pick a plane clear of the profile";
        return {};
      }
      if (std::fabs(std::fabs(along) - 1) < 1e-9) return BRepPrimAPI_MakePrism(base, gp_Vec(dir) * at, Standard_True).Shape();  // parallel: a plain prism
      eps = std::min(eps, std::max(at * 0.5, 1e-7));  // the start point stays short of the plane
      const TopoDS_Shape far_prism = BRepPrimAPI_MakePrism(base, gp_Vec(dir) * reach, Standard_True).Shape();
      // The half-space on the profile's side, told by a point behind the profile (never on the plane).
      const gp_Pnt behind = inside.Translated(gp_Vec(dir) * -std::max(1.0, reach * 1e-3));
      BRepPrimAPI_MakeHalfSpace side(BRepBuilderAPI_MakeFace(plane).Face(), behind);
      if (!side.IsDone()) {
        why = "the extrusion cannot be ended on that plane";
        return {};
      }
      trimmed = boolean(BoolOp::Common, far_prism, side.Solid());
    } else if (target.body) {
      const TopoDS_Shape far_prism = BRepPrimAPI_MakePrism(base, gp_Vec(dir) * reach, Standard_True).Shape();
      const TopoDS_Shape body = offset != 0 ? moved(target.shape, shift) : target.shape;
      if (BRepClass3d_SolidClassifier(body, inside.Translated(gp_Vec(dir) * eps), 1e-7).State() == TopAbs_IN) {
        why = "the profile starts inside that body";
        return {};
      }
      trimmed = boolean(BoolOp::Cut, far_prism, body);
    } else {
      const TopoDS_Shape far_prism = BRepPrimAPI_MakePrism(base, gp_Vec(dir) * reach, Standard_True).Shape();
      const TopoDS_Shape face = offset != 0 ? moved(target.shape, shift) : target.shape;
      for (const double fuzzy : {1e-6, 1e-5, 1e-4}) {  // as boolean(): a tangent face can need a coarser tolerance
        BRepAlgoAPI_Splitter split;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(far_prism);
        tools.Append(face);
        split.SetArguments(arguments);
        split.SetTools(tools);
        split.SetFuzzyValue(fuzzy);
        split.SetNonDestructive(Standard_True);
        split.Build();
        if (split.IsDone() && !split.HasErrors()) {
          trimmed = split.Shape();
          break;
        }
      }
      if (trimmed.IsNull()) {
        why = "the extrusion cannot be cut at that face (it only touches the extrusion's path, or runs along it); pick another face or extrude to a distance";
        return {};
      }
    }
    const gp_Pnt start = inside.Translated(gp_Vec(dir) * eps);
    TopoDS_Shape kept;
    for (const auto& piece : solids_of(trimmed))
      if (BRepClass3d_SolidClassifier(piece, start, 1e-7).State() == TopAbs_IN) kept = piece;
    if (kept.IsNull()) return {};
    // Reaching the far end anywhere: part of the profile passes the target.
    gp_Trsf local;
    local.SetTransformation(gp_Ax3(gp_Pnt(0, 0, 0), dir));
    Bnd_Box box;
    BRepBndLib::Add(BRepBuilderAPI_Transform(kept, local, Standard_True).Shape(), box, Standard_False);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    Bnd_Box from;
    BRepBndLib::Add(BRepBuilderAPI_Transform(base, local, Standard_True).Shape(), from, Standard_False);
    double a0, b0, c0, a1, b1, c1;
    from.Get(a0, b0, c0, a1, b1, c1);
    if (z1 - c0 > reach * 0.999) {
      // The body was not met at all this way: not reached (the other way may); met only in part: it misses.
      if (target.body && std::fabs(volume_of(kept) - volume_of(BRepPrimAPI_MakePrism(base, gp_Vec(dir) * reach, Standard_True).Shape())) < 1e-9 * std::max(1.0, volume_of(kept))) return {};
      why = target.shape.IsNull() || target.body ? "part of the profile misses the target, so the extrusion would not stop; extrude to a distance instead"
            : target.extended ? "part of the profile misses that face even carried on past its edges, so the extrusion would not stop; pick a larger face or extrude to a distance"
                              : "part of the profile misses that face, so the extrusion would not stop: turn on Extend the face, or extrude to a distance";
      return {};
    }
    // End at the nearest or the farthest contact: a flat end square to the axis where the extrusion first touches the target
    // (the lowest of the end faces, those clear of the profile's plane), or where all of it has reached it (the highest point).
    if (end == "nearest_contact" || end == "farthest_contact") {
      const auto [k0, k1] = heights(kept, inside, dir);
      double h = k1;
      if (end == "nearest_contact")
        for (TopExp_Explorer f(kept, TopAbs_FACE); f.More(); f.Next()) {
          const double low = heights(f.Current(), inside, dir).first;
          if (low > 1e-6 * std::max(1.0, k1)) h = std::min(h, low);
        }
      if (h <= 1e-7) {
        why = "the target touches the profile, so the extrusion would have no length there; end at the farthest contact or follow the face";
        return {};
      }
      return BRepPrimAPI_MakePrism(base, gp_Vec(dir) * h, Standard_True).Shape();
    }
    return kept;
  } catch (const Standard_Failure&) {
  } catch (const Error&) {  // boolean(): the kernel gave up at every tolerance
  }
  why = target.body ? "the extrusion cannot be stopped at that body: it only touches the extrusion's path, or runs along it; pick another target or extrude to a distance"
                    : "the extrusion cannot be stopped at that target: it only touches the extrusion's path, or runs along it; pick another target or extrude to a distance";
  return {};
}

// Up to the target (To face, To body), going whichever way reaches it: the extrusion's direction (Flip direction) first,
// then the other, as Fusion's To object goes towards the object. Refused, saying why, when neither way does.
TopoDS_Shape up_to(const Ctx& ctx, const json& in, const TopoDS_Shape& base, const gp_Vec& n, double reach) {
  const UpTo target = up_to_target(ctx, in, gp_Dir(n), reach);
  const double offset = in.contains("extent_offset") ? ctx.length(in, "extent_offset") : 0.0;
  const std::string end = in.value("extent_end", "follow_face");
  const std::vector<UpTo> tries = tries_of(target);  // the face itself, then carried on past its edges
  std::string why;
  for (const gp_Dir dir : {gp_Dir(n), gp_Dir(-n)}) {
    std::string reason;
    for (const UpTo& t : tries) {
      reason.clear();
      const TopoDS_Shape kept = trim_to(t, base, dir, reach, offset, end, reason);
      if (!kept.IsNull()) return kept;
    }
    if (why.empty()) why = reason;
  }
  if (!why.empty()) throw Error(why);
  throw Error(target.body ? "the extrusion does not reach that body from this profile" : "the extrusion does not reach that target from this profile");
}

// A point inside a face and the face's normal there (out of its body): the first of its spread samples.
std::pair<gp_Pnt, gp_Dir> face_tangent(const TopoDS_Face& face) {
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  BRepTopAdaptor_FClass2d inside(face, 1e-7);
  gp_Pnt2d uv((u0 + u1) / 2, (v0 + v1) / 2);
  if (inside.Perform(uv) != TopAbs_IN)
    for (int i = 0; i < 25; ++i) {
      const gp_Pnt2d q(u0 + (u1 - u0) * (i % 5 + 0.5) / 5, v0 + (v1 - v0) * (i / 5 + 0.5) / 5);
      if (inside.Perform(q) == TopAbs_IN) {
        uv = q;
        break;
      }
    }
  BRepAdaptor_Surface surface(face);
  gp_Pnt p;
  gp_Vec du, dv;
  surface.D1(uv.X(), uv.Y(), p, du, dv);
  gp_Vec n = du.Crossed(dv);
  if (n.Magnitude() < 1e-12) throw Error("the start face has no normal where it is picked");
  if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
  return {p, gp_Dir(n)};
}

TopoDS_Face picked_start_face(const Ctx& ctx, const json& in) {
  const json& refs = in.at("start_face");
  if (!refs.is_array() || refs.size() != 1) throw Error("pick one start face");
  const ResolvedRef r = ctx.resolve(refs.front());
  if (r.sub.ShapeType() != TopAbs_FACE) throw Error("the start must be a face");
  return TopoDS::Face(r.sub);
}

// Start from: Face. A planar face square to the axis is a start distance (extrusion_start); any other face (tilted, curved) is
// a surface the start follows or touches: a plane, or the face and its surface carried on past its edges.
std::optional<UpTo> start_surface(const Ctx& ctx, const json& in, const gp_Vec& n, double reach) {
  if (in.value("start", "profile") != "face") return std::nullopt;
  const TopoDS_Face face = picked_start_face(ctx, in);
  BRepAdaptor_Surface surface(face);
  if (surface.GetType() == GeomAbs_Plane && std::fabs(std::fabs(surface.Plane().Axis().Direction().Dot(gp_Dir(n))) - 1) < 1e-7) return std::nullopt;
  UpTo t;
  if (surface.GetType() == GeomAbs_Plane) {
    if (std::fabs(surface.Plane().Axis().Direction().Dot(gp_Dir(n))) < 1e-7) throw Error("the start face runs along the extrusion, so it is never met");
    t.plane = surface.Plane();
  } else {
    t.shape = face;
    t.wide = extended_face(face, reach);
  }
  return t;
}

// Where a start face meets the profile's extrusion along `dir`, as heights from the profile's plane: the nearest and the
// farthest contact, and the part of a long prism past the face, whose near end follows the face. Coming from far behind the
// profile, the first crossing of the face is the start (a round face may be crossed again further on). The face itself first;
// carried on past its edges when it does not cover the profile.
struct Contact {
  double nearest = 0, farthest = 0;
  TopoDS_Shape beyond;
};

Contact start_contact(const TopoDS_Face& profile, const UpTo& surface, const gp_Dir& dir, double reach) {
  const gp_Pnt on = face_samples(profile).front();
  gp_Trsf back;
  back.SetTranslation(gp_Vec(dir) * -reach);
  const TopoDS_Shape longer = BRepPrimAPI_MakePrism(moved(profile, back), gp_Vec(dir) * (2 * reach), Standard_True).Shape();
  const gp_Pnt behind = on.Translated(gp_Vec(dir) * (-reach * 0.98));
  const std::vector<UpTo> tries = tries_of(surface);
  for (const UpTo& t : tries) {
    TopoDS_Shape before;  // the piece from far behind up to the face
    for (const auto& piece : split_by(longer, t, reach))
      if (BRepClass3d_SolidClassifier(piece, behind, 1e-7).State() == TopAbs_IN) before = piece;
    if (before.IsNull()) continue;
    const auto [low, high] = heights(before, on, dir);
    if (high > reach * 0.97) continue;  // not separated: part of the profile misses the face
    Contact c;
    c.farthest = c.nearest = high;
    for (TopExp_Explorer f(before, TopAbs_FACE); f.More(); f.Next()) {
      const double bottom = heights(f.Current(), on, dir).first;
      if (bottom > -reach * 0.97) c.nearest = std::min(c.nearest, bottom);  // the face's part: the sides run back to the far end
    }
    try {
      c.beyond = boolean(BoolOp::Cut, longer, before);
    } catch (const Error&) {
      continue;
    }
    return c;
  }
  throw Error("part of the profile misses the start face, even carried on past its edges, so the extrusion has no start there; pick a larger face");
}

// Start from: Face, Sketch on face: the profiles moved onto the start face's plane (a curved face's tangent plane at its middle)
// and the face offset along its normal, as a sketch projecting them there draws them (derived_extrude_ops makes that sketch,
// in this frame when `frame` is given); the extrusion goes along that plane's normal, on the side the profiles' own normal
// points to.
Profiles profiles_on_face(const Ctx& ctx, const json& in, const Profiles& prof, const Frame* frame = nullptr) {
  const TopoDS_Face face = picked_start_face(ctx, in);
  auto [at, normal] = face_tangent(face);
  BRepAdaptor_Surface surface(face);
  if (surface.GetType() == GeomAbs_Plane) {
    normal = surface.Plane().Axis().Direction();
    if (!surface.Plane().Position().Direct()) normal.Reverse();
    if (face.Orientation() == TopAbs_REVERSED) normal.Reverse();
  }
  const double along = normal.Dot(prof.normal);
  if (std::fabs(along) < 1e-6) throw Error("the start face is edge-on to the profile, so the profile cannot be sketched on it");
  if (along < 0) normal.Reverse();
  const double offset = in.contains("face_offset") ? ctx.length(in, "face_offset") : 0.0;
  const Frame f = frame ? *frame : plane_through(at.Translated(gp_Vec(normal) * offset), gp_Vec(normal));
  // The sources as a projecting sketch names them: a sketch whole, a body's face.
  std::vector<json> sources;
  for (const auto& r : in.value("profiles", json::array())) {
    const json source = r.is_object() && r.contains("sketch") ? json{{"sketch", r["sketch"]}} : r;
    if (std::find(sources.begin(), sources.end(), source) == sources.end()) sources.push_back(source);
  }
  std::vector<Region> regions;
  for (const auto& source : sources) {
    const Sketch projected = derive_sketch(ctx.doc, ctx.scene, f, source, "project", ctx.fresh);
    for (auto& g : sketch_regions(projected, f)) regions.push_back(std::move(g));
  }
  Profiles out;
  out.normal = normal;
  for (const auto& p : prof.faces) {
    const gp_Pnt sample = face_samples(p).front();
    double u = 0, v = 0;
    f.to_local({sample.X(), sample.Y(), sample.Z()}, u, v);
    const int i = region_at(regions, f, u, v);
    if (i < 0) throw Error("a profile does not come out as a region on the start face's plane (its curves overlap there)");
    out.faces.push_back(regions[static_cast<size_t>(i)].face);
  }
  return out;
}

// The extrusion's solid. `starts` (when given) gets each profile's start, as a height along the extrusion from its plane.
TopoDS_Shape make_extrusion(const Ctx& ctx, const json& in, const Profiles& prof, std::vector<double>* starts_out = nullptr) {
  const std::string direction = in.value("direction", "one");
  const std::string extent = in.value("extent", "distance");
  const bool upto = extent == "to_face" || extent == "to_body";
  if (upto && direction != "one") throw Error("up to a face or a body goes one way: set the direction to one");
  if (extent != "distance" && extent != "all" && !upto) throw Error("unknown extent \"" + extent + "\": distance, all, to_face or to_body");
  gp_Vec n(prof.normal);
  if (in.value("flip", false)) n.Reverse();
  const bool from_face = in.value("start", "profile") == "face";
  const double reach = extent == "distance" && !from_face ? 0.0 : scene_reach(ctx, compound_of(std::vector<TopoDS_Shape>(prof.faces.begin(), prof.faces.end())));
  // Each profile's start: a distance along the axis, or where a curved or tilted start face meets it (a flat start at the
  // nearest or farthest contact, or one that follows the face: carved by the part of space beyond the face).
  const std::optional<UpTo> surface = start_surface(ctx, in, n, reach);
  const std::string start_shape = in.value("start_shape", "follow_face");
  const bool follow = surface && start_shape == "follow_face";
  if (follow && direction != "one") throw Error("a start that follows a face goes one way: set the direction to one, or start at the nearest or farthest contact");
  const double face_offset = surface && in.contains("face_offset") ? ctx.length(in, "face_offset") : 0.0;
  std::vector<double> starts, spans;
  std::vector<TopoDS_Shape> beyonds;
  for (const auto& face : prof.faces) {
    ctx.check_cancel();
    if (surface) {
      const Contact c = start_contact(face, *surface, gp_Dir(n), reach);
      starts.push_back((start_shape == "farthest_contact" ? c.farthest : c.nearest) + face_offset);
      spans.push_back(follow ? c.farthest - c.nearest : 0.0);
      gp_Trsf shift;
      shift.SetTranslation(n * face_offset);
      beyonds.push_back(follow ? moved(c.beyond, shift) : TopoDS_Shape());
    } else {
      GProp_GProps g;
      BRepGProp::SurfaceProperties(face, g);
      starts.push_back(extrusion_start(ctx, in, n, g.CentreOfMass()));
      spans.push_back(0.0);
      beyonds.emplace_back();
    }
  }
  if (starts_out) *starts_out = starts;
  double d1 = 0, d2 = 0;
  if (extent == "distance") {
    d1 = ctx.length(in, "distance");
    d2 = direction == "two" ? ctx.length(in, "distance2") : 0;
    if (direction == "symmetric") d1 = d2 = d1 / 2;
    if (std::fabs(d1 + d2) < 1e-7) throw Error("the extrusion distance is zero");
    if (follow && d1 < 0) throw Error("a start that follows a face needs a positive distance: flip the direction instead");
  } else if (extent == "all") {
    std::tie(d1, d2) = through_all(ctx, in, prof, n, reach, direction, starts);
  }
  const double taper = extent != "distance" ? 0.0 : (in.contains("taper") ? ctx.angle(in, "taper") : 0.0);
  std::vector<TopoDS_Shape> solids;
  for (size_t i = 0; i < prof.faces.size(); ++i) {
    ctx.check_cancel();
    const TopoDS_Face& face = prof.faces[i];
    TopoDS_Shape base = face;
    const double start = starts[i];
    if (std::fabs(start-d2) > 1e-12) {
      gp_Trsf back;
      back.SetTranslation(n * (start-d2));
      base = moved(face, back);
    }
    TopoDS_Shape prism;
    if (upto) {
      prism = up_to(ctx, in, base, n, reach);
    } else {
      // Following the start face, the distance runs from it everywhere: from the nearest contact over the spread, then
      // the part beyond the face moved on by the distance is cut away below.
      prism = BRepPrimAPI_MakePrism(base, n * (d1 + d2 + (extent == "distance" ? spans[i] : 0.0)), Standard_True).Shape();
      if (std::fabs(taper) > 1e-9) {
        // Tilt every side face about the base plane; positive opens up away from the profile.
        const gp_Dir pull(n * ((d1 + d2) < 0 ? -1.0 : 1.0));
        gp_Pnt on;
        {
          GProp_GProps g;
          BRepGProp::SurfaceProperties(base, g);
          on = g.CentreOfMass();
        }
        const gp_Pln neutral(on, pull);
        BRepOffsetAPI_DraftAngle draft(prism);
        for (TopExp_Explorer ex(prism, TopAbs_FACE); ex.More(); ex.Next()) {
          const TopoDS_Face f = TopoDS::Face(ex.Current());
          BRepAdaptor_Surface s(f);
          if (s.GetType() == GeomAbs_Plane && std::fabs(std::fabs(s.Plane().Axis().Direction().Dot(pull)) - 1.0) < 1e-9) continue;  // the caps
          draft.Add(f, pull, -taper, neutral);
          if (!draft.AddDone()) throw Error("that taper angle cannot be applied to this profile");
        }
        draft.Build();
        if (!draft.IsDone()) throw Error("that taper angle cannot be applied to this profile");
        prism = draft.Shape();
      }
    }
    if (!beyonds[i].IsNull()) {
      try {
        prism = boolean(BoolOp::Common, prism, beyonds[i]);
        if (extent == "distance") {
          gp_Trsf on;
          on.SetTranslation(n * d1);
          prism = boolean(BoolOp::Cut, prism, moved(beyonds[i], on));
        }
      } catch (const Error&) {
        throw Error("the start cannot follow that face here (the kernel cannot cut the extrusion along it); start at the nearest or farthest contact instead");
      }
      const auto pieces = solids_of(prism);
      if (pieces.empty()) throw Error("following the start face leaves nothing of the extrusion; check the distance and the direction");
      prism = bundle(pieces);
    }
    for (const auto& s : solids_of(prism)) solids.push_back(outward(s));
  }
  if (solids.empty()) throw Error("the extrusion produced no solid");
  // Profiles that touch become one body.
  TopoDS_Shape acc = solids.front();
  for (size_t i = 1; i < solids.size(); ++i) acc = boolean(BoolOp::Fuse, acc, solids[i]);
  return acc;
}

TopoDS_Shape make_revolution(const Ctx& ctx, const json& in, const Profiles& prof) {
  const gp_Ax1 axis = ctx.axis(in.value("axis", json()));
  double angle = ctx.angle(in, "angle");
  if (std::fabs(angle) < 1e-9) throw Error("the revolve angle is zero");
  const bool full = std::fabs(std::fabs(angle) - 2 * M_PI) < 1e-9 || std::fabs(angle) > 2 * M_PI;
  std::vector<TopoDS_Shape> solids;
  for (const auto& face : prof.faces) {
    ctx.check_cancel();
    TopoDS_Shape base = face;
    if (!full && in.value("symmetric", false)) {
      gp_Trsf back;
      back.SetRotation(axis, -angle / 2);
      base = moved(face, back);
    }
    BRepPrimAPI_MakeRevol revol = full ? BRepPrimAPI_MakeRevol(base, axis, Standard_True) : BRepPrimAPI_MakeRevol(base, axis, angle, Standard_True);
    if (!revol.IsDone()) throw Error("the profile cannot be revolved about that axis (it must not cross it)");
    solids.push_back(outward(revol.Shape()));
  }
  TopoDS_Shape acc = solids.front();
  for (size_t i = 1; i < solids.size(); ++i) acc = boolean(BoolOp::Fuse, acc, solids[i]);
  return acc;
}

TopoDS_Wire outer_wire(const TopoDS_Face& f) { return BRepTools::OuterWire(f); }

// A planar face moved along a path, corners of the path mitred. Holes in the face stay holes.
TopoDS_Shape sweep_face(const TopoDS_Wire& path, const TopoDS_Face& face, const char* failure) {
  auto solid_of = [&](const TopoDS_Wire& section) {
    BRepOffsetAPI_MakePipeShell mk(path);
    mk.SetMode(Standard_False);  // corrected Frenet: no twisting on planar paths
    mk.SetTransitionMode(BRepBuilderAPI_RightCorner);
    mk.Add(section);
    if (!mk.IsReady()) throw Error(failure);
    mk.Build();
    if (!mk.IsDone() || !mk.MakeSolid()) throw Error(failure);
    return outward(mk.Shape());
  };
  const TopoDS_Wire outer = outer_wire(face);
  TopoDS_Shape result = solid_of(outer);
  for (TopExp_Explorer ex(face, TopAbs_WIRE); ex.More(); ex.Next())
    if (!ex.Current().IsSame(outer)) result = boolean(BoolOp::Cut, result, solid_of(TopoDS::Wire(ex.Current())));
  return result;
}

TopoDS_Shape make_hole_tool(const Ctx& ctx, const json& in, const gp_Pnt& at, const gp_Dir& into, double reach) {
  const double d = ctx.length(in, "diameter");
  if (d <= 0) throw Error("the hole diameter must be positive");
  const bool all = in.value("extent", "distance") == "all";
  const double depth = all ? reach : ctx.length(in, "depth");
  if (depth <= 0) throw Error("the hole depth must be positive");
  const gp_Ax2 ax(at, into);
  TopoDS_Shape tool = BRepPrimAPI_MakeCylinder(ax, d / 2, depth).Shape();
  if (!all && in.value("tip", "flat") == "angled") {
    const double tip = ctx.angle(in, "tip_angle");
    if (tip <= 0 || tip >= M_PI) throw Error("the tip angle must be between 0 and 180 degrees");
    const double h = (d / 2) / std::tan(tip / 2);
    const gp_Ax2 tipAx(at.Translated(gp_Vec(into) * depth), into);
    tool = boolean(BoolOp::Fuse, tool, BRepPrimAPI_MakeCone(tipAx, d / 2, 0.0, h).Shape());
  }
  const std::string type = in.value("type", "simple");
  // Start a hair above the surface so the opening is cut clean even on a slightly uneven face.
  const gp_Ax2 above(at.Translated(gp_Vec(into) * -1e-3), into);
  if (type == "counterbore") {
    const double cbd = ctx.length(in, "cb_diameter"), cbh = ctx.length(in, "cb_depth");
    if (cbd <= d) throw Error("the counterbore must be wider than the hole");
    tool = boolean(BoolOp::Fuse, tool, BRepPrimAPI_MakeCylinder(above, cbd / 2, cbh + 1e-3).Shape());
  } else if (type == "countersink") {
    const double csd = ctx.length(in, "cs_diameter"), csa = ctx.angle(in, "cs_angle");
    if (csd <= d) throw Error("the countersink must be wider than the hole");
    if (csa <= 0 || csa >= M_PI) throw Error("the countersink angle must be between 0 and 180 degrees");
    const double h = (csd / 2) / std::tan(csa / 2);
    tool = boolean(BoolOp::Fuse, tool, BRepPrimAPI_MakeCone(gp_Ax2(at, into), csd / 2, 0.0, h).Shape());
  } else {
    tool = boolean(BoolOp::Fuse, tool, BRepPrimAPI_MakeCylinder(above, d / 2, 2e-3).Shape());
  }
  return tool;
}

std::vector<TopoDS_Shape> world_bodies(const Ctx& ctx, const json& refs, std::vector<std::string>* ids = nullptr) {
  std::vector<TopoDS_Shape> out;
  const std::vector<std::string> nodes = body_ids(ctx, refs);
  if (nodes.empty()) throw Error("pick at least one body");
  for (const auto& id : nodes) out.push_back(ctx.node_shape(id));
  if (ids) *ids = nodes;
  return out;
}

}  // namespace

Out compute_feature(const Ctx& ctx, const std::string& kind, const json& in) {
  const FeatureSpec* spec = feature_spec(kind);
  if (!spec) throw Error("unknown feature kind: " + kind);
  Out out;

  // ---- primitives
  if (kind == "box" || kind == "cylinder" || kind == "sphere" || kind == "cone" || kind == "torus") {
    const Frame frame = ctx.plane(in.value("plane", json{{"base", "xy"}}));
    const gp_Trsf place = placement(frame, in.contains("x") ? ctx.length(in, "x") : 0.0, in.contains("y") ? ctx.length(in, "y") : 0.0);
    TopoDS_Shape s;
    if (kind == "box") {
      const double l = ctx.length(in, "length"), w = ctx.length(in, "width"), h = ctx.length(in, "height");
      if (l <= 0 || w <= 0 || std::fabs(h) < 1e-9) throw Error("the box sizes must be positive");
      const bool c = in.value("centered", true);
      s = BRepPrimAPI_MakeBox(gp_Pnt(c ? -l / 2 : 0, c ? -w / 2 : 0, std::min(0.0, h)), l, w, std::fabs(h)).Shape();
    } else if (kind == "cylinder") {
      const double d = ctx.length(in, "diameter"), h = ctx.length(in, "height");
      if (d <= 0 || std::fabs(h) < 1e-9) throw Error("the cylinder sizes must be positive");
      s = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, std::min(0.0, h)), gp_Dir(0, 0, 1)), d / 2, std::fabs(h)).Shape();
    } else if (kind == "sphere") {
      const double d = ctx.length(in, "diameter");
      if (d <= 0) throw Error("the sphere diameter must be positive");
      s = BRepPrimAPI_MakeSphere(d / 2).Shape();
    } else if (kind == "cone") {
      const double d1 = ctx.length(in, "diameter"), d2 = ctx.length(in, "top_diameter"), h = ctx.length(in, "height");
      if (d1 < 0 || d2 < 0 || h <= 0 || std::fabs(d1 - d2) < 1e-9) throw Error("a cone needs two different diameters and a positive height");
      s = BRepPrimAPI_MakeCone(d1 / 2, d2 / 2, h).Shape();
    } else {
      const double d = ctx.length(in, "diameter"), sec = ctx.length(in, "section");
      if (d <= 0 || sec <= 0 || sec >= d) throw Error("the section must be thinner than the ring");
      s = BRepPrimAPI_MakeTorus(d / 2, sec / 2).Shape();
    }
    apply_operation(ctx, in, moved(s, place), out);
    return out;
  }

  if (kind == "extrude") {
    Profiles profiles=resolve_profiles(ctx,in.value("profiles",json()));
    // Sketch on face: the profiles on the start face's plane, extruded from there (derived_extrude_ops makes that sketch).
    json use = in;
    if (in.value("start", "profile") == "face" && in.value("start_shape", "follow_face") == "sketch_on_face") {
      profiles = profiles_on_face(ctx, in, profiles);
      use["start"] = "profile";
    }
    std::vector<double> starts;
    const TopoDS_Shape tool = healed(make_extrusion(ctx, use, profiles, &starts));
    if (in.value("operation", "new") == "auto") {
      try {
        out.operation = extrude_operation(ctx, use, profiles, tool, starts);
      } catch (const Standard_Failure&) {  // a point the kernel cannot classify: the tool's overlap decides (apply_operation)
      }
    }
    apply_operation(ctx, in, tool, out);
    GProp_GProps properties;BRepGProp::SurfaceProperties(profiles.faces.front(),properties);
    gp_Vec axis(profiles.normal);if(in.value("flip",false))axis.Reverse();
    const auto origin=properties.CentreOfMass().Translated(axis*starts.at(0));
    if(in.value("direction","one")=="symmetric")axis*=.5;
    if(in.value("extent","distance")=="distance")out.extra["distance_handle"]={{"origin",{origin.X(),origin.Y(),origin.Z()}},{"axis",{axis.X(),axis.Y(),axis.Z()}},{"value",ctx.length(in,"distance")}};
    return out;
  }
  if (kind == "revolve") {
    apply_operation(ctx, in, make_revolution(ctx, in, resolve_profiles(ctx, in.value("profiles", json()))), out);
    return out;
  }
  if (kind == "sweep") {
    const Profiles prof = resolve_profiles(ctx, in.value("profiles", json()));
    const TopoDS_Wire path = resolve_path(ctx, in.value("path", json()));
    std::vector<TopoDS_Shape> solids;
    for (const auto& f : prof.faces) solids.push_back(sweep_face(path, f, "the profile cannot be swept along that path"));
    TopoDS_Shape acc = solids.front();
    for (size_t i = 1; i < solids.size(); ++i) acc = boolean(BoolOp::Fuse, acc, solids[i]);
    apply_operation(ctx, in, acc, out);
    return out;
  }
  if (kind == "loft") {
    const Profiles prof = resolve_profiles(ctx, in.value("profiles", json()));
    if (prof.faces.size() < 2) throw Error("a loft needs at least two profiles");
    BRepOffsetAPI_ThruSections loft(Standard_True, in.value("ruled", false), 1e-6);
    loft.CheckCompatibility(Standard_True);
    for (const auto& f : prof.faces) loft.AddWire(outer_wire(f));
    loft.Build();
    if (!loft.IsDone()) throw Error("these profiles cannot be lofted");
    apply_operation(ctx, in, outward(loft.Shape()), out);
    return out;
  }
  if (kind == "pipe") {
    const TopoDS_Wire path = resolve_path(ctx, in.value("path", json()));
    const double d = ctx.length(in, "diameter");
    if (d <= 0) throw Error("the pipe diameter must be positive");
    // Section at the start of the path, normal to it.
    TopExp_Explorer ex(path, TopAbs_EDGE);
    BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
    gp_Pnt p;
    gp_Vec t;
    const bool rev = ex.Current().Orientation() == TopAbs_REVERSED;
    c.D1(rev ? c.LastParameter() : c.FirstParameter(), p, t);
    if (rev) t.Reverse();
    const gp_Ax2 ax(p, gp_Dir(t));
    auto disc = [&](double r) {
      const TopoDS_Wire w = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(ax, r)).Edge()).Wire();
      return BRepBuilderAPI_MakeFace(w, Standard_True).Face();
    };
    auto swept = [&](double r) { return sweep_face(path, disc(r), "the section cannot follow that path (bends tighter than the pipe?)"); };
    TopoDS_Shape s = swept(d / 2);
    if (in.value("hollow", false)) {
      const double wall = ctx.length(in, "thickness");
      if (wall <= 0 || wall >= d / 2) throw Error("the wall must be thinner than the pipe radius");
      s = boolean(BoolOp::Cut, s, swept(d / 2 - wall));
    }
    apply_operation(ctx, in, s, out);
    return out;
  }
  if (kind == "coil") {
    const Frame frame = ctx.plane(in.value("plane", json{{"base", "xy"}}));
    const gp_Trsf place = placement(frame, in.contains("x") ? ctx.length(in, "x") : 0.0, in.contains("y") ? ctx.length(in, "y") : 0.0);
    const double d = ctx.length(in, "diameter"), pitch = ctx.length(in, "pitch"), turns = ctx.number(in, "turns"), size = ctx.length(in, "size");
    if (d <= 0 || pitch <= 0 || turns <= 0 || size <= 0) throw Error("the coil sizes must be positive");
    if (size >= d / 2) throw Error("the section must be smaller than the coil radius");
    if (turns > 500) throw Error("that is more than 500 turns");
    // A helix is a straight line in the (angle, height) parameter space of a cylinder.
    const double r = d / 2, hand = in.value("left", false) ? -1.0 : 1.0;
    Handle(Geom_CylindricalSurface) cyl = new Geom_CylindricalSurface(gp_Ax3(), r);
    Handle(Geom2d_Line) line = new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(hand * 2 * M_PI, pitch));
    const double len = turns * std::hypot(2 * M_PI, pitch);
    TopoDS_Edge helix = BRepBuilderAPI_MakeEdge(line, cyl, 0.0, len).Edge();
    BRepLib::BuildCurves3d(helix);
    const TopoDS_Wire spine = BRepBuilderAPI_MakeWire(helix).Wire();
    // Section at the start of the helix, in the plane normal to it.
    const gp_Dir tangent(gp_Vec(0, hand * 2 * M_PI * r, pitch));
    const gp_Ax2 ax(gp_Pnt(r, 0, 0), tangent, gp_Dir(1, 0, 0));
    TopoDS_Wire section;
    if (in.value("section", "circle") == "circle") {
      section = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(ax, size / 2)).Edge()).Wire();
    } else {
      const gp_Vec u(ax.XDirection()), v(ax.YDirection());
      const gp_Pnt c = ax.Location();
      const gp_Pnt p[4] = {c.Translated(u * (-size / 2) + v * (-size / 2)), c.Translated(u * (size / 2) + v * (-size / 2)), c.Translated(u * (size / 2) + v * (size / 2)), c.Translated(u * (-size / 2) + v * (size / 2))};
      BRepBuilderAPI_MakeWire mw;
      for (int i = 0; i < 4; ++i) mw.Add(BRepBuilderAPI_MakeEdge(p[i], p[(i + 1) % 4]).Edge());
      section = mw.Wire();
    }
    BRepOffsetAPI_MakePipeShell mk(spine);
    mk.SetMode(gp_Dir(0, 0, 1));  // keep the section upright about the coil axis: no twisting along the helix
    mk.Add(section);
    mk.Build();
    if (!mk.IsDone() || !mk.MakeSolid()) throw Error("the coil could not be built (section too large for the pitch?)");
    apply_operation(ctx, in, moved(outward(mk.Shape()), place), out);
    return out;
  }
  if (kind == "thicken") {
    const double t = ctx.length(in, "thickness") * (in.value("flip", false) ? -1.0 : 1.0);
    if (std::fabs(t) < 1e-9) throw Error("the thickness is zero");
    std::vector<TopoDS_Shape> skins;
    for (const auto& [node, faces] : by_body(ctx, in.value("faces", json()), TopAbs_FACE, "faces")) {
      ctx.check_cancel();
      TopoDS_Compound comp;
      BRep_Builder bb;
      bb.MakeCompound(comp);
      for (const auto& f : faces) bb.Add(comp, BRepBuilderAPI_Copy(f).Shape());
      BRepOffset_MakeOffset off;
      off.Initialize(comp, t, 1e-5, BRepOffset_Skin, Standard_False, Standard_False, GeomAbs_Intersection, Standard_True /* thickening */);
      off.MakeOffsetShape();
      if (!off.IsDone()) throw Error("these faces cannot be thickened by that much");
      for (const auto& s : solids_of(off.Shape())) skins.push_back(outward(s));
    }
    if (skins.empty()) throw Error("these faces cannot be thickened by that much");
    apply_operation(ctx, in, compound_of(skins), out);
    return out;
  }
  if (kind == "hole") {
    const Points pts = resolve_points(ctx, in.value("points", json()));
    if (pts.at.empty()) throw Error("pick at least one sketch point");
    if (!pts.from_sketch) throw Error("holes are placed at sketch points (the sketch gives the drilling direction)");
    gp_Dir into = pts.normal.Reversed();
    if (in.value("flip", false)) into.Reverse();
    std::vector<TopoDS_Shape> tools;
    const double reach = scene_reach(ctx, TopoDS_Shape());
    for (const auto& p : pts.at) tools.push_back(make_hole_tool(ctx, in, p, into, reach));
    json cut = in;
    cut["operation"] = "cut";
    apply_operation(ctx, cut, compound_of(tools), out);
    return out;
  }

  // ---- modify
  if (kind == "fillet" || kind == "chamfer") {
    const double r = ctx.length(in, kind == "fillet" ? "radius" : "distance");
    if (r <= 0) throw Error(kind == "fillet" ? "the radius must be positive" : "the distance must be positive");
    for (const auto& [node, edges] : by_body(ctx, in.value("edges", json()), TopAbs_EDGE, "edges")) {
      ctx.check_cancel();
      const TopoDS_Shape body = ctx.node_shape(node);
      TopoDS_Shape result;
      if (kind == "fillet") {
        BRepFilletAPI_MakeFillet mk(body);
        for (const auto& e : edges) mk.Add(r, TopoDS::Edge(same_in(body, e)));
        mk.Build();
        if (!mk.IsDone()) throw Error("that radius does not fit these edges");
        result = mk.Shape();
      } else {
        BRepFilletAPI_MakeChamfer mk(body);
        const bool two = in.value("type", "equal") == "two";
        const double r2 = two ? ctx.length(in, "distance2") : r;
        if (r2 <= 0) throw Error("the distance must be positive");
        TopTools_IndexedDataMapOfShapeListOfShape faces;
        TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, faces);
        for (const auto& e : edges) {
          const TopoDS_Edge edge = TopoDS::Edge(same_in(body, e));
          if (!two) { mk.Add(r, edge); continue; }
          if (!faces.Contains(edge) || faces.FindFromKey(edge).IsEmpty()) throw Error("an edge to chamfer has no face");
          mk.Add(r, r2, edge, TopoDS::Face(faces.FindFromKey(edge).First()));
        }
        mk.Build();
        if (!mk.IsDone()) throw Error("that distance does not fit these edges");
        result = mk.Shape();
      }
      const auto pieces = solids_of(result);
      if (pieces.empty()) throw Error("the result is not a solid");
      out.bodies.push_back({node, healed(pieces.size() == 1 ? pieces.front() : result)});
    }
    return out;
  }
  if (kind == "shell") {
    const double t = ctx.length(in, "thickness");
    if (t <= 0) throw Error("the thickness must be positive");
    const double offset = in.value("direction", "inside") == "inside" ? -t : t;
    auto shell = [&](const std::string& node, const std::vector<TopoDS_Shape>& open) {
      ctx.check_cancel();
      const TopoDS_Shape body = ctx.node_shape(node);
      TopTools_ListOfShape remove;
      for (const auto& f : open) remove.Append(same_in(body, f));
      TopoDS_Shape result;
      if (remove.IsEmpty()) {
        // No opening: a closed cavity. The thick-solid builder wants a face to remove, so offset and subtract.
        BRepOffsetAPI_MakeOffsetShape off;
        off.PerformByJoin(body, -std::fabs(t), 1e-4);
        if (!off.IsDone()) throw Error("the body cannot be hollowed by that thickness");
        TopoDS_Shape inner = off.Shape();
        if (inner.ShapeType() != TopAbs_SOLID) {
          BRepBuilderAPI_MakeSolid ms;
          for (TopExp_Explorer ex(inner, TopAbs_SHELL); ex.More(); ex.Next()) ms.Add(TopoDS::Shell(ex.Current()));
          inner = ms.Solid();
        }
        if (offset > 0) throw Error("an outside shell needs a face to remove");
        result = boolean(BoolOp::Cut, body, outward(inner));
      } else {
        BRepOffsetAPI_MakeThickSolid mk;
        mk.MakeThickSolidByJoin(body, remove, offset, 1e-4);
        if (!mk.IsDone()) throw Error("the body cannot be shelled by that thickness");
        result = mk.Shape();
      }
      const auto pieces = solids_of(result);
      if (pieces.empty()) throw Error("the shell is not a solid");
      // The kernel returns the body unchanged, and says it is done, when a removed face blends smoothly into a kept
      // one (the flat faces either side of a fillet, or the fillet itself): say so rather than record a shell that is not.
      if (std::fabs(volume_of(pieces.front()) - volume_of(body)) <= 1e-9 * std::max(1.0, volume_of(body)))
        throw Error("the modelling kernel could not shell the body with these faces open (a face that blends smoothly into a fillet cannot be removed); open other faces, or shell before filleting");
      out.bodies.push_back({node, healed(pieces.front())});
    };
    bool any = false;
    if (in.contains("faces") && !in["faces"].empty())
      for (const auto& [node, faces] : by_body(ctx, in["faces"], TopAbs_FACE, "faces")) { shell(node, faces); any = true; }
    for (const auto& node : body_ids(ctx, in.value("bodies", json::array()))) { shell(node, {}); any = true; }
    if (!any) throw Error("pick faces to remove, or a body to hollow");
    return out;
  }
  if (kind == "draft") {
    const double a = ctx.angle(in, "angle");
    const Frame neutral = ctx.plane(in.value("plane", json()));
    const gp_Dir pull(vec(neutral.normal()));
    for (const auto& [node, faces] : by_body(ctx, in.value("faces", json()), TopAbs_FACE, "faces")) {
      const TopoDS_Shape body = ctx.node_shape(node);
      BRepOffsetAPI_DraftAngle draft(body);
      for (const auto& f : faces) {
        draft.Add(TopoDS::Face(same_in(body, f)), pull, a, frame_plane(neutral));
        if (!draft.AddDone()) throw Error("a picked face cannot be drafted about that plane");
      }
      draft.Build();
      if (!draft.IsDone()) throw Error("that draft angle cannot be applied");
      out.bodies.push_back({node, healed(draft.Shape())});
    }
    return out;
  }
  if (kind == "offset_face") {
    const double d = ctx.length(in, "distance");
    if (std::fabs(d) < 1e-9) throw Error("the distance is zero");
    for (const auto& [node, faces] : by_body(ctx, in.value("faces", json()), TopAbs_FACE, "faces")) {
      TopoDS_Shape body = ctx.node_shape(node);
      for (const auto& f : faces) {
        BRepAdaptor_Surface surf(TopoDS::Face(f));
        if (surf.GetType() != GeomAbs_Plane) throw Error("press pull works on planar faces (use Shell, Fillet or Draft for the others)");
        gp_Dir n = surf.Plane().Axis().Direction();
        if (!surf.Plane().Position().Direct()) n.Reverse();
        if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
        const TopoDS_Shape prism = BRepPrimAPI_MakePrism(BRepBuilderAPI_Copy(f).Shape(), gp_Vec(n) * d, Standard_True).Shape();
        body = boolean(d > 0 ? BoolOp::Fuse : BoolOp::Cut, body, outward(prism));
      }
      const auto pieces = solids_of(body);
      if (pieces.empty()) throw Error("nothing is left of the body");
      out.bodies.push_back({node, healed(pieces.size() == 1 ? pieces.front() : body)});
    }
    return out;
  }
  if (kind == "remove_faces") {  // the faces go, their neighbours are extended until they close the gap
    for (const auto& [node, faces] : by_body(ctx, in.value("faces", json()), TopAbs_FACE, "faces")) {
      ctx.check_cancel();
      const TopoDS_Shape body = ctx.node_shape(node);
      BRepAlgoAPI_Defeaturing df;
      df.SetShape(body);
      for (const auto& f : faces) df.AddFaceToRemove(same_in(body, f));
      df.SetRunParallel(Standard_True);
      df.SetToFillHistory(Standard_False);
      df.Build();
      // The kernel keeps a feature it cannot remove and only warns: that is a failure here.
      if (!df.IsDone() || df.HasErrors() || df.HasWarning(STANDARD_TYPE(BOPAlgo_AlertUnableToRemoveTheFeature)))
        throw Error("these faces cannot be removed: the faces around them do not meet when extended (a round on a convex corner, or neighbours tangent to each other); pick the whole detail, or fewer faces");
      const auto pieces = solids_of(df.Shape());
      if (pieces.empty()) throw Error("removing these faces leaves no solid");
      if (std::fabs(volume_of(df.Shape()) - volume_of(body)) <= 1e-9 * std::max(1.0, std::fabs(volume_of(body))) && subshape_count(df.Shape(), Ref::Kind::Face) == subshape_count(body, Ref::Kind::Face))
        throw Error("removing these faces changes nothing");
      out.bodies.push_back({node, healed(pieces.size() == 1 ? pieces.front() : bundle(pieces))});
    }
    return out;
  }
  if (kind == "scale") {
    const double k = ctx.number(in, "factor");
    if (k <= 0) throw Error("the scale factor must be positive");
    std::vector<std::string> ids;
    const auto bodies = world_bodies(ctx, in.value("bodies", json()), &ids);
    for (size_t i = 0; i < ids.size(); ++i) {
      gp_Pnt about(0, 0, 0);
      if (in.value("about", "origin") == "centre") {
        GProp_GProps g;
        BRepGProp::VolumeProperties(bodies[i], g);
        about = g.CentreOfMass();
      }
      gp_Trsf t;
      t.SetScale(about, k);
      out.bodies.push_back({ids[i], moved(bodies[i], t)});
    }
    return out;
  }

  // ---- combine
  if (kind == "combine") {
    const std::vector<std::string> targets = body_ids(ctx, in.value("target", json()));
    std::vector<std::string> tools = body_ids(ctx, in.value("tools", json()));
    if (targets.empty()) throw Error("pick a target body");
    const bool pickedTools = !tools.empty();
    for (const auto& target : targets) tools.erase(std::remove(tools.begin(), tools.end(), target), tools.end());
    if (tools.empty()) throw Error(pickedTools ? "the tool bodies are the target bodies: pick other bodies as tools" : "pick at least one tool body");
    const std::string op = in.value("operation", "join");
    // Several targets (gap log #14): a cut or an intersection works on each; a join makes the first of them one body
    // with the others and the tools.
    const bool join = op == "join";
    std::vector<std::string> taking = tools;
    if (join) taking.insert(taking.begin(), targets.begin() + 1, targets.end());
    for (const auto& target : join ? std::vector<std::string>{targets.front()} : targets) {
      TopoDS_Shape acc = ctx.node_shape(target);
      for (const auto& t : taking) {
        ctx.check_cancel();
        acc = boolean(join ? BoolOp::Fuse : op == "cut" ? BoolOp::Cut : BoolOp::Common, acc, ctx.node_shape(t));
      }
      const auto pieces = solids_of(acc);
      if (pieces.empty()) {
        if (op == "intersect") throw Error("the bodies do not intersect");
        out.removed.push_back(target);
      } else {
        // Joined tools that do not touch the target, or a cut that parts it: the pieces stay one body.
        std::vector<TopoDS_Shape> healedPieces;
        for (const auto& piece : pieces) healedPieces.push_back(healed(piece));
        out.bodies.push_back({target, bundle(healedPieces)});
      }
    }
    if (join)
      for (size_t i = 1; i < targets.size(); ++i) out.removed.push_back(targets[i]);
    if (!in.value("keep_tools", false))
      for (const auto& t : tools) out.removed.push_back(t);
    return out;
  }
  if (kind == "split") {
    const Frame frame = ctx.plane(in.value("plane", json()));
    std::vector<std::string> ids;
    const auto bodies = world_bodies(ctx, in.value("bodies", json()), &ids);
    bool any = false;
    for (size_t i = 0; i < ids.size(); ++i) {
      ctx.check_cancel();
      Bnd_Box box = box_of(bodies[i]);
      box.Add(pnt(frame.origin));
      BRepAlgoAPI_Splitter split;
      TopTools_ListOfShape args, tools;
      args.Append(bodies[i]);
      tools.Append(big_plane_face(frame, std::sqrt(box.SquareExtent()) * 2 + 10));
      split.SetArguments(args);
      split.SetTools(tools);
      split.SetFuzzyValue(1e-6);
      split.SetNonDestructive(Standard_True);
      split.Build();
      if (!split.IsDone() || split.HasErrors()) throw Error("the split failed");
      const auto pieces = solids_of(split.Shape());
      if (pieces.size() < 2) continue;
      any = true;
      out.bodies.push_back({ids[i], pieces.front()});
      for (size_t k = 1; k < pieces.size(); ++k) out.bodies.push_back({"", pieces[k], ids[i]});
    }
    if (!any) throw Error("the plane does not cut through the picked bodies");
    return out;
  }

  // ---- copies
  if (kind == "mirror" || kind == "pattern_rect" || kind == "pattern_circ") {
    std::vector<std::string> ids;
    const auto bodies = world_bodies(ctx, in.value("bodies", json()), &ids);
    std::vector<gp_Trsf> places;
    if (kind == "mirror") {
      const Frame frame = ctx.plane(in.value("plane", json()));
      gp_Trsf t;
      t.SetMirror(gp_Ax2(pnt(frame.origin), gp_Dir(vec(frame.normal()))));
      places.push_back(t);
    } else if (kind == "pattern_rect") {
      const gp_Dir d1 = ctx.axis(in.value("axis", json())).Direction();
      const int n1 = ctx.count(in, "count");
      const double s1 = ctx.length(in, "spacing");
      const bool second = in.value("second", false);
      const gp_Dir d2 = second ? ctx.axis(in.value("axis2", json())).Direction() : d1;
      const int n2 = second ? ctx.count(in, "count2") : 1;
      const double s2 = second ? ctx.length(in, "spacing2") : 0;
      if (n1 < 1 || n2 < 1 || n1 * n2 < 2) throw Error("a pattern needs at least two instances");
      if (n1 * n2 > 2000) throw Error("that is more than 2000 instances");
      for (int i = 0; i < n1; ++i)
        for (int k = 0; k < n2; ++k) {
          if (i == 0 && k == 0) continue;
          gp_Trsf t;
          t.SetTranslation(gp_Vec(d1) * (s1 * i) + gp_Vec(d2) * (s2 * k));
          places.push_back(t);
        }
    } else {
      const gp_Ax1 axis = ctx.axis(in.value("axis", json()));
      const int n = ctx.count(in, "count");
      const double total = ctx.angle(in, "angle");
      if (n < 2) throw Error("a pattern needs at least two instances");
      if (n > 2000) throw Error("that is more than 2000 instances");
      const bool full = std::fabs(std::fabs(total) - 2 * M_PI) < 1e-9;
      const double step = full ? total / n : total / (n - 1);
      for (int i = 1; i < n; ++i) {
        gp_Trsf t;
        t.SetRotation(axis, step * i);
        places.push_back(t);
      }
    }
    // Mirrored solids come out inside out: turn each one outward, keeping a body of several solids as one.
    auto copy = [](const TopoDS_Shape& body, const gp_Trsf& t) {
      std::vector<TopoDS_Shape> solids;
      for (const auto& s : solids_of(moved(body, t))) solids.push_back(outward(s));
      return bundle(solids);
    };
    if (in.value("operation", "new") == "new") {
      // One new body per copy of each picked body, named, placed and coloured after it (TODO 10 C2).
      for (const auto& t : places)
        for (size_t i = 0; i < bodies.size(); ++i) {
          ctx.check_cancel();
          out.bodies.push_back({"", copy(bodies[i], t), ids[i]});
        }
      return out;
    }
    std::vector<TopoDS_Shape> copies;
    for (const auto& t : places)
      for (const auto& b : bodies) {
        ctx.check_cancel();
        copies.push_back(copy(b, t));
      }
    apply_operation(ctx, in, compound_of(copies), out);
    return out;
  }
  if (kind == "move") {
    std::vector<std::string> ids;
    const auto bodies = world_bodies(ctx, in.value("bodies", json()), &ids);
    gp_Trsf t;
    t.SetTranslation(gp_Vec(ctx.length(in, "dx"), ctx.length(in, "dy"), ctx.length(in, "dz")));
    if (in.value("rotate", false)) {
      gp_Trsf r;
      r.SetRotation(ctx.axis(in.value("axis", json())), ctx.angle(in, "angle"));
      t = t * r;
    }
    if (t.Form() == gp_Identity) throw Error("the move does nothing");
    const bool copying = in.value("copy", false);
    for (size_t i = 0; i < ids.size(); ++i) out.bodies.push_back({copying ? std::string() : ids[i], moved(bodies[i], t), copying ? ids[i] : std::string()});
    return out;
  }
  if (kind == "remove") {
    out.removed = body_ids(ctx, in.value("bodies", json()));
    if (in.value("bodies", json()).is_array())  // a component goes whole: its node after its bodies, an empty one too
      for (const auto& r : in["bodies"])
        if (const Node* n = ctx.scene.node(Ref::from_json(r).body); n && n->kind == Node::Kind::Component && std::find(out.removed.begin(), out.removed.end(), n->id) == out.removed.end())
          out.removed.push_back(n->id);
    if (out.removed.empty()) throw Error("pick at least one body");
    return out;
  }

  // ---- construction
  if (kind == "plane") {
    const std::string mode = in.value("mode", "offset");
    Frame f;
    if (mode == "three_points") {
      const Points p = resolve_points(ctx, in.value("points", json()));
      if (p.at.size() != 3) throw Error("pick three points");
      const gp_Vec a(p.at[0], p.at[1]), b(p.at[0], p.at[2]);
      if (a.Crossed(b).Magnitude() < 1e-9) throw Error("the three points are on one line");
      f = frame_from_ax3(gp_Ax3(p.at[0], gp_Dir(a.Crossed(b)), gp_Dir(a)));
    } else if (mode == "point_normal") {  // through a point, square to an edge, an axis or a face (TODO 10 B5)
      const Points p = resolve_points(ctx, in.value("point", json()));
      if (p.at.size() != 1) throw Error("pick the point the plane goes through");
      f = plane_through(p.at[0], gp_Vec(ctx.axis(in.value("normal", json())).Direction()));
    } else {
      f = ctx.plane(in.value("plane", json()));
      if (mode == "offset") {
        const double d = ctx.length(in, "distance");
        const Vec3 n = f.normal();
        f.origin = {f.origin[0] + n[0] * d, f.origin[1] + n[1] * d, f.origin[2] + n[2] * d};
      } else if (mode == "angle") {
        gp_Trsf t;
        t.SetRotation(ctx.axis(in.value("axis", json())), ctx.angle(in, "angle"));
        f = frame_from_ax3(frame_ax3(f).Transformed(t));
      } else if (mode == "midplane") {
        const Frame g = ctx.plane(in.value("plane2", json()));
        const gp_Vec n1(vec(f.normal())), n2(vec(g.normal()));
        if (n1.Crossed(n2).Magnitude() > 1e-6) throw Error("the two planes must be parallel");
        // Half way along the normal; the first plane's origin keeps its place in the plane.
        const double d = gp_Vec(pnt(f.origin), pnt(g.origin)).Dot(n1);
        f.origin = {f.origin[0] + n1.X() * d / 2, f.origin[1] + n1.Y() * d / 2, f.origin[2] + n1.Z() * d / 2};
      }
    }
    out.extra["plane"] = f.to_json();
    return out;
  }
  if (kind == "interference") {
    // The picked bodies (components stand for theirs), or every solid body so far on the timeline.
    std::vector<std::string> bodies;
    if (in.contains("bodies") && in["bodies"].is_array() && !in["bodies"].empty()) {
      bodies = body_ids(ctx, in["bodies"]);
    } else {
      for (const auto& id : ctx.scene.all_bodies())
        if (const Node* n = ctx.scene.node(id); n && n->representation == "solid" && (!n->body_missing || ctx.fresh.count(n->body_key))) bodies.push_back(id);
    }
    const double clearance = in.contains("clearance") ? ctx.length(in, "clearance") : 0.0;
    json report = interference_of(ctx.scene, bodies, {{"clearance_mm", clearance}, {"limit", 200}}, [&](const std::string& id) { return ctx.node_shape(id); },
                                  [&](const std::string& id) { return box_of(ctx.node_shape(id)); }, [&] { return ctx.cancel && ctx.cancel(); });
    report.erase("offset");
    out.extra["check"] = report;
    const std::string fail = in.value("fail_on", "nothing");
    const size_t overlaps = report.value("interferences", 0), close = report.value("too_close", 0);
    if ((fail == "interference" && overlaps) || (fail == "clearance" && (overlaps || close))) {
      std::string pairs;
      for (const auto& item : report["items"]) {
        if (pairs.size() > 300) break;
        pairs += (pairs.empty() ? "" : "; ") + item.value("a_name", std::string()) + " / " + item.value("b_name", std::string()) +
                 (item["kind"] == "interference" ? " overlap " + json(item.value("volume_mm3", 0.0)).dump() + " mm3" : " " + json(item.value("distance_mm", 0.0)).dump() + " mm apart");
      }
      out.extra["error"] = std::to_string(overlaps) + " interference(s), " + std::to_string(close) + " pair(s) closer than " + json(clearance).dump() + " mm: " + pairs;
    }
    return out;
  }
  if (kind == "axis") {
    const std::string mode = in.value("mode", "edge");
    gp_Ax1 a;
    if (mode == "edge") {
      a = ctx.axis(in.value("edge", json()));
    } else if (mode == "two_points") {
      const Points p = resolve_points(ctx, in.value("points", json()));
      if (p.at.size() != 2 || p.at[0].Distance(p.at[1]) < 1e-9) throw Error("pick two different points");
      a = gp_Ax1(p.at[0], gp_Dir(gp_Vec(p.at[0], p.at[1])));
    } else {
      const Frame f = ctx.plane(in.value("plane", json()));
      const Points p = resolve_points(ctx, in.value("point", json()));
      if (p.at.size() != 1) throw Error("pick the point the axis goes through");
      a = gp_Ax1(p.at[0], gp_Dir(vec(f.normal())));
    }
    out.extra["axis"] = {{"origin", {a.Location().X(), a.Location().Y(), a.Location().Z()}}, {"dir", {a.Direction().X(), a.Direction().Y(), a.Direction().Z()}}};
    return out;
  }
  throw Error("feature kind \"" + kind + "\" is not implemented");
}

// ---------------------------------------------------------------- drag handles (TODO 11 P2)
namespace {

// The normal of `face` pointing out of its body, where `edge` runs along it at curve parameter t.
std::optional<gp_Vec> normal_along(const TopoDS_Face& face, const TopoDS_Edge& edge, double t) {
  double f = 0, l = 0;
  const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(edge, face, f, l);
  if (pcurve.IsNull()) return std::nullopt;
  const gp_Pnt2d uv = pcurve->Value(t);
  BRepAdaptor_Surface surface(face);
  gp_Pnt p;
  gp_Vec du, dv;
  surface.D1(uv.X(), uv.Y(), p, du, dv);
  gp_Vec n = du.Crossed(dv);
  if (n.Magnitude() < 1e-12) return std::nullopt;
  n.Normalize();
  if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
  return n;
}

// A point inside `face` and its outward normal there: the middle of its parameter box, or when that falls in a hole or
// outside a trimmed face, the middle of its first boundary edge.
std::optional<std::pair<gp_Pnt, gp_Vec>> face_point(const TopoDS_Face& face) {
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  gp_Pnt2d uv((u0 + u1) / 2, (v0 + v1) / 2);
  if (BRepTopAdaptor_FClass2d(face, 1e-7).Perform(uv) == TopAbs_OUT) {
    TopExp_Explorer edges(face, TopAbs_EDGE);
    if (!edges.More()) return std::nullopt;
    double f = 0, l = 0;
    const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(TopoDS::Edge(edges.Current()), face, f, l);
    if (pcurve.IsNull()) return std::nullopt;
    uv = pcurve->Value((f + l) / 2);
  }
  BRepAdaptor_Surface surface(face);
  gp_Pnt p;
  gp_Vec du, dv;
  surface.D1(uv.X(), uv.Y(), p, du, dv);
  gp_Vec n = du.Crossed(dv);
  if (n.Magnitude() < 1e-12) return std::nullopt;
  n.Normalize();
  if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
  return std::make_pair(p, n);
}

json handle_json(const std::string& input, const gp_Pnt& origin, const gp_Vec& axis, double value, double scale = 1) {
  const gp_Vec a = axis.Normalized();
  return {{"input", input}, {"origin", {origin.X(), origin.Y(), origin.Z()}}, {"axis", {a.X(), a.Y(), a.Z()}}, {"value", value}, {"scale", scale}};
}

}  // namespace

json feature_handles(const Document& doc, const Scene& scene, const std::string& kind, const json& in) {
  json handles = json::array();
  try {
    std::vector<ParamDef> defs;
    for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    const ParamTable params(defs);  // as plan_ops evaluates them
    const std::map<std::string, TopoDS_Shape> fresh;
    const Ctx ctx{doc, params, scene, fresh, {}};
    auto first = [&](const char* input) {
      const auto all = ctx.resolve_all(in.value(input, json()));
      if (all.empty()) throw Error("nothing picked");
      return all.front();
    };
    if (kind == "fillet" || kind == "chamfer") {
      // On the first edge, half way along it, pointing out between its two faces: the way a bigger radius or distance eats
      // into them (the guide's arrow).
      const ResolvedRef r = first("edges");
      if (r.sub.ShapeType() != TopAbs_EDGE) return handles;
      const TopoDS_Shape body = ctx.node_shape(r.node);
      const TopoDS_Edge edge = TopoDS::Edge(same_in(body, r.sub));
      TopTools_IndexedDataMapOfShapeListOfShape faces;
      TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, faces);
      if (!faces.Contains(edge)) return handles;
      double f = 0, l = 0;
      BRep_Tool::Range(edge, f, l);
      const double t = (f + l) / 2;
      const gp_Pnt mid = BRepAdaptor_Curve(edge).Value(t);
      gp_Vec sum(0, 0, 0);
      int found = 0;
      for (TopTools_ListIteratorOfListOfShape it(faces.FindFromKey(edge)); it.More() && found < 2; it.Next())
        if (const auto n = normal_along(TopoDS::Face(it.Value()), edge, t)) sum += *n, ++found;
      if (found == 0 || sum.Magnitude() < 1e-9) return handles;
      handles.push_back(handle_json(kind == "fillet" ? "radius" : "distance", mid, sum, ctx.length(in, kind == "fillet" ? "radius" : "distance")));
    } else if (kind == "thicken" || kind == "offset_face") {
      // Off the first face along its outward normal: the skin's outer side (the inner one with Other side), the moved face.
      const ResolvedRef r = first("faces");
      if (r.sub.ShapeType() != TopAbs_FACE) return handles;
      const TopoDS_Face face = TopoDS::Face(same_in(ctx.node_shape(r.node), r.sub));
      if (kind == "thicken") {
        const auto at = face_point(face);
        if (!at) return handles;
        handles.push_back(handle_json("thickness", at->first, in.value("flip", false) ? at->second.Reversed() : at->second, ctx.length(in, "thickness")));
      } else {
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) return handles;
        gp_Dir n = surface.Plane().Axis().Direction();
        if (!surface.Plane().Position().Direct()) n.Reverse();
        if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
        GProp_GProps g;
        BRepGProp::SurfaceProperties(face, g);
        handles.push_back(handle_json("distance", g.CentreOfMass(), gp_Vec(n), ctx.length(in, "distance")));
      }
    } else if (kind == "move" && in.value("rotate", false)) {
      // The axis it turns about (an edge, a face or a construction axis resolved here): the app's ring goes round it.
      const gp_Ax1 a = ctx.axis(in.value("axis", json()));
      json ring = handle_json("angle", a.Location(), gp_Vec(a.Direction()), ctx.angle(in, "angle"));
      ring["ring"] = true;
      handles.push_back(ring);
    } else if (kind == "plane" && in.value("mode", "offset") == "offset") {
      // Off the plane's origin; off a face's middle when it is a face (its frame starts at a corner, for sketches).
      const json from = in.value("plane", json());
      const Frame f = ctx.plane(from);
      gp_Pnt at = pnt(f.origin);
      if (from.is_object() && from.contains("face")) {
        const ResolvedRef r = ctx.resolve(from["face"]);
        GProp_GProps g;
        BRepGProp::SurfaceProperties(r.sub, g);
        at = g.CentreOfMass();
      }
      handles.push_back(handle_json("distance", at, vec(f.normal()), ctx.length(in, "distance")));
    } else if (kind == "box" || kind == "cylinder" || kind == "cone") {
      // At the middle of the footprint, along the plane's normal: the height (negative grows the other way).
      const Frame f = ctx.plane(in.value("plane", json{{"base", "xy"}}));
      double x = in.contains("x") ? ctx.length(in, "x") : 0.0, y = in.contains("y") ? ctx.length(in, "y") : 0.0;
      if (kind == "box" && !in.value("centered", true)) x += ctx.length(in, "length") / 2, y += ctx.length(in, "width") / 2;
      handles.push_back(handle_json("height", pnt(f.to_world(x, y)), vec(f.normal()), ctx.length(in, "height")));
    }
  } catch (const Standard_Failure&) {
    return json::array();
  } catch (const std::exception&) {  // a pick that does not resolve, a value that does not evaluate: no handle
    return json::array();
  }
  return handles;
}

std::vector<json> derived_extrude_ops(const Document& doc, const Scene& scene, const json& inputs, const std::string& name, const std::string& component) {
  std::vector<ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  const ParamTable params(defs);
  const std::map<std::string, TopoDS_Shape> fresh;
  const Ctx ctx{doc, params, scene, fresh, {}};
  if (inputs.value("start", "profile") != "face" || inputs.value("start_shape", "follow_face") != "sketch_on_face") throw Error("the extrusion does not sketch on its start face");
  const Profiles prof = resolve_profiles(ctx, inputs.value("profiles", json()));
  const TopoDS_Face face = picked_start_face(ctx, inputs);
  const json picked = inputs.at("start_face").front();
  const gp_Dir normal = profiles_on_face(ctx, inputs, prof).normal;  // the plane's side, as the extrusion takes it
  const double offset = inputs.contains("face_offset") ? ctx.length(inputs, "face_offset") : 0.0;
  std::vector<json> ops;
  auto place = [&](json op) {
    op["id"] = new_uuid();
    if (!component.empty()) op["component"] = component;
    ops.push_back(op);
    return op["id"].get<std::string>();
  };
  std::string source_name = "Sketch";
  for (const auto& r : inputs.value("profiles", json::array()))
    if (r.is_object() && r.contains("sketch"))
      if (const SketchItem* s = scene.sketch(r["sketch"].get<std::string>())) {
        source_name = s->name;
        break;
      }
  // The sketch's plane, and the frame the sketch will have on it.
  json plane;
  Frame frame;
  BRepAdaptor_Surface surface(face);
  if (surface.GetType() == GeomAbs_Plane) {
    frame = ctx.plane({{"face", picked}});
    const gp_Vec n(vec(frame.normal()));
    const double distance = offset * (n.Dot(gp_Vec(normal)) < 0 ? -1.0 : 1.0);
    if (std::fabs(distance) > 1e-12) {
      std::ostringstream text;
      text.precision(15);
      text << distance << " mm";
      const std::string id = place(make_feature_op("plane", source_name + " plane", {{"mode", "offset"}, {"plane", {{"face", picked}}}, {"distance", text.str()}}));
      frame.origin = {frame.origin[0] + n.X() * distance, frame.origin[1] + n.Y() * distance, frame.origin[2] + n.Z() * distance};
      plane = {{"feature", id}};
    } else {
      plane = {{"face", picked}};
    }
  } else {
    const gp_Pnt o = face_tangent(face).first.Translated(gp_Vec(normal) * offset);
    plane = {{"origin", {o.X(), o.Y(), o.Z()}}, {"normal", {normal.X(), normal.Y(), normal.Z()}}};
    frame = ctx.plane(plane);
  }
  // The projection, linked to its sources as the sketch's Project draws it.
  Sketch derived;
  std::vector<json> sources;
  for (const auto& r : inputs.value("profiles", json::array())) {
    const json source = r.is_object() && r.contains("sketch") ? json{{"sketch", r["sketch"]}} : r;
    if (std::find(sources.begin(), sources.end(), source) != sources.end()) continue;
    sources.push_back(source);
    append_reference(derived, derive_sketch(doc, scene, frame, source, "project"), source, "project", true);
  }
  const std::string sketch = place(make_sketch_op(source_name + " (derived)", plane, derived.to_json()));
  // The extrusion from those regions, along the sketch's normal the way it went.
  const std::vector<Region> regions = sketch_regions(derived, frame);
  json profiles = json::array();
  for (const auto& p : prof.faces) {
    const gp_Pnt sample = face_samples(p).front();
    double u = 0, v = 0;
    frame.to_local({sample.X(), sample.Y(), sample.Z()}, u, v);
    if (region_at(regions, frame, u, v) < 0) throw Error("a profile does not come out as a region on the start face's plane (its curves overlap there)");
    profiles.push_back({{"sketch", sketch}, {"at", {u, v}}});
  }
  json extrude = inputs;
  extrude["profiles"] = profiles;
  extrude["start"] = "profile";
  for (const char* gone : {"start_face", "start_shape", "face_offset", "start_offset"}) extrude.erase(gone);
  const gp_Vec way = gp_Vec(normal) * (inputs.value("flip", false) ? -1.0 : 1.0);
  extrude["flip"] = gp_Vec(vec(frame.normal())).Dot(way) < 0;
  place(make_feature_op("extrude", name, extrude));
  return ops;
}

}  // namespace opad::design
