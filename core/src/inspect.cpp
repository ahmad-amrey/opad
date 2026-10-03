#include "opad/inspect.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepExtrema_ExtCC.hxx>
#include <BRepExtrema_ExtPC.hxx>
#include <BRepExtrema_TriangleSet.hxx>
#include <BVH_PairDistance.hxx>
#include <BVH_Tools.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Version.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "opad/geometry.hpp"
#include "opad/mass.hpp"

namespace opad {

namespace {

json pnt(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
gp_Pnt json_pnt(const json& p) { return gp_Pnt(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()); }
json dir(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }
json vec(const Vec3& v) { return {v[0], v[1], v[2]}; }

json bbox_json(const Bnd_Box& box) {
  json j;
  if (box.IsVoid()) return j;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  j["min"] = {x0, y0, z0};
  j["max"] = {x1, y1, z1};
  j["size"] = {x1 - x0, y1 - y0, z1 - z0};
  j["center"] = {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2};
  j["diagonal"] = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
  return j;
}

// Boxes this file reports are tight (TODO 10 B10): the exact geometry, without tolerance or meshing padding.
Bnd_Box shape_bbox(const TopoDS_Shape& s) { return tight_bbox(s); }

const char* surface_type(GeomAbs_SurfaceType t) {
  switch (t) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    case GeomAbs_BezierSurface: return "bezier";
    case GeomAbs_BSplineSurface: return "bspline";
    case GeomAbs_SurfaceOfRevolution: return "revolution";
    case GeomAbs_SurfaceOfExtrusion: return "extrusion";
    case GeomAbs_OffsetSurface: return "offset";
    default: return "other";
  }
}

const char* curve_type(GeomAbs_CurveType t) {
  switch (t) {
    case GeomAbs_Line: return "line";
    case GeomAbs_Circle: return "circle";
    case GeomAbs_Ellipse: return "ellipse";
    case GeomAbs_Hyperbola: return "hyperbola";
    case GeomAbs_Parabola: return "parabola";
    case GeomAbs_BezierCurve: return "bezier";
    case GeomAbs_BSplineCurve: return "bspline";
    case GeomAbs_OffsetCurve: return "offset";
    default: return "other";
  }
}

// World-space shape for a reference.
TopoDS_Shape ref_shape(const Document& doc, const Scene& scene, const Ref& ref) {
  if (ref.kind == Ref::Kind::Point) return BRepBuilderAPI_MakeVertex(gp_Pnt(ref.point[0], ref.point[1], ref.point[2]));
  Node sketchNode;
  if(const auto* sk=scene.sketch(ref.body)) { sketchNode.kind=Node::Kind::Body; sketchNode.name=sk->name; }
  const Node* n = scene.sketch(ref.body)?&sketchNode:scene.node(ref.body);
  if (!n) throw Error("unknown node: " + ref.body);
  if (n->kind != Node::Kind::Body) {
    if (ref.kind != Ref::Kind::Body) throw Error("sub-shape references need a body node: " + ref.str());
    throw Error("node is a component, not a body: " + ref.body + " (use its bodies)");
  }
  if (n->body_missing) throw Error("body entry missing for node " + ref.body);
  TopoDS_Shape world = node_world_shape(doc, scene, ref.body);
  if (ref.kind == Ref::Kind::Body) return world;
  return subshape(world, ref.kind, ref.index);
}

// Direction associated with a reference (planar normal, line direction, axis of revolution).
// `at` is a point of the reference the direction belongs to: on the face for a normal, the middle of a line or
// chord, and on the axis (beside the face, or the curve's centre) for an axis.
bool ref_direction(const Document& doc, const Scene& scene, const Ref& ref, gp_Dir& out, std::string& what, gp_Pnt& at) {
  TopoDS_Shape s = ref_shape(doc, scene, ref);
  if (ref.kind == Ref::Kind::Face) {
    BRepAdaptor_Surface surf(TopoDS::Face(s));
    const gp_Pnt mid = surf.Value((surf.FirstUParameter() + surf.LastUParameter()) * 0.5, (surf.FirstVParameter() + surf.LastVParameter()) * 0.5);
    auto axis = [&](const gp_Ax1& ax) {
      out = ax.Direction(); what = "axis";
      at = ax.Location().Translated(gp_Vec(ax.Direction()) * gp_Vec(ax.Location(), mid).Dot(gp_Vec(ax.Direction())));
      return true;
    };
    switch (surf.GetType()) {
      case GeomAbs_Plane: {
        gp_Dir n = surf.Plane().Axis().Direction();
        if (s.Orientation() == TopAbs_REVERSED) n.Reverse();
        out = n; what = "normal"; at = mid; return true;
      }
      case GeomAbs_Cylinder: return axis(surf.Cylinder().Axis());
      case GeomAbs_Cone: return axis(surf.Cone().Axis());
      case GeomAbs_Torus: return axis(surf.Torus().Axis());
      default: return false;
    }
  }
  if (ref.kind == Ref::Kind::Edge) {
    BRepAdaptor_Curve c(TopoDS::Edge(s));
    switch (c.GetType()) {
      case GeomAbs_Line: out = c.Line().Direction(); what = "direction"; at = c.Value((c.FirstParameter() + c.LastParameter()) * 0.5); return true;
      case GeomAbs_Circle: out = c.Circle().Axis().Direction(); what = "axis"; at = c.Circle().Location(); return true;
      case GeomAbs_Ellipse: out = c.Ellipse().Axis().Direction(); what = "axis"; at = c.Ellipse().Location(); return true;
      default: {
        gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
        if (a.Distance(b) < 1e-9) return false;
        out = gp_Dir(gp_Vec(a, b)); what = "chord"; at = gp_Pnt((a.XYZ() + b.XYZ()) * 0.5); return true;
      }
    }
  }
  return false;
}

}  // namespace

bool scene_bbox(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, Vec3& lo, Vec3& hi) {
  Bnd_Box box;
  std::vector<std::string> ids = bodies;
  if (ids.empty())
    for (const auto& b : scene.all_bodies())
      if (scene.effectively_visible(b)) ids.push_back(b);
  for (const auto& id : ids) {
    const Node* n = scene.node(id);
    if (!n || n->kind != Node::Kind::Body || n->body_missing) continue;
    box.Add(node_world_bbox(doc, scene, id));
  }
  if (box.IsVoid()) return false;
  box.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  return true;
}

bool scene_tight_bbox(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, Vec3& lo, Vec3& hi,
                      const std::function<bool()>& cancelled) {
  Bnd_Box box;
  std::vector<std::string> ids = bodies;
  if (ids.empty())
    for (const auto& b : scene.all_bodies())
      if (scene.effectively_visible(b)) ids.push_back(b);
  if (ids.size() > 1) {
    std::vector<std::string> keys;
    for (const auto& id : ids)
      if (const Node* n = scene.node(id); n && n->kind == Node::Kind::Body && !n->body_missing) keys.push_back(n->body_key);
    warm_tight_bboxes(doc, keys, cancelled);
  }
  for (const auto& id : ids) {
    if (cancelled && cancelled()) throw Error("cancelled");
    const Node* n = scene.node(id);
    if (!n || n->kind != Node::Kind::Body || n->body_missing) continue;
    box.Add(node_tight_bbox(doc, scene, id, ids.size() == 1));
  }
  if (box.IsVoid()) return false;
  box.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  return true;
}

json document_info(const Document& doc, const Scene& scene) {
  json j;
  j["uuid"] = doc.header.uuid;
  j["format"] = doc.header.format;
  j["units"] = doc.header.units;
  j["created"] = doc.header.created;
  j["generator"] = doc.header.generator;
  j["path"] = doc.path.string();
  j["dirty"] = doc.dirty;
  j["ops"] = doc.ops.size();
  j["live_ops"] = doc.ops.size() - scene.deleted_ops.size();
  j["deleted_ops"] = scene.deleted_ops.size();
  j["body_entries"] = doc.body_count();
  int components = 0, bodies = 0;
  for (const auto& [id, n] : scene.nodes) (n.kind == Node::Kind::Body ? bodies : components)++;
  j["components"] = components;
  j["bodies"] = bodies;
  j["annotations"] = scene.annotations.size();
  j["measurements"] = scene.measurements.size();
  j["sections"] = scene.sections.size();
  j["views"] = scene.views.size();
  j["unresolved"] = scene.unresolved.size();
  Vec3 lo, hi;
  if (scene_tight_bbox(doc, scene, {}, lo, hi)) {
    Bnd_Box b;
    b.Update(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    j["bbox"] = bbox_json(b);
  }
  size_t brep_bytes = 0;
  for (const auto& b : doc.bodies()) brep_bytes += b.brep.size();
  j["body_store_bytes"] = brep_bytes;
  if (!doc.path.empty()) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(doc.path, ec);
    if (!ec) {
      j["file_bytes"] = sz;
      if (sz > 25u * 1024u * 1024u) j["size_warning"] = "document exceeds 25 MB; consider git LFS for *.opad (F11)";
    }
  }
  return j;
}

json bbox_to_json(const Bnd_Box& box) { return bbox_json(box); }

json describe_entity(const TopoDS_Shape& sub) {
  json j;
  j["bbox"] = bbox_json(shape_bbox(sub));
  if (sub.ShapeType() == TopAbs_FACE) {
    const TopoDS_Face& face = TopoDS::Face(sub);
    BRepAdaptor_Surface surf(face);
    j["type"] = "face";
    j["surface"] = surface_type(surf.GetType());
    switch (surf.GetType()) {
      case GeomAbs_Plane: {
        gp_Dir n = surf.Plane().Axis().Direction();
        if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
        j["normal"] = dir(n);
        break;
      }
      case GeomAbs_Cylinder: j["axis"] = dir(surf.Cylinder().Axis().Direction()); j["radius"] = surf.Cylinder().Radius(); break;
      case GeomAbs_Sphere: j["radius"] = surf.Sphere().Radius(); break;
      case GeomAbs_Cone: j["axis"] = dir(surf.Cone().Axis().Direction()); break;
      case GeomAbs_Torus: j["axis"] = dir(surf.Torus().Axis().Direction()); break;
      default: break;
    }
  } else if (sub.ShapeType() == TopAbs_EDGE) {
    BRepAdaptor_Curve c(TopoDS::Edge(sub));
    j["type"] = "edge";
    j["curve"] = curve_type(c.GetType());
    if (c.GetType() == GeomAbs_Line) j["direction"] = dir(c.Line().Direction());
    if (c.GetType() == GeomAbs_Circle) {
      j["radius"] = c.Circle().Radius();
      j["axis"] = dir(c.Circle().Axis().Direction());
    }
  } else if (sub.ShapeType() == TopAbs_VERTEX) {
    j["type"] = "vertex";
    j["point"] = pnt(BRep_Tool::Pnt(TopoDS::Vertex(sub)));
  }
  return j;
}

bool entity_matches(const json& detail, const json& filters, double tolerance) {
  static const std::set<std::string> known = {"curve", "surface", "radius_min", "radius_max", "parallel_to", "normal", "at_plane", "bounds"};
  for (const auto& [key, value] : filters.items())
    if (!known.count(key)) throw Error("unknown filter \"" + key + "\" (curve, surface, radius_min, radius_max, parallel_to, normal, at_plane, bounds)");
  auto axis = [](const std::string& a) { return a == "x" ? 0 : a == "y" ? 1 : 2; };
  // A direction filter as a unit vector: "x" / "+x" / "-z" or [x, y, z].
  auto direction = [&](const json& v, bool signed_axis) {
    gp_Vec d;
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      const bool minus = !s.empty() && s[0] == '-';
      const std::string name = !s.empty() && (s[0] == '-' || s[0] == '+') ? s.substr(1) : s;
      if (name != "x" && name != "y" && name != "z") throw Error("a direction filter is x, y or z" + std::string(signed_axis ? " with + or -" : "") + ", or [x, y, z]");
      // Not SetCoord(index, value): OCCT writes (&x)[index - 1], which clang may assume only reaches x.
      const double s1 = minus ? -1.0 : 1.0;
      const int i = axis(name);
      d = gp_Vec(i == 0 ? s1 : 0.0, i == 1 ? s1 : 0.0, i == 2 ? s1 : 0.0);
    } else if (v.is_array() && v.size() == 3) {
      d = gp_Vec(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
    }
    if (d.Magnitude() < 1e-12) throw Error("a direction filter must not be zero");
    return d.Normalized();
  };
  auto vec_of = [](const json& v) { return gp_Vec(v[0].get<double>(), v[1].get<double>(), v[2].get<double>()); };
  if (filters.contains("curve") && detail.value("curve", "") != filters["curve"].get<std::string>()) return false;
  if (filters.contains("surface") && detail.value("surface", "") != filters["surface"].get<std::string>()) return false;
  if (filters.contains("radius_min") && (!detail.contains("radius") || detail["radius"].get<double>() < filters["radius_min"].get<double>() - tolerance)) return false;
  if (filters.contains("radius_max") && (!detail.contains("radius") || detail["radius"].get<double>() > filters["radius_max"].get<double>() + tolerance)) return false;
  if (filters.contains("parallel_to")) {
    if (!detail.contains("direction")) return false;
    if (std::fabs(vec_of(detail["direction"]).Dot(direction(filters["parallel_to"], false))) < 1 - 1e-8) return false;
  }
  if (filters.contains("normal")) {
    if (!detail.contains("normal")) return false;
    if (vec_of(detail["normal"]).Dot(direction(filters["normal"], true)) < 1 - 1e-8) return false;
  }
  if (filters.contains("at_plane") || filters.contains("bounds")) {
    if (!detail.contains("bbox") || detail["bbox"].is_null() || detail["bbox"].empty()) return false;
    const auto& box = detail["bbox"];
    if (filters.contains("at_plane")) {
      const auto& plane = filters["at_plane"];
      const int k = axis(plane["axis"].get<std::string>());
      const double at = plane["value"].get<double>();
      if (std::fabs(box["min"][k].get<double>() - at) > tolerance || std::fabs(box["max"][k].get<double>() - at) > tolerance) return false;
    }
    if (filters.contains("bounds"))
      for (int k = 0; k < 3; ++k)
        if (box["min"][k].get<double>() < filters["bounds"]["min"][k].get<double>() - tolerance ||
            box["max"][k].get<double>() > filters["bounds"]["max"][k].get<double>() + tolerance)
          return false;
  }
  return true;
}

json node_properties(const Document& doc, const Scene& scene, const std::string& node_id, bool geometry, const std::function<bool()>& cancelled) {
  if(const auto* sk=scene.sketch(node_id)) {
    json out={{"id",sk->id},{"name",sk->name},{"type","sketch"},{"visible",sk->visible},{"entities",sk->geometry.value("entities",json::array()).size()},{"frame",sk->frame.to_json()}};
    if(geometry) out["bbox"]=bbox_json(tight_bbox(node_world_shape(doc,scene,node_id)));
    return out;
  }
  const Node* n = scene.node(node_id);
  if (!n) throw Error("unknown node: " + node_id);
  json j;
  j["id"] = n->id;
  j["type"] = n->kind == Node::Kind::Body ? "body" : "component";
  j["name"] = n->name;
  json path = json::array();
  for (const auto& p : scene.path_to(node_id)) path.push_back(scene.node(p)->name);
  j["path"] = path;
  j["parent"] = n->parent.empty() ? json(nullptr) : json(n->parent);
  if (!n->parent.empty()) j["component"] = scene.node(n->parent)->name;
  j["source_op"] = n->source_op;
  j["modified_by"] = n->modified_by;
  j["transform"] = n->local.to_json();
  j["world"] = scene.world(node_id).to_json();
  if (n->has_color) j["color"] = {n->color[0], n->color[1], n->color[2]};
  j["opacity"] = n->opacity;
  j["visible"] = n->visible;
  j["effectively_visible"] = scene.effectively_visible(node_id);
  j["locked"] = n->locked;
  if (n->kind == Node::Kind::Body) {
    j["key"] = n->body_key;
    auto it = scene.instance_count.find(n->body_key);
    j["instances"] = it == scene.instance_count.end() ? 1 : it->second;
    j["representation"] = n->representation;  // solid | mesh | drawing2d
    if (const BodyEntry* b = doc.body(n->body_key)) {  // as the file had them (glTF, OBJ and newer STEP name materials)
      if (b->meta.contains("material")) j["material"] = b->meta["material"];
      if (b->meta.contains("source")) j["source"] = b->meta["source"];
    }
    if (n->body_missing) {
      j["missing"] = true;
      return j;
    }
    if (!geometry) return j;
    TopoDS_Shape world = node_world_shape(doc, scene, node_id);
    TopoDS_Shape proto = body_shape(doc, n->body_key);
    j["faces"] = subshape_count(proto, Ref::Kind::Face);
    j["edges"] = subshape_count(proto, Ref::Kind::Edge);
    j["vertices"] = subshape_count(proto, Ref::Kind::Vertex);
    bool has_solid = false;
    for (TopExp_Explorer e(proto, TopAbs_SOLID); e.More(); e.Next()) has_solid = true;
    j["solid"] = has_solid;
    // Span by span (gap log #4): BRepGProp read a disc on a long spline several per cent off. A body whose faces
    // the kernel cannot walk still gets its other properties.
    try {
      if (has_solid) {
        const MassProperties volume = volume_properties(world);
        j["volume"] = volume.mass;
        j["center_of_mass"] = pnt(volume.centre);
      }
      const MassProperties area = area_properties(world);
      j["area"] = area.mass;
      if (!has_solid) j["center_of_mass"] = pnt(area.centre);
    } catch (const Error& e) {
      j["mass_error"] = e.what();
    }
    j["bbox"] = bbox_json(node_tight_bbox(doc, scene, node_id));
  } else {
    j["children"] = n->children.size();
    auto bodies = scene.bodies_under(node_id);
    j["bodies"] = bodies.size();
    Vec3 lo, hi;
    if (geometry && scene_tight_bbox(doc, scene, bodies, lo, hi, cancelled)) {
      Bnd_Box b;
      b.Update(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
      j["bbox"] = bbox_json(b);
    }
  }
  return j;
}

json inspect_ref(const Document& doc, const Scene& scene, const Ref& ref) {
  if ((ref.kind==Ref::Kind::Center || ref.kind==Ref::Kind::Edge) && scene.node(ref.body)
      && scene.node(ref.body)->representation=="mesh") {
    for(const auto& fit:mesh_circles(node_world_shape(doc,scene,ref.body))) {
      if((ref.kind==Ref::Kind::Center && fit.index==ref.index) || (ref.kind==Ref::Kind::Edge && std::find(fit.edges.begin(),fit.edges.end(),ref.index)!=fit.edges.end())) {
        const auto& c=fit.circle;
        return {{"ref",ref.str()},{"type",ref.kind==Ref::Kind::Center?"center":"edge"},{"curve","faceted circle"},
          {"point",pnt(c.Location())},{"center",pnt(c.Location())},{"radius",c.Radius()},{"diameter",2*c.Radius()},
          {"axis",dir(c.Axis().Direction())},{"segments",fit.segments},{"approximate",true}};
      }
    }
  }
  if (ref.kind == Ref::Kind::Center) {
    Ref edge = ref;
    edge.kind = Ref::Kind::Edge;
    BRepAdaptor_Curve curve(TopoDS::Edge(ref_shape(doc, scene, edge)));
    if (curve.GetType() != GeomAbs_Circle) throw Error("center requires a circular edge");
    const gp_Circ circle = curve.Circle();
    return {{"ref", ref.str()}, {"type", "center"}, {"body", ref.body}, {"index", ref.index},
            {"point", pnt(circle.Location())}, {"center", pnt(circle.Location())},
            {"radius", circle.Radius()}, {"diameter", 2 * circle.Radius()}, {"axis", dir(circle.Axis().Direction())}};
  }
  if (ref.kind == Ref::Kind::Point) {
    json j;
    j["ref"] = ref.str();
    j["type"] = "point";
    j["point"] = vec(ref.point);
    return j;
  }
  if (ref.kind == Ref::Kind::Body) {
    if(const auto* sk=scene.sketch(ref.body)) return {{"ref",ref.str()},{"type","sketch"},{"name",sk->name}};
    json j = node_properties(doc, scene, ref.body);
    j["ref"] = ref.str();
    return j;
  }
  Node sketchNode;
  if(const auto* sk=scene.sketch(ref.body)) { sketchNode.kind=Node::Kind::Body; sketchNode.name=sk->name; }
  const Node* n = scene.sketch(ref.body)?&sketchNode:scene.node(ref.body);
  if (!n) throw Error("unknown node: " + ref.body);
  if (n->kind != Node::Kind::Body || n->body_missing) throw Error("not an available body: " + ref.body);
  TopoDS_Shape proto = scene.sketch(ref.body)?node_world_shape(doc,scene,ref.body):body_shape(doc, n->body_key);
  TopoDS_Shape world_body = node_world_shape(doc, scene, ref.body);
  TopoDS_Shape sub = subshape(world_body, ref.kind, ref.index);
  json j;
  j["ref"] = ref.str();
  j["body"] = ref.body;
  j["body_name"] = n->name;
  j["index"] = ref.index;
  j["bbox"] = bbox_json(shape_bbox(sub));

  if (ref.kind == Ref::Kind::Face) {
    const TopoDS_Face& face = TopoDS::Face(sub);
    BRepAdaptor_Surface surf(face);
    j["type"] = "face";
    j["surface"] = surface_type(surf.GetType());
    const MassProperties area = area_properties(face);
    j["area"] = area.mass;
    j["center"] = pnt(area.centre);
    bool reversed = face.Orientation() == TopAbs_REVERSED;
    switch (surf.GetType()) {
      case GeomAbs_Plane: {
        gp_Dir nrm = surf.Plane().Axis().Direction();
        if (reversed) nrm.Reverse();
        j["normal"] = dir(nrm);
        j["origin"] = pnt(surf.Plane().Location());
        break;
      }
      case GeomAbs_Cylinder:
        j["axis"] = dir(surf.Cylinder().Axis().Direction());
        j["axis_origin"] = pnt(surf.Cylinder().Location());
        j["radius"] = surf.Cylinder().Radius();
        j["diameter"] = 2 * surf.Cylinder().Radius();
        break;
      case GeomAbs_Cone:
        j["axis"] = dir(surf.Cone().Axis().Direction());
        j["apex"] = pnt(surf.Cone().Apex());
        j["half_angle_deg"] = surf.Cone().SemiAngle() * 180.0 / M_PI;
        j["ref_radius"] = surf.Cone().RefRadius();
        break;
      case GeomAbs_Sphere:
        j["center"] = pnt(surf.Sphere().Location());
        j["radius"] = surf.Sphere().Radius();
        j["diameter"] = 2 * surf.Sphere().Radius();
        break;
      case GeomAbs_Torus:
        j["axis"] = dir(surf.Torus().Axis().Direction());
        j["center"] = pnt(surf.Torus().Location());
        j["major_radius"] = surf.Torus().MajorRadius();
        j["minor_radius"] = surf.Torus().MinorRadius();
        break;
      default:
        break;
    }
    // Adjacent faces and bounding edges, by ordinal in the prototype.
    TopoDS_Shape proto_face = subshape(proto, Ref::Kind::Face, ref.index);
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(proto, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    TopTools_IndexedMapOfShape faces, edges;
    TopExp::MapShapes(proto, TopAbs_FACE, faces);
    TopExp::MapShapes(proto, TopAbs_EDGE, edges);
    std::set<int> adj, my_edges;
    for (TopExp_Explorer e(proto_face, TopAbs_EDGE); e.More(); e.Next()) {
      int ei = edges.FindIndex(e.Current());
      if (ei > 0) my_edges.insert(ei - 1);
      const TopTools_ListOfShape* owners = edge_faces.Seek(e.Current());
      if (!owners) continue;
      for (const auto& f : *owners) {
        int fi = faces.FindIndex(f);
        if (fi > 0 && fi - 1 != ref.index) adj.insert(fi - 1);
      }
    }
    j["edges"] = json(std::vector<int>(my_edges.begin(), my_edges.end()));
    j["adjacent_faces"] = json(std::vector<int>(adj.begin(), adj.end()));
    return j;
  }
  if (ref.kind == Ref::Kind::Edge) {
    const TopoDS_Edge& edge = TopoDS::Edge(sub);
    BRepAdaptor_Curve c(edge);
    j["type"] = "edge";
    j["curve"] = curve_type(c.GetType());
    j["length"] = GCPnts_AbscissaPoint::Length(c);
    j["start"] = pnt(c.Value(c.FirstParameter()));
    j["end"] = pnt(c.Value(c.LastParameter()));
    switch (c.GetType()) {
      case GeomAbs_Line: j["direction"] = dir(c.Line().Direction()); break;
      case GeomAbs_Circle:
        j["center"] = pnt(c.Circle().Location());
        j["axis"] = dir(c.Circle().Axis().Direction());
        j["radius"] = c.Circle().Radius();
        j["diameter"] = 2 * c.Circle().Radius();
        break;
      case GeomAbs_Ellipse:
        j["center"] = pnt(c.Ellipse().Location());
        j["major_radius"] = c.Ellipse().MajorRadius();
        j["minor_radius"] = c.Ellipse().MinorRadius();
        break;
      default: break;
    }
    TopoDS_Shape proto_edge = subshape(proto, Ref::Kind::Edge, ref.index);
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(proto, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    TopTools_IndexedMapOfShape faces, verts;
    TopExp::MapShapes(proto, TopAbs_FACE, faces);
    TopExp::MapShapes(proto, TopAbs_VERTEX, verts);
    std::set<int> adj, vs;
    if (const TopTools_ListOfShape* owners = edge_faces.Seek(proto_edge))
      for (const auto& f : *owners) {
        int fi = faces.FindIndex(f);
        if (fi > 0) adj.insert(fi - 1);
      }
    for (TopExp_Explorer e(proto_edge, TopAbs_VERTEX); e.More(); e.Next()) {
      int vi = verts.FindIndex(e.Current());
      if (vi > 0) vs.insert(vi - 1);
    }
    j["adjacent_faces"] = json(std::vector<int>(adj.begin(), adj.end()));
    j["vertices"] = json(std::vector<int>(vs.begin(), vs.end()));
    return j;
  }
  // vertex
  j["type"] = "vertex";
  j["point"] = pnt(BRep_Tool::Pnt(TopoDS::Vertex(sub)));
  return j;
}

// ---------------------------------------------------------------- distance
// BRepExtrema_DistShapeShape over two whole bodies tries every vertex/edge/face pair whose boxes do not rule it
// out: 10 s to minutes between two Engine bodies. The display meshes already sit on the shapes, so: find the
// closest triangles with a BVH (milliseconds), keep the face pairs that come within the meshes' deflection of that
// minimum, and run the exact extrema on those few pairs only. Without triangulation (CLI on an unmeshed document,
// non-rigid instances) a copy is meshed for the search (gap log #4).
namespace {
using Vec3d = BVH_Vec3d;

// Squared distance of two segments, with the closest points.
double segSeg(const Vec3d& p1, const Vec3d& q1, const Vec3d& p2, const Vec3d& q2, Vec3d& c1, Vec3d& c2) {
  const Vec3d d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
  const double a = d1.Dot(d1), e = d2.Dot(d2), f = d2.Dot(r);
  double s = 0, t = 0;
  if (a <= 1e-30 && e <= 1e-30) {
    c1 = p1;
    c2 = p2;
    return r.Dot(r);
  }
  if (a <= 1e-30) {
    t = std::clamp(f / e, 0.0, 1.0);
  } else {
    const double c = d1.Dot(r);
    if (e <= 1e-30) {
      s = std::clamp(-c / a, 0.0, 1.0);
    } else {
      const double b = d1.Dot(d2), denom = a * e - b * b;
      s = denom > 1e-30 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
      t = (b * s + f) / e;
      if (t < 0) { t = 0; s = std::clamp(-c / a, 0.0, 1.0); }
      else if (t > 1) { t = 1; s = std::clamp((b - c) / a, 0.0, 1.0); }
    }
  }
  c1 = p1 + d1 * s;
  c2 = p2 + d2 * t;
  const Vec3d d = c1 - c2;
  return d.Dot(d);
}

// Where segment p-q passes through triangle t0 t1 t2, if it does (Moller-Trumbore).
bool segmentCrosses(const Vec3d& p, const Vec3d& q, const Vec3d& t0, const Vec3d& t1, const Vec3d& t2, Vec3d& at) {
  const Vec3d d = q - p, e1 = t1 - t0, e2 = t2 - t0, h = Vec3d::Cross(d, e2);
  const double det = e1.Dot(h);
  if (std::abs(det) < 1e-30) return false;
  const Vec3d s = p - t0, g = Vec3d::Cross(s, e1);
  const double u = s.Dot(h) / det, v = d.Dot(g) / det, t = e2.Dot(g) / det;
  if (u < 0 || v < 0 || u + v > 1 || t < 0 || t > 1) return false;
  at = p + d * t;
  return true;
}

// Two triangles: 0 where they cross (so that the meshes' distance is a lower bound, gap log #4), else closest at a
// vertex of one against the other or edge against edge; with the closest points.
double triTri(const Vec3d a[3], const Vec3d b[3], Vec3d& pa, Vec3d& pb) {
  for (int i = 0; i < 3; ++i) {
    Vec3d at;
    if (segmentCrosses(a[i], a[(i + 1) % 3], b[0], b[1], b[2], at) || segmentCrosses(b[i], b[(i + 1) % 3], a[0], a[1], a[2], at)) {
      pa = pb = at;
      return 0;
    }
  }
  double best = std::numeric_limits<double>::max();
  auto keep = [&](double d, const Vec3d& x, const Vec3d& y) {
    if (d < best) {
      best = d;
      pa = x;
      pb = y;
    }
  };
  for (int i = 0; i < 3; ++i) {
    const Vec3d onB = BVH_Tools<double, 3>::PointTriangleProjection(a[i], b[0], b[1], b[2]);
    keep((a[i] - onB).Dot(a[i] - onB), a[i], onB);
    const Vec3d onA = BVH_Tools<double, 3>::PointTriangleProjection(b[i], a[0], a[1], a[2]);
    keep((b[i] - onA).Dot(b[i] - onA), onA, b[i]);
    for (int k = 0; k < 3; ++k) {
      Vec3d c1, c2;
      const double d = segSeg(a[i], a[(i + 1) % 3], b[k], b[(k + 1) % 3], c1, c2);
      keep(d, c1, c2);
    }
  }
  return best;
}

class TrianglePairs : public BVH_PairDistance<double, 3, BRepExtrema_TriangleSet> {
 public:
  // Pass 1 (collect == nullptr): the minimum. Pass 2: every face pair within `within` (squared), with its closest
  // points on the meshes.
  struct Near {
    double sq;
    Vec3d a, b;
  };
  std::map<std::pair<int, int>, Near>* collect = nullptr;
  void setWithin(double sq) { myDistance = sq; }
  Standard_Boolean Accept(const Standard_Integer i1, const Standard_Integer i2) override {
    Vec3d a[3], b[3], pa, pb;
    myBVHSet1->GetVertices(i1, a[0], a[1], a[2]);
    myBVHSet2->GetVertices(i2, b[0], b[1], b[2]);
    const double d = triTri(a, b, pa, pb);
    if (collect) {
      if (d > myDistance) return Standard_False;
      const auto key = std::make_pair(myBVHSet1->GetFaceID(i1), myBVHSet2->GetFaceID(i2));
      auto it = collect->find(key);
      if (it == collect->end()) collect->emplace(key, Near{d, pa, pb});
      else if (d < it->second.sq) it->second = Near{d, pa, pb};
      return Standard_True;
    }
    if (d >= myDistance) return Standard_False;
    myDistance = d;
    return Standard_True;
  }
  Standard_Boolean Stop() const override { return !collect && myDistance == 0.0; }
};

// The faces of a shape and the coarsest deflection of their meshes; false if a face has no mesh.
bool meshedFaces(const TopoDS_Shape& shape, BRepExtrema_ShapeList& faces, double& deflection) {
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
    if (tri.IsNull() || tri->NbTriangles() == 0) return false;
    deflection = std::max(deflection, tri->Deflection());
    faces.Append(ex.Current());
  }
  return faces.Size() > 0;
}

// The faces of a shape for the pair search, meshed: its own meshes (the viewer's) when every face has one, else a
// copy meshed here, coarse for its size. Without meshes the search used to fall back to solid against solid, whose
// inside test put a vertex of one body inside the other and read 0 (gap log #4).
bool pairFaces(const TopoDS_Shape& shape, BRepExtrema_ShapeList& faces, double& deflection) {
  double own = 0;
  if (meshedFaces(shape, faces, own)) {
    deflection = std::max(deflection, own);
    return true;
  }
  faces.Clear();
  if (!TopExp_Explorer(shape, TopAbs_FACE).More()) return false;
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  const double size = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
  const TopoDS_Shape copy = BRepBuilderAPI_Copy(shape, Standard_False, Standard_False).Shape();
  BRepMesh_IncrementalMesh(copy, std::clamp(size * 1e-3, 1e-3, 0.5), Standard_False, 0.5, Standard_True);
  own = 0;
  if (!meshedFaces(copy, faces, own)) {
    faces.Clear();
    return false;
  }
  deflection = std::max(deflection, own);
  return true;
}

// A body's faces in place of its solids: the distance is between surfaces, and BRepExtrema's inside test for a
// solid misclassifies points near long spline faces (gap log #4).
TopoDS_Shape surfaceOf(const TopoDS_Shape& shape) {
  if (!TopExp_Explorer(shape, TopAbs_SOLID).More()) return shape;
  TopoDS_Compound faces;
  BRep_Builder builder;
  builder.MakeCompound(faces);
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) builder.Add(faces, e.Current());
  return faces;
}

class CancelIndicator : public Message_ProgressIndicator {
 public:
  explicit CancelIndicator(std::function<bool()> cancelled) : m_cancelled(std::move(cancelled)) {}
  Standard_Boolean UserBreak() override { return m_cancelled && m_cancelled(); }
 protected:
  void Show(const Message_ProgressScope&, const Standard_Boolean) override {}
 private:
  std::function<bool()> m_cancelled;
};

struct Closest {
  double value = std::numeric_limits<double>::max();
  gp_Pnt a, b;
};

struct EdgePoint {
  std::string name;
  gp_Pnt point;
};

struct DistanceAnchor {
  std::string kind;
  gp_Pnt a, b;
};

bool samePoint(const gp_Pnt& a, const gp_Pnt& b) { return a.SquareDistance(b) <= 1e-14; }

void addEdgePoint(std::vector<EdgePoint>& points, const std::string& name, const gp_Pnt& point) {
  for (const auto& p : points)
    if (samePoint(p.point, point)) return;
  points.push_back({name, point});
}

std::vector<EdgePoint> edgePoints(const TopoDS_Edge& edge) {
  BRepAdaptor_Curve curve(edge);
  const double first = curve.FirstParameter(), last = curve.LastParameter();
  std::vector<EdgePoint> points;
  if (!std::isfinite(first) || !std::isfinite(last)) return points;
  const gp_Pnt start = curve.Value(first), end = curve.Value(last);
  addEdgePoint(points, "start", start);
  if (!samePoint(start, end)) addEdgePoint(points, "end", end);
  addEdgePoint(points, "midpoint", curve.Value((first + last) * 0.5));
  // A closed circle has no visually privileged seam. Quarter points make its familiar cardinal anchors
  // available, while curve/curve extrema below add the points nearest and farthest from the other edge.
  if (curve.GetType() == GeomAbs_Circle && samePoint(start, end)) {
    addEdgePoint(points, "quarter", curve.Value(first + (last - first) * 0.25));
    addEdgePoint(points, "three_quarter", curve.Value(first + (last - first) * 0.75));
  }
  return points;
}

void addAnchor(std::vector<DistanceAnchor>& anchors, const std::string& kind, const gp_Pnt& a, const gp_Pnt& b) {
  for (const auto& option : anchors) {
    if (option.kind == kind && samePoint(option.a, a) && samePoint(option.b, b)) return;
    // Keep the named closest/farthest choices even when one currently coincides with the picked anchors.
    // Their meaning remains useful, and a user should always be able to request either extreme explicitly.
    if (kind != "closest" && kind != "farthest" && samePoint(option.a, a) && samePoint(option.b, b)) return;
  }
  anchors.push_back({kind, a, b});
}

json anchorJson(const DistanceAnchor& option) {
  const gp_Vec delta(option.a, option.b);
  return {{"kind", option.kind}, {"value", option.a.Distance(option.b)}, {"point_a", pnt(option.a)},
          {"point_b", pnt(option.b)}, {"delta", {delta.X(), delta.Y(), delta.Z()}}};
}

// Exact minimum between two shapes; only the minimum is searched (the default also looks for maxima).
void exactDistance(const TopoDS_Shape& s1, const TopoDS_Shape& s2, bool parallel, const std::function<bool()>& cancelled, Closest& best) {
  BRepExtrema_DistShapeShape dist;
  dist.LoadS1(s1);
  dist.LoadS2(s2);
  dist.SetFlag(Extrema_ExtFlag_MIN);
#if OCC_VERSION_HEX >= 0x070700
  dist.SetMultiThread(parallel ? Standard_True : Standard_False);
#endif
  Handle(CancelIndicator) progress = new CancelIndicator(cancelled);
  dist.Perform(progress->Start());
  if (cancelled && cancelled()) throw Error("cancelled");
  if (!dist.IsDone() || dist.NbSolution() < 1) return;
  if (dist.Value() < best.value) best = Closest{dist.Value(), dist.PointOnShape1(1), dist.PointOnShape2(1)};
}
}  // namespace

json measure_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b, const std::function<bool()>& cancelled) {
  const TopoDS_Shape s1 = ref_shape(doc, scene, a), s2 = ref_shape(doc, scene, b);
  json j = shape_distance(s1, s2, cancelled);
  j["kind"] = "distance";
  j["refs"] = {a.str(), b.str()};
  // Touching, or read off a shape the kernel does not hold as valid (a self-intersecting profile): say which.
  if (j["value"].get<double>() < 1e-6) {
    json warnings = json::array();
    for (const auto& [ref, shape] : {std::make_pair(&a, &s1), std::make_pair(&b, &s2)})
      if (!BRepCheck_Analyzer(*shape).IsValid()) warnings.push_back(ref->str() + " is not a valid shape (the kernel's check fails): a distance to it may be wrong");
    if (!warnings.empty()) j["warnings"] = warnings;
  }
  return j;
}

json shape_distance(const TopoDS_Shape& s1, const TopoDS_Shape& s2, const std::function<bool()>& cancelled) {
  Closest best;
  bool approximate = false;
  BRepExtrema_ShapeList f1, f2;
  double deflection = 0;
  if (pairFaces(s1, f1, deflection) && pairFaces(s2, f2, deflection)) {
    Handle(BRepExtrema_TriangleSet) t1 = new BRepExtrema_TriangleSet(f1), t2 = new BRepExtrema_TriangleSet(f2);
    TrianglePairs nearest;
    nearest.SetBVHSets(t1.get(), t2.get());
    const double meshMin = std::sqrt(nearest.ComputeDistance());
    if (!nearest.IsDone()) throw Error("distance computation failed");
    // The surfaces lie within one deflection of their meshes, so the true closest pair is at most this far out.
    const double within = meshMin + 2.0 * deflection + 1e-6;
    std::map<std::pair<int, int>, TrianglePairs::Near> pairs;
    TrianglePairs gather;
    gather.collect = &pairs;
    gather.setWithin(within * within);
    gather.SetBVHSets(t1.get(), t2.get());
    gather.Select();
    std::vector<std::pair<double, std::pair<int, int>>> order;
    for (const auto& p : pairs) order.push_back({p.second.sq, p.first});
    std::sort(order.begin(), order.end());
    // The exact face-to-face extrema is the slow part (~0.3 s a pair between Engine castings) and the pairs are
    // independent: run them side by side, each allowed to split further, and keep the smallest.
    std::mutex mu;
    std::atomic<size_t> next{0};
    auto worker = [&] {
      for (size_t i = next++; i < order.size(); i = next++) {
        if (cancelled && cancelled()) return;
        const auto& meshed = pairs.at(order[i].second);
        Closest mine;
        bool trusted = false;
        try {
          exactDistance(f1(order[i].second.first), f2(order[i].second.second), true, cancelled, mine);
          // The pair's meshes bound it from below. An exact answer well under that came from a point on a surface
          // wrongly taken to be inside its face (gap log #4: a disc's plane "reaching" the ring at 0 mm).
          trusted = mine.value != std::numeric_limits<double>::max() && mine.value >= std::sqrt(meshed.sq) - 3.0 * deflection - 1e-6;
        } catch (...) {
        }
        // Then the meshes' own closest points, within their deflection of the truth, and said so.
        if (!trusted) mine = Closest{std::sqrt(meshed.sq), gp_Pnt(meshed.a.x(), meshed.a.y(), meshed.a.z()), gp_Pnt(meshed.b.x(), meshed.b.y(), meshed.b.z())};
        std::lock_guard<std::mutex> lock(mu);
        if (mine.value < best.value) {
          best = mine;
          approximate = !trusted;
        }
      }
    };
    std::vector<std::thread> pool;
    for (size_t i = 1; i < std::min<size_t>(order.size(), std::max(2u, std::thread::hardware_concurrency())); ++i) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    if (cancelled && cancelled()) throw Error("cancelled");
  } else {
    // A point, a vertex or an edge against anything: the exact search, between surfaces.
    exactDistance(surfaceOf(s1), surfaceOf(s2), true, cancelled, best);
  }
  if (best.value == std::numeric_limits<double>::max()) throw Error("distance computation failed");
  json j;
  j["value"] = best.value;
  j["unit"] = "mm";
  j["point_a"] = pnt(best.a);
  j["point_b"] = pnt(best.b);
  j["delta"] = {best.b.X() - best.a.X(), best.b.Y() - best.a.Y(), best.b.Z() - best.a.Z()};
  if (approximate) {
    j["approximate"] = true;
    j["tolerance_mm"] = 2 * deflection;
  }
  return j;
}

json measure_edge_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b,
                           const Vec3& picked_a, const Vec3& picked_b, double snap_tolerance,
                           const std::function<bool()>& cancelled) {
  if (a.kind != Ref::Kind::Edge || b.kind != Ref::Kind::Edge)
    return measure_distance(doc, scene, a, b, cancelled);
  const TopoDS_Edge edge_a = TopoDS::Edge(ref_shape(doc, scene, a));
  const TopoDS_Edge edge_b = TopoDS::Edge(ref_shape(doc, scene, b));
  json result = measure_distance(doc, scene, a, b, cancelled);
  std::vector<EdgePoint> points_a = edgePoints(edge_a), points_b = edgePoints(edge_b);
  std::vector<DistanceAnchor> extrema;

  BRepExtrema_ExtCC cc(edge_a, edge_b);
  if (cc.IsDone() && !cc.IsParallel()) {
    for (int i = 1; i <= cc.NbExt(); ++i) {
      const gp_Pnt pa = cc.PointOnE1(i), pb = cc.PointOnE2(i);
      addAnchor(extrema, "extremum", pa, pb);
      addEdgePoint(points_a, "extremum", pa);
      addEdgePoint(points_b, "extremum", pb);
    }
  }
  // Curve/curve extrema do not include every trimmed-boundary case. In particular, the far side of a
  // circle relative to the endpoint of another edge is a point/curve extremum.
  const std::vector<EdgePoint> base_a = points_a, base_b = points_b;
  for (const auto& fixed : base_a) {
    BRepExtrema_ExtPC pc(BRepBuilderAPI_MakeVertex(fixed.point).Vertex(), edge_b);
    if (!pc.IsDone()) continue;
    for (int i = 1; i <= pc.NbExt(); ++i) {
      const gp_Pnt other = pc.Point(i);
      addAnchor(extrema, "extremum", fixed.point, other);
      addEdgePoint(points_b, "extremum", other);
    }
  }
  for (const auto& fixed : base_b) {
    BRepExtrema_ExtPC pc(BRepBuilderAPI_MakeVertex(fixed.point).Vertex(), edge_a);
    if (!pc.IsDone()) continue;
    for (int i = 1; i <= pc.NbExt(); ++i) {
      const gp_Pnt other = pc.Point(i);
      addAnchor(extrema, "extremum", other, fixed.point);
      addEdgePoint(points_a, "extremum", other);
    }
  }

  auto project = [&](const gp_Pnt& point, const TopoDS_Edge& edge, bool pointFirst) {
    Closest nearest;
    const TopoDS_Shape vertex = BRepBuilderAPI_MakeVertex(point);
    if (pointFirst) exactDistance(vertex, edge, false, cancelled, nearest);
    else exactDistance(edge, vertex, false, cancelled, nearest);
    if (nearest.value == std::numeric_limits<double>::max()) return point;
    return pointFirst ? nearest.b : nearest.a;
  };
  auto snap = [&](const gp_Pnt& picked, const TopoDS_Edge& edge, const std::vector<EdgePoint>& points, bool pointFirst) {
    const gp_Pnt projected = project(picked, edge, pointFirst);
    gp_Pnt snapped = projected;
    double best = std::max(snap_tolerance, 1e-9);
    for (const auto& candidate : points) {
      const double distance = projected.Distance(candidate.point);
      if (distance <= best) { best = distance; snapped = candidate.point; }
    }
    return snapped;
  };

  std::vector<DistanceAnchor> options;
  const gp_Pnt picked1(picked_a[0], picked_a[1], picked_a[2]), picked2(picked_b[0], picked_b[1], picked_b[2]);
  addAnchor(options, "picked", snap(picked1, edge_a, points_a, true), snap(picked2, edge_b, points_b, false));
  addAnchor(options, "closest", json_pnt(result["point_a"]), json_pnt(result["point_b"]));

  // The farthest of the curve/curve stationary pairs and trimmed endpoint pairs is a useful opposite
  // extreme for circles, arcs and splines. Endpoint-to-edge options below cover their boundary extrema.
  DistanceAnchor farthest{"farthest", options.front().a, options.front().b};
  for (const auto& option : extrema)
    if (option.a.SquareDistance(option.b) > farthest.a.SquareDistance(farthest.b)) farthest = {"farthest", option.a, option.b};
  for (const auto& pa : points_a)
    for (const auto& pb : points_b)
      if (pa.point.SquareDistance(pb.point) > farthest.a.SquareDistance(farthest.b)) farthest = {"farthest", pa.point, pb.point};
  addAnchor(options, "farthest", farthest.a, farthest.b);

  auto pointOptions = [&](const std::vector<EdgePoint>& points, const TopoDS_Edge& other, bool fromA) {
    for (const auto& candidate : points) {
      if (candidate.name == "extremum") continue;
      const gp_Pnt nearest = project(candidate.point, other, fromA);
      addAnchor(options, std::string(fromA ? "edge1_" : "edge2_") + candidate.name,
                fromA ? candidate.point : nearest, fromA ? nearest : candidate.point);
    }
  };
  pointOptions(points_a, edge_b, true);
  pointOptions(points_b, edge_a, false);

  result["anchors"] = json::array();
  for (const auto& option : options) result["anchors"].push_back(anchorJson(option));
  result["anchor_index"] = 0;
  for (const char* key : {"value", "point_a", "point_b", "delta"}) result[key] = result["anchors"][0][key];
  return result;
}

