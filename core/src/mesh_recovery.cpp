#include "opad/mesh.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_Context.hxx>
#include <BRepMesh_DelabellaMeshAlgoFactory.hxx>
#include <BRepMesh_FaceDiscret.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepMesh_MeshAlgoFactory.hxx>
#include <BRepTools_Modifier.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeCustom_ConvertToRevolution.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace opad {
namespace {
// A face with hundreds of holes (a circuit board's drills, a perforated plate): the default triangulator grew about
// quadratically with them (2400 drills: 12 s), Delabella stays near linear (1.3 s), so such shapes are meshed with it.
// Only their planes, though: Delabella leaves linear extrusions empty (a handset's walls beside its speaker grille
// vanished), and the drilled faces are planar, so the other faces keep the default triangulator.
class PlanesByDelabella : public IMeshTools_MeshAlgoFactory {
 public:
  Handle(IMeshTools_MeshAlgo) GetAlgo(const GeomAbs_SurfaceType type, const IMeshTools_Parameters& parameters) const override {
    return (type == GeomAbs_Plane ? m_delabella : m_default)->GetAlgo(type, parameters);
  }

 private:
  Handle(IMeshTools_MeshAlgoFactory) m_delabella = new BRepMesh_DelabellaMeshAlgoFactory, m_default = new BRepMesh_MeshAlgoFactory;
};

bool many_holes(const TopoDS_Shape& shape) {
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    int wires = 0;
    for (TopoDS_Iterator w(e.Current()); w.More(); w.Next())
      if (++wires > 64) return true;
  }
  return false;
}

double triangle_area(const Handle(Poly_Triangulation)& mesh) {
  if (mesh.IsNull()) return 0;
  double area = 0;
  for (int i = 1; i <= mesh->NbTriangles(); ++i) {
    int a, b, c;
    mesh->Triangle(i).Get(a, b, c);
    area += gp_Vec(mesh->Node(a), mesh->Node(b)).Crossed(gp_Vec(mesh->Node(a), mesh->Node(c))).Magnitude() * 0.5;
  }
  return area;
}

bool recover_cone(TopoDS_Face face, double expected, double tolerance, double angle) {
  // OCCT's cone range splitter can silently return only a small fragment of a
  // trimmed face (even with status=NoError). An equivalent revolution uses its
  // general surface mesher. Work on a deep copy so its pcurves and edge polygons
  // cannot change those shared with adjacent original faces.
  BRepBuilderAPI_Copy copy(face, true, false);
  const auto copied = copy.Shape();
  BRepTools_Modifier convert(copied);
  convert.Perform(new ShapeCustom_ConvertToRevolution());
  if (!convert.IsDone()) return false;
  const auto replacement = TopoDS::Face(convert.ModifiedShape(copied));
  BRepMesh_IncrementalMesh mesher(replacement, tolerance, false, angle, false);
  TopLoc_Location location, originalLocation;
  auto mesh = BRep_Tool::Triangulation(replacement, location);
  const auto oldMesh = BRep_Tool::Triangulation(face, originalLocation);
  if (mesh.IsNull() || mesh->NbTriangles() == 0 || location != originalLocation) return false;
  const double area = triangle_area(mesh);
  if (!std::isfinite(area) || std::abs(area - expected) > expected * 0.05) return false;

  // Retain the original face and edge identities. Transfer both polygons of a
  // seam as well, otherwise shaded edges and edge picking lose their boundary.
  struct Boundary {
    TopoDS_Edge edge;
    Handle(Poly_PolygonOnTriangulation) first, second;
    bool modified, checked;
  };
  std::vector<Boundary> boundaries;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(face, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    auto edge = TopoDS::Edge(edges(i).Oriented(TopAbs_FORWARD));
    auto converted = TopoDS::Edge(convert.ModifiedShape(copy.ModifiedShape(edge)).Oriented(TopAbs_FORWARD));
    auto first = BRep_Tool::PolygonOnTriangulation(converted, mesh, location);
    Handle(Poly_PolygonOnTriangulation) second;
    if (BRep_Tool::IsClosed(converted, mesh, location))
      second = BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(converted.Reversed()), mesh, location);
    if (first.IsNull() && !BRep_Tool::Degenerated(edge)) return false;
    boundaries.push_back({edge, first, second, edge.Modified(), edge.Checked()});
  }
  BRep_Builder builder;
  for (auto& boundary : boundaries) {
    if (!oldMesh.IsNull())
      builder.UpdateEdge(boundary.edge, Handle(Poly_PolygonOnTriangulation)(), oldMesh, originalLocation);
    if (!boundary.second.IsNull()) builder.UpdateEdge(boundary.edge, boundary.first, boundary.second, mesh, location);
    else builder.UpdateEdge(boundary.edge, boundary.first, mesh, location);
    boundary.edge.Modified(boundary.modified);
    boundary.edge.Checked(boundary.checked);
  }
  const bool modified = face.Modified(), checked = face.Checked();
  builder.UpdateFace(face, mesh);
  face.Modified(modified);
  face.Checked(checked);
  return true;
}

