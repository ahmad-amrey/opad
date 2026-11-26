#pragma once
// Fitted references on meshes (3D scans, STL from CAD): planes, lines and axes estimated from a region of triangles or
// from picked points, robust to noise and to broken or missing parts, each with a report that tells a good fit from a bad
// one (a curved region taken for a plane); and the candidate frames that line a scan up with the main axes (its flats'
// normals by mean shift on the sphere, its walls' directions by a histogram in the base plane, PCA when it has no flats).
// The design layer turns them into plane and axis references (design/fit_refs.cpp, `{"fit": ...}`).
// Everything here walks the whole mesh: workers only.
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <TopoDS_Shape.hxx>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"

namespace opad::scanfit {

// A mesh body's triangles numbered as a pick numbers them (opad::subshape on a mesh: triangles across the faces in the
// explorer's order, nodes face by face), with welded adjacency for growing regions. In the shape's own coordinates.
struct FitMesh {
  std::vector<gp_Pnt> nodes;              // by node ordinal (as picked)
  std::vector<std::array<int, 3>> tri;    // node ordinals of each triangle
  std::vector<gp_Vec> normal;             // unit; zero for a degenerate triangle
  std::vector<gp_Vec> smooth;             // normals averaged over the neighbours twice: a scan's noise taken out, for classifying
  std::vector<double> area;
  std::vector<gp_Pnt> centre;
  std::vector<std::array<int, 3>> across; // the triangle across each side (welded), -1 none
  double diagonal = 0, total_area = 0;
  gp_Pnt centroid;                        // of the surface (area weighted)
  int triangles() const { return int(tri.size()); }
};
// Throws when the shape has no triangles.
std::shared_ptr<const FitMesh> fit_mesh(const TopoDS_Shape& mesh_shape, const std::function<bool()>& cancelled = {});
// The same, kept for the last few body keys (a pick, a refit with another tolerance and the commit read it once).
std::shared_ptr<const FitMesh> cached_fit_mesh(const std::string& key, const TopoDS_Shape& mesh_shape, const std::function<bool()>& cancelled = {});

// How well a fit follows what it was fitted to.
struct Quality {
  double rms = 0, max = 0;  // of the inliers' distances to the fitted plane, line or cylinder (mm)
  int used = 0, total = 0;  // inliers of all the triangles or points given
  double extent = 0;        // the inliers' size (mm)
  std::string grade;        // good | fair | poor: rms against extent, and the share left out
  json to_json() const;
};

struct Plane {
  gp_Pnt origin;  // the inliers' centre, on the plane
  gp_Dir normal;
  gp_Dir x;       // the inliers' longest direction in the plane
  Quality q;
};
struct Line {
  gp_Pnt origin;  // a cylinder: on the axis, in the middle of the inliers along it
  gp_Dir dir;
  double radius = 0;  // a cylinder's
  Quality q;
};

// Triangles reached from `seed` across shared sides: `follow` false keeps those whose normal is within `angle` of the
// region's mean normal (a flat), true those within `angle` of the neighbour they are reached from (a curved face up to a
// sharp edge). At most `limit` triangles.
std::vector<int> grow_region(const FitMesh& m, int seed, double angle_rad, bool follow, size_t limit = 50'000'000);

// A round face's triangles reached from `seed`: a small region grown neighbour to neighbour, then, three times over, the
// triangles connected to the seed that lie on the cylinder fitted to it (corners within three times its rms, facing square
// to its axis). Noise does not stop it and a rim, rounded or sharp, does.
std::vector<int> grow_round(const FitMesh& m, int seed, double angle_rad);

// Robust plane through points (weights optional): least median of squares over sampled hypotheses (three points, or a
// point and its normal when `normals` are given), then least squares on the inliers. The normal faces along `facing`
// when given (non-zero), else the right-hand normal of the first three points.
Plane fit_plane(const std::vector<gp_Pnt>& points, const std::vector<double>& weights = {}, const std::vector<gp_Vec>& normals = {},
                const gp_Vec& facing = gp_Vec());
Plane fit_plane_region(const FitMesh& m, const std::vector<int>& triangles);
// Robust line through points (two-point hypotheses, then the principal direction of the inliers).
Line fit_line(const std::vector<gp_Pnt>& points);
// A cylinder's axis fitted to a region: the axis from pairs of normals (robust to the flats around a hole), the circle in the
// plane across it, refined on the inliers. Throws when the region is not round.
Line fit_cylinder_region(const FitMesh& m, const std::vector<int>& triangles);
// A wall's long direction: its fitted plane, then the inliers' longest direction in it, through their centre.
Line wall_direction(const FitMesh& m, const std::vector<int>& triangles);
// Where two planes meet; throws when they are within `min_angle` of parallel.
Line plane_intersection(const Plane& a, const Plane& b, double min_angle_rad = 5 * 3.14159265358979323846 / 180);

// ---------------------------------------------------------------- lining a scan up with the axes
// A flat a scan can stand on: its normal points away from the material (the body lies on its back side).
struct ScanBase {
  gp_Dir normal;
  gp_Pnt origin;        // on the fitted plane
  double share = 0;     // of the whole surface's area facing that way (0..1)
  Quality q;
};
struct ScanAxes {
  std::string method;               // "flats" (bases are flats), "pca" (no flats: the principal axes)
  std::vector<ScanBase> bases;      // best first
};
// The scan's candidate bases: flats found by mean shift of the area-weighted normals on the sphere, the largest plane of
// each direction fitted robustly; when no flat holds `min_share` of the area, the six principal directions (PCA).
ScanAxes scan_axes(const FitMesh& m, const std::function<bool()>& cancelled = {}, double min_share = 0.04);
// The X directions offered with a base, in turn order: the main wall direction's quarter turns (walls: triangles standing
// upright on the base, their normals' histogram in the base plane), then other wall directions'; without walls, the
// surface's longest direction in the plane and its quarter turns. Unit vectors in the base plane.
std::vector<gp_Dir> scan_turns(const FitMesh& m, const ScanAxes& axes, int base);
// The frame a candidate puts on the scan: normal = the base's (away from the material), x = turn `turn` (cycled), origin =
// the base plane under the box centre ("base") or the box centre ("centre"; PCA's default). Aligned face to face onto XY
// with its X onto X, the scan stands on XY centred on the origin.
struct ScanFrame {
  gp_Pnt origin;
  gp_Dir normal, x;
  int bases = 0, turns = 0;  // how many there are to cycle through
  std::string method;
  Quality q;                 // the base's fit
  double share = 0;
};
ScanFrame scan_frame(const FitMesh& m, const ScanAxes& axes, int base, int turn, const std::string& origin);

}  // namespace opad::scanfit