json measure_angle(const Document& doc, const Scene& scene, const Ref& a, const Ref& b) {
  gp_Dir da, db;
  gp_Pnt pa, pb;
  std::string wa, wb;
  if (!ref_direction(doc, scene, a, da, wa, pa)) throw Error("reference has no direction (need a planar face, line, circle or cylinder): " + a.str());
  if (!ref_direction(doc, scene, b, db, wb, pb)) throw Error("reference has no direction (need a planar face, line, circle or cylinder): " + b.str());
  double ang = da.Angle(db) * 180.0 / M_PI;
  json j;
  j["kind"] = "angle";
  j["refs"] = {a.str(), b.str()};
  j["value"] = ang;
  j["supplement"] = 180.0 - ang;
  j["unit"] = "deg";
  j["direction_a"] = dir(da);
  j["direction_b"] = dir(db);
  j["meaning_a"] = wa;
  j["meaning_b"] = wb;
  j["point_a"] = pnt(pa);
  j["point_b"] = pnt(pb);
  j["origin"] = pnt(pa);  // where the two directions are compared when there is no construction below

  // The diagram on the objects themselves: the vertex where the two lines (or the two planes, seen along their
  // common line) meet, and from it one ray along each object, towards it. `reach` is how far along its ray the
  // object's own point lies. The rays include either the angle or its supplement; in the latter case ray b is
  // the extension of its object beyond the vertex (`ray_b_extended`), so that the drawn angle is the value.
  const gp_Vec va(da), vb(db);
  const bool normal_a = wa == "normal", normal_b = wb == "normal";
  gp_Pnt vertex, vertex_b;
  gp_Vec ray_a, ray_b;
  double reach_a = 0, reach_b = 0;
  bool built = false;
  if (!normal_a && !normal_b) {  // two lines: the closest points of one to the other (the same point when they meet)
    const gp_Vec w(pb, pa);
    const double dot = va.Dot(vb), d = va.Dot(w), e = vb.Dot(w), denom = 1.0 - dot * dot;
    if (denom > 1e-6) {
      const double s = (dot * e - d) / denom, t = (e - dot * d) / denom;
      vertex = pa.Translated(va * s);
      vertex_b = pb.Translated(vb * t);
      ray_a = s > 0 ? -va : va;
      ray_b = t > 0 ? -vb : vb;
      reach_a = std::abs(s);
      reach_b = std::abs(t);
      built = true;
    }
  } else if (normal_a && normal_b) {  // two planes: a section across their common line
    const gp_Vec along = va.Crossed(vb);
    if (along.SquareMagnitude() > 1e-6) {
      const double ha = va.Dot(gp_Vec(pa.XYZ())), hb = vb.Dot(gp_Vec(pb.XYZ()));
      const gp_Vec t = along.Normalized();
      const gp_Pnt on_line(((vb * ha - va * hb).Crossed(along) / along.SquareMagnitude()).XYZ());
      const gp_Pnt middle((pa.XYZ() + pb.XYZ()) * 0.5);
      vertex = vertex_b = on_line.Translated(t * gp_Vec(on_line, middle).Dot(t));
      ray_a = t.Crossed(va);
      ray_b = t.Crossed(vb);
      reach_a = gp_Vec(vertex, pa).Dot(ray_a);
      reach_b = gp_Vec(vertex, pb).Dot(ray_b);
      if (reach_a < 0) { ray_a.Reverse(); reach_a = -reach_a; }
      if (reach_b < 0) { ray_b.Reverse(); reach_b = -reach_b; }
      built = true;
    }
  }
  if (built) {
    const double drawn = ray_a.Angle(ray_b) * 180.0 / M_PI;
    const bool extended = std::abs(drawn - ang) > std::abs(180.0 - drawn - ang);
    if (extended) ray_b.Reverse();
    j["vertex"] = pnt(vertex);
    j["vertex_b"] = pnt(vertex_b);
    j["ray_a"] = {ray_a.X(), ray_a.Y(), ray_a.Z()};
    j["ray_b"] = {ray_b.X(), ray_b.Y(), ray_b.Z()};
    j["reach_a"] = reach_a;
    j["reach_b"] = reach_b;
    j["ray_b_extended"] = extended;
    j["origin"] = j["vertex"];
  }
  return j;
}