// One face of straighten_ruled_faces: two rims sampled at matching points (same count, the same positions once the
// sweep direction is projected out), joined only by straight lines along that direction (the sides or the seam).
bool straighten(TopoDS_Face face) {
  if (BRep_Tool::Surface(face).IsNull()) return false;
  BRepAdaptor_Surface adaptor(face, Standard_False);
  gp_Dir axis;
  if (adaptor.GetType() == GeomAbs_Cylinder) axis = adaptor.Cylinder().Axis().Direction();
  else if (adaptor.GetType() == GeomAbs_SurfaceOfExtrusion) axis = adaptor.Direction();
  else return false;
  TopLoc_Location location;
  const auto mesh = BRep_Tool::Triangulation(face, location);
  if (mesh.IsNull() || !mesh->HasUVNodes() || mesh->NbTriangles() == 0) return false;
  const gp_Trsf toWorld = location.Transformation();
  auto flat = [&](int node) {  // world position with the sweep direction projected out
    const gp_XYZ p = mesh->Node(node).Transformed(toWorld).XYZ();
    return p - axis.XYZ() * p.Dot(axis.XYZ());
  };
  struct Boundary {
    TopoDS_Edge edge;
    Handle(Poly_PolygonOnTriangulation) first, second;
    bool rim;
  };
  std::vector<Boundary> boundaries;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(face, TopAbs_EDGE, edges);
  Bnd_Box box;
  for (int i = 1; i <= mesh->NbNodes(); ++i) box.Add(mesh->Node(i).Transformed(toWorld));
  const double tolerance = std::max(1e-7, std::sqrt(box.SquareExtent()) * 1e-9);
  std::vector<int> rimNodes[2];
  for (int i = 1; i <= edges.Extent(); ++i) {
    const auto edge = TopoDS::Edge(edges(i).Oriented(TopAbs_FORWARD));
    if (BRep_Tool::Degenerated(edge)) return false;
    Boundary b{edge, BRep_Tool::PolygonOnTriangulation(edge, mesh, location), {}, false};
    if (b.first.IsNull() || b.first->NbNodes() < 2) return false;
    if (BRep_Tool::IsClosed(edge, mesh, location)) b.second = BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(edge.Reversed()), mesh, location);
    // A side (or the seam) is a straight line along the sweep: all its nodes project onto one point.
    const gp_XYZ start = flat(b.first->Node(1));
    for (int k = 2; k <= b.first->NbNodes() && !b.rim; ++k) b.rim = (flat(b.first->Node(k)) - start).Modulus() > tolerance;
    if (b.rim) {
      if (!b.second.IsNull()) return false;  // a rim is never the seam
      const int which = rimNodes[0].empty() ? 0 : rimNodes[1].empty() ? 1 : -1;
      if (which < 0) return false;
      for (int k = 1; k <= b.first->NbNodes(); ++k) rimNodes[which].push_back(b.first->Node(k));
    }
    boundaries.push_back(b);
  }
  auto& a = rimNodes[0];
  auto& b = rimNodes[1];
  const size_t n = a.size();
  if (n < 2 || b.size() != n) return false;
  if ((flat(a.front()) - flat(b.front())).Modulus() > tolerance) std::reverse(b.begin(), b.end());
  for (size_t k = 0; k < n; ++k)
    if ((flat(a[k]) - flat(b[k])).Modulus() > tolerance) return false;
  // New numbering: rim a is 1..n, rim b (matched to a) is n+1..2n. Every other boundary node must be a rim node.
  std::map<int, int> renumber;
  for (size_t k = 0; k < n; ++k) {
    renumber.emplace(a[k], int(k) + 1);
    renumber.emplace(b[k], int(n + k) + 1);
  }
  if (renumber.size() != 2 * n) return false;
  auto remap = [&](const Handle(Poly_PolygonOnTriangulation)& polygon) -> Handle(Poly_PolygonOnTriangulation) {
    if (polygon.IsNull()) return polygon;
    TColStd_Array1OfInteger nodes(1, polygon->NbNodes());
    for (int k = 1; k <= polygon->NbNodes(); ++k) {
      const auto found = renumber.find(polygon->Node(k));
      if (found == renumber.end()) return {};
      nodes(k) = found->second;
    }
    Handle(Poly_PolygonOnTriangulation) out;
    if (polygon->HasParameters()) {
      TColStd_Array1OfReal parameters(1, polygon->NbNodes());
      for (int k = 1; k <= polygon->NbNodes(); ++k) parameters(k) = polygon->Parameter(k);
      out = new Poly_PolygonOnTriangulation(nodes, parameters);
    } else out = new Poly_PolygonOnTriangulation(nodes);
    out->Deflection(polygon->Deflection());
    return out;
  };
  std::vector<std::pair<Handle(Poly_PolygonOnTriangulation), Handle(Poly_PolygonOnTriangulation)>> remapped;
  for (const auto& boundary : boundaries) {
    remapped.emplace_back(remap(boundary.first), remap(boundary.second));
    if (remapped.back().first.IsNull() || (!boundary.second.IsNull() && remapped.back().second.IsNull())) return false;
  }
  Handle(Poly_Triangulation) strips = new Poly_Triangulation(int(2 * n), int(2 * (n - 1)), Standard_True);
  for (size_t k = 0; k < n; ++k) {
    strips->SetNode(int(k) + 1, mesh->Node(a[k]));
    strips->SetUVNode(int(k) + 1, mesh->UVNode(a[k]));
    strips->SetNode(int(n + k) + 1, mesh->Node(b[k]));
    strips->SetUVNode(int(n + k) + 1, mesh->UVNode(b[k]));
  }
  // Keep the winding BRepMesh chose relative to the surface normal (lighting and back faces depend on it). Both
  // are taken in world coordinates: the adaptor evaluates the located surface, the nodes are moved by `location`.
  auto sense = [&](const Handle(Poly_Triangulation)& t, int i) {
    int p, q, r;
    t->Triangle(i).Get(p, q, r);
    const gp_XY uv = (t->UVNode(p).XY() + t->UVNode(q).XY() + t->UVNode(r).XY()) / 3;
    gp_Pnt point;
    gp_Vec du, dv;
    adaptor.D1(uv.X(), uv.Y(), point, du, dv);
    const gp_Pnt P = t->Node(p).Transformed(toWorld), Q = t->Node(q).Transformed(toWorld), R = t->Node(r).Transformed(toWorld);
    return gp_Vec(P, Q).Crossed(gp_Vec(P, R)).Dot(du.Crossed(dv));
  };
  for (int k = 0; k + 1 < int(n); ++k) {
    strips->SetTriangle(2 * k + 1, Poly_Triangle(k + 1, k + 2, int(n) + k + 2));
    strips->SetTriangle(2 * k + 2, Poly_Triangle(k + 1, int(n) + k + 2, int(n) + k + 1));
  }
  double reference = 0;
  for (int i = 1; i <= mesh->NbTriangles() && std::abs(reference) < 1e-12; ++i) reference = sense(mesh, i);
  double mine = 0;
  for (int i = 1; i <= strips->NbTriangles() && std::abs(mine) < 1e-12; ++i) mine = sense(strips, i);
  if (std::abs(reference) < 1e-12 || std::abs(mine) < 1e-12) return false;
  if ((reference > 0) != (mine > 0))
    for (int i = 1; i <= strips->NbTriangles(); ++i) {
      int p, q, r;
      strips->Triangle(i).Get(p, q, r);
      strips->SetTriangle(i, Poly_Triangle(p, r, q));
    }
  strips->Deflection(mesh->Deflection());
  BRep_Builder builder;
  for (size_t i = 0; i < boundaries.size(); ++i) {
    auto edge = boundaries[i].edge;
    const bool modified = edge.Modified(), checked = edge.Checked();
    builder.UpdateEdge(edge, Handle(Poly_PolygonOnTriangulation)(), mesh, location);
    if (!remapped[i].second.IsNull()) builder.UpdateEdge(edge, remapped[i].first, remapped[i].second, strips, location);
    else builder.UpdateEdge(edge, remapped[i].first, strips, location);
    edge.Modified(modified);
    edge.Checked(checked);
  }
  const bool modified = face.Modified(), checked = face.Checked();
  builder.UpdateFace(face, strips);
  face.Modified(modified);
  face.Checked(checked);
  return true;
}
}  // namespace

