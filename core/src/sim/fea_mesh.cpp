#include "fea_mesh.hpp"

#include <mutex>

#ifdef OPAD_HAVE_NETGEN
#include <occgeom.hpp>
#include <meshing.hpp>
#endif

namespace opad::sim {

#ifndef OPAD_HAVE_NETGEN

bool netgen_available() { return false; }
VolumeMesh mesh_solids(const TopoDS_Shape&, double, double, bool, const std::function<bool()>&) {
  throw Error("structural studies need Netgen: this build has none (configure with -DOPAD_NETGEN=ON)");
}

#else

bool netgen_available() { return true; }

// Mid-side nodes moved onto curved faces can fold a thin element inside out (CalculiX: "nonpositive jacobian determinant"),
// a bolt hole's rim through a thin plate. Such an element's mid-side nodes go back to its straight edges' middles (the
// neighbours sharing them become a little straighter too) until every element's Jacobian, sampled at its corners, edge
// middles and centre, keeps a tenth of its straight-sided value.
void straighten_inverted(VolumeMesh& m) {
  static const int mid[6][2] = {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {1, 3}, {2, 3}};  // C3D10's nodes 5 to 10
  auto det3 = [](const std::array<double, 9>& a) {
    return a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  };
  auto jacobian = [&](const std::array<int, 10>& t, const std::array<double, 4>& L) {
    static const double dL[4][3] = {{-1, -1, -1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::array<double, 9> J{};  // J[3 i + k] = d x_i / d xi_k
    auto add = [&](int node, const double* g) {
      const Vec3& x = m.nodes[size_t(node)];
      for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) J[size_t(3 * i + k)] += x[size_t(i)] * g[k];
    };
    for (int c = 0; c < 4; ++c) {
      double g[3];
      for (int k = 0; k < 3; ++k) g[k] = (4 * L[size_t(c)] - 1) * dL[c][k];
      add(t[size_t(c)], g);
    }
    for (int e = 0; e < 6; ++e) {
      const int a = mid[e][0], b = mid[e][1];
      double g[3];
      for (int k = 0; k < 3; ++k) g[k] = 4 * (L[size_t(a)] * dL[b][k] + L[size_t(b)] * dL[a][k]);
      add(t[size_t(4 + e)], g);
    }
    return det3(J);
  };
  std::vector<std::array<double, 4>> samples;
  for (int c = 0; c < 4; ++c) {
    std::array<double, 4> L{};
    L[size_t(c)] = 1;
    samples.push_back(L);
  }
  for (const auto& e : mid) {
    std::array<double, 4> L{};
    L[size_t(e[0])] = L[size_t(e[1])] = 0.5;
    samples.push_back(L);
  }
  samples.push_back({0.25, 0.25, 0.25, 0.25});
  for (int pass = 0; pass < 4; ++pass) {
    int fixed = 0;
    for (const auto& t : m.tets) {
      const Vec3 &x0 = m.nodes[size_t(t[0])], &x1 = m.nodes[size_t(t[1])], &x2 = m.nodes[size_t(t[2])], &x3 = m.nodes[size_t(t[3])];
      const double lin = det3({x1[0] - x0[0], x2[0] - x0[0], x3[0] - x0[0], x1[1] - x0[1], x2[1] - x0[1], x3[1] - x0[1], x1[2] - x0[2], x2[2] - x0[2], x3[2] - x0[2]});
      bool bad = false;
      for (const auto& L : samples)
        if (jacobian(t, L) * (lin < 0 ? -1 : 1) < 0.1 * std::fabs(lin)) {
          bad = true;
          break;
        }
      if (!bad) continue;
      for (int e = 0; e < 6; ++e) {
        const Vec3 &a = m.nodes[size_t(t[size_t(mid[e][0])])], &b = m.nodes[size_t(t[size_t(mid[e][1])])];
        m.nodes[size_t(t[size_t(4 + e)])] = {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2};
      }
      ++fixed;
    }
    m.straightened += fixed;
    if (!fixed) break;
  }
}

VolumeMesh mesh_solids(const TopoDS_Shape& shape, double maxh, double grading, bool second_order, const std::function<bool()>& cancelled) {
  // Netgen keeps global state (its parameters, its message level, the "global mesh"): one mesh at a time.
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  ngcore::printmessage_importance = 0;  // nothing on stdout: it is an MCP server's channel
  auto geo = std::make_shared<netgen::OCCGeometry>(shape);
  auto mesh = std::make_shared<netgen::Mesh>();
  mesh->SetGeometry(geo);
  netgen::MeshingParameters mp;
  mp.maxh = maxh;
  mp.grading = grading;
  mp.secondorder = false;  // made below: GenerateMesh leaves it to the caller
  mp.optsteps3d = 3;
  if (cancelled && cancelled()) throw Error("cancelled");
  int status = 1;
  try {
    status = geo->GenerateMesh(mesh, mp);
  } catch (const std::exception& e) {
    throw Error(std::string("Netgen could not mesh the bodies: ") + e.what());
  }
  if (status != 0 || mesh->GetNE() == 0) throw Error("Netgen could not mesh the bodies (try another mesh size, or check the bodies are valid solids)");
  if (cancelled && cancelled()) throw Error("cancelled");
  if (second_order) mesh->GetGeometry()->GetRefinement().MakeSecondOrder(*mesh);  // mid-side nodes on the true surfaces
  VolumeMesh out;
  out.faces = geo->fmap;
  out.solids = geo->somap;
  const int first = int(netgen::IndexBASE<netgen::PointIndex>());
  out.nodes.reserve(mesh->GetNP());
  for (size_t i = 0; i < mesh->GetNP(); ++i) {
    const auto& p = mesh->Point(netgen::PointIndex(int(i) + first));
    out.nodes.push_back({p[0], p[1], p[2]});
  }
  // Netgen's tetrahedra to CalculiX's node order (netgen's own Abaqus writer).
  static const int perm10[10] = {0, 1, 3, 2, 4, 8, 6, 5, 7, 9};
  static const int perm4[4] = {0, 1, 3, 2};
  for (size_t e = 0; e < mesh->GetNE(); ++e) {
    const netgen::Element& el = mesh->VolumeElement(netgen::ElementIndex(int(e)));
    std::array<int, 10> t{};
    const int np = el.GetNP();
    if (np != 4 && np != 10) throw Error("Netgen made an element that is not a tetrahedron");
    out.tet_nodes = np;
    for (int k = 0; k < np; ++k) t[size_t(k)] = int(el[np == 10 ? perm10[k] : perm4[k]]) - first;
    out.tets.push_back(t);
    out.tet_solid.push_back(el.GetIndex() - 1);
  }
  if (out.tet_nodes == 10) straighten_inverted(out);
  for (size_t e = 0; e < mesh->GetNSE(); ++e) {
    const netgen::Element2d& el = mesh->SurfaceElement(netgen::SurfaceElementIndex(int(e)));
    std::array<int, 6> t{};
    const int np = el.GetNP();
    out.tri_nodes = np;
    for (int k = 0; k < np && k < 6; ++k) t[size_t(k)] = int(el[k]) - first;
    out.tris.push_back(t);
    out.tri_face.push_back(mesh->GetFaceDescriptor(el.GetIndex()).SurfNr() - 1);  // 1-based, as fmap
  }
  return out;
}

#endif

}  // namespace opad::sim