json measure_radius(const Document& doc, const Scene& scene, const Ref& a) {
  if (scene.node(a.body) && scene.node(a.body)->representation=="mesh") {
    const auto info=inspect_ref(doc,scene,a);
    if(info.contains("radius")) {
      auto result=info; result["kind"]="radius"; result["refs"]={a.str()}; result["value"]=info["radius"]; result["unit"]="mm";
      result["point_a"]=info["center"];
      for(const auto& c:mesh_circles(node_world_shape(doc,scene,a.body))) if(c.index==a.index || std::find(c.edges.begin(),c.edges.end(),a.index)!=c.edges.end()) {result["point_b"]=pnt(c.rim.front()); break;}
      return result;
    }
  }
  if (a.kind == Ref::Kind::Center) {
    Ref edge = a;
    edge.kind = Ref::Kind::Edge;
    json result = measure_radius(doc, scene, edge);
    result["refs"] = {a.str()};
    return result;
  }
  json info = inspect_ref(doc, scene, a);
  if (!info.contains("radius")) throw Error("reference has no radius (need a cylindrical/spherical face or circular edge): " + a.str());
  json j;
  j["kind"] = "radius";
  j["refs"] = {a.str()};
  j["value"] = info["radius"];
  j["diameter"] = info["diameter"];
  j["unit"] = "mm";
  if (info.contains("center")) j["center"] = info["center"];
  if (info.contains("axis")) j["axis"] = info["axis"];
  // Radius endpoints on the analytic geometry, including a cylinder's axis rather than
  // the surface's centre of mass (which is off-axis for a trimmed cylindrical face).
  const TopoDS_Shape shape = ref_shape(doc, scene, a);
  gp_Pnt center, rim;
  if (a.kind == Ref::Kind::Edge) {
    BRepAdaptor_Curve curve(TopoDS::Edge(shape));
    center = curve.Circle().Location();
    rim = curve.Value((curve.FirstParameter() + curve.LastParameter()) * 0.5);
  } else {
    BRepAdaptor_Surface surface(TopoDS::Face(shape));
    rim = surface.Value((surface.FirstUParameter() + surface.LastUParameter()) * 0.5,
                        (surface.FirstVParameter() + surface.LastVParameter()) * 0.5);
    if (surface.GetType() == GeomAbs_Cylinder) {
      const gp_Ax1 axis = surface.Cylinder().Axis();
      center = axis.Location().Translated(gp_Vec(axis.Direction()) * gp_Vec(axis.Location(), rim).Dot(gp_Vec(axis.Direction())));
    } else center = surface.Sphere().Location();
  }
  j["point_a"] = pnt(center);
  j["point_b"] = pnt(rim);
  return j;
}

json measure_bbox(const Document& doc, const Scene& scene, const std::vector<Ref>& refs) {
  Bnd_Box box;  // tight (TODO 10 B10)
  if (refs.empty()) {
    Vec3 lo, hi;
    if (scene_tight_bbox(doc, scene, {}, lo, hi)) box.Update(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  } else {
    for (const auto& r : refs) {
      if (r.kind == Ref::Kind::Body) {
        const Node* n = scene.node(r.body);
        if (n && n->kind == Node::Kind::Component) {
          Vec3 lo, hi;
          if (scene_tight_bbox(doc, scene, scene.bodies_under(r.body), lo, hi)) box.Update(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
          continue;
        }
        if (n && n->kind == Node::Kind::Body && !n->body_missing) {
          box.Add(node_tight_bbox(doc, scene, r.body));
          continue;
        }
      }
      box.Add(tight_bbox(ref_shape(doc, scene, r)));
    }
  }
  if (box.IsVoid()) throw Error("bounding box is empty");
  json j = bbox_json(box);
  j["kind"] = "bbox";
  json rs = json::array();
  for (const auto& r : refs) rs.push_back(r.str());
  j["refs"] = rs;
  j["unit"] = "mm";
  return j;
}

}  // namespace opad
