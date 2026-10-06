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
