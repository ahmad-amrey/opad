#include "opad/sim/fea.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_BuilderAlgo.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "../design/engine.hpp"
#include "../import_common.hpp"
#include "fea_mesh.hpp"
#include "opad/geometry.hpp"
#include "opad/materials.hpp"
#include "opad/sim/joints.hpp"

namespace opad::sim {

namespace {

using V = Vec3;
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V add(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V mul(const V& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const V& a) { return std::sqrt(dot(a, a)); }
V unit(const V& a) {
  const double n = norm(a);
  return n > 0 ? mul(a, 1 / n) : a;
}

std::filesystem::path exe_dir() {
#ifdef _WIN32
  return {};
#else
  std::error_code e;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", e);
  return e ? std::filesystem::path() : self.parent_path();
#endif
}

double von_mises(const std::array<double, 6>& s) {
  const double a = s[0] - s[1], b = s[1] - s[2], c = s[2] - s[0];
  return std::sqrt(0.5 * (a * a + b * b + c * c) + 3 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]));
}

struct Mat {
  double E = 210000, nu = 0.3, rho = 7.85, yield = 250;  // MPa, -, g/cm3, MPa
  std::string name = "Steel";
  bool assumed = true;
};

Mat material_for(const Document& doc, const Scene& scene, const std::string& body, const json& overrides) {
  Mat m;
  const MaterialChoice c = material_of(doc, scene, body);
  if (!c.id.empty())
    if (const Mechanical* mech = mechanical(c.id)) {
      m.E = mech->youngs, m.nu = mech->poisson, m.yield = mech->yield, m.assumed = false;
      m.name = c.shown();
    }
  if (c.density > 0) m.rho = c.density;
  if (overrides.is_object()) {
    const json o = overrides.contains(body) ? overrides[body] : overrides.value("all", json());
    if (o.is_object()) {
      m.E = o.value("E", m.E), m.nu = o.value("nu", m.nu), m.rho = o.value("density", m.rho), m.yield = o.value("yield", m.yield);
      m.assumed = false;
      if (o.contains("name")) m.name = o["name"].get<std::string>();
    }
  }
  return m;
}

// The faces a shape's face became through a history of operations (itself when untouched, none when gone).
template <class H>
std::vector<TopoDS_Shape> after(H& history, const TopoDS_Shape& f) {
  std::vector<TopoDS_Shape> out;
  const TopTools_ListOfShape& m = history.Modified(f);
  if (!m.IsEmpty()) {
    for (const auto& x : m) out.push_back(x);
  } else if (!history.IsDeleted(f)) {
    out.push_back(f);
  }
  return out;
}

// A bolt: the body, its axis and the plane across the middle of its shank.
struct Bolt {
  std::string body;
  gp_Ax1 axis;
  gp_Pnt at;
  double preload = 0;
  std::string load;
};

// The axis of a bolt body: its largest cylindrical face's, cut at the middle of that face.
Bolt find_bolt(const TopoDS_Shape& shape, const std::string& body) {
  Bolt b;
  b.body = body;
  double best = 0;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    BRepAdaptor_Surface s(TopoDS::Face(e.Current()));
    if (s.GetType() != GeomAbs_Cylinder) continue;
    GProp_GProps g;
    BRepGProp::SurfaceProperties(e.Current(), g);
    if (g.Mass() <= best) continue;
    best = g.Mass();
    b.axis = s.Cylinder().Axis();
    // The middle of the face along the axis: its centre projected.
    const gp_Pnt c = g.CentreOfMass();
    const gp_Vec v(b.axis.Location(), c);
    b.at = b.axis.Location().Translated(gp_Vec(b.axis.Direction()) * v.Dot(gp_Vec(b.axis.Direction())));
  }
  if (best == 0) throw Error("a bolt preload needs a bolt with a cylindrical shank (or give its axis)");
  return b;
}

// ---------------------------------------------------------------- .frd
struct Frd {
  struct Block {
    std::string name;  // DISP, STRESS
    int step = 0;
    bool modal = false;
    double value = 0;  // the step's time or frequency
    std::map<int, std::vector<double>> values;
  };
  std::vector<Block> blocks;
};

Frd read_frd(const std::filesystem::path& p) {
  Frd out;
  std::ifstream in(p);
  if (!in) throw Error("CalculiX wrote no results (" + p.filename().string() + ")");
  std::string line;
  Frd::Block cur;
  bool in_block = false;
  double step_value = 0;
  bool step_modal = false;
  int step = 0;
  while (std::getline(in, line)) {
    if (line.size() >= 6 && line.compare(0, 6, "  100C") == 0) {
      // "  100CL  101 1.000000000          10                     0    1           1"
      step_value = std::strtod(line.substr(12, 13).c_str(), nullptr);
      step_modal = line.find("MODAL") != std::string::npos;
      ++step;
      continue;
    }
    if (line.size() >= 3 && line.compare(0, 3, " -4") == 0) {
      cur = Frd::Block();
      std::istringstream ss(line.substr(3));
      ss >> cur.name;
      cur.step = step;
      cur.modal = step_modal;
      cur.value = step_value;
      in_block = true;
      continue;
    }
    if (!in_block) continue;
    if (line.size() >= 3 && line.compare(0, 3, " -3") == 0) {
      out.blocks.push_back(std::move(cur));
      in_block = false;
      continue;
    }
    if (line.size() >= 13 && line.compare(0, 3, " -1") == 0) {
      const int node = std::atoi(line.substr(3, 10).c_str());
      std::vector<double> v;
      for (size_t at = 13; at + 12 <= line.size(); at += 12) v.push_back(std::strtod(line.substr(at, 12).c_str(), nullptr));
      cur.values[node] = std::move(v);
    }
  }
  return out;
}

std::vector<double> read_frequencies(const std::filesystem::path& dat) {
  std::ifstream in(dat);
  std::vector<double> out;
  std::string line;
  bool table = false;
  while (std::getline(in, line)) {
    if (line.find("E I G E N V A L U E   O U T P U T") != std::string::npos) {
      table = true;
      continue;
    }
    if (!table) continue;
    if (line.find("P A R T I C I P A T I O N") != std::string::npos) break;
    std::istringstream ss(line);
    int mode;
    double eig, rad, cyc;
    if (ss >> mode >> eig >> rad >> cyc) out.push_back(cyc);
  }
  return out;
}

