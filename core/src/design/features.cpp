// What each feature kind asks for (the spec table the UI builds its forms from) and how it is computed.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBndLib.hxx>
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
#include <Geom_CylindricalSurface.hxx>
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
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <map>

#include "engine.hpp"
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
  add("torus", "Torus", "torus", "create", "A ring lying on a plane.", placed({in("diameter", "Ring diameter", "length", "40 mm"), in("section", "Section diameter", "length", "10 mm")}), "new");
  add("extrude", "Extrude", "extrude", "create", "Pull sketch profiles or planar faces along their normal. Symmetric splits the distance in half on each side; a start offset moves the start along the sketch normal.",
      {pick("profiles", "Profiles", "profiles", 1, 0),choice("start", "Start from", {"profile", "offset", "face"}),
       in("start_offset", "Start offset", "length", "0 mm", "start=offset"),pick("start_face", "Start face", "faces", 1, 1, "start=face"),
       choice("direction", "Direction", {"one", "symmetric", "two"}), choice("extent", "Extent", {"distance", "all"}),
       in("distance", "Distance", "length", "10 mm", "extent=distance"), in("distance2", "Distance, other side", "length", "10 mm", "direction=two"),
       in("taper", "Taper angle", "angle", "0 deg", "extent=distance"), in("flip", "Flip direction", "bool", false)},
      "new");
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
  add("scale", "Scale", "scale", "modify", "Resize bodies uniformly.", {pick("bodies", "Bodies", "bodies", 1, 0), in("factor", "Factor", "number", "2"), choice("about", "About", {"origin", "centre"})});
  add("combine", "Combine", "combine", "combine", "Join, cut or intersect bodies.",
      {pick("target", "Target body", "bodies", 1, 1), pick("tools", "Tool bodies", "bodies", 1, 0), choice("operation", "Operation", {"join", "cut", "intersect"}), in("keep_tools", "Keep tools", "bool", false)});
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
      {choice("mode", "Type", {"offset", "angle", "midplane", "three_points"}), in("plane", "From plane", "plane", xy, "mode=offset|angle|midplane"),
       in("distance", "Distance", "length", "10 mm", "mode=offset"), in("axis", "About axis", "axis", json{{"base", "x"}}, "mode=angle"), in("angle", "Angle", "angle", "45 deg", "mode=angle"),
       in("plane2", "Second plane", "plane", nullptr, "mode=midplane"), pick("points", "Three points", "points", 3, 3, "mode=three_points")});
  add("axis", "Construction axis", "axis", "construct", "An axis to revolve or pattern about.",
      {choice("mode", "Type", {"edge", "two_points", "normal"}), in("edge", "Edge or round face", "axis", nullptr, "mode=edge"), pick("points", "Two points", "points", 2, 2, "mode=two_points"),
       in("plane", "Plane", "plane", xy, "mode=normal"), pick("point", "Through point", "points", 1, 1, "mode=normal")});
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
  tools.Append(b);
  auto run = [&](BRepAlgoAPI_BooleanOperation& algo) {
    algo.SetArguments(args);
    algo.SetTools(tools);
    algo.SetFuzzyValue(1e-6);
    algo.SetRunParallel(Standard_True);
    algo.SetNonDestructive(Standard_True);
    algo.Build();
    if (!algo.IsDone() || algo.HasErrors()) throw Error("the boolean operation failed");
    algo.SimplifyResult(Standard_True, Standard_True);  // coplanar faces left by the cut are merged, like any CAD user expects
    return algo.Shape();
  };
  if (op == BoolOp::Fuse) { BRepAlgoAPI_Fuse f; return run(f); }
  if (op == BoolOp::Cut) { BRepAlgoAPI_Cut c; return run(c); }
  BRepAlgoAPI_Common c;
  return run(c);
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
      if (r.value("all", false) || !r.contains("at")) {
        for (const auto& g : regions) out.faces.push_back(g.face);
      } else {
        int i=-1;
        if(r.contains("boundary")) {
          const auto boundary=r.at("boundary").get<std::vector<int>>();
          for(size_t k=0;k<regions.size();++k)if(regions[k].boundary==boundary) {
            if(i>=0)throw Error("a picked profile has become ambiguous; pick it again");
            i=int(k);
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
    } else if (r.contains("kind") && r["kind"] == "point") {
      const Ref p = Ref::from_json(r);
      out.at.push_back(pnt(p.point));
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

// The shared ending of every feature that makes material: new body, or join / cut / intersect with others.
void apply_operation(const Ctx& ctx, const json& inputs, const TopoDS_Shape& tool_in, Out& out) {
  const TopoDS_Shape tool = healed(tool_in);
  if (solids_of(tool).empty()) throw Error("the operation produced no solid");
  const std::string op = inputs.value("operation", "new");
  if (op == "new") {
    for (const auto& s : solids_of(tool)) out.bodies.push_back({"", outward(s)});
    return;
  }
  std::vector<std::string> targets = body_ids(ctx, inputs.value("targets", json::array()));
  const bool automatic = targets.empty();
  if (automatic) {
    const Bnd_Box tb = box_of(tool);
    for (const auto& id : ctx.scene.all_bodies()) {
      const Node* n = ctx.scene.node(id);
      if (!n || n->representation != "solid" || (n->body_missing && !ctx.fresh.count(n->body_key))) continue;
      if (!box_of(ctx.node_shape(id)).IsOut(tb)) targets.push_back(id);
    }
  }
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
  return gp_Vec(center,pnt(frame.origin)).Dot(normal);
}
TopoDS_Shape make_extrusion(const Ctx& ctx, const json& in, const Profiles& prof) {
  const std::string direction = in.value("direction", "one");
  const bool all = in.value("extent", "distance") == "all";
  gp_Vec n(prof.normal);
  if (in.value("flip", false)) n.Reverse();
  double d1 = all ? 0 : ctx.length(in, "distance");
  double d2 = direction == "two" ? (all ? 0 : ctx.length(in, "distance2")) : 0;
  if (all) {
    d1 = scene_reach(ctx, compound_of(std::vector<TopoDS_Shape>(prof.faces.begin(), prof.faces.end())));
    d2 = direction == "one" ? 0 : d1;
  } else if (direction == "symmetric") {
    d1 = d2 = d1 / 2;
  }
  if (std::fabs(d1 + d2) < 1e-7) throw Error("the extrusion distance is zero");
  const double taper = all ? 0.0 : (in.contains("taper") ? ctx.angle(in, "taper") : 0.0);
  std::vector<TopoDS_Shape> solids;
  for (const auto& face : prof.faces) {
    ctx.check_cancel();
    TopoDS_Shape base = face;
    GProp_GProps properties;BRepGProp::SurfaceProperties(face,properties);
    const double start=extrusion_start(ctx,in,n,properties.CentreOfMass());
    if (std::fabs(start-d2) > 1e-12) {
      gp_Trsf back;
      back.SetTranslation(n * (start-d2));
      base = moved(face, back);
    }
    TopoDS_Shape prism = BRepPrimAPI_MakePrism(base, n * (d1 + d2), Standard_True).Shape();
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
    solids.push_back(outward(prism));
  }
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
    const auto profiles=resolve_profiles(ctx,in.value("profiles",json()));
    apply_operation(ctx, in, make_extrusion(ctx, in, profiles), out);
    GProp_GProps properties;BRepGProp::SurfaceProperties(profiles.faces.front(),properties);
    gp_Vec axis(profiles.normal);if(in.value("flip",false))axis.Reverse();
    const auto origin=properties.CentreOfMass().Translated(axis*extrusion_start(ctx,in,axis,properties.CentreOfMass()));
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
    const std::vector<std::string> target = body_ids(ctx, in.value("target", json()));
    std::vector<std::string> tools = body_ids(ctx, in.value("tools", json()));
    if (target.size() != 1) throw Error("pick one target body");
    tools.erase(std::remove(tools.begin(), tools.end(), target.front()), tools.end());
    if (tools.empty()) throw Error("pick at least one tool body");
    const std::string op = in.value("operation", "join");
    TopoDS_Shape acc = ctx.node_shape(target.front());
    for (const auto& t : tools) {
      ctx.check_cancel();
      acc = boolean(op == "join" ? BoolOp::Fuse : op == "cut" ? BoolOp::Cut : BoolOp::Common, acc, ctx.node_shape(t));
    }
    const auto pieces = solids_of(acc);
    if (pieces.empty()) {
      if (op == "intersect") throw Error("the bodies do not intersect");
      out.removed.push_back(target.front());
    } else {
      // Joined tools that do not touch the target, or a cut that parts it: the pieces stay one body.
      std::vector<TopoDS_Shape> healedPieces;
      for (const auto& piece : pieces) healedPieces.push_back(healed(piece));
      out.bodies.push_back({target.front(), bundle(healedPieces)});
    }
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

}  // namespace opad::design
