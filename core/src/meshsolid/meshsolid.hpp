#pragma once
// Internals of mesh to solid (opad/mesh_solid.hpp): the mesh's adjacency, the fitted surfaces and a nearest-point tree.
#include <Geom_Surface.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "opad/mesh_solid.hpp"

namespace opad::meshsolid {

// Triangle adjacency: for side k of triangle t (from vertex k to k+1), the triangle across it (-1: an open edge, -2: an edge
// more than two triangles share), unit normals, areas and the angle across every side.
struct Topology {
  const TriMesh* mesh = nullptr;
  std::vector<std::array<int, 3>> across;
  std::vector<gp_Vec> normal;  // unit, (b - a) x (c - a)
  std::vector<double> area;
  std::vector<std::array<double, 3>> dihedral;  // radians between the normals across each side (pi when open)
  explicit Topology(const TriMesh& m);
  gp_Pnt centroid(int t) const;
  int side_of(int t, int a, int b) const;  // the side running a -> b, or -1
};

// A fitted surface. Plane: o on it, a the normal. Cylinder: o on the axis, a its direction, r radius. Cone: o the apex, a the
// axis into the opening, angle the half angle. Sphere: o centre, r. Torus: o centre, a axis, r major radius, r2 minor.
struct Prim {
  enum Type { Plane, Cylinder, Cone, Sphere, Torus } type = Plane;
  gp_Pnt o;
  gp_Dir a;
  double r = 0, r2 = 0, angle = 0;
  double distance(const gp_Pnt& p) const;  // signed: positive on the side the outward normal points to
  gp_Dir normal(const gp_Pnt& p) const;    // the surface's own normal (away from the axis / centre)
  gp_Pnt project(const gp_Pnt& p) const;
  Handle(Geom_Surface) surface(const gp_Pnt& near) const;  // near: where the face is (a cone's reference circle)
  const char* name() const;
  bool same(const Prim& other, double tolerance, double size) const;  // the same surface within the tolerance
};

// Fits of each kind to points (and the unit normals of the facets around them, for the axis); nullopt when the kind does
// not come out (degenerate, or a radius past `size` * 50).
std::optional<Prim> fit_plane(const std::vector<gp_Pnt>& points);
std::optional<Prim> fit_sphere(const std::vector<gp_Pnt>& points, double size);
std::optional<Prim> fit_cylinder(const std::vector<gp_Pnt>& points, const std::vector<gp_Vec>& normals, double size);
std::optional<Prim> fit_cone(const std::vector<gp_Pnt>& points, const std::vector<gp_Vec>& normals, const std::vector<gp_Pnt>& at, double size);
std::optional<Prim> fit_torus(const std::vector<gp_Pnt>& points, const std::vector<gp_Vec>& normals, const std::vector<gp_Pnt>& at, double size);
// Least squares refinement of any kind on the points (Levenberg-Marquardt); keeps `p` when it does not improve.
Prim refine(const Prim& p, const std::vector<gp_Pnt>& points, double size);
double max_distance(const Prim& p, const std::vector<gp_Pnt>& points);
// A 2D circle through points (algebraic fit, then geometric): centre and radius; false when degenerate.
bool fit_circle_2d(const std::vector<std::array<double, 2>>& pts, double& cx, double& cy, double& r);

// Nearest point on a set of triangles (AABB tree).
class NearestTree {
 public:
  NearestTree(std::vector<gp_Pnt> points, std::vector<std::array<int, 3>> triangles);
  double distance(const gp_Pnt& p, int* triangle = nullptr) const;
  const std::vector<gp_Pnt>& points() const { return m_points; }
  const std::vector<std::array<int, 3>>& triangles() const { return m_triangles; }

 private:
  struct Node {
    double lo[3], hi[3];
    int left = -1, right = -1, first = 0, count = 0;
  };
  int build(int first, int count, int depth);
  std::vector<gp_Pnt> m_points;
  std::vector<std::array<int, 3>> m_triangles;
  std::vector<int> m_order;
  std::vector<std::array<double, 3>> m_centroid;
  std::vector<Node> m_nodes;
};
double point_triangle_distance(const gp_Pnt& p, const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c);

// The mesh's chord error estimated from its smooth edges (facets meeting at less than `angle`): the sagitta of the arc two
// facets of that width and angle stand for.
double mesh_sagitta(const Topology& topo, double angle);

}  // namespace opad::meshsolid