// "total force (fx,fy,fz) for set FIX1 and time ..." then a line with the three numbers.
std::map<std::string, V> read_reactions(const std::filesystem::path& dat) {
  std::ifstream in(dat);
  std::map<std::string, V> out;
  std::string line, set;
  while (std::getline(in, line)) {
    const auto at = line.find("total force (fx,fy,fz) for set ");
    if (at != std::string::npos) {
      std::istringstream ss(line.substr(at + 31));
      ss >> set;
      continue;
    }
    if (!set.empty()) {
      std::istringstream ss(line);
      V v;
      if (ss >> v[0] >> v[1] >> v[2]) {
        out[set] = v;
        set.clear();
      }
    }
  }
  return out;
}

}  // namespace

std::filesystem::path ccx_program() {
  if (const char* e = std::getenv("OPAD_CCX"); e && *e) return path_from_utf8(e);
  std::error_code err;
  for (const char* name : {"ccx", "ccx_2.22", "ccx_2.21", "ccx_2.20", "ccx_2.19"}) {
#ifdef _WIN32
    const auto beside = exe_dir() / (std::string(name) + ".exe");
#else
    const auto beside = exe_dir() / name;
#endif
    if (!exe_dir().empty() && std::filesystem::exists(beside, err)) return beside;
  }
  const char* path = std::getenv("PATH");
  if (!path) return {};
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::string p = path;
  size_t start = 0;
  while (start <= p.size()) {
    const size_t end = std::min(p.find(sep, start), p.size());
    const std::filesystem::path dir = p.substr(start, end - start);
    for (const char* name : {"ccx", "ccx_2.22", "ccx_2.21", "ccx_2.20", "ccx_2.19"}) {
#ifdef _WIN32
      const auto f = dir / (std::string(name) + ".exe");
#else
      const auto f = dir / name;
#endif
      if (!dir.empty() && std::filesystem::is_regular_file(f, err)) return f;
    }
    start = end + 1;
  }
  return {};
}

double probe(const FeaResult& r, const Vec3& at, const std::string& field, int mode) {
  if (r.nodes.empty()) throw Error("no results to probe");
  auto value = [&](size_t i) -> double {
    if (field == "von_mises") return r.von_mises.at(i);
    if (field == "displacement") return norm(r.displacement.at(i));
    if (field == "dx" || field == "dy" || field == "dz") return r.displacement.at(i)[size_t(field[1] - 'x')];
    if (field == "sxx" || field == "syy" || field == "szz") return r.stress.at(i)[size_t(field[1] - 'x')];
    if (field == "sxy") return r.stress.at(i)[3];
    if (field == "syz") return r.stress.at(i)[4];
    if (field == "szx") return r.stress.at(i)[5];
    if (field == "mode") return norm(r.modes.at(size_t(mode)).at(i));
    throw Error("probe: unknown field " + field + " (von_mises, displacement, dx, dy, dz, sxx, syy, szz, sxy, syz, szx, mode)");
  };
  // Inside an element: its corner values weighed by the point's barycentric coordinates (the best element when the point
  // is on the surface or a hair outside it).
  double best_out = 1e300;
  double best_value = 0;
  for (const auto& t : r.tets) {
    const V a = r.nodes[size_t(t[0])], b = r.nodes[size_t(t[1])], c = r.nodes[size_t(t[2])], d = r.nodes[size_t(t[3])];
    const V ab = sub(b, a), ac = sub(c, a), ad = sub(d, a), ap = sub(at, a);
    const double vol = dot(ab, cross(ac, ad));
    if (std::fabs(vol) < 1e-300) continue;
    const double l1 = dot(ap, cross(ac, ad)) / vol, l2 = dot(ab, cross(ap, ad)) / vol, l3 = dot(ab, cross(ac, ap)) / vol, l0 = 1 - l1 - l2 - l3;
    const double outside = std::max({0.0, -l0, -l1, -l2, -l3});
    if (outside < best_out) {
      best_out = outside;
      best_value = l0 * value(size_t(t[0])) + l1 * value(size_t(t[1])) + l2 * value(size_t(t[2])) + l3 * value(size_t(t[3]));
      if (outside == 0) break;
    }
  }
  if (best_out < 0.05) return best_value;
  size_t best = 0;
  double dist = 1e300;
  for (size_t i = 0; i < r.nodes.size(); ++i) {
    const double e = norm(sub(r.nodes[i], at));
    if (e < dist) dist = e, best = i;
  }
  return value(best);
}

json engines() {
#ifdef OPAD_HAVE_CHRONO
  const bool chrono = true;
#else
  const bool chrono = false;
#endif
  const auto ccx = ccx_program();
  const bool fea = netgen_available() && !ccx.empty();
  json out = {{"motion", true}, {"dynamic", chrono}, {"static", fea}, {"modal", fea}, {"netgen", netgen_available()}};
  out["ccx"] = ccx.empty() ? json(nullptr) : json(path_to_utf8(ccx));
  if (!fea)
    out["note"] = ccx.empty() ? "static and modal studies need CalculiX's ccx: install it (Ubuntu: apt install calculix-ccx; Windows: put ccx.exe beside OPAD) or set OPAD_CCX"
                              : "this build has no Netgen";
  return out;
}

