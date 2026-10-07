#include "opad/sim/frames.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>

#include <cmath>
#include <map>

#include "../design/engine.hpp"

namespace opad::sim {

namespace {

gp_Vec vec3(const json& v, const char* what) {
  if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
    throw Error(std::string("a joint frame's ") + what + " is [x, y, z]");
  return gp_Vec(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

Frame make_frame(const gp_Pnt& origin, const gp_Vec& z, const gp_Vec* x) {
  if (z.Magnitude() < 1e-12) throw Error("a joint frame's axis must not be zero");
  return design::plane_through(origin, z, x);
}

gp_Pnt centre_of(const TopoDS_Shape& s) {
  GProp_GProps g;
  if (s.ShapeType() == TopAbs_FACE) BRepGProp::SurfaceProperties(s, g);
  else BRepGProp::LinearProperties(s, g);
  return g.CentreOfMass();
}

gp_Pnt onto(const gp_Ax1& axis, const gp_Pnt& p) {
  const gp_Vec v(axis.Location(), p);
  return axis.Location().Translated(gp_Vec(axis.Direction()) * v.Dot(gp_Vec(axis.Direction())));
}

// A frame from one picked entity.
std::pair<gp_Pnt, gp_Vec> from_entity(const TopoDS_Shape& sub) {
  if (sub.ShapeType() == TopAbs_VERTEX) return {BRep_Tool::Pnt(TopoDS::Vertex(sub)), gp_Vec(0, 0, 1)};
  if (sub.ShapeType() == TopAbs_EDGE) {
    BRepAdaptor_Curve c(TopoDS::Edge(sub));
    if (c.GetType() == GeomAbs_Circle || c.GetType() == GeomAbs_Ellipse) {
      const gp_Ax1 a = c.GetType() == GeomAbs_Circle ? c.Circle().Axis() : c.Ellipse().Axis();
      return {a.Location(), gp_Vec(a.Direction())};
    }
    if (c.GetType() == GeomAbs_Line) return {centre_of(sub), gp_Vec(c.Line().Direction())};
    throw Error("a joint frame on an edge needs a straight or circular edge");
  }
  if (sub.ShapeType() == TopAbs_FACE) {
    BRepAdaptor_Surface s(TopoDS::Face(sub));
    gp_Ax1 axis;
    switch (s.GetType()) {
      case GeomAbs_Plane: {
        gp_Dir n = s.Plane().Axis().Direction();
        if (!s.Plane().Position().Direct()) n.Reverse();
        if (sub.Orientation() == TopAbs_REVERSED) n.Reverse();  // outward from the body
        return {centre_of(sub), gp_Vec(n)};
      }
      case GeomAbs_Cylinder: axis = s.Cylinder().Axis(); break;
      case GeomAbs_Cone: axis = s.Cone().Axis(); break;
      case GeomAbs_Torus: axis = s.Torus().Axis(); break;
      case GeomAbs_Sphere: return {s.Sphere().Location(), gp_Vec(0, 0, 1)};
      default: throw Error("a joint frame on a face needs a planar, cylindrical, conical, toroidal or spherical face");
    }
    return {onto(axis, centre_of(sub)), gp_Vec(axis.Direction())};
  }
  throw Error("a joint frame needs a face, an edge or a vertex");
}

bool is_ref(const json& j) {
  if (j.is_string()) return true;
  return j.is_object() && (j.contains("body") || j.contains("select"));
}

}  // namespace

std::string frame_node(const json& at) {
  try {
    const json* r = &at;
    if (at.is_object())
      for (const char* k : {"face", "edge", "vertex"})
        if (at.contains(k)) r = &at[k];
    if (r->is_string()) {
      const Ref ref = Ref::parse(r->get<std::string>());
      return ref.kind == Ref::Kind::Point ? std::string() : ref.body;
    }
    if (r->is_object() && r->contains("body") && (*r)["body"].is_string()) return (*r)["body"].get<std::string>();
  } catch (const std::exception&) {
  }
  return {};
}

Frame joint_frame(const Document& doc, const Scene& scene, const json& at) {
  if (at.is_null()) throw Error("no joint frame was chosen");
  gp_Pnt origin;
  gp_Vec z(0, 0, 1);
  const json opts = at.is_object() ? at : json::object();
  const std::map<std::string, TopoDS_Shape> fresh;
  const design::ParamTable params;
  const design::Ctx ctx{doc, params, scene, fresh, {}};
  if (is_ref(at) || (at.is_object() && (at.contains("face") || at.contains("edge") || at.contains("vertex")))) {
    const json& r = at.is_object() && at.contains("face") ? at["face"] : at.is_object() && at.contains("edge") ? at["edge"]
                    : at.is_object() && at.contains("vertex")                                    ? at["vertex"]
                                                                                                  : at;
    if (r.is_string() && Ref::parse(r.get<std::string>()).kind == Ref::Kind::Point) {
      const Ref p = Ref::parse(r.get<std::string>());
      origin = gp_Pnt(p.point[0], p.point[1], p.point[2]);
    } else {
      const design::ResolvedRef rr = ctx.resolve(r);
      if (rr.sub.ShapeType() != TopAbs_FACE && rr.sub.ShapeType() != TopAbs_EDGE && rr.sub.ShapeType() != TopAbs_VERTEX)
        throw Error("a joint frame needs a face, an edge or a vertex, not a whole body");
      std::tie(origin, z) = from_entity(rr.sub);
      // A circular edge's axis points out of the flat face it borders (a hole's rim on a panel: out of the panel), so
      // the same pick gives the same direction every time.
      if (rr.sub.ShapeType() == TopAbs_EDGE && BRepAdaptor_Curve(TopoDS::Edge(rr.sub)).GetType() == GeomAbs_Circle) {
        TopTools_IndexedDataMapOfShapeListOfShape faces;
        TopExp::MapShapesAndAncestors(ctx.node_shape(rr.node), TopAbs_EDGE, TopAbs_FACE, faces);
        if (const int i = faces.FindIndex(rr.sub); i > 0)
          for (const TopoDS_Shape& f : faces.FindFromIndex(i)) {
            BRepAdaptor_Surface surf(TopoDS::Face(f));
            if (surf.GetType() != GeomAbs_Plane) continue;
            const gp_Vec n = from_entity(f).second;
            if (std::fabs(std::fabs(n.Dot(z) / (n.Magnitude() * z.Magnitude())) - 1) < 1e-6) {
              z = n;
              break;
            }
          }
      }
    }
  } else if (at.is_object() && at.contains("point")) {
    const gp_Vec p = vec3(at["point"], "point");
    origin = gp_Pnt(p.XYZ());
  } else if (at.is_object() && at.contains("z")) {
    origin = gp_Pnt(vec3(at.value("origin", json::array({0, 0, 0})), "origin").XYZ());
    z = vec3(at["z"], "z");
  } else if (at.is_object() && at.contains("feature") && scene.feature(at["feature"].get<std::string>()) &&
             scene.feature(at["feature"].get<std::string>())->result.contains("plane")) {
    const Frame f = Frame::from_json(scene.feature(at["feature"].get<std::string>())->result["plane"]);
    origin = gp_Pnt(f.origin[0], f.origin[1], f.origin[2]);
    const Vec3 n = f.normal();
    z = gp_Vec(n[0], n[1], n[2]);
  } else if (at.is_object()) {
    const gp_Ax1 a = ctx.axis(at);  // {"base"}, {"direction", "origin"}, {"feature"}, {"sketch", "entity"}
    origin = a.Location();
    z = gp_Vec(a.Direction());
  } else {
    throw Error("a joint frame is a face, edge or vertex reference, an axis or {\"origin\", \"z\"}");
  }
  if (opts.value("flip", false)) z.Reverse();
  if (opts.contains("offset")) {
    if (!opts["offset"].is_number()) throw Error("a joint frame's offset is a number of mm");
    origin.Translate(gp_Vec(gp_Dir(z)) * opts["offset"].get<double>());
  }
  if (opts.contains("x")) {
    const gp_Vec x = vec3(opts["x"], "x");
    return make_frame(origin, z, &x);
  }
  return make_frame(origin, z, nullptr);
}

}  // namespace opad::sim
