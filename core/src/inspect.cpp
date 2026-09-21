#include "opad/inspect.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
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

namespace opad {

namespace {

json pnt(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
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

Bnd_Box shape_bbox(const TopoDS_Shape& s) {
  Bnd_Box box;
  if (!s.IsNull()) BRepBndLib::Add(s, box, Standard_False);
  return box;
}

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
  const Node* n = scene.node(ref.body);
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
bool ref_direction(const Document& doc, const Scene& scene, const Ref& ref, gp_Dir& out, std::string& what) {
  TopoDS_Shape s = ref_shape(doc, scene, ref);
  if (ref.kind == Ref::Kind::Face) {
    BRepAdaptor_Surface surf(TopoDS::Face(s));
    switch (surf.GetType()) {
      case GeomAbs_Plane: {
        gp_Dir n = surf.Plane().Axis().Direction();
        if (s.Orientation() == TopAbs_REVERSED) n.Reverse();
        out = n; what = "normal"; return true;
      }
      case GeomAbs_Cylinder: out = surf.Cylinder().Axis().Direction(); what = "axis"; return true;
      case GeomAbs_Cone: out = surf.Cone().Axis().Direction(); what = "axis"; return true;
      case GeomAbs_Torus: out = surf.Torus().Axis().Direction(); what = "axis"; return true;
      default: return false;
    }
  }
  if (ref.kind == Ref::Kind::Edge) {
    BRepAdaptor_Curve c(TopoDS::Edge(s));
    switch (c.GetType()) {
      case GeomAbs_Line: out = c.Line().Direction(); what = "direction"; return true;
      case GeomAbs_Circle: out = c.Circle().Axis().Direction(); what = "axis"; return true;
      case GeomAbs_Ellipse: out = c.Ellipse().Axis().Direction(); what = "axis"; return true;
      default: {
        gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
        if (a.Distance(b) < 1e-9) return false;
        out = gp_Dir(gp_Vec(a, b)); what = "chord"; return true;
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
  if (scene_bbox(doc, scene, {}, lo, hi)) {
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

json node_properties(const Document& doc, const Scene& scene, const std::string& node_id, bool geometry) {
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
    GProp_GProps props;
    if (has_solid) {
      BRepGProp::VolumeProperties(world, props);
      j["volume"] = props.Mass();
      j["center_of_mass"] = pnt(props.CentreOfMass());
    }
    GProp_GProps sprops;
    BRepGProp::SurfaceProperties(world, sprops);
    j["area"] = sprops.Mass();
    if (!has_solid) j["center_of_mass"] = pnt(sprops.CentreOfMass());
    j["bbox"] = bbox_json(shape_bbox(world));
  } else {
    j["children"] = n->children.size();
    auto bodies = scene.bodies_under(node_id);
    j["bodies"] = bodies.size();
    Vec3 lo, hi;
    if (geometry && scene_bbox(doc, scene, bodies, lo, hi)) {
      Bnd_Box b;
      b.Update(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
      j["bbox"] = bbox_json(b);
    }
  }
  return j;
}

json inspect_ref(const Document& doc, const Scene& scene, const Ref& ref) {
  if (ref.kind == Ref::Kind::Point) {
    json j;
    j["ref"] = ref.str();
    j["type"] = "point";
    j["point"] = vec(ref.point);
    return j;
  }
  if (ref.kind == Ref::Kind::Body) {
    json j = node_properties(doc, scene, ref.body);
    j["ref"] = ref.str();
    return j;
  }
  const Node* n = scene.node(ref.body);
  if (!n) throw Error("unknown node: " + ref.body);
  if (n->kind != Node::Kind::Body || n->body_missing) throw Error("not an available body: " + ref.body);
  TopoDS_Shape proto = body_shape(doc, n->body_key);
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
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    j["area"] = props.Mass();
    j["center"] = pnt(props.CentreOfMass());
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
// non-rigid instances) it falls back to the exact search over everything.
namespace {
using Vec3d = BVH_Vec3d;

double segSegSq(const Vec3d& p1, const Vec3d& q1, const Vec3d& p2, const Vec3d& q2) {
  const Vec3d d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
  const double a = d1.Dot(d1), e = d2.Dot(d2), f = d2.Dot(r);
  double s = 0, t = 0;
  if (a <= 1e-30 && e <= 1e-30) return r.Dot(r);
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
  const Vec3d d = (p1 + d1 * s) - (p2 + d2 * t);
  return d.Dot(d);
}

// Two triangles that do not cross are closest at a vertex of one against the other, or edge against edge.
// (Crossing ones come out slightly above zero; their faces still reach the exact stage, which says 0.)
double triTriSq(const Vec3d a[3], const Vec3d b[3]) {
  double best = std::numeric_limits<double>::max();
  for (int i = 0; i < 3; ++i) {
    best = std::min(best, BVH_Tools<double, 3>::PointTriangleSquareDistance(a[i], b[0], b[1], b[2]));
    best = std::min(best, BVH_Tools<double, 3>::PointTriangleSquareDistance(b[i], a[0], a[1], a[2]));
    for (int k = 0; k < 3; ++k) best = std::min(best, segSegSq(a[i], a[(i + 1) % 3], b[k], b[(k + 1) % 3]));
  }
  return best;
}

class TrianglePairs : public BVH_PairDistance<double, 3, BRepExtrema_TriangleSet> {
 public:
  // Pass 1 (collect == nullptr): the minimum. Pass 2: every face pair within `within` (squared), closest first.
  std::map<std::pair<int, int>, double>* collect = nullptr;
  void setWithin(double sq) { myDistance = sq; }
  Standard_Boolean Accept(const Standard_Integer i1, const Standard_Integer i2) override {
    Vec3d a[3], b[3];
    myBVHSet1->GetVertices(i1, a[0], a[1], a[2]);
    myBVHSet2->GetVertices(i2, b[0], b[1], b[2]);
    const double d = triTriSq(a, b);
    if (collect) {
      if (d > myDistance) return Standard_False;
      const auto key = std::make_pair(myBVHSet1->GetFaceID(i1), myBVHSet2->GetFaceID(i2));
      auto it = collect->find(key);
      if (it == collect->end()) collect->emplace(key, d);
      else it->second = std::min(it->second, d);
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
  Closest best;
  int exactPairs = 0;
  BRepExtrema_ShapeList f1, f2;
  double deflection = 0;
  // Worth it from a handful of face pairs on; a single face against a single face is what the exact search does anyway.
  if (meshedFaces(s1, f1, deflection) && meshedFaces(s2, f2, deflection) && f1.Size() * f2.Size() > 8) {
    Handle(BRepExtrema_TriangleSet) t1 = new BRepExtrema_TriangleSet(f1), t2 = new BRepExtrema_TriangleSet(f2);
    TrianglePairs nearest;
    nearest.SetBVHSets(t1.get(), t2.get());
    const double meshMin = std::sqrt(nearest.ComputeDistance());
    if (nearest.IsDone()) {
      // The surfaces lie within one deflection of their meshes, so the true closest pair is at most this far out.
      const double within = meshMin + 2.0 * deflection + 1e-6;
      std::map<std::pair<int, int>, double> pairs;
      TrianglePairs gather;
      gather.collect = &pairs;
      gather.setWithin(within * within);
      gather.SetBVHSets(t1.get(), t2.get());
      gather.Select();
      std::vector<std::pair<double, std::pair<int, int>>> order;
      for (const auto& p : pairs) order.push_back({p.second, p.first});
      std::sort(order.begin(), order.end());
      // The exact face-to-face extrema is the slow part (~0.3 s a pair between Engine castings) and the pairs are
      // independent: run them side by side, each allowed to split further, and keep the smallest.
      std::mutex mu;
      std::atomic<size_t> next{0};
      std::atomic<bool> failed{false};
      auto worker = [&] {
        for (size_t i = next++; i < order.size(); i = next++) {
          if (failed || (cancelled && cancelled())) return;
          try {
            Closest mine;
            exactDistance(f1(order[i].second.first), f2(order[i].second.second), true, cancelled, mine);
            std::lock_guard<std::mutex> lock(mu);
            if (mine.value < best.value) best = mine;
          } catch (...) {
            failed = true;
          }
        }
      };
      std::vector<std::thread> pool;
      for (size_t i = 1; i < std::min<size_t>(order.size(), std::max(2u, std::thread::hardware_concurrency())); ++i) pool.emplace_back(worker);
      worker();
      for (auto& t : pool) t.join();
      if (cancelled && cancelled()) throw Error("cancelled");
      if (!failed) exactPairs = static_cast<int>(order.size());
    }
  }
  if (exactPairs == 0) exactDistance(s1, s2, true, cancelled, best);
  if (best.value == std::numeric_limits<double>::max()) throw Error("distance computation failed");
  json j;
  j["kind"] = "distance";
  j["refs"] = {a.str(), b.str()};
  j["value"] = best.value;
  j["unit"] = "mm";
  j["point_a"] = pnt(best.a);
  j["point_b"] = pnt(best.b);
  j["delta"] = {best.b.X() - best.a.X(), best.b.Y() - best.a.Y(), best.b.Z() - best.a.Z()};
  return j;
}

json measure_angle(const Document& doc, const Scene& scene, const Ref& a, const Ref& b) {
  gp_Dir da, db;
  std::string wa, wb;
  if (!ref_direction(doc, scene, a, da, wa)) throw Error("reference has no direction (need a planar face, line, circle or cylinder): " + a.str());
  if (!ref_direction(doc, scene, b, db, wb)) throw Error("reference has no direction (need a planar face, line, circle or cylinder): " + b.str());
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
  return j;
}

json measure_radius(const Document& doc, const Scene& scene, const Ref& a) {
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
  return j;
}

json measure_bbox(const Document& doc, const Scene& scene, const std::vector<Ref>& refs) {
  Bnd_Box box;
  if (refs.empty()) {
    Vec3 lo, hi;
    if (scene_bbox(doc, scene, {}, lo, hi)) box.Update(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  } else {
    for (const auto& r : refs) {
      if (r.kind == Ref::Kind::Body) {
        const Node* n = scene.node(r.body);
        if (n && n->kind == Node::Kind::Component) {
          Vec3 lo, hi;
          if (scene_bbox(doc, scene, scene.bodies_under(r.body), lo, hi)) box.Update(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
          continue;
        }
      }
      BRepBndLib::Add(ref_shape(doc, scene, r), box, Standard_False);
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
