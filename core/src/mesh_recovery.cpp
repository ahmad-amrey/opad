#include "opad/mesh.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools_Modifier.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeCustom_ConvertToRevolution.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>

namespace opad {
namespace {
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
}  // namespace

MeshingReport mesh_shape(const TopoDS_Shape& shape, double tolerance, double angular_deg) {
  MeshingReport report;
  if (shape.IsNull()) return report;
  const double angle = angular_deg * M_PI / 180.0;
  BRepMesh_IncrementalMesh mesher(shape, tolerance, false, angle, true);
  report.status = mesher.GetStatusFlags();
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