StudyRun run_structural(const Document& doc, const Scene& scene, const std::string& kind, const json& st, const Progress& progress) {
  StudyRun run;
  run.kind = kind;
  auto report = [&](double f, const std::string& phase) {
    if (progress && !progress(f, phase)) throw Error("cancelled");
  };
  if (!netgen_available()) throw Error("static and modal studies need Netgen: this build has none");
  const std::filesystem::path ccx = ccx_program();
  if (ccx.empty()) throw Error(engines()["note"].get<std::string>());

  // ---- the case and its loads
  std::string load_case = st.value("case", std::string());
  if (load_case.empty()) load_case = scene.loads.empty() ? std::string("Load case 1") : scene.loads.front().load_case;
  std::vector<const Load*> loads;
  for (const auto& l : scene.loads)
    if (l.load_case == load_case) {
      if (!l.error.empty()) throw Error("load \"" + l.name + "\": " + l.error);
      loads.push_back(&l);
    }
  if (kind == "static" && loads.empty()) throw Error("load case \"" + load_case + "\" has no loads: add some with the load command");

  // ---- bodies: as given, else every body a load names
  std::vector<std::string> bodies;
  auto add_body = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!n) throw Error("no body " + id);
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
      if (std::find(bodies.begin(), bodies.end(), b) == bodies.end()) bodies.push_back(b);
  };
  for (const auto& b : st.value("bodies", json::array())) add_body(b.get<std::string>());
  if (bodies.empty())
    for (const Load* l : loads)
      for (const auto& r : l->refs) add_body(r.body);
  if (bodies.empty()) throw Error("a structural study needs bodies: give settings.bodies or loads on their faces");
  std::vector<TopoDS_Shape> world;  // each body's solids, world coordinates
  for (const auto& b : bodies) {
    const Node* n = scene.node(b);
    if (n->body_missing || n->representation != "solid") throw Error("\"" + n->name + "\" is not a solid");
    TopoDS_Compound solids;
    BRep_Builder bb;
    bb.MakeCompound(solids);
    int count = 0;
    const TopoDS_Shape shape = node_world_shape(doc, scene, b);
    for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next(), ++count) bb.Add(solids, e.Current());
    if (!count) throw Error("\"" + n->name + "\" has no solid to analyse");
    world.push_back(count == 1 ? TopoDS_Shape(TopExp_Explorer(shape, TopAbs_SOLID).Current()) : TopoDS_Shape(solids));
  }
  run.summary["case"] = load_case;

  // ---- bolts: cut across the shank
  const std::map<std::string, TopoDS_Shape> fresh;
  const design::ParamTable params;
  const design::Ctx ctx{doc, params, scene, fresh, {}};
  std::vector<Bolt> bolts;
  for (const Load* l : loads) {
    if (l->kind != "bolt_preload") continue;
    if (l->refs.empty()) throw Error("bolt preload \"" + l->name + "\" names no bolt body");
    const std::string body = l->refs.front().body;
    const size_t i = size_t(std::find(bodies.begin(), bodies.end(), body) - bodies.begin());
    if (i >= bodies.size()) throw Error("bolt preload \"" + l->name + "\": its bolt is not among the study's bodies");
    Bolt b = find_bolt(world[i], body);
    if (l->def.contains("axis")) {
      b.axis = ctx.axis(l->def["axis"]);
      b.at = b.axis.Location();
    }
    b.preload = l->def.value("value", 0.0);
    b.load = l->name;
    bolts.push_back(b);
  }
  std::vector<TopoDS_Shape> cut = world;  // bodies after their bolt cuts
  std::vector<std::unique_ptr<BRepAlgoAPI_Splitter>> splits(world.size());
  for (const auto& b : bolts) {
    const size_t i = size_t(std::find(bodies.begin(), bodies.end(), b.body) - bodies.begin());
    const Bnd_Box box = tight_bbox(world[i]);
    const double reach = std::sqrt(box.SquareExtent()) * 2;
    const TopoDS_Face plane = BRepBuilderAPI_MakeFace(gp_Pln(b.at, b.axis.Direction()), -reach, reach, -reach, reach).Face();
    auto sp = std::make_unique<BRepAlgoAPI_Splitter>();
    TopTools_ListOfShape args, tools;
    args.Append(cut[i]);
    tools.Append(plane);
    sp->SetArguments(args);
    sp->SetTools(tools);
    sp->Build();
    if (!sp->IsDone()) throw Error("the bolt \"" + scene.node(b.body)->name + "\" could not be cut across its shank");
    cut[i] = sp->Shape();
    splits[i] = std::move(sp);
  }
  report(0.05, "Joining the bodies");
  // ---- join: one shape whose touching faces are shared
  // A single body needs no joining: its shapes stand for themselves.
  struct Join {
    BRepAlgoAPI_BuilderAlgo algo;
    bool used = false;
    const TopTools_ListOfShape& Modified(const TopoDS_Shape& f) { return used ? algo.Modified(f) : empty; }
    bool IsDeleted(const TopoDS_Shape& f) { return used && algo.IsDeleted(f); }
    TopTools_ListOfShape empty;
  } fuse;
  TopoDS_Shape joined = cut.front();
  if (cut.size() > 1) {
    TopTools_ListOfShape args;
    for (const auto& s : cut) args.Append(s);
    fuse.algo.SetArguments(args);
    fuse.algo.SetNonDestructive(true);
    fuse.algo.Build();
    if (!fuse.algo.IsDone()) throw Error("the bodies could not be joined for meshing");
    fuse.used = true;
    joined = fuse.algo.Shape();
  }
  TopoDS_Compound solids;
  BRep_Builder bb;
  bb.MakeCompound(solids);
  for (TopExp_Explorer e(joined, TopAbs_SOLID); e.More(); e.Next()) bb.Add(solids, e.Current());
  // Which body each final solid belongs to.
  std::vector<std::vector<TopoDS_Shape>> body_solids(bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i)
    for (TopExp_Explorer e(cut[i], TopAbs_SOLID); e.More(); e.Next())
      for (const auto& f : after(fuse, e.Current())) body_solids[i].push_back(f);

  // ---- mesh
  const Bnd_Box all = tight_bbox(solids);
  const double size = std::sqrt(all.SquareExtent());
  double maxh = st.value("mesh_size", 0.0);
  if (maxh <= 0) maxh = std::max(size / 25, 0.5);
  run.summary["mesh_size"] = maxh;
  report(0.1, "Meshing");
  const VolumeMesh mesh = mesh_solids(solids, maxh, st.value("grading", 0.3), st.value("second_order", true), [&] { return progress && !progress(0.1, "Meshing"); });
  if (mesh.straightened)
    run.warnings.push_back(std::to_string(mesh.straightened) + " element(s) curved inside out on a tight curve were made straight-sided (a finer mesh_size there keeps them curved)");
  const int tn = mesh.tet_nodes;
  // Solids of the mesh -> bodies.
  std::vector<int> solid_body(size_t(mesh.solids.Extent()), -1);
  for (int k = 1; k <= mesh.solids.Extent(); ++k)
    for (size_t i = 0; i < bodies.size(); ++i)
      for (const auto& s : body_solids[i])
        if (s.IsSame(mesh.solids(k))) solid_body[size_t(k - 1)] = int(i);
  for (size_t k = 0; k < solid_body.size(); ++k)
    if (solid_body[k] < 0) throw Error("a meshed solid belongs to no body (the join changed it)");

  // Tet faces by their sorted corners, to find the element under a surface triangle (CalculiX face numbers S1..S4 of
  // C3D10/C3D4 in Abaqus order: 1-2-3, 1-4-2, 2-4-3, 3-4-1).
  static const int faces_of[4][3] = {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {2, 3, 0}};
  std::map<std::array<int, 3>, std::vector<std::pair<int, int>>> tet_face;  // corners -> (element, face 1..4)
  for (size_t e = 0; e < mesh.tets.size(); ++e)
    for (int f = 0; f < 4; ++f) {
      std::array<int, 3> k = {mesh.tets[e][size_t(faces_of[f][0])], mesh.tets[e][size_t(faces_of[f][1])], mesh.tets[e][size_t(faces_of[f][2])]};
      std::sort(k.begin(), k.end());
      tet_face[k].push_back({int(e), f + 1});
    }
  auto corners = [&](size_t t) {
    std::array<int, 3> k = {mesh.tris[t][0], mesh.tris[t][1], mesh.tris[t][2]};
    std::sort(k.begin(), k.end());
    return k;
  };
  auto centroid = [&](int e) {
    V c{0, 0, 0};
    for (int k = 0; k < 4; ++k) c = add(c, mesh.nodes[size_t(mesh.tets[size_t(e)][size_t(k)])]);
    return mul(c, 0.25);
  };

  // Load faces -> mesh triangles. A reference is resolved on the body as it is, then followed through the cut and the join.
  auto triangles_of = [&](const Load& l) {
    std::set<int> faces;  // mesh face indices (0-based)
    for (const auto& rj : l.def.value("refs", json::array())) {
      for (const auto& rr : ctx.resolve_all(rj)) {
        const size_t i = size_t(std::find(bodies.begin(), bodies.end(), rr.node) - bodies.begin());
        if (i >= bodies.size()) throw Error("load \"" + l.name + "\" acts on a body outside the study");
        if (rr.sub.ShapeType() != TopAbs_FACE) throw Error("load \"" + l.name + "\" needs faces");
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(world[i], TopAbs_FACE, map);
        const TopoDS_Shape original = map(rr.index + 1);
        std::vector<TopoDS_Shape> now = {original};
        if (splits[i]) {
          std::vector<TopoDS_Shape> next;
          for (const auto& f : now)
            for (const auto& g : after(*splits[i], f)) next.push_back(g);
          now = next;
        }
        std::vector<TopoDS_Shape> fin;
        for (const auto& f : now)
          for (const auto& g : after(fuse, f)) fin.push_back(g);
        for (const auto& f : fin)
          if (const int k = mesh.faces.FindIndex(f); k > 0) faces.insert(k - 1);
      }
    }
    std::vector<size_t> tris;
    for (size_t t = 0; t < mesh.tris.size(); ++t)
      if (faces.count(mesh.tri_face[t])) tris.push_back(t);
    if (tris.empty()) throw Error("load \"" + l.name + "\": its faces are not on the meshed bodies' surface");
    return tris;
  };
  // A triangle's area, outward unit normal (away from its element) and consistent nodal weights (sum 1).
  struct Tri {
    double area;
    V normal;
    std::vector<std::pair<int, double>> weights;
  };
  auto tri_info = [&](size_t t) {
    Tri out;
    const V a = mesh.nodes[size_t(mesh.tris[t][0])], b = mesh.nodes[size_t(mesh.tris[t][1])], c = mesh.nodes[size_t(mesh.tris[t][2])];
    V n = cross(sub(b, a), sub(c, a));
    out.area = norm(n) / 2;
    n = unit(n);
    const auto it = tet_face.find(corners(t));
    if (it != tet_face.end() && !it->second.empty()) {
      const V inside = centroid(it->second.front().first);
      if (dot(sub(inside, a), n) > 0) n = mul(n, -1);
    }
    out.normal = n;
    if (mesh.tri_nodes == 6) {
      for (int k = 3; k < 6; ++k) out.weights.push_back({mesh.tris[t][size_t(k)], 1.0 / 3});  // a quadratic triangle's uniform load: all at mid-sides
    } else {
      for (int k = 0; k < 3; ++k) out.weights.push_back({mesh.tris[t][size_t(k)], 1.0 / 3});
    }
    return out;
  };
  // A surface triangle's area, its mid-side nodes counted (four flat parts of a curved quadratic triangle).
  auto tri_area = [&](size_t t) {
    const auto& tri = mesh.tris[t];
    auto area3 = [&](int i, int j, int k) {
      return norm(cross(sub(mesh.nodes[size_t(i)], mesh.nodes[size_t(k)]), sub(mesh.nodes[size_t(j)], mesh.nodes[size_t(k)]))) / 2;
    };
    if (mesh.tri_nodes != 6) return area3(tri[0], tri[1], tri[2]);
    // Mid-side node of each edge: the one nearest the edge's middle.
    int mid[3];
    for (int e = 0; e < 3; ++e) {
      const V m = mul(add(mesh.nodes[size_t(tri[size_t(e)])], mesh.nodes[size_t(tri[size_t((e + 1) % 3)])]), 0.5);
      double best = 1e300;
      for (int k = 3; k < 6; ++k)
        if (norm(sub(mesh.nodes[size_t(tri[size_t(k)])], m)) < best) best = norm(sub(mesh.nodes[size_t(tri[size_t(k)])], m)), mid[e] = tri[size_t(k)];
    }
    return area3(tri[0], mid[0], mid[2]) + area3(mid[0], tri[1], mid[1]) + area3(mid[2], mid[1], tri[2]) + area3(mid[0], mid[1], mid[2]);
  };
  auto nodes_of = [&](const std::vector<size_t>& tris) {
    std::set<int> out;
    for (size_t t : tris)
      for (int k = 0; k < mesh.tri_nodes; ++k) out.insert(mesh.tris[t][size_t(k)]);
    return out;
  };

  std::ostringstream cload;
  // Bolt pre-tension sections, made here rather than by CalculiX's *PRE-TENSION SECTION (whose cut stays bridged by elements
  // that touch it only at an edge): the cut's nodes are doubled for every element of the bolt below the cut, the two copies
  // tied across (no sliding) by equations, and their overlap along the axis is a reference node's displacement, which the
  // preload pushes: u_ref = (u_below - u_above) . axis, so the preload shortens the bolt's grip as tightening a nut does.
  std::vector<Vec3> nodes_out = mesh.nodes;
  std::vector<std::array<int, 10>> tets_out = mesh.tets;
  std::ostringstream equations;
  json bolt_info = json::array();
  std::vector<int> ref_nodes;
  for (size_t b = 0; b < bolts.size(); ++b) {
    const Bolt& bo = bolts[b];
    const V n{bo.axis.Direction().X(), bo.axis.Direction().Y(), bo.axis.Direction().Z()};
    const V at{bo.at.X(), bo.at.Y(), bo.at.Z()};
    const size_t bi = size_t(std::find(bodies.begin(), bodies.end(), bo.body) - bodies.begin());
    const double tol = 1e-6 * size;
    double area = 0;
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
      bool on = true;
      for (int k = 0; k < 3 && on; ++k) on = std::fabs(dot(sub(mesh.nodes[size_t(mesh.tris[t][size_t(k)])], at), n)) < tol;
      if (!on) continue;
      const auto it = tet_face.find(corners(t));
      if (it == tet_face.end()) continue;
      bool bolt_face = false;
      for (const auto& [e, f] : it->second) bolt_face = bolt_face || solid_body[size_t(mesh.tet_solid[size_t(e)])] == int(bi);
      if (bolt_face) area += tri_area(t);
    }
    if (area <= 0) throw Error("bolt \"" + scene.node(bo.body)->name + "\": no elements on its cut (is the axis right?)");
    std::map<int, int> twin;  // node on the cut -> its copy below
    for (size_t e = 0; e < tets_out.size(); ++e) {
      if (solid_body[size_t(mesh.tet_solid[e])] != int(bi) || dot(sub(centroid(int(e)), at), n) >= 0) continue;
      for (int k = 0; k < tn; ++k) {
        const int node = tets_out[e][size_t(k)];
        if (std::fabs(dot(sub(mesh.nodes[size_t(node)], at), n)) >= tol) continue;
        auto [it, fresh] = twin.try_emplace(node, int(nodes_out.size()));
        if (fresh) nodes_out.push_back(mesh.nodes[size_t(node)]);
        tets_out[e][size_t(k)] = it->second;
      }
    }
    if (twin.empty()) throw Error("bolt \"" + scene.node(bo.body)->name + "\": nothing of it lies on the cut");
    ref_nodes.push_back(int(b));  // numbered after every node, below
    // Two directions across the axis.
    const V t1 = unit(std::fabs(n[0]) < 0.9 ? cross(n, V{1, 0, 0}) : cross(n, V{0, 1, 0})), t2 = cross(n, t1);
    const std::array<V, 3> dirs = {n, t1, t2};
    // Which of the copy's degrees of freedom each equation eliminates: the largest coefficients, each used once.
    std::array<int, 3> dep{};
    std::array<bool, 3> used{};
    for (int q = 0; q < 3; ++q) {
      int best = -1;
      for (int k = 0; k < 3; ++k)
        if (!used[size_t(k)] && (best < 0 || std::fabs(dirs[size_t(q)][size_t(k)]) > std::fabs(dirs[size_t(q)][size_t(best)]))) best = k;
      dep[size_t(q)] = best, used[size_t(best)] = true;
    }
    for (const auto& [above, below] : twin)
      for (int q = 0; q < 3; ++q) {
        const V& d = dirs[size_t(q)];
        std::vector<std::tuple<int, int, double>> terms;  // node (1-based), dof, coefficient
        terms.push_back({below + 1, dep[size_t(q)] + 1, -d[size_t(dep[size_t(q)])]});
        for (int k = 0; k < 3; ++k)
          if (k != dep[size_t(q)] && std::fabs(d[size_t(k)]) > 1e-12) terms.push_back({below + 1, k + 1, -d[size_t(k)]});
        for (int k = 0; k < 3; ++k)
          if (std::fabs(d[size_t(k)]) > 1e-12) terms.push_back({above + 1, k + 1, d[size_t(k)]});
        if (q == 0) terms.push_back({-1 - int(b), 1, 1.0});  // the reference node, numbered below
        equations << "*EQUATION\n" << terms.size() << "\n";
        for (size_t i = 0; i < terms.size(); ++i) {
          const auto& [node, dof, c] = terms[i];
          equations << "@" << node << "@, " << dof << ", " << c << ((i + 1) % 4 == 0 || i + 1 == terms.size() ? "\n" : ", ");
        }
      }
    bolt_info.push_back({{"name", bo.load}, {"bolt", scene.node(bo.body)->name}, {"preload_N", bo.preload}, {"section_area_mm2", area},
                         {"nominal_stress_MPa", bo.preload / area}, {"at", at}, {"axis", n}, {"section_nodes", twin.size()}});
  }
  const int bolt_node0 = int(nodes_out.size()) + 1;
  {
    // Reference nodes get their numbers now that every node is in.
    std::string text = equations.str(), out;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '@') {
        out += text[i];
        continue;
      }
      const size_t j = text.find('@', i + 1);
      int node = std::stoi(text.substr(i + 1, j - i - 1));
      if (node < 0) node = bolt_node0 + (-node - 1);
      out += std::to_string(node);
      i = j;
    }
    equations.str(out);
  }
  for (size_t b = 0; b < bolts.size(); ++b) cload << bolt_node0 + int(b) << ", 1, " << bolts[b].preload << "\n";

  // ---- the input file
  report(0.3, "Writing the CalculiX input");
  std::ostringstream inp;
  inp.precision(10);
  inp << "*HEADING\nOPAD " << kind << " study, case " << load_case << "\n*NODE, NSET=NALL\n";
  for (size_t i = 0; i < nodes_out.size(); ++i) inp << i + 1 << ", " << nodes_out[i][0] << ", " << nodes_out[i][1] << ", " << nodes_out[i][2] << "\n";
  for (size_t b = 0; b < bolts.size(); ++b) inp << bolt_node0 + int(b) << ", " << bolts[b].at.X() << ", " << bolts[b].at.Y() << ", " << bolts[b].at.Z() << "\n";
  std::vector<Mat> mats;
  for (size_t i = 0; i < bodies.size(); ++i) {
    mats.push_back(material_for(doc, scene, bodies[i], st.value("materials", json())));
    if (mats.back().assumed) run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" has no material with elastic properties: steel assumed");
  }
  for (size_t i = 0; i < bodies.size(); ++i) {
    inp << "*ELEMENT, TYPE=" << (tn == 10 ? "C3D10" : "C3D4") << ", ELSET=B" << i + 1 << "\n";
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      if (solid_body[size_t(mesh.tet_solid[e])] != int(i)) continue;
      inp << e + 1;
      for (int k = 0; k < tn; ++k) inp << ", " << tets_out[e][size_t(k)] + 1;
      inp << "\n";
    }
  }
  inp << "*ELSET, ELSET=EALL\n";
  for (size_t i = 0; i < bodies.size(); ++i) inp << "B" << i + 1 << "\n";
  for (size_t i = 0; i < bodies.size(); ++i) {
    inp << "*MATERIAL, NAME=M" << i + 1 << "\n*ELASTIC\n" << mats[i].E << ", " << mats[i].nu << "\n*DENSITY\n" << mats[i].rho * 1e-9 << "\n";
    inp << "*SOLID SECTION, ELSET=B" << i + 1 << ", MATERIAL=M" << i + 1 << "\n";
  }
  // Supports.
  std::ostringstream boundary, dload;
  std::vector<std::string> fixed_sets;
  bool supported = false;
  std::map<int, V> forces;  // node -> force N
  json applied = json::array();
  int set_no = 0;
  for (const Load* l : loads) {
    const std::string set = "S" + std::to_string(++set_no);
    if (l->kind == "fixed" || l->kind == "displacement") {
      const auto tris = triangles_of(*l);
      const auto nodes = nodes_of(tris);
      inp << "*NSET, NSET=" << set << "\n";
      int n = 0;
      for (int k : nodes) inp << k + 1 << (++n % 12 ? ", " : ",\n");
      inp << "\n";
      if (l->kind == "fixed") {
        boundary << set << ", 1, 3\n";
        fixed_sets.push_back(set);
        supported = true;
      } else {
        const V d = l->def.value("vector", V{0, 0, 0});
        for (int k = 0; k < 3; ++k) boundary << set << ", " << k + 1 << ", " << k + 1 << ", " << d[size_t(k)] << "\n";
        fixed_sets.push_back(set);
        supported = true;
      }
      applied.push_back({{"name", l->name}, {"kind", l->kind}, {"nodes", nodes.size()}});
    } else if (l->kind == "force" || l->kind == "pressure" || l->kind == "moment") {
      const auto tris = triangles_of(*l);
      double area = 0;
      std::vector<Tri> info;
      for (size_t t : tris) info.push_back(tri_info(t)), area += info.back().area;
      if (l->kind == "force") {
        const V F = l->def.value("vector", V{0, 0, 0});
        for (const auto& ti : info)
          for (const auto& [node, w] : ti.weights) forces[node] = add(forces[node], mul(F, ti.area / area * w));
      } else if (l->kind == "pressure") {
        const double p = l->def.value("value", 0.0);
        for (const auto& ti : info)
          for (const auto& [node, w] : ti.weights) forces[node] = add(forces[node], mul(ti.normal, -p * ti.area * w));
      } else {
        // A moment as the linear force field about the faces' centre whose resultant moment is M.
        const V M = l->def.value("vector", V{0, 0, 0});
        const V axis = unit(M);
        std::map<int, double> weight;
        V c{0, 0, 0};
        for (const auto& ti : info)
          for (const auto& [node, w] : ti.weights) weight[node] += ti.area * w;
        double total = 0;
        for (const auto& [node, w] : weight) c = add(c, mul(mesh.nodes[size_t(node)], w)), total += w;
        c = mul(c, 1 / total);
        double J = 0;
        for (const auto& [node, w] : weight) {
          V r = sub(mesh.nodes[size_t(node)], c);
          r = sub(r, mul(axis, dot(r, axis)));
          J += w * dot(r, r);
        }
        if (J <= 0) throw Error("moment \"" + l->name + "\": its faces have no extent across the moment's axis");
        for (const auto& [node, w] : weight) {
          V r = sub(mesh.nodes[size_t(node)], c);
          r = sub(r, mul(axis, dot(r, axis)));
          forces[node] = add(forces[node], mul(cross(M, r), w / J));
        }
      }
      applied.push_back({{"name", l->name}, {"kind", l->kind}, {"faces_area_mm2", area}});
    } else if (l->kind == "gravity") {
      const V g = l->def.value("vector", V{0, 0, -9810});
      const double mag = norm(g);
      if (mag > 0) dload << "EALL, GRAV, " << mag << ", " << g[0] / mag << ", " << g[1] / mag << ", " << g[2] / mag << "\n";
      applied.push_back({{"name", l->name}, {"kind", l->kind}});
    }
  }
  // Nothing held: the least that keeps it still (3-2-1 on three far nodes), which carries no load when the loads balance.
  if (!supported) {
    size_t a = 0, b = 0, c = 0;
    double far = 0;
    for (size_t i = 0; i < mesh.nodes.size(); ++i)
      if (norm(sub(mesh.nodes[i], mesh.nodes[0])) > far) far = norm(sub(mesh.nodes[i], mesh.nodes[0])), a = i;
    far = 0;
    for (size_t i = 0; i < mesh.nodes.size(); ++i)
      if (norm(sub(mesh.nodes[i], mesh.nodes[a])) > far) far = norm(sub(mesh.nodes[i], mesh.nodes[a])), b = i;
    far = 0;
    const V ab = unit(sub(mesh.nodes[b], mesh.nodes[a]));
    for (size_t i = 0; i < mesh.nodes.size(); ++i) {
      V r = sub(mesh.nodes[i], mesh.nodes[a]);
      r = sub(r, mul(ab, dot(r, ab)));
      if (norm(r) > far) far = norm(r), c = i;
    }
    // a: all three; b: the two across ab; c: the one out of the abc plane.
    const V n = unit(cross(ab, sub(mesh.nodes[c], mesh.nodes[a])));
    int keep_b = std::fabs(ab[0]) > std::fabs(ab[1]) && std::fabs(ab[0]) > std::fabs(ab[2]) ? 1 : std::fabs(ab[1]) > std::fabs(ab[2]) ? 2 : 3;
    int keep_c = std::fabs(n[0]) > std::fabs(n[1]) && std::fabs(n[0]) > std::fabs(n[2]) ? 1 : std::fabs(n[1]) > std::fabs(n[2]) ? 2 : 3;
    boundary << a + 1 << ", 1, 3\n";
    for (int k = 1; k <= 3; ++k)
      if (k != keep_b) boundary << b + 1 << ", " << k << ", " << k << "\n";
    boundary << c + 1 << ", " << keep_c << ", " << keep_c << "\n";
    if (kind == "static") run.warnings.push_back("nothing holds the bodies: three nodes hold them still (3-2-1); if the loads do not balance, add a fixed support");
  }
  inp << equations.str();
  inp << "*BOUNDARY\n" << boundary.str();
  if (kind == "static") {
    inp << "*STEP\n*STATIC\n";
    if (!forces.empty() || !cload.str().empty()) {
      inp << "*CLOAD\n" << cload.str();
      for (const auto& [node, f] : forces)
        for (int k = 0; k < 3; ++k)
          if (f[size_t(k)] != 0) inp << node + 1 << ", " << k + 1 << ", " << f[size_t(k)] << "\n";
    }
    if (!dload.str().empty()) inp << "*DLOAD\n" << dload.str();
    inp << "*NODE FILE\nU\n*EL FILE\nS\n";
    for (const auto& s : fixed_sets) inp << "*NODE PRINT, NSET=" << s << ", TOTALS=ONLY\nRF\n";
    inp << "*END STEP\n";
  } else {
    const int modes = std::clamp(st.value("modes", 6), 1, 100);
    inp << "*STEP\n*FREQUENCY\n" << modes << "\n*NODE FILE\nU\n*END STEP\n";
  }

  // ---- run
  report(0.35, "Solving (CalculiX)");
  struct Dir {
    std::filesystem::path p;
    bool keep = false;
    ~Dir() {
      std::error_code e;
      if (!keep && !p.empty()) std::filesystem::remove_all(p, e);
    }
  } dir;
  dir.p = std::filesystem::temp_directory_path() / ("opad-ccx-" + new_uuid().substr(0, 12));
  std::filesystem::create_directories(dir.p);
  if (const char* k = std::getenv("OPAD_KEEP_CCX"); k && *k) dir.keep = true;
  write_text_file(dir.p / "job.inp", inp.str());
  // One thread for the solver unless asked: CalculiX 2.21's threaded SPOOLES factorisation (Ubuntu's ccx) races and now and
  // then returns wrong displacements for the same input (seen on a cantilever: 3.04 mm three runs out of five, 5.44 mm or
  // 1.82 mm the others); single-threaded it is exact and repeatable. OPAD_CCX_THREADS=n for a ccx known to be safe.
  {
    const char* asked = std::getenv("OPAD_CCX_THREADS");
    const std::string threads = asked && *asked ? asked : "1";
#ifdef _WIN32
    _putenv_s("OMP_NUM_THREADS", threads.c_str());
    _putenv_s("CCX_NPROC_EQUATION_SOLVER", threads.c_str());
#else
    setenv("OMP_NUM_THREADS", threads.c_str(), 1);
    setenv("CCX_NPROC_EQUATION_SOLVER", threads.c_str(), 1);
#endif
  }
  detail::RunOptions ro;
  ro.output = dir.p / "ccx.log";
  ro.timeout_ms = int(st.value("timeout", 1800.0) * 1000);
  ro.cancelled = [&] { return progress && !progress(0.5, "Solving (CalculiX)"); };
  const int status = detail::run_program(ccx, {"-i", "job"}, dir.p, ro);
  std::string log;
  try {
    log = read_text_file(dir.p / "ccx.log");
  } catch (...) {
  }
  if (status != 0 || log.find("*ERROR") != std::string::npos) {
    const auto at = log.find("*ERROR");
    std::string why = at == std::string::npos ? "it stopped with status " + std::to_string(status) : log.substr(at, std::min<size_t>(400, log.size() - at));
    throw Error("CalculiX failed: " + why);
  }
  report(0.9, "Reading the results");
  const Frd frd = read_frd(dir.p / "job.frd");

  // ---- results
  auto res = std::make_shared<FeaResult>();
  res->kind = kind;
  res->nodes = mesh.nodes;
  res->bodies = bodies;
  res->mesh_size = maxh;
  res->elements = mesh.tets.size();
  for (const auto& t : mesh.tets) res->tets.push_back({t[0], t[1], t[2], t[3]});
  // The outer skin: triangles with one element behind them, quadratic ones split in four.
  for (size_t t = 0; t < mesh.tris.size(); ++t) {
    const auto it = tet_face.find(corners(t));
    if (it == tet_face.end() || it->second.size() != 1) continue;
    const int body = solid_body[size_t(mesh.tet_solid[size_t(it->second.front().first)])];
    const auto& tri = mesh.tris[t];
    if (mesh.tri_nodes == 6) {
      // Which mid-side node sits on which edge: the nearest edge middle.
      int mid[3] = {-1, -1, -1};  // edge 01, 12, 20
      for (int k = 3; k < 6; ++k) {
        const V p = mesh.nodes[size_t(tri[size_t(k)])];
        double best = 1e300;
        int e = 0;
        for (int j = 0; j < 3; ++j) {
          const V m = mul(add(mesh.nodes[size_t(tri[size_t(j)])], mesh.nodes[size_t(tri[size_t((j + 1) % 3)])]), 0.5);
          if (norm(sub(m, p)) < best) best = norm(sub(m, p)), e = j;
        }
        mid[e] = tri[size_t(k)];
      }
      if (mid[0] >= 0 && mid[1] >= 0 && mid[2] >= 0) {
        for (const auto& s : {std::array<int, 3>{tri[0], mid[0], mid[2]}, {mid[0], tri[1], mid[1]}, {mid[2], mid[1], tri[2]}, {mid[0], mid[1], mid[2]}}) {
          res->skin.push_back(s);
          res->skin_body.push_back(body);
        }
        continue;
      }
    }
    res->skin.push_back({tri[0], tri[1], tri[2]});
    res->skin_body.push_back(body);
  }
  const size_t nn = mesh.nodes.size();
  auto field = [&](const Frd::Block& blk, size_t width) {
    std::vector<std::vector<double>> out(nn, std::vector<double>(width, 0.0));
    for (const auto& [node, v] : blk.values)
      if (node >= 1 && size_t(node) <= nn)
        for (size_t k = 0; k < width && k < v.size(); ++k) out[size_t(node - 1)][k] = v[k];
    return out;
  };
  json summary;
  if (kind == "static") {
    const Frd::Block* disp = nullptr;
    const Frd::Block* stress = nullptr;
    for (const auto& b : frd.blocks) {
      if (b.name == "DISP" && !disp) disp = &b;
      if (b.name == "STRESS" && !stress) stress = &b;
    }
    if (!disp || !stress) throw Error("CalculiX wrote no displacements or stresses");
    const auto d = field(*disp, 3), s = field(*stress, 6);
    res->displacement.resize(nn);
    res->stress.resize(nn);
    res->von_mises.resize(nn);
    for (size_t i = 0; i < nn; ++i) {
      res->displacement[i] = {d[i][0], d[i][1], d[i][2]};
      for (size_t k = 0; k < 6; ++k) res->stress[i][k] = s[i][k];
      res->von_mises[i] = von_mises(res->stress[i]);
    }
    // Peaks, overall and per body.
    size_t vm_at = 0, d_at = 0;
    for (size_t i = 0; i < nn; ++i) {
      if (res->von_mises[i] > res->von_mises[vm_at]) vm_at = i;
      if (norm(res->displacement[i]) > norm(res->displacement[d_at])) d_at = i;
    }
    summary["max_von_mises_MPa"] = res->von_mises[vm_at];
    summary["max_von_mises_at"] = res->nodes[vm_at];
    summary["max_displacement_mm"] = norm(res->displacement[d_at]);
    summary["max_displacement_at"] = res->nodes[d_at];
    summary["max_displacement"] = res->displacement[d_at];
    std::vector<double> body_peak(bodies.size(), 0.0);
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      const int b = solid_body[size_t(mesh.tet_solid[e])];
      for (int k = 0; k < tn; ++k) body_peak[size_t(b)] = std::max(body_peak[size_t(b)], res->von_mises[size_t(mesh.tets[e][size_t(k)])]);
    }
    json per_body = json::object();
    double worst_sf = 1e300;
    for (size_t i = 0; i < bodies.size(); ++i) {
      const double sf = body_peak[i] > 0 ? mats[i].yield / body_peak[i] : 1e300;
      per_body[scene.node(bodies[i])->name] = {{"max_von_mises_MPa", body_peak[i]}, {"material", mats[i].name}, {"yield_MPa", mats[i].yield},
                                               {"safety_factor", sf < 1e299 ? json(sf) : json(nullptr)}};
      worst_sf = std::min(worst_sf, sf);
    }
    summary["bodies"] = per_body;
    if (worst_sf < 1e299) summary["min_safety_factor"] = worst_sf;
    const auto reactions = read_reactions(dir.p / "job.dat");
    json rj = json::object();
    for (size_t k = 0; k < fixed_sets.size(); ++k)
      if (const auto it = reactions.find(fixed_sets[k]); it != reactions.end()) {
        // the set's load name
        int counter = 0;
        for (const Load* l : loads)
          if ("S" + std::to_string(++counter) == fixed_sets[k]) rj[l->name] = it->second;
      }
    if (!rj.empty()) summary["reactions_N"] = rj;
    // Bolts: the axial stress across the shank half way between the cut and the bolt's ends is what the preload makes.
    for (auto& bj : bolt_info) {
      const V n = bj["axis"].get<V>(), at = bj["at"].get<V>();
      const std::string bolt = bj["bolt"].get<std::string>();
      double sum = 0, count = 0;
      for (size_t e = 0; e < mesh.tets.size(); ++e) {
        const int b = solid_body[size_t(mesh.tet_solid[e])];
        if (scene.node(bodies[size_t(b)])->name != bolt) continue;
        for (int k = 0; k < 4; ++k) {
          const size_t node = size_t(mesh.tets[e][size_t(k)]);
          const double along = dot(sub(res->nodes[node], at), n);
          if (std::fabs(along) < 1.5 * maxh || std::fabs(along) > 2.5 * maxh) continue;  // a slice two elements off the cut
          const auto& s = res->stress[node];
          const double axial = n[0] * n[0] * s[0] + n[1] * n[1] * s[1] + n[2] * n[2] * s[2] + 2 * (n[0] * n[1] * s[3] + n[1] * n[2] * s[4] + n[2] * n[0] * s[5]);
          sum += axial, count += 1;
        }
      }
      if (count > 0) bj["axial_stress_MPa"] = sum / count;
    }
    if (!bolt_info.empty()) summary["bolts"] = bolt_info;
  } else {
    res->frequencies = read_frequencies(dir.p / "job.dat");
    for (const auto& b : frd.blocks) {
      if (b.name != "DISP" || !b.modal) continue;
      const auto d = field(b, 3);
      std::vector<V> shape(nn);
      for (size_t i = 0; i < nn; ++i) shape[i] = {d[i][0], d[i][1], d[i][2]};
      res->modes.push_back(std::move(shape));
    }
    if (res->frequencies.empty()) throw Error("CalculiX found no natural frequencies");
    summary["frequencies_Hz"] = res->frequencies;
    if (!supported) summary["note"] = "free: the first six modes are the rigid-body motions (about 0 Hz)";
  }
  summary["nodes"] = nn;
  summary["elements"] = mesh.tets.size();
  summary["element_type"] = tn == 10 ? "C3D10 (quadratic tetrahedra)" : "C3D4 (linear tetrahedra)";
  summary["mesh_size_mm"] = maxh;
  summary["loads"] = applied;
  json mat_json = json::object();
  for (size_t i = 0; i < bodies.size(); ++i)
    mat_json[scene.node(bodies[i])->name] = {{"material", mats[i].name}, {"E_MPa", mats[i].E}, {"nu", mats[i].nu}, {"density", mats[i].rho}};
  summary["materials"] = mat_json;
  if (!run.warnings.empty()) summary["warnings"] = run.warnings;
  if (dir.keep) summary["files"] = path_to_utf8(dir.p);
  for (const auto& [k, v] : summary.items()) run.summary[k] = v;
  // Frames: static has one (the loaded state), modal one per mode; the parts stay where they are.
  if (kind == "static") run.t = {0.0};
  else run.t.assign(res->frequencies.begin(), res->frequencies.end());
  run.fea = res;
  report(1, "Done");
  return run;
}

}  // namespace opad::sim