int straighten_ruled_faces(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return 0;
  int changed = 0;
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    try {
      changed += straighten(TopoDS::Face(faces(i)));
    } catch (const Standard_Failure&) {  // keep BRepMesh's triangles for this face
    }
  }
  return changed;
}

MeshingReport mesh_shape(const TopoDS_Shape& shape, double tolerance, double angular_deg) {
  MeshingReport report;
  if (shape.IsNull()) return report;
  const double angle = angular_deg * M_PI / 180.0;
  if (many_holes(shape)) {
    BRepMesh_IncrementalMesh mesher;
    mesher.SetShape(shape);
    IMeshTools_Parameters& p = mesher.ChangeParameters();
    p.Deflection = tolerance, p.Angle = angle, p.Relative = false, p.InParallel = true;
    Handle(BRepMesh_Context) context = new BRepMesh_Context();
    context->SetFaceDiscret(new BRepMesh_FaceDiscret(new PlanesByDelabella));
    mesher.Perform(context);
    report.status = mesher.GetStatusFlags();
  } else {
    BRepMesh_IncrementalMesh mesher(shape, tolerance, false, angle, true);
    report.status = mesher.GetStatusFlags();
  }
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const auto face = TopoDS::Face(faces(i));
    if (BRep_Tool::Surface(face).IsNull() || BRepAdaptor_Surface(face).GetType() != GeomAbs_Cone) continue;
    try {
      GProp_GProps properties;
      BRepGProp::SurfaceProperties(face, properties);
      // Use local surface area, matching the triangulation's local coordinates.
      const double scale = face.Location().Transformation().ScaleFactor();
      const double expected = std::abs(properties.Mass()) / (scale * scale);
      TopLoc_Location location;
      auto mesh = BRep_Tool::Triangulation(face, location);
      const double area = triangle_area(mesh);
      // Ignore sub-tolerance slivers; coarse chords naturally underestimate area.
      if (!std::isfinite(expected) || expected <= 4 * tolerance * tolerance) continue;
      const double allowance = std::max(expected * 0.10, 4 * tolerance * std::sqrt(expected));
      if (std::isfinite(area) && std::abs(area - expected) <= allowance) continue;
      if (recover_cone(face, expected, tolerance, angle)) ++report.recovered_faces;
      else ++report.incomplete_cones;
    } catch (const Standard_Failure&) {
      ++report.incomplete_cones;  // Preserve the existing mesh if recovery fails.
    }
  }
  return report;
}
}  // namespace opad
