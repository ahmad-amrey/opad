#include "opad/sim/cfd.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepTools.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <gp_Lin.hxx>
#include <TopoDS_Wire.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <OSD_Parallel.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <tuple>
#include <optional>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>

#include "../import_common.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"
#include "opad/materials.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/fea.hpp"

namespace opad::sim {

namespace fs = std::filesystem;

namespace {

using V = Vec3;
constexpr double kPi = 3.14159265358979323846;
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V add(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V mul(const V& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const V& a) { return std::sqrt(dot(a, a)); }
V unit(const V& a) {
  const double n = norm(a);
  return n > 1e-12 ? mul(a, 1 / n) : V{0, 0, 0};
}

std::string num(double v) {
  std::ostringstream s;
  s.precision(10);
  s << v;
  return s.str();
}
std::string vec(const V& v) { return "(" + num(v[0]) + " " + num(v[1]) + " " + num(v[2]) + ")"; }

// ---------------------------------------------------------------- OpenFOAM's files
std::string header(const std::string& cls, const std::string& object, const std::string& location = {}) {
  return "FoamFile\n{\n    version 2.0;\n    format ascii;\n    class " + cls + ";\n" + (location.empty() ? "" : "    location \"" + location + "\";\n") +
         "    object " + object + ";\n}\n";
}
void put(const fs::path& file, const std::string& cls, const std::string& object, const std::string& body) {
  fs::create_directories(file.parent_path());
  write_text_file(file, header(cls, object) + body);
}

// The list after "N\n(" in an OpenFOAM file (points, faces, labels, a field), as text from its opening bracket.
std::string_view list_after(std::string_view t, size_t from = 0) {
  for (size_t i = from; i < t.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(t[i]))) continue;
    size_t j = i;
    while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
    size_t k = j;
    while (k < t.size() && std::isspace(static_cast<unsigned char>(t[k]))) ++k;
    if (k < t.size() && t[k] == '(' && (i == 0 || std::isspace(static_cast<unsigned char>(t[i - 1])))) return t.substr(k);
    i = j;
  }
  throw Error("cfd: an OpenFOAM list is missing");
}
size_t count_before(std::string_view list_from_bracket, std::string_view whole) {
  // the number written before the list's bracket
  const size_t at = size_t(list_from_bracket.data() - whole.data());
  size_t e = at;
  while (e > 0 && std::isspace(static_cast<unsigned char>(whole[e - 1]))) --e;
  size_t b = e;
  while (b > 0 && std::isdigit(static_cast<unsigned char>(whole[b - 1]))) --b;
  return size_t(std::stoull(std::string(whole.substr(b, e - b))));
}
std::vector<V> read_points(const std::string& t) {
  const auto l = list_after(t, t.find("FoamFile") == std::string::npos ? 0 : t.find('}'));
  const size_t n = count_before(l, t);
  std::vector<V> out;
  out.reserve(n);
  const char* p = l.data() + 1;
  const char* end = t.data() + t.size();
  while (out.size() < n && p < end) {
    while (p < end && *p != '(') ++p;
    ++p;
    char* q;
    V v;
    v[0] = std::strtod(p, &q), v[1] = std::strtod(q, &q), v[2] = std::strtod(q, &q);
    out.push_back(v);
    p = q;
  }
  return out;
}
std::vector<std::vector<int>> read_faces(const std::string& t) {
  const auto l = list_after(t, t.find('}'));
  const size_t n = count_before(l, t);
  std::vector<std::vector<int>> out;
  out.reserve(n);
  const char* p = l.data() + 1;
  const char* end = t.data() + t.size();
  while (out.size() < n && p < end) {
    char* q;
    const long m = std::strtol(p, &q, 10);
    p = q;
    while (p < end && *p != '(') ++p;
    ++p;
    std::vector<int> f(size_t(std::max(0L, m)));
    for (auto& x : f) x = int(std::strtol(p, &q, 10)), p = q;
    while (p < end && *p != ')') ++p;
    ++p;
    out.push_back(std::move(f));
  }
  return out;
}
std::vector<double> read_numbers(std::string_view whole, std::string_view l) {
  const size_t n = count_before(l, whole);
  std::vector<double> out;
  out.reserve(n);
  const char* p = l.data() + 1;
  const char* end = whole.data() + whole.size();
  while (out.size() < n && p < end) {
    while (p < end && (std::isspace(static_cast<unsigned char>(*p)) || *p == '(' || *p == ')')) ++p;
    char* q;
    out.push_back(std::strtod(p, &q));
    if (q == p) break;
    p = q;
  }
  return out;
}
std::vector<int> read_labels(const std::string& t) {
  const auto v = read_numbers(t, list_after(t, t.find('}')));
  return std::vector<int>(v.begin(), v.end());
}
// A field's internal values: scalars, or vectors flattened (3 per cell); a uniform field gives its one value n times.
std::vector<double> read_internal(const std::string& t, size_t cells, int width) {
  const size_t at = t.find("internalField");
  if (at == std::string::npos) throw Error("cfd: a field without internalField");
  const size_t semi = t.find(';', at);
  const std::string head = t.substr(at, std::min(semi, t.find('(', at)) - at);
  if (head.find("nonuniform") == std::string::npos) {
    std::istringstream s(t.substr(at + 13, semi - at - 13));
    std::string word;
    s >> word;
    std::vector<double> one(size_t(width), 0.0);
    if (width == 1) s >> one[0];
    else {
      char c;
      s >> c >> one[0] >> one[1] >> one[2];
    }
    std::vector<double> out;
    for (size_t i = 0; i < cells; ++i) out.insert(out.end(), one.begin(), one.end());
    return out;
  }
  const auto l = list_after(t, at);
  if (width == 1) return read_numbers(t, l);
  const size_t n = count_before(l, t);
  std::vector<double> out;
  out.reserve(n * 3);
  const char* p = l.data() + 1;
  const char* end = t.data() + t.size();
  for (size_t i = 0; i < n && p < end; ++i) {
    while (p < end && *p != '(') ++p;
    ++p;
    char* q;
    for (int k = 0; k < 3; ++k) out.push_back(std::strtod(p, &q)), p = q;
  }
  return out;
}
// A patch's values in a field file's boundaryField (scalar), empty when the patch has none written.
std::vector<double> read_patch(const std::string& t, const std::string& patch) {
  const size_t bf = t.find("boundaryField");
  if (bf == std::string::npos) return {};
  size_t at = bf;
  for (;;) {
    at = t.find(patch, at + 1);
    if (at == std::string::npos) return {};
    const char before = t[at - 1], after = at + patch.size() < t.size() ? t[at + patch.size()] : ' ';
    if (std::isspace(static_cast<unsigned char>(before)) && std::isspace(static_cast<unsigned char>(after))) break;
  }
  const size_t open = t.find('{', at), close = t.find('}', open);
  const std::string blk = t.substr(open, close - open);
  const size_t v = blk.find("value");
  if (v == std::string::npos) return {};
  if (blk.find("nonuniform", v) != std::string::npos) return read_numbers(blk, list_after(blk, v));
  std::istringstream s(blk.substr(v + 5));
  std::string word;
  double x = 0;
  s >> word >> x;
  return {x};
}

// A blower opening's flow in a velocity file: the volumetricFlowRate of its patch's entry (m3/s).
std::string set_flow_rate(std::string U, const std::string& patch, double Q) {
  const size_t bf = U.find("boundaryField");
  size_t at = bf;
  for (;;) {
    at = U.find(patch, at + 1);
    if (at == std::string::npos || bf == std::string::npos) return U;
    const char before = U[at - 1], after = at + patch.size() < U.size() ? U[at + patch.size()] : ' ';
    if (std::isspace(static_cast<unsigned char>(before)) && std::isspace(static_cast<unsigned char>(after))) break;
  }
  const size_t open = U.find('{', at), close = U.find('}', open);
  const size_t key = U.find("volumetricFlowRate", open);
  if (key == std::string::npos || key > close) return U;
  const size_t end = U.find(';', key);
  std::ostringstream v;
  v.precision(10);
  v << "volumetricFlowRate constant " << Q;
  return U.replace(key, end - key, v.str());
}

// A region's mesh: its cells' centres and volumes (from its faces), mm.
struct Cells {
  std::vector<V> centre;
  std::vector<double> volume;
};
Cells read_cells(const fs::path& poly) {
  const auto P = read_points(read_text_file(poly / "points"));
  const auto F = read_faces(read_text_file(poly / "faces"));
  const auto own = read_labels(read_text_file(poly / "owner"));
  const auto nei = read_labels(read_text_file(poly / "neighbour"));
  // A cell may own no face (the last, when all its faces are inner ones with lower neighbours): count both lists.
  int nc = 0;
  for (int o : own) nc = std::max(nc, o + 1);
  for (int n : nei) nc = std::max(nc, n + 1);
  // Face centres and area vectors from a fan of triangles about the points' mean; a cell's volume by the divergence
  // theorem, its centre as the volume-weighted centre of the pyramids on its faces.
  std::vector<V> fc(F.size()), fa(F.size());
  for (size_t f = 0; f < F.size(); ++f) {
    V m{0, 0, 0};
    for (int k : F[f]) m = add(m, P[size_t(k)]);
    m = mul(m, 1.0 / double(F[f].size()));
    V area{0, 0, 0}, c{0, 0, 0};
    double total = 0;
    for (size_t k = 0; k < F[f].size(); ++k) {
      const V& a = P[size_t(F[f][k])];
      const V& b = P[size_t(F[f][(k + 1) % F[f].size()])];
      const V s = mul(cross(sub(a, m), sub(b, m)), 0.5);
      const double w = norm(s);
      area = add(area, s);
      c = add(c, mul(add(add(a, b), m), w / 3));
      total += w;
    }
    fc[f] = total > 0 ? mul(c, 1 / total) : m;
    fa[f] = area;
  }
  // Mean of each cell's face centres: a point inside it for the pyramids.
  std::vector<V> guess(size_t(nc), V{0, 0, 0});
  std::vector<int> nf(size_t(nc), 0);
  for (size_t f = 0; f < F.size(); ++f) {
    guess[size_t(own[f])] = add(guess[size_t(own[f])], fc[f]), ++nf[size_t(own[f])];
    if (f < nei.size()) guess[size_t(nei[f])] = add(guess[size_t(nei[f])], fc[f]), ++nf[size_t(nei[f])];
  }
  for (int c = 0; c < nc; ++c) guess[size_t(c)] = mul(guess[size_t(c)], 1.0 / std::max(1, nf[size_t(c)]));
  Cells out;
  out.centre.assign(size_t(nc), V{0, 0, 0});
  out.volume.assign(size_t(nc), 0.0);
  std::vector<V> moment(size_t(nc), V{0, 0, 0});
  auto pyramid = [&](int c, size_t f, double sign) {
    const double v = sign * dot(fa[f], sub(fc[f], guess[size_t(c)])) / 3;
    out.volume[size_t(c)] += v;
    moment[size_t(c)] = add(moment[size_t(c)], mul(add(mul(fc[f], 0.75), mul(guess[size_t(c)], 0.25)), v));
  };
  for (size_t f = 0; f < F.size(); ++f) {
    pyramid(own[f], f, 1);
    if (f < nei.size()) pyramid(nei[f], f, -1);
  }
  for (int c = 0; c < nc; ++c)
    out.centre[size_t(c)] = out.volume[size_t(c)] > 0 ? mul(moment[size_t(c)], 1 / out.volume[size_t(c)]) : guess[size_t(c)];
  return out;
}

// Nearest of a set of points: a uniform grid of buckets.
class Nearest {
 public:
  Nearest(const std::vector<V>& pts, double cell) : m_pts(pts), m_cell(std::max(cell, 1e-6)) {
    for (size_t i = 0; i < pts.size(); ++i) m_grid[key(pts[i])].push_back(i);
  }
  size_t operator()(const V& p) const { return (*this)(p, nullptr); }
  // The nearest that `ok` accepts (npos when none is within 64 buckets).
  static constexpr size_t npos = size_t(-1);
  size_t operator()(const V& p, const std::function<bool(size_t)>& ok) const {
    size_t best = ok ? npos : 0;
    double bd = 1e300;
    for (int r = 0; r < 64 && (bd == 1e300 || double(r - 1) * m_cell < std::sqrt(bd)); ++r) {
      const auto k = key(p);
      for (int i = -r; i <= r; ++i)
        for (int j = -r; j <= r; ++j)
          for (int l = -r; l <= r; ++l) {
            if (std::max({std::abs(i), std::abs(j), std::abs(l)}) != r) continue;
            const auto it = m_grid.find({k[0] + i, k[1] + j, k[2] + l});
            if (it == m_grid.end()) continue;
            for (size_t n : it->second)
              if (const double d = dot(sub(m_pts[n], p), sub(m_pts[n], p)); d < bd && (!ok || ok(n))) bd = d, best = n;
          }
    }
    return best;
  }

 private:
  std::array<long, 3> key(const V& p) const { return {long(std::floor(p[0] / m_cell)), long(std::floor(p[1] / m_cell)), long(std::floor(p[2] / m_cell))}; }
  const std::vector<V>& m_pts;
  double m_cell;
  std::map<std::array<long, 3>, std::vector<size_t>> m_grid;
};

std::string latest_time(const fs::path& dir) {
  double best = -1;
  std::string name;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_directory()) continue;
    const std::string s = e.path().filename().string();
    char* end = nullptr;
    const double t = std::strtod(s.c_str(), &end);
    if (end && *end == 0 && !s.empty() && t > best) best = t, name = s;
  }
  return name;
}

// A mesh's patches (constant/polyMesh/boundary): name, type, faces.
struct Patch {
  std::string name, type;
  size_t faces = 0, start = 0;
};
std::vector<Patch> read_boundary(const std::string& t) {
  std::vector<Patch> out;
  const std::string_view body = list_after(t);
  size_t at = 0;
  while (at < body.size()) {
    const size_t open = body.find('{', at);
    if (open == std::string_view::npos) break;
    const size_t close = body.find('}', open);
    size_t n0 = body.find_last_not_of(" \t\r\n", open - 1);
    size_t n1 = body.find_last_of(" \t\r\n(", n0);
    Patch p;
    p.name = std::string(body.substr(n1 + 1, n0 - n1));
    const std::string_view e = body.substr(open, close - open);
    auto value = [&](const char* key) {
      const size_t k = e.find(key);
      if (k == std::string_view::npos) return std::string();
      size_t v = k + std::strlen(key);
      while (v < e.size() && std::isspace(static_cast<unsigned char>(e[v]))) ++v;
      const size_t w = e.find(';', v);
      return std::string(e.substr(v, w - v));
    };
    p.type = value("type");
    p.faces = size_t(std::strtoull(value("nFaces").c_str(), nullptr, 10));
    p.start = size_t(std::strtoull(value("startFace").c_str(), nullptr, 10));
    out.push_back(p);
    at = close + 1;
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- where OpenFOAM is
OpenFoam openfoam() {
  OpenFoam f;
  auto has_bin = [](const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "simpleFoam", ec) || fs::is_regular_file(dir / "simpleFoam.exe", ec);
  };
  if (const char* env = std::getenv("OPAD_OPENFOAM"); env && *env) {
    const fs::path p = path_from_utf8(env);
    std::error_code ec;
    if (fs::is_regular_file(p / "etc" / "openfoam", ec) && fs::is_regular_file(p / "bin" / "foamEtcFile", ec)) f.wrapper = p / "etc" / "openfoam";
    if (has_bin(p / "bin")) f.bin = p / "bin";
    f.project = p;
    return f;
  }
  // On the PATH, with the project directory from the environment or where packages put it.
  if (const char* path = std::getenv("PATH")) {
    std::string s = path;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    for (size_t a = 0; a <= s.size();) {
      const size_t b = std::min(s.find(sep, a), s.size());
      const fs::path dir = s.substr(a, b - a);
      if (!dir.empty() && has_bin(dir)) {
        f.bin = dir;
        break;
      }
      a = b + 1;
    }
  }
  if (f.bin.empty()) {
    // An OpenCFD package's own wrapper (openfoam2306, ...), which sets everything up.
    std::error_code ec;
    for (const char* root : {"/usr/lib/openfoam", "/opt"})
      for (const auto& e : fs::directory_iterator(root, ec))
        if (e.path().filename().string().rfind("openfoam", 0) == 0 && fs::is_regular_file(e.path() / "etc" / "openfoam", ec) &&
            fs::is_regular_file(e.path() / "bin" / "foamEtcFile", ec)) {
          f.wrapper = e.path() / "etc" / "openfoam", f.project = e.path();
          return f;
        }
    return f;
  }
  if (const char* wm = std::getenv("WM_PROJECT_DIR"); wm && *wm) f.project = path_from_utf8(wm);
  else
    for (const char* guess : {"/usr/share/openfoam", "/usr/lib/openfoam"})
      if (std::error_code ec; fs::is_regular_file(fs::path(guess) / "etc" / "controlDict", ec)) f.project = guess;
  return f;
}

BodyBoxes body_boxes(const Document& doc, const Scene& scene) {
  BodyBoxes out;
  std::vector<std::string> ids;
  for (const auto& id : scene.all_bodies()) {
    const Node* n = scene.node(id);
    if (n && n->kind == Node::Kind::Body && !n->body_missing && n->representation == "solid" && scene.effectively_visible(id)) ids.push_back(id);
  }
  // Each body's box once, side by side: measuring every body again for every candidate box held the UI for a minute.
  std::vector<BodyBoxes::Box> boxes(ids.size());
  std::vector<char> ok(ids.size(), 0);
  OSD_Parallel::For(0, int(ids.size()), [&](int i) {
    try {
      Bnd_Box b;
      BRepBndLib::Add(node_world_shape(doc, scene, ids[size_t(i)]), b);
      if (b.IsVoid()) return;
      auto& c = boxes[size_t(i)];
      b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
      ok[size_t(i)] = 1;
    } catch (const std::exception&) {
    } catch (const Standard_Failure&) {
    }
  });
  for (size_t i = 0; i < ids.size(); ++i)
    if (ok[i]) {
      out.at[ids[i]] = out.ids.size();
      out.ids.push_back(ids[i]);
      out.boxes.push_back(boxes[i]);
    }
  return out;
}

namespace {
// A fan's opening as a face in the world and its normal (fan_profile, below).
TopoDS_Face profile_face(const Document& doc, const Scene& scene, const json& ref, V& normal) {
  if (ref.is_object() && ref.contains("sketch")) {
    const std::string id = ref["sketch"].get<std::string>();
    const SketchItem* item = nullptr;
    for (const auto& k : scene.sketches)
      if (k.id == id) item = &k;
    if (!item) throw Error("a fan's opening names a sketch that is not there");
    const auto regions = design::sketch_regions(design::Sketch::from_json(item->geometry), item->frame);
    if (regions.empty()) throw Error("the sketch of a fan's opening has no closed profile");
    int i = -1;
    if (ref.contains("boundary")) {
      auto key = [](std::vector<int> ids) {
        for (int& v : ids) v = std::abs(v);
        std::sort(ids.begin(), ids.end());
        return ids;
      };
      const auto want = key(ref["boundary"].get<std::vector<int>>());
      for (size_t k = 0; k < regions.size() && i < 0; ++k)
        if (key(regions[k].boundary) == want) i = int(k);
    }
    if (i < 0 && ref.contains("at")) i = design::region_at(regions, item->frame, ref["at"][0].get<double>(), ref["at"][1].get<double>());
    if (i < 0 && !ref.contains("at") && !ref.contains("boundary"))  // the sketch alone: its smallest region (an opening drawn
      for (size_t k = 0; k < regions.size(); ++k)                    // inside its face's outline, projected)
        if (i < 0 || regions[k].area < regions[size_t(i)].area) i = int(k);
    if (i < 0) throw Error("a fan's opening is no longer a closed region of its sketch: pick it again");
    normal = item->frame.normal();
    return regions[size_t(i)].face;
  }
  const Ref r = Ref::from_json(ref);
  const TopoDS_Shape f = subshape(node_world_shape(doc, scene, r.body), Ref::Kind::Face, r.index);
  if (f.IsNull() || f.ShapeType() != TopAbs_FACE) throw Error("a fan's opening is a sketch's closed region or a flat face");
  BRepAdaptor_Surface surface(TopoDS::Face(f));
  if (surface.GetType() != GeomAbs_Plane) throw Error("a fan's opening must be flat: a sketch's closed region or a flat face");
  gp_Dir n = surface.Plane().Axis().Direction();
  if (!surface.Plane().Position().Direct()) n.Reverse();
  if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
  normal = {n.X(), n.Y(), n.Z()};
  return TopoDS::Face(f);
}
}  // namespace

FanProfile fan_profile(const Document& doc, const Scene& scene, const json& ref) {
  FanProfile out;
  V n{0, 0, 1};
  const TopoDS_Face face = profile_face(doc, scene, ref, n);
  out.normal = n;
  GProp_GProps g;
  BRepGProp::SurfaceProperties(face, g);
  out.area = g.Mass();
  out.centre = {g.CentreOfMass().X(), g.CentreOfMass().Y(), g.CentreOfMass().Z()};
  for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
    std::vector<std::array<double, 3>> loop;
    for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
      BRepAdaptor_Curve c(e.Current());
      const bool reversed = e.Orientation() == TopAbs_REVERSED;
      const int n_pts = c.GetType() == GeomAbs_Line ? 1 : 24;
      for (int k = 0; k < n_pts; ++k) {
        const double t = double(k) / n_pts;
        const double u = reversed ? c.LastParameter() - t * (c.LastParameter() - c.FirstParameter()) : c.FirstParameter() + t * (c.LastParameter() - c.FirstParameter());
        const gp_Pnt p = c.Value(u);
        loop.push_back({p.X(), p.Y(), p.Z()});
      }
    }
    if (loop.size() >= 3) out.loops.push_back(loop);
  }
  return out;
}

json vent_estimate(const Document& doc, const Scene& scene, const std::vector<std::string>& walls, const std::vector<json>& fans, double watts,
                   double ambient_C, const std::function<bool(int, int)>& progress) {
  using P = std::array<double, 3>;
  auto dot3 = [](const P& a, const P& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  auto minus = [](const P& a, const P& b) { return P{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
  auto unit = [&](P v) {
    const double l = std::sqrt(dot3(v, v));
    return l > 1e-12 ? P{v[0] / l, v[1] / l, v[2] / l} : P{0, 0, 0};
  };
  auto neg = [](const P& v) { return P{-v[0], -v[1], -v[2]}; };
  std::vector<TopoDS_Shape> shapes;
  Bnd_Box all;
  for (const auto& w : walls) {
    if (!scene.node(w)) continue;
    shapes.push_back(node_world_shape(doc, scene, w));
    BRepBndLib::Add(shapes.back(), all);
  }
  if (all.IsVoid()) throw Error("the box has no walls to find its vents in");
  double x0, y0, z0, x1, y1, z1;
  all.Get(x0, y0, z0, x1, y1, z1);
  const P mid{0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.5 * (z0 + z1)};
  // What may fill a hole or stand behind it: the walls, and the bodies in the box (a connector in its cut-out, a screw in its
  // hole), their shapes read only where a probe falls in their box. Each one's classifier is made once (made per probe, it
  // walked a detailed box's every face each time: minutes).
  struct Blocker {
    std::string id;
    Bnd_Box box;
    TopoDS_Shape shape;
    bool wall = false;
    std::unique_ptr<BRepClass3d_SolidClassifier> classifier;
  };
  std::vector<Blocker> blockers;
  for (size_t k = 0; k < shapes.size(); ++k) {
    Bnd_Box b;
    BRepBndLib::Add(shapes[k], b);
    blockers.push_back({walls[k], b, shapes[k], true, nullptr});
  }
  for (const auto& id : scene.all_bodies()) {
    if (std::find(walls.begin(), walls.end(), id) != walls.end() || !scene.effectively_visible(id)) continue;
    const Bnd_Box b = node_world_bbox(doc, scene, id);
    if (!b.IsVoid() && !b.IsOut(all)) blockers.push_back({id, b, TopoDS_Shape(), false, nullptr});
  }
  auto shape_of = [&](Blocker& b) -> const TopoDS_Shape& {
    if (b.shape.IsNull()) b.shape = node_world_shape(doc, scene, b.id);
    return b.shape;
  };
  auto inside = [&](Blocker& b, const gp_Pnt& p) {
    if (b.box.IsOut(p)) return false;
    if (!b.classifier) b.classifier = std::make_unique<BRepClass3d_SolidClassifier>(shape_of(b));
    b.classifier->Perform(p, 1e-6);
    return b.classifier->State() == TopAbs_IN;
  };
  auto solid_at = [&](const gp_Pnt& p, bool walls_only) {
    for (auto& b : blockers)
      if ((!walls_only || b.wall) && inside(b, p)) return true;
    return false;
  };
  // The walls loaded once for rays out of the box.
  std::vector<IntCurvesFace_ShapeIntersector> outward(shapes.size());
  for (size_t k = 0; k < shapes.size(); ++k) outward[k].Load(shapes[k], 1e-6);
  const double reach = std::sqrt(all.SquareExtent());

  // A hole: an inner loop of a flat face, through its body (and a part sitting in it fills it).
  struct Hole {
    P centre, normal;  // on the face, the face's own normal
    double area = 0, width = 0, thickness = 1;  // mm2; hydraulic width (4 area / perimeter) and length through, mm
    std::string body;
    int fan = -1;  // the fan whose own path it is
    bool exit = false;  // the air leaves the box (or the fan) through it
  };
  int blind = 0, filled = 0;
  // The holes through one body's flat faces that `keep` (centre, normal) accepts; to_room: one counts only when straight out
  // of it the box's walls leave the way open (a tray's open top under its lid leads into the lid).
  auto holes_of = [&](size_t bk, const std::function<bool(const gp_Pnt&, const gp_Dir&)>& keep, bool to_room, int* examined, int total) {
    std::vector<Hole> out;
    Blocker& own = blockers[bk];
    for (TopExp_Explorer e(shape_of(own), TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face face = TopoDS::Face(e.Current());
      BRepAdaptor_Surface surface(face);
      if (surface.GetType() != GeomAbs_Plane) continue;  // vents are cut through flat panels
      gp_Dir n = surface.Plane().Axis().Direction();
      if (!surface.Plane().Position().Direct()) n.Reverse();
      if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
      const TopoDS_Wire outer = BRepTools::OuterWire(face);
      for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
        if (examined && progress && !progress(++*examined, total)) throw Error("cancelled");
        if (w.Current().IsSame(outer)) continue;
        BRepBuilderAPI_MakeFace hole(surface.Plane(), TopoDS::Wire(w.Current()));
        if (!hole.IsDone()) continue;
        TopoDS_Face hf = hole.Face();
        GProp_GProps g;
        BRepGProp::SurfaceProperties(hf, g);
        const double area = std::fabs(g.Mass());
        if (area < 1e-6) continue;
        gp_Pnt c = g.CentreOfMass();
        if (!keep(c, n)) continue;
        // A point in the hole itself (an L-shaped slot's middle may not be): one of its triangles' middles.
        if (BRepClass_FaceClassifier(TopoDS::Face(hf.Oriented(TopAbs_FORWARD)), c, 1e-6).State() != TopAbs_IN) {
          BRepMesh_IncrementalMesh(hf, 0.1, false, 0.5, false);
          TopLoc_Location loc;
          const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(hf, loc);
          if (!tri.IsNull() && tri->NbTriangles() > 0) {
            int a, b, cc;
            tri->Triangle(1).Get(a, b, cc);
            c = gp_Pnt((tri->Node(a).XYZ() + tri->Node(b).XYZ() + tri->Node(cc).XYZ()) / 3).Transformed(loc.Transformation());
          }
        }
        GProp_GProps rimProps;
        BRepGProp::LinearProperties(w.Current(), rimProps);
        // The body's thickness beside the hole: from a point of the face just past the hole's rim, inwards until out of it.
        double thickness = 0;
        {
          BRepAdaptor_Curve rim(TopoDS::Edge(TopExp_Explorer(w.Current(), TopAbs_EDGE).Current()));
          const gp_Pnt r = rim.Value(0.5 * (rim.FirstParameter() + rim.LastParameter()));
          gp_Vec away(c, r);
          away -= gp_Vec(n) * away.Dot(gp_Vec(n));
          if (away.Magnitude() > 1e-9) {
            const gp_Pnt beside = r.Translated(away.Normalized() * 0.3);
            for (double d = 0.1; d < 30; d += std::max(0.1, 0.1 * d)) {
              if (!inside(own, beside.Translated(gp_Vec(n) * -d))) break;
              thickness = d;
            }
          }
        }
        if (thickness <= 0) thickness = 1;  // not measured: a millimetre
        if (to_room) {
          bool covered = false;
          for (auto& x : outward) {
            x.Perform(gp_Lin(c.Translated(gp_Vec(n) * 0.05), n), 0, reach);
            if (x.IsDone() && x.NbPnt() > 0) covered = true;
          }
          if (covered) {
            ++blind;
            continue;
          }
        }
        // Through: from its middle the way in stays clear of every solid through the body's own thickness there (a boss, a
        // foot or a blind pocket goes into solid at once or before that depth; a part sitting in it fills it). What stands
        // behind it (a fan drawing through it) does not close it.
        bool clear = !solid_at(c.Translated(gp_Vec(n) * 0.05), false);
        bool ownOnly = clear || inside(own, c.Translated(gp_Vec(n) * 0.05));
        for (double d = 0.1; clear && d < thickness - 0.05; d += std::max(0.1, thickness / 12)) {
          const gp_Pnt q = c.Translated(gp_Vec(n) * -d);
          if (solid_at(q, false)) clear = false, ownOnly = inside(own, q);
        }
        if (!clear) {
          ++(ownOnly ? blind : filled);
          continue;
        }
        const double perimeter = rimProps.Mass();
        out.push_back({{c.X(), c.Y(), c.Z()}, {n.X(), n.Y(), n.Z()}, area, perimeter > 0 ? 4 * area / perimeter : std::sqrt(area), thickness, own.id});
      }
    }
    return out;
  };
  int total = 0, examined = 0;  // for progress: every inner loop of the walls' flat faces
  for (const auto& shape : shapes)
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next())
      if (BRepAdaptor_Surface(TopoDS::Face(e.Current())).GetType() == GeomAbs_Plane)
        for (TopExp_Explorer w(e.Current(), TopAbs_WIRE); w.More(); w.Next()) ++total;
  std::vector<Hole> vents;  // the box's: each inner loop of a face that looks out of the box (a through hole's inside one looks in)
  for (size_t k = 0; k < shapes.size(); ++k) {
    const auto found = holes_of(k, [&](const gp_Pnt& c, const gp_Dir& n) { return n.X() * (c.X() - mid[0]) + n.Y() * (c.Y() - mid[1]) + n.Z() * (c.Z() - mid[2]) > 0; },
                                true, &examined, total);
    vents.insert(vents.end(), found.begin(), found.end());
  }

  // The fans: their curves, their openings and which way the air goes through each. A fan's opening that lies right behind
  // holes (in its outline, facing the same way, within a few millimetres) draws or blows through those holes alone: they are
  // its own path, in series with it, whether they are in the box's wall (a perforated panel in front of a blower's intake)
  // or in another part covering the opening (a grille). One hole about as large as the opening is the cut-out the fan is
  // mounted in: no restriction. Otherwise the fan draws from (or blows into) the box's air.
  struct Path {
    std::vector<size_t> holes;  // into `vents`
    bool direct = false;        // through holes of its own (in series with it), not straight from (or into) the box's air
    bool room = true;           // those holes lead to the room (the box's wall), not to the box's air (a part inside it)
    bool mount = false;         // through the fan's own cut-out: no restriction
    std::string part;           // a part covering the opening, whose holes these are
    P dir{0, 0, 0};             // the way out of the fan on this side (towards the room)
    double gap = 0, leak = 0;   // the gap between the opening and what its holes are in (mm), and the way round through it
                                // to the box's air (mm2: the opening's outline times the gap), in parallel with the holes
  };
  struct F {
    air::Fan fan;
    std::string name;
    P way{0, 0, 0}, at{0, 0, 0};
    std::optional<P> draws;  // a blower's inlet: its vents are those nearer it than its outlet
    Path in, out;
    double Q = 0, dp = 0;
  };
  std::vector<F> list;
  json notes = json::array();
  double free_flow = 0;
  constexpr double kFront = 10;  // mm: how far in front of an opening its holes may be
  // Whether a hole (centre c, normal n) lies in front of an opening along `dir`, inside its outline (1 mm to spare).
  auto in_front = [&](const FanProfile& o, const P& dir, const P& c, const P& n, double far) {
    if (std::fabs(dot3(n, dir)) < 0.9) return false;
    const P d = minus(c, o.centre);
    const double along = dot3(d, dir);
    if (along < -1 || along > far) return false;
    const P u = unit(std::fabs(dir[0]) < 0.9 ? P{0, dir[2], -dir[1]} : P{-dir[2], 0, dir[0]});
    const P v{dir[1] * u[2] - dir[2] * u[1], dir[2] * u[0] - dir[0] * u[2], dir[0] * u[1] - dir[1] * u[0]};
    if (o.loops.empty()) return std::sqrt(std::max(0.0, dot3(d, d) - along * along)) < std::sqrt(o.area / kPi) + 1;
    const double px = dot3(d, u), py = dot3(d, v);
    bool in = false;
    double nearest = 1e300;
    const auto& loop = o.loops.front();
    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
      const double xi = dot3(minus(loop[i], o.centre), u), yi = dot3(minus(loop[i], o.centre), v);
      const double xj = dot3(minus(loop[j], o.centre), u), yj = dot3(minus(loop[j], o.centre), v);
      if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
      const double ex = xj - xi, ey = yj - yi, l2 = ex * ex + ey * ey;
      const double t = l2 > 0 ? std::clamp(((px - xi) * ex + (py - yi) * ey) / l2, 0.0, 1.0) : 0;
      nearest = std::min(nearest, std::hypot(px - xi - t * ex, py - yi - t * ey));
    }
    return in || nearest < 1;
  };
  for (const json& d : fans) {
    F f;
    f.fan = air::fan_from(d.value("fan", json("80x25")));
    f.name = d.value("name", "Fan " + std::to_string(list.size() + 1));
    const bool blower = d.value("type", std::string("axial")) == "blower";
    std::optional<FanProfile> outlet, inlet;
    try {
      if (d.contains("outlet") && (d["outlet"].is_string() || !d["outlet"].contains("centre"))) outlet = fan_profile(doc, scene, d["outlet"]);
      if (blower && d.contains("inlet")) inlet = fan_profile(doc, scene, d["inlet"]);
    } catch (const std::exception&) {
      outlet.reset(), inlet.reset();
    }
    // Each side's opening and the way out of the fan there: a blower's outlet along its way (out of its side, away from its
    // inlet), its inlet out of its housing; an axial fan's one opening, along its way downstream and against it upstream.
    std::vector<std::tuple<Path*, const FanProfile*, P>> sides;
    if (outlet) {
      f.at = outlet->centre;
      if (blower && inlet) {
        f.way = unit(outlet->normal);
        if (dot3(f.way, minus(outlet->centre, inlet->centre)) < 0) f.way = neg(f.way);
        if (d.value("flip_outlet", false)) f.way = neg(f.way);
        P in_dir = unit(inlet->normal);
        if (dot3(in_dir, minus(inlet->centre, outlet->centre)) < 0) in_dir = neg(in_dir);
        if (d.value("flip_inlet", false)) in_dir = neg(in_dir);
        sides = {{&f.out, &*outlet, f.way}, {&f.in, &*inlet, in_dir}};
        f.draws = inlet->centre;
      } else {
        f.way = unit(d.contains("vector") ? d["vector"].get<P>() : outlet->normal);
        sides = {{&f.out, &*outlet, f.way}, {&f.in, &*outlet, neg(f.way)}};
      }
    } else {  // an older fan: its body's middle, its vector; no opening to find holes in front of
      Bnd_Box b;
      for (const auto& r : d.value("refs", json::array()))
        if (const std::string id = r.is_string() ? r.get<std::string>() : r.value("body", std::string()); scene.node(id)) BRepBndLib::Add(node_world_shape(doc, scene, id), b);
      if (b.IsVoid()) continue;
      double a0, b0, c0, a1, b1, c1;
      b.Get(a0, b0, c0, a1, b1, c1);
      f.at = {0.5 * (a0 + a1), 0.5 * (b0 + b1), 0.5 * (c0 + c1)};
      f.way = unit(d.value("vector", P{1, 0, 0}));
    }
    const size_t me = list.size();
    for (auto& [path, o, dir] : sides) {
      path->dir = dir;
      // The box's holes in front of it.
      for (size_t i = 0; i < vents.size(); ++i)
        if (vents[i].fan < 0 && in_front(*o, dir, vents[i].centre, vents[i].normal, kFront)) path->holes.push_back(i);
      if (path->holes.empty()) {
        // A part covering it: rays from its middle and around its outline (two rings), out along its way: most meet the same
        // part within 15 mm before any wall (the others pass through its holes).
        std::vector<gp_Pnt> starts{gp_Pnt(o->centre[0], o->centre[1], o->centre[2])};
        if (!o->loops.empty())
          for (double ring : {0.4, 0.8})
            for (size_t k = 0; k < o->loops.front().size(); k += std::max<size_t>(1, o->loops.front().size() / 8)) {
              const P& q = o->loops.front()[k];
              starts.push_back(gp_Pnt(o->centre[0] + ring * (q[0] - o->centre[0]), o->centre[1] + ring * (q[1] - o->centre[1]), o->centre[2] + ring * (q[2] - o->centre[2])));
            }
        std::map<std::string, int> met;
        for (const gp_Pnt& st : starts) {
          const gp_Lin ray(st.Translated(gp_Vec(dir[0], dir[1], dir[2]) * 0.3), gp_Dir(dir[0], dir[1], dir[2]));
          std::string hit;
          double best = 15;
          for (auto& b : blockers) {
            if (b.box.IsOut(ray)) continue;
            IntCurvesFace_ShapeIntersector x;
            x.Load(shape_of(b), 1e-6);
            x.Perform(ray, 0, best);
            if (x.IsDone() && x.NbPnt() > 0) {
              double first = 1e300;
              for (int k = 1; k <= x.NbPnt(); ++k) first = std::min(first, x.WParameter(k));
              if (first < best) best = first, hit = b.wall ? std::string() : b.id;
            }
          }
          if (!hit.empty()) ++met[hit];
        }
        std::string part;
        int most = 0;
        for (const auto& [id, n] : met)
          if (n > most) most = n, part = id;
        const bool covers = 2 * most >= int(starts.size());
        if (covers && !part.empty()) {
          const auto at = std::find_if(blockers.begin(), blockers.end(), [&](const Blocker& b) { return b.id == part; });
          const auto found = holes_of(size_t(at - blockers.begin()),
                                      [&](const gp_Pnt& c, const gp_Dir& n) { return in_front(*o, dir, {c.X(), c.Y(), c.Z()}, {n.X(), n.Y(), n.Z()}, 30); }, false,
                                      nullptr, 0);
          const opad::Node* pn = scene.node(part);
          if (found.empty()) {
            notes.push_back("\"" + f.name + "\": \"" + (pn ? pn->name : part) + "\" covers its opening with no hole in front of it: the air is taken to go round it");
          } else {
            // Each through hole of the part listed once (a hole has a loop on each side of the part: the one facing the fan
            // and the one facing away; keep the one facing the way out).
            for (const Hole& h : found)
              if (dot3(h.normal, dir) > 0) {
                vents.push_back(h);
                path->holes.push_back(vents.size() - 1);
              }
            path->part = part;
            path->room = false;  // a part in the box: its holes lead to the box's air
            if (!path->holes.empty())
              notes.push_back("\"" + f.name + "\": its air goes through \"" + (pn ? pn->name : part) + "\" in front of its opening (taken to seal against it)");
          }
        }
      }
      for (size_t i : path->holes) {
        vents[i].fan = int(me), vents[i].exit = path == &f.out;
        if (vents[i].area >= 0.5 * o->area) path->mount = true;  // the cut-out it is mounted in
      }
      path->direct = !path->holes.empty();
      // Not sealed against the panel or the part: the gap between them (their distance less the panel's thickness) lets the
      // box's air round to the opening too; through the opening's outline times the gap, in parallel with the holes. (A
      // blower 2 mm under a perforated lid drew most of its air that way in the run: the holes alone said a third of it.)
      if (path->direct && !path->mount) {
        std::vector<double> gaps;
        for (size_t i : path->holes) gaps.push_back(dot3(minus(vents[i].centre, o->centre), dir) - vents[i].thickness);
        std::nth_element(gaps.begin(), gaps.begin() + long(gaps.size() / 2), gaps.end());
        path->gap = std::max(0.0, gaps[gaps.size() / 2]);
        double perimeter = 0;
        if (!o->loops.empty())
          for (size_t k = 0; k < o->loops.front().size(); ++k) {
            const P& a0 = o->loops.front()[k];
            const P& b0 = o->loops.front()[(k + 1) % o->loops.front().size()];
            perimeter += std::sqrt(dot3(minus(b0, a0), minus(b0, a0)));
          }
        else
          perimeter = 2 * std::sqrt(kPi * o->area);
        if (path->gap > 0.2) path->leak = perimeter * path->gap;
      }
    }
    const double q = f.fan.Qmax;
    free_flow += q;
    list.push_back(f);
  }
  if (list.empty()) throw Error("no fan to estimate the vents for");
  // The box's other holes: intakes behind the fans' air, exhausts ahead of it; all exhausts when no fan draws from the box's
  // air (each takes the room's through holes of its own), all intakes when none blows into it.
  bool fromBox = false, intoBox = false;
  for (const F& f : list)
    fromBox = fromBox || !f.in.direct || !f.in.room || f.in.leak > 0, intoBox = intoBox || !f.out.direct || !f.out.room || f.out.leak > 0;
  std::vector<size_t> intakes, exhausts;
  for (size_t i = 0; i < vents.size(); ++i)
    if (vents[i].fan < 0) {
      // Each fan's say, by its free flow: a blower's by which of its openings the hole is nearer (its outlet may point up
      // while the box's air goes along), an axial fan's by the side of it the hole is on.
      double vote = 0;
      for (const F& f : list) {
        const P& c = vents[i].centre;
        const double ahead = f.draws ? std::sqrt(dot3(minus(c, *f.draws), minus(c, *f.draws))) - std::sqrt(dot3(minus(c, f.at), minus(c, f.at)))
                                     : dot3(minus(c, f.at), f.way);
        vote += (ahead > 0 ? 1 : -1) * f.fan.Qmax;
      }
      vents[i].exit = fromBox && intoBox ? vote > 0 : intoBox;
      (vents[i].exit ? exhausts : intakes).push_back(i);
    }

  // Each hole: the orifice's loss (discharge 0.6) and, for a hole long against its width, the laminar friction along it:
  // dp = a Q^2 + b Q (Q in m3/s through that hole). Holes of one size grouped: a panel of 200 alike is one group.
  const air::Air a = air::properties(ambient_C);
  constexpr double kCd = 0.6;
  struct Group {
    double a, b;
    int n;
  };
  auto groups = [&](const std::vector<size_t>& holes, double scale) {  // scale: a share of each hole's area (the "half each" split)
    std::map<std::tuple<long, long, long>, Group> by;
    for (size_t i : holes) {
      const Hole& h = vents[i];
      const double A = h.area * 1e-6 * scale, D = h.width * 1e-3, L = h.thickness * 1e-3;
      const auto key = std::make_tuple(std::lround(h.area * 1e3), std::lround(h.width * 1e3), std::lround(h.thickness * 1e2));
      auto [it, made] = by.try_emplace(key, Group{a.rho / (2 * std::pow(kCd * A, 2)), 32 * a.mu * L / (A * D * D), 0});
      ++it->second.n;
    }
    std::vector<Group> out;
    for (const auto& [k, g] : by) out.push_back(g);
    return out;
  };
  auto flow_of = [](const std::vector<Group>& gs, double dp) {  // m3/s through them all at that pressure
    double q = 0;
    for (const auto& g : gs) q += g.n * (-g.b + std::sqrt(g.b * g.b + 4 * g.a * dp)) / (2 * g.a);
    return q;
  };
  auto drop_of = [&](const std::vector<Group>& gs, double Q) {  // the pressure that drives Q through them
    if (Q <= 0 || gs.empty()) return gs.empty() && Q > 0 ? 1e12 : 0.0;
    double lo = 0, hi = 1;
    while (flow_of(gs, hi) < Q && hi < 1e12) hi *= 4;
    for (int i = 0; i < 60; ++i) {
      const double m = 0.5 * (lo + hi);
      (flow_of(gs, m) < Q ? lo : hi) = m;
    }
    return 0.5 * (lo + hi);
  };
  // No general vent on one side of the fans that need it: the air comes in and goes out through the same vents, half each.
  double inScale = 1, outScale = 1;
  if (fromBox && intoBox && (intakes.empty() != exhausts.empty())) {
    notes.push_back(std::string("no ") + (intakes.empty() ? "intake" : "exhaust") +
                    " vents found on that side of the fans: the air comes in and goes out through the same vents, half each");
    if (intakes.empty()) intakes = exhausts;
    else exhausts = intakes;
    inScale = outScale = 0.5;
  }
  const std::vector<Group> gin = groups(intakes, inScale), gout = groups(exhausts, outScale);
  std::vector<std::vector<Group>> fin, fout, lin, lout;  // each fan's own holes, and the way round them (the gap) on each side
  auto leak = [&](double mm2) { return mm2 > 0 ? std::vector<Group>{{a.rho / (2 * std::pow(kCd * mm2 * 1e-6, 2)), 0.0, 1}} : std::vector<Group>{}; };
  for (const F& f : list) {
    fin.push_back(f.in.mount ? std::vector<Group>{} : groups(f.in.holes, 1)), fout.push_back(f.out.mount ? std::vector<Group>{} : groups(f.out.holes, 1));
    lin.push_back(leak(f.in.leak)), lout.push_back(leak(f.out.leak));
  }
  // The network: the box's air in two parts, before the fans (pU, fed by the intakes) and after them (pD, let out by the
  // exhausts); each fan between where it draws (pU, or the room through its own path) and where it blows (pD, or the room
  // through its own). For given pU and pD each fan's flow is where its curve meets that; pU and pD are where the box's air
  // balances (in = out), found in turns.
  double Pmax = 0;
  for (const F& f : list) Pmax = std::max(Pmax, f.fan.pressure(0));
  // The pressure at one side of a fan carrying Q, given its zone's (pU before the fans, pD after), and the part of Q that
  // side takes from (or gives to) the box's air: through the box's air alone; its cut-out; its own holes (to the room, or to
  // the box's air for a part inside it); or its holes and the gap round them together (the pressure where both carry Q).
  auto side_at = [&](const Path& p, const std::vector<Group>& holes, const std::vector<Group>& gap, double zone, double Q, bool intake, double* boxShare) {
    const double sgn = intake ? -1 : 1;  // the side's pressure is below its sources (intake) or above its sinks (outlet)
    if (!p.direct) return *boxShare = Q, zone;
    if (p.mount) return *boxShare = p.room ? 0.0 : Q, p.room ? 0.0 : zone;
    if (!p.room) {  // a part's holes and the gap, both to the box's air
      std::vector<Group> both = holes;
      both.insert(both.end(), gap.begin(), gap.end());
      return *boxShare = Q, zone + sgn * drop_of(both, Q);
    }
    if (gap.empty()) return *boxShare = 0.0, sgn * drop_of(holes, Q);
    // The room's through the holes, the box's through the gap: p where they carry Q together.
    const double edge = intake ? std::min(0.0, zone) : std::max(0.0, zone);
    double near = edge, far = edge + sgn * std::max(drop_of(holes, Q), drop_of(gap, Q));
    for (int k = 0; k < 50; ++k) {
      const double m = 0.5 * (near + far);
      const double carried = flow_of(holes, sgn * (m - 0.0)) + flow_of(gap, sgn * (m - zone));
      (carried < Q ? near : far) = m;
    }
    const double at = 0.5 * (near + far);
    *boxShare = flow_of(gap, std::max(0.0, sgn * (at - zone)));
    return at;
  };
  // A fan's flow for given pU and pD: where its curve meets what its two sides ask; with the part each side takes from the
  // box's air (inShare: drawn from before the fans, outShare: blown into after them).
  auto fan_flow = [&](size_t i, double pU, double pD, double* dp, double* inShare = nullptr, double* outShare = nullptr) {
    const F& f = list[i];
    double si = 0, so = 0;
    auto rise = [&](double Q) {  // what the fan must make at Q
      return side_at(f.out, fout[i], lout[i], pD, Q, false, &so) - side_at(f.in, fin[i], lin[i], pU, Q, true, &si);
    };
    double lo = 0, hi = f.fan.Qmax, Q = 0;
    if (f.fan.pressure(0) > rise(0)) {
      for (int k = 0; k < 50; ++k) {
        const double m = 0.5 * (lo + hi);
        (f.fan.pressure(m) > rise(m) ? lo : hi) = m;
      }
      Q = 0.5 * (lo + hi);
    }
    const double r = rise(Q);
    if (dp) *dp = r;
    if (inShare) *inShare = si;
    if (outShare) *outShare = so;
    return Q;
  };
  double pU = 0, pD = 0;
  for (int turn = 0; turn < 16; ++turn) {
    if (fromBox) {  // in through the intakes = what the fans drawing from the box take
      double lo = -Pmax, hi = 0;
      for (int k = 0; k < 40; ++k) {
        const double m = 0.5 * (lo + hi);
        double drawn = 0;
        for (size_t i = 0; i < list.size(); ++i) {
          double share = 0;
          fan_flow(i, m, pD, nullptr, &share, nullptr);
          drawn += share;
        }
        (flow_of(gin, -m) > drawn ? lo : hi) = m;
      }
      pU = 0.5 * (lo + hi);
    }
    if (intoBox) {  // out through the exhausts = what the fans blowing into the box give
      double lo = 0, hi = Pmax;
      for (int k = 0; k < 40; ++k) {
        const double m = 0.5 * (lo + hi);
        double blown = 0;
        for (size_t i = 0; i < list.size(); ++i) {
          double share = 0;
          fan_flow(i, pU, m, nullptr, nullptr, &share);
          blown += share;
        }
        (blown > flow_of(gout, m) ? lo : hi) = m;
      }
      pD = 0.5 * (lo + hi);
    }
  }
  double Q = 0;
  std::vector<double> inBox(list.size(), 0.0), outBox(list.size(), 0.0);
  for (size_t i = 0; i < list.size(); ++i) list[i].Q = fan_flow(i, pU, pD, &list[i].dp, &inBox[i], &outBox[i]), Q += list[i].Q;
  const bool sealed = intakes.empty() && exhausts.empty() &&
                      std::none_of(list.begin(), list.end(), [](const F& f) { return (f.in.direct && f.in.room) || (f.out.direct && f.out.room); });
  if (sealed) {
    Q = 0;
    for (auto& f : list) f.Q = 0;
    notes.push_back("no vents found: no air goes through the box, the fans only stir the air inside");
  }
  // What holds them back most: the largest pressure drop on their way.
  std::string by = sealed ? "vents" : Q >= 0.9 * free_flow ? "fans" : "";
  json limiting;
  if (by.empty()) {
    double worst = -1;
    auto consider = [&](double drop, const std::string& what, const json& detail) {
      if (drop > worst) worst = drop, by = what, limiting = detail;
    };
    if (fromBox) consider(-pU, "intake", json());
    if (intoBox) consider(pD, "exhaust", json());
    for (size_t i = 0; i < list.size(); ++i) {
      double share = 0;  // across its own holes: from where they start to the fan's side
      if (list[i].in.direct && !list[i].in.mount)
        consider((list[i].in.room ? 0.0 : pU) - side_at(list[i].in, fin[i], lin[i], pU, list[i].Q, true, &share), "fan_intake", {{"fan", list[i].name}, {"side", "intake"}});
      if (list[i].out.direct && !list[i].out.mount)
        consider(side_at(list[i].out, fout[i], lout[i], pD, list[i].Q, false, &share) - (list[i].out.room ? 0.0 : pD), "fan_outlet", {{"fan", list[i].name}, {"side", "outlet"}});
    }
  }
  auto area_of = [&](const std::vector<size_t>& holes) {
    double s = 0;
    for (size_t i : holes) s += vents[i].area;
    return s;
  };
  auto side = [&](const std::vector<size_t>& holes, double scale, double drop) {
    return json{{"area_mm2", area_of(holes) * scale}, {"openings", int(holes.size())}, {"pressure_Pa", drop}};
  };
  auto name_of = [&](const std::string& id) {
    const opad::Node* n = scene.node(id);
    return n ? n->name : id;
  };
  constexpr double kCfmM3s = 4.719474e-4;
  json per_fan = json::array();
  for (size_t i = 0; i < list.size(); ++i) {
    const F& f = list[i];
    auto path = [&](const Path& p, const std::vector<Group>& gs, const std::vector<Group>& gap, double zone, bool exit) {
      if (!p.direct) return json{{"through", "box"}};
      double share = 0;
      const double at = side_at(p, gs, gap, zone, f.Q, !exit, &share);
      const double across = std::fabs(at - (p.room ? 0.0 : zone));
      json j = {{"through", p.mount ? "mount" : "holes"}, {"openings", int(p.holes.size())}, {"area_mm2", area_of(p.holes)},
                {"pressure_Pa", p.mount ? 0.0 : across}, {"body", name_of(vents[p.holes.front()].body)}};
      if (!p.part.empty()) j["part"] = name_of(p.part);
      if (p.leak > 0) {  // the way round through the gap, and what it carries (of the box's air)
        j["gap_mm"] = p.gap, j["round_mm2"] = p.leak;
        if (p.room) j["round_m3h"] = share * 3600, j["holes_m3h"] = (f.Q - share) * 3600;
      }
      return j;
    };
    per_fan.push_back({{"name", f.name}, {"flow_m3h", f.Q * 3600}, {"flow_cfm", f.Q / kCfmM3s}, {"free_flow_cfm", f.fan.Qmax / kCfmM3s},
                       {"pressure_Pa", f.dp}, {"intake", path(f.in, fin[i], lin[i], pU, false)}, {"outlet", path(f.out, fout[i], lout[i], pD, true)}});
  }
  json out_j = {{"intake", side(intakes, inScale, fromBox ? -pU : 0.0)},
                {"exhaust", side(exhausts, outScale, intoBox ? pD : 0.0)},
                {"fans", per_fan},
                {"free_flow_m3h", free_flow * 3600},
                {"free_flow_cfm", free_flow / kCfmM3s},
                {"flow_m3h", Q * 3600},
                {"flow_cfm", Q / kCfmM3s},
                {"fraction", free_flow > 0 ? Q / free_flow : 0.0},
                {"pressure_Pa", pD - pU},
                {"limited_by", by}};
  if (!limiting.is_null()) out_j["limiting"] = limiting;
  // Compatibility with the one-path summary: a side all of whose fans go through their own cut-outs is the fans'.
  if (std::all_of(list.begin(), list.end(), [](const F& f) { return f.in.mount; })) out_j["intake"]["fan"] = true;
  if (std::all_of(list.begin(), list.end(), [](const F& f) { return f.out.mount; })) out_j["exhaust"]["fan"] = true;
  json listed = json::array();  // the openings found, largest first (what the counts are made of)
  std::vector<const Hole*> order;
  for (const auto& v : vents) order.push_back(&v);
  std::sort(order.begin(), order.end(), [](const Hole* a, const Hole* b) { return a->area > b->area; });
  for (const Hole* v : order) {
    json o = {{"centre", v->centre}, {"normal", v->normal}, {"area_mm2", v->area}, {"width_mm", v->width}, {"thickness_mm", v->thickness}, {"body", name_of(v->body)}};
    if (v->fan >= 0) {
      const F& f = list[size_t(v->fan)];
      const Path& p = v->exit ? f.out : f.in;
      o["side"] = p.mount ? "fan" : v->exit ? "fan_outlet" : "fan_intake";
      o["fan"] = f.name;
    } else {
      o["side"] = v->exit ? "exhaust" : "intake";
    }
    listed.push_back(o);
  }
  out_j["openings"] = listed;
  out_j["left_out"] = {{"blind", blind}, {"filled", filled}};
  if (watts > 0) {
    const double need = watts / (a.rho * a.cp * 10);  // a 10 K rise of the air
    out_j["needed_m3h"] = need * 3600;
    out_j["needed_cfm"] = need / kCfmM3s;
    if (Q > 0) out_j["rise_K"] = watts / (a.rho * a.cp * Q);
  }
  if (!notes.empty()) out_j["notes"] = notes;
  return out_j;
}

namespace {
using Box = BodyBoxes::Box;
constexpr Box kEmpty{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};

void grow(Box& o, const Box& c) {
  for (int k = 0; k < 3; ++k) o[k] = std::min(o[k], c[k]), o[k + 3] = std::max(o[k + 3], c[k + 3]);
}
bool holds(const Box& o, const Box& i, double tol) {
  for (int k = 0; k < 3; ++k)
    if (i[k] < o[k] - tol || i[k + 3] > o[k + 3] + tol) return false;
  return true;
}
double extent(const Box& b) {
  double e = 0;
  for (int k = 0; k < 3; ++k) e += (b[k + 3] - b[k]) * (b[k + 3] - b[k]);
  return e;
}
bool contains(const std::vector<std::string>& v, const std::string& id) { return std::find(v.begin(), v.end(), id) != v.end(); }

// An enclosure of several bodies around `bodies` when no one body holds them (a base and its lid, a frame and its panels):
// the smallest group, of two or three, whose boxes together hold them and that every other shown solid either lies within or
// stays clear of. That rule tells the box's halves from a heatsink and the board it sits on (the halves would stick through
// their box) and leaves out the desk it stands on (touching is clear). Empty when no group does.
std::vector<std::string> enclosure_group(const BodyBoxes& all, const std::vector<std::string>& bodies) {
  Box want = kEmpty;
  for (const auto& b : bodies)
    if (const auto it = all.at.find(b); it != all.at.end()) grow(want, all.boxes[it->second]);
  if (want[0] > want[3]) return {};
  double size = 0;
  for (const auto& c : all.boxes)
    for (int k = 0; k < 3; ++k) size = std::max(size, c[k + 3] - c[k]);
  const double tol = 1e-4 * std::max(size, 1.0);
  auto overlaps = [&](const Box& x, const Box& y) {  // by more than a touch
    for (int k = 0; k < 3; ++k)
      if (std::min(x[k + 3], y[k + 3]) - std::max(x[k], y[k]) <= tol) return false;
    return true;
  };
  // Walls reach out past the parts (a body within the parts' own box is one of them, a chip or a connector).
  std::vector<size_t> walls;
  for (size_t i = 0; i < all.ids.size(); ++i)
    if (!contains(bodies, all.ids[i]) && !holds(want, all.boxes[i], tol)) walls.push_back(i);
  std::vector<size_t> best;
  double bestExtent = 1e300;
  auto consider = [&](const std::vector<size_t>& group) {
    Box u = kEmpty;
    for (size_t g : group) grow(u, all.boxes[g]);
    if (!holds(u, want, tol)) return;
    const double e = extent(u);
    if (e >= bestExtent) return;
    for (size_t i = 0; i < all.ids.size(); ++i)
      if (std::find(group.begin(), group.end(), i) == group.end() && !holds(u, all.boxes[i], tol) && overlaps(u, all.boxes[i])) return;
    // Each one a wall: none inside the others' box (that one is a part in the box, not a side of it).
    for (size_t g : group) {
      Box rest = kEmpty;
      for (size_t h : group)
        if (h != g) grow(rest, all.boxes[h]);
      if (holds(rest, all.boxes[g], tol)) return;
    }
    best = group, bestExtent = e;
  };
  const size_t n = walls.size();
  for (size_t a = 0; a < n; ++a)
    for (size_t b = a + 1; b < n; ++b) consider({walls[a], walls[b]});
  if (best.empty() && n <= 40)
    for (size_t a = 0; a < n; ++a)
      for (size_t b = a + 1; b < n; ++b)
        for (size_t c = b + 1; c < n; ++c) consider({walls[a], walls[b], walls[c]});
  std::vector<std::string> out;
  for (size_t g : best) out.push_back(all.ids[g]);
  return out;
}

// The bodies that close a box found by itself: a lid on a base, a panel on a frame. Each touches or overlaps the walls' box,
// is not within it, and lies within its outline across at least two axes (a desk under the box is wider than it). Grows
// until none is left; `bodies` (the parts the loads name) never are walls.
std::vector<std::string> closing_walls(const BodyBoxes& all, std::vector<std::string> walls, const std::vector<std::string>& bodies) {
  std::vector<size_t> others;
  for (size_t i = 0; i < all.ids.size(); ++i)
    if (!contains(walls, all.ids[i]) && !contains(bodies, all.ids[i])) others.push_back(i);
  Box u = kEmpty;
  for (const auto& w : walls)
    if (const auto it = all.at.find(w); it != all.at.end()) grow(u, all.boxes[it->second]);
  if (u[0] > u[3]) return walls;
  const double tol = 1e-4 * std::max({u[3] - u[0], u[4] - u[1], u[5] - u[2], 1.0});
  for (;;) {
    // What caps the box as it is ...
    std::vector<size_t> caps;
    for (size_t i : others) {
      const Box& c = all.boxes[i];
      bool touches = true;
      int within = 0;
      for (int k = 0; k < 3; ++k) {
        touches = touches && c[k] <= u[k + 3] + tol && c[k + 3] >= u[k] - tol;
        within += c[k] >= u[k] - tol && c[k + 3] <= u[k + 3] + tol;
      }
      if (touches && within >= 2 && !holds(u, c, tol)) caps.push_back(i);
    }
    // ... but not what lies inside the box with the other caps on it (a heatsink standing up past a base's rim, under the lid).
    std::vector<size_t> now;
    for (size_t i : caps) {
      Box rest = u;
      for (size_t j : caps)
        if (j != i) grow(rest, all.boxes[j]);
      if (!holds(rest, all.boxes[i], tol)) now.push_back(i);
    }
    if (now.empty()) break;
    for (size_t i : now) {
      walls.push_back(all.ids[i]);
      grow(u, all.boxes[i]);
      others.erase(std::find(others.begin(), others.end(), i));
    }
  }
  return walls;
}
}  // namespace

Enclosure find_enclosure(const BodyBoxes& all, const std::vector<std::string>& bodies, const std::vector<std::string>& named) {
  Enclosure out;
  std::vector<std::string> walls;
  if (!named.empty()) {
    for (const auto& id : named) {
      if (!all.at.count(id)) throw Error("cfd.enclosure: " + id + " is not a shown solid body");
      if (!contains(walls, id)) walls.push_back(id);
    }
  } else if (!bodies.empty()) {
    // The smallest body holding them, or a group of them; then whatever closes it (a lid on a base) is a wall too.
    Box want = kEmpty;
    for (const auto& b : bodies)
      if (const auto it = all.at.find(b); it != all.at.end()) grow(want, all.boxes[it->second]);
    if (want[0] > want[3]) return out;
    std::string single;
    double best = 1e300;
    for (size_t i = 0; i < all.ids.size(); ++i) {
      if (contains(bodies, all.ids[i])) continue;
      const double e = extent(all.boxes[i]);
      if (holds(all.boxes[i], want, 1e-6 * std::sqrt(e)) && e < best) best = e, single = all.ids[i];
    }
    const std::vector<std::string> found = single.empty() ? enclosure_group(all, bodies) : std::vector<std::string>{single};
    if (found.empty()) return out;
    walls = closing_walls(all, found, bodies);
  }
  if (walls.empty()) return out;
  out.walls = walls;
  out.enclosure = walls.front();
  Box e = kEmpty;
  for (const auto& w : walls) grow(e, all.boxes[all.at.at(w)]);
  const double tol = 1e-6 * std::sqrt(extent(e));
  for (size_t i = 0; i < all.ids.size(); ++i)
    if (!contains(walls, all.ids[i]) && holds(e, all.boxes[i], tol)) out.inside.push_back(all.ids[i]);
  return out;
}

std::vector<std::string> likely_enclosure(const BodyBoxes& all) {
  // The body holding the most others, and what closes it (its lid, its other half) when the parts it holds say so.
  size_t most = 0;
  std::string holder;
  for (size_t i = 0; i < all.ids.size(); ++i) {
    const double tol = 1e-6 * std::sqrt(extent(all.boxes[i]));
    size_t n = 0;
    for (size_t j = 0; j < all.ids.size(); ++j) n += j != i && holds(all.boxes[i], all.boxes[j], tol);
    if (n > most) most = n, holder = all.ids[i];
  }
  if (holder.empty()) return {};
  const std::vector<std::string> inside = find_enclosure(all, {}, {holder}).inside;
  if (inside.empty()) return {holder};
  const Enclosure found = find_enclosure(all, inside, {});
  return found.enclosure == holder ? found.walls : std::vector<std::string>{holder};
}

Enclosure find_enclosure(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, const std::string& named) {
  return find_enclosure(body_boxes(doc, scene), bodies, named.empty() ? std::vector<std::string>() : std::vector<std::string>{named});
}

Enclosure find_enclosure(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, const std::vector<std::string>& named) {
  if (named.empty()) return {};  // as named: nothing named, nothing found
  return find_enclosure(body_boxes(doc, scene), bodies, named);
}

std::string enclosure_name(const Scene& scene, const std::vector<std::string>& walls) {
  std::string out;
  for (const auto& id : walls)
    if (const Node* n = scene.node(id)) out += (out.empty() ? "" : " + ") + n->name;
  return out;
}

StudyRun run_cfd(const Document& doc, const Scene& scene, const json& st, const Progress& progress) {
  const auto t_start = std::chrono::steady_clock::now();
  auto seconds_since = [](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); };
  json timing = json::object();
  StudyRun run;
  run.kind = "thermal";
  auto report = [&](double f, const std::string& phase) {
    if (progress && !progress(f, phase)) throw Error("cancelled");
  };
  const OpenFoam foam = openfoam();
  if (!foam.found())
    throw Error("the CFD air needs OpenFOAM: install it (Ubuntu: apt install openfoam; or OpenCFD's packages) or set OPAD_OPENFOAM to its directory");
  // The parts' heat is solved in CalculiX on a Netgen mesh: said now, not after the air has taken its minutes.
  if (const json e = engines(); !e.value("thermal", false)) throw Error(e.value("note", std::string("thermal studies need Netgen and CalculiX")));
  const double ambient = st.value("ambient", 25.0);
  const double T0 = ambient + 273.15;
  // quality: quick (screening a sweep), normal, fine: the box's cells, the flow's iterations, how settled the heat must be;
  // each given on its own wins.
  json cfd = st.value("cfd", json::object());
  {
    const std::string q = cfd.value("quality", std::string("normal"));
    if (q != "quick" && q != "normal" && q != "fine") throw Error("cfd.quality is quick, normal or fine");
    const json preset = q == "quick" ? json{{"divisions", 20}, {"flow_iterations", 400}, {"settle", 0.3}}
                        : q == "fine" ? json{{"divisions", 45}, {"flow_iterations", 1500}, {"settle", 0.05}}
                                      : json{{"divisions", 30}, {"flow_iterations", 1000}, {"settle", 0.1}};
    for (const auto& [k, v] : preset.items())
      if (!cfd.contains(k)) cfd[k] = v;
  }

  // How the air carries heat between cells: upwind (first order, bounded) unless asked (e.g. "limitedLinear 1").
  const std::string heat_scheme = cfd.value("heat_scheme", std::string("upwind"));
  for (char c : heat_scheme)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '.') throw Error("cfd.heat_scheme: an OpenFOAM scheme such as upwind or \"limitedLinear 1\"");

  // ---- the case's thermal loads: heat sources in bodies; fans (one at a duct's inlet, any number placed in an enclosure) or
  // one forced stream
  std::string load_case = st.value("case", std::string());
  if (load_case.empty())
    for (const auto& l : scene.loads)
      if (l.kind == "heat") {
        load_case = l.load_case;
        break;
      }
  std::vector<const Load*> heats, fan_loads, held;  // held: fixed temperatures
  const Load* stream = nullptr;
  for (const auto& l : scene.loads) {
    if (l.load_case != load_case) continue;
    if (!l.error.empty()) throw Error("load \"" + l.name + "\": " + l.error);
    if (l.kind == "heat") heats.push_back(&l);
    else if (l.kind == "temperature") held.push_back(&l);
    else if (l.kind == "fan") fan_loads.push_back(&l);
    else if (l.kind == "convection" && l.def.value("h", json()) == "forced") stream = &l;
    else if (l.kind == "convection" || l.kind == "radiation")
      run.warnings.push_back("\"" + l.name + "\" is left out: with the CFD air, the parts lose heat only to the air the fan or the stream moves");
  }
  if (heats.empty() && held.empty()) throw Error("load case \"" + load_case + "\" has no heat source: add one (load kind heat, W, on a body)");
  std::vector<std::string> bodies;
  auto add_body = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!n) throw Error("no body " + id);
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
      if (std::find(bodies.begin(), bodies.end(), b) == bodies.end()) bodies.push_back(b);
  };
  for (const auto& b : st.value("bodies", json::array())) add_body(b.get<std::string>());
  const bool listed = !bodies.empty();
  if (!listed) {
    for (const Load* l : heats)
      for (const auto& r : l->refs) add_body(r.body);
    for (const Load* l : held)
      for (const auto& r : l->refs) add_body(r.body);
    for (const Load* l : fan_loads) {
      for (const auto& r : l->refs) add_body(r.body);
      for (const auto& h : l->def.value("heatsink", json::array()))
        if (h.is_string()) add_body(h.get<std::string>());
    }
    if (stream)
      for (const auto& r : stream->refs) add_body(r.body);
  }

  // An enclosure (cfd.enclosure, or the smallest shown body whose box holds every body the loads name): then the air inside
  // it and a margin of the room around it are solved, its vents open to the room, its fans placed inside, and every shown
  // body within it (the board, connectors) takes part.
  // cfd.enclosure: a body, or a list of them (a base and its lid, a frame and its panels: its walls together).
  const json named = cfd.value("enclosure", json());
  std::string enclosure;
  std::vector<std::string> box_walls;
  if (named.is_string() || named.is_array() || (!listed && named != json(false))) {
    std::vector<std::string> list;
    if (named.is_array())
      for (const auto& v : named)
        if (v.is_string()) list.push_back(v.get<std::string>());
    const Enclosure found = named.is_array() ? find_enclosure(doc, scene, bodies, list)
                                             : find_enclosure(doc, scene, bodies, named.is_string() ? named.get<std::string>() : std::string());
    enclosure = found.enclosure;
    box_walls = found.walls;
    for (const auto& w : box_walls) add_body(w);
    if (!enclosure.empty() && !listed)
      for (const auto& id : found.inside) add_body(id);
  }
  auto is_wall = [&](const std::string& id) { return std::find(box_walls.begin(), box_walls.end(), id) != box_walls.end(); };
  // Before anything is meshed: whether the vents let the fans blow what they can (the summary's vents.estimate, beside the
  // flows the run finds).
  json vents_est;
  if (!enclosure.empty() && !fan_loads.empty()) {
    std::vector<json> defs;
    for (const Load* l : fan_loads) {
      defs.push_back(l->def);
      defs.back()["name"] = l->name;
    }
    double watts = 0;
    for (const Load* l : heats) watts += l->def.value("value", 0.0);
    try {
      vents_est = vent_estimate(doc, scene, box_walls, defs, watts, ambient);
    } catch (const std::exception&) {
    }
  }
  if (!enclosure.empty()) {
    if (fan_loads.empty() && !cfd.value("buoyancy", true) && !cfd.value("sealed", false))
      throw Error("the CFD air in an enclosure moves by fans (load kind fan) or by warm air rising (cfd.buoyancy): with neither it stands still "
                  "(only a sealed box's air may: cfd.sealed)");
    if (stream) run.warnings.push_back("\"" + stream->name + "\" is left out: in an enclosure the fans move the air");
    stream = nullptr;
  } else {
    if (fan_loads.size() > 1) throw Error("the CFD air in a duct takes one fan (a fan's count puts several side by side; in an enclosure, any number)");
    if (fan_loads.empty() && !stream)
      throw Error("the CFD air needs a fan, or a convection with h forced (its velocity and way): still air stays with the correlations");
  }
  // The parts' heat in CalculiX, the air meshed alone with thin cells along the parts' walls for its boundary layers
  // (cfd.parts "calculix", the default; an enclosure always); or, in a duct, "openfoam": the parts as zones of one
  // mesh, their heat in chtMultiRegionSimpleFoam (no layers there: a flat plate's film came out half as large again).
  const std::string parts_solver = cfd.value("parts", std::string("calculix"));
  if (parts_solver != "calculix" && parts_solver != "openfoam") throw Error("cfd.parts is calculix or openfoam");
  const bool air_only = !enclosure.empty() || parts_solver == "calculix";
  // In OpenFOAM the parts' heat is in whole bodies; CalculiX takes heat on faces and fixed temperatures too.
  if (!air_only)
    for (const Load* l : heats)
      for (const auto& r : l->refs)
        if (r.kind != Ref::Kind::Body) throw Error("heat \"" + l->name + "\": the CFD air in a duct takes heat sources in whole bodies, not on faces");
  if (!air_only && !held.empty()) {
    if (heats.empty()) throw Error("the CFD air in a duct takes heat sources (load kind heat); fixed temperatures only in an enclosure");
    for (const Load* l : held) run.warnings.push_back("\"" + l->name + "\" is left out: fixed temperatures are taken in an enclosure, not in a duct");
  }
  const Load* fan_load = enclosure.empty() && !fan_loads.empty() ? fan_loads.front() : nullptr;
  // The frame: along the duct's air; an enclosure's is the world's.
  const V way = !enclosure.empty() ? V{1, 0, 0} : unit((fan_load ? fan_load : stream)->def.value("vector", V{1, 0, 0}));
  if (norm(way) < 0.5) throw Error("the air's way (vector) has no length");

  // ---- the parts: world solids, triangulated for the mesher and for showing the results
  std::vector<TopoDS_Shape> world;
  Bnd_Box all;
  for (const auto& b : bodies) {
    const Node* n = scene.node(b);
    if (n->body_missing || n->representation != "solid") throw Error("\"" + n->name + "\" is not a solid");
    world.push_back(node_world_shape(doc, scene, b));
    BRepBndLib::Add(world.back(), all);
  }
  auto index_of = [&](const std::string& id) { return size_t(std::find(bodies.begin(), bodies.end(), id) - bodies.begin()); };
  double x0, y0, z0, x1, y1, z1;
  all.Get(x0, y0, z0, x1, y1, z1);
  // A frame along the air: the world axes least along it.
  V axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  std::sort(std::begin(axes), std::end(axes), [&](const V& a, const V& b) { return std::fabs(dot(a, way)) < std::fabs(dot(b, way)); });
  const V e1 = enclosure.empty() ? unit(sub(axes[0], mul(way, dot(axes[0], way)))) : V{0, 1, 0};
  const V e2 = unit(cross(way, e1));
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (double x : {x0, x1})
    for (double y : {y0, y1})
      for (double z : {z0, z1}) {
        const V p{x, y, z};
        const double c[3] = {dot(p, way), dot(p, e1), dot(p, e2)};
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
      }
  // Several bodies' extent along a direction (a fan's model of a frame, a hub and blades: their union's).
  auto extent_of = [&](const std::vector<size_t>& parts, const V& d, double& from, double& to) {
    from = 1e300, to = -1e300;
    for (size_t i : parts) {
      Bnd_Box b;
      BRepBndLib::AddOptimal(world[i], b, false, false);
      double c[6];
      b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
      for (double x : {c[0], c[3]})
        for (double y : {c[1], c[4]})
          for (double z : {c[2], c[5]}) from = std::min(from, dot({x, y, z}, d)), to = std::max(to, dot({x, y, z}, d));
    }
  };
  // The parts a list names, bodies or components (a component: its bodies, one part), those taking part only.
  auto parts_of = [&](const json& names) {
    std::vector<size_t> out;
    for (const auto& v : names) {
      const std::string id = v.is_string() ? v.get<std::string>() : v.is_object() ? v.value("body", std::string()) : std::string();
      const Node* n = scene.node(id);
      if (!n) continue;
      for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
        if (const size_t i = index_of(b); i < bodies.size() && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    }
    return out;
  };
  // The finest cell: two thirds of a fin's thickness and a sixth of the gap between fins (the gap's boundary layers want
  // six cells across: on six 3 mm fins 8.4 mm apart, cells of 2, 1.5 and 1 mm gave the parts' rise as 129, 106 and 89 %
  // of the correlations' and the flow as 135, 120 and 103 %), else a 40th of the parts' size.
  // In an enclosure the box's cells are a 30th of its size (quick: a 20th, fine: a 45th; cfd.cell_size: twice that), and each body's surface is refined
  // down to what its fins, walls and vents need (below).
  const double given = cfd.value("cell_size", 0.0);
  const double size = std::sqrt(std::pow(hi[0] - lo[0], 2) + std::pow(hi[1] - lo[1], 2) + std::pow(hi[2] - lo[2], 2));
  double fine = given;
  json fins_json;
  std::vector<bool> finned(bodies.size(), false);
  std::vector<double> fin_want(bodies.size(), 0.0);
  for (const Load* l : fan_loads) {
    const V along = unit(l->def.value("vector", V{1, 0, 0}));
    for (const auto& r : l->refs)
      if (const size_t i = index_of(r.body); i < bodies.size())
        if (const auto f = air::fin_array(world[i], along)) {
          finned[i] = true;
          fin_want[i] = std::max(0.2, std::min(f->t / 1.5, f->gap / 6) * 1e3);
          if (l == fan_load) fins_json = f->to_json();
          if (given <= 0 && enclosure.empty()) fine = std::min(fine > 0 ? fine : 1e300, fin_want[i]);
        }
    // The heatsink the fan blows through (load "heatsink": bodies or components, one part): its fins as one array.
    const std::vector<size_t> sink = parts_of(l->def.value("heatsink", json::array()));
    if (sink.empty()) continue;
    TopoDS_Compound all_fins;
    BRep_Builder bb;
    bb.MakeCompound(all_fins);
    for (size_t i : sink) bb.Add(all_fins, world[i]);
    if (const auto f = air::fin_array(all_fins, along)) {
      for (size_t i : sink) {
        finned[i] = true;
        fin_want[i] = std::max(0.2, std::min(f->t / 1.5, f->gap / 6) * 1e3);
      }
      if (l == fan_load && fins_json.is_null()) fins_json = f->to_json();
      if (given <= 0 && enclosure.empty()) fine = std::min(fine > 0 ? fine : 1e300, fin_want[sink.front()]);
    }
  }
  if (fine <= 0) fine = std::max(0.25, size / 40);
  const double coarse = enclosure.empty() ? 2 * fine : given > 0 ? 2 * given : std::max(0.5, size / cfd.value("divisions", 30.0));

  // Cells: each body's surface refined until its thinnest wall has a cell and a quarter across and its narrowest gap three
  // (an enclosure's vents two: refining all of its surface for them took a small box to a million cells), a finned body's
  // to its fins' cells, at most four levels below the box's cells.
  std::vector<int> level(bodies.size(), 1);
  int deepest = 1;
  json refinement = json::object();  // per body: its level and what set it (summary "refinement")
  // In an enclosure a gap's cells are refined only in a box around it (the faces that look across it): refining all of a box's
  // surface, outside too, for its 3 mm slots made three quarters of a fan-cooled box's 320k cells.
  struct GapBox {
    V lo, hi;
    int level;
  };
  std::vector<GapBox> gap_boxes;
  auto level_for = [&](double want) { return std::clamp(int(std::ceil(std::log2(coarse / want) - 0.2)), 1, 4); };
  for (size_t i = 0; i < bodies.size(); ++i) {
    const air::Thinness t = air::thinness(world[i]);
    double want = enclosure.empty() ? 1e300 : given > 0 ? given : (fin_want[i] > 0 ? fin_want[i] : 1e300);
    if (t.wall > 0) want = std::min(want, t.wall / cfd.value("wall_cells", 1.25));
    const double across = is_wall(bodies[i]) ? 2 : cfd.value("gap_cells", 3.0);
    if (enclosure.empty() && t.gap > 0 && t.gap < 0.5 * size) want = std::min(want, t.gap / across);
    if (want < 1e300) level[i] = level_for(want);
    int gap_level = 0;
    if (!enclosure.empty() && given <= 0)
      for (const auto& g : t.gaps) {
        if (g.width >= 0.5 * size) continue;
        const int L = level_for(g.width / across);
        if (L <= level[i]) continue;
        const double pad = coarse / std::pow(2.0, L);
        gap_boxes.push_back({{g.lo[0] - pad, g.lo[1] - pad, g.lo[2] - pad}, {g.hi[0] + pad, g.hi[1] + pad, g.hi[2] + pad}, L});
        gap_level = std::max(gap_level, L);
      }
    deepest = std::max({deepest, level[i], gap_level});
    refinement[scene.node(bodies[i])->name] = {{"level", level[i]}, {"cell_mm", coarse / std::pow(2.0, level[i])}, {"wall_mm", t.wall}, {"gap_mm", t.gap}};
    if (gap_level > 0) refinement[scene.node(bodies[i])->name]["gap_level"] = gap_level;
  }
  // Boxes of one level that overlap made one.
  for (bool merged = true; merged;) {
    merged = false;
    for (size_t a = 0; a < gap_boxes.size() && !merged; ++a)
      for (size_t b = a + 1; b < gap_boxes.size() && !merged; ++b) {
        GapBox &x = gap_boxes[a], &y = gap_boxes[b];
        if (x.level != y.level) continue;
        bool overlap = true;
        for (int k = 0; k < 3; ++k) overlap = overlap && x.lo[k] <= y.hi[k] && y.lo[k] <= x.hi[k];
        if (!overlap) continue;
        for (int k = 0; k < 3; ++k) x.lo[k] = std::min(x.lo[k], y.lo[k]), x.hi[k] = std::max(x.hi[k], y.hi[k]);
        gap_boxes.erase(gap_boxes.begin() + long(b));
        merged = true;
      }
  }
  fine = coarse / std::pow(2.0, deepest);  // the finest cell
  // Vents narrower than two of the finest cells are closed in the mesh, or nearly: the run lets little or no air through them
  // where the estimate counted them. Said, with the quality that would hold them (its cells scale with its divisions).
  if (vents_est.is_object() && given <= 0) {
    int closed = 0, narrow = 0;
    double smallest = 1e300;
    for (const auto& o : vents_est.value("openings", json::array())) {
      const double w = o.value("width_mm", 1e300);
      if (w >= 2 * fine) continue;
      ++(w < fine ? closed : narrow);
      smallest = std::min(smallest, w);
    }
    if (closed + narrow > 0) {
      const double divisions = cfd.value("divisions", 30.0);
      std::string better;
      for (const auto& [q, d] : {std::pair<const char*, double>{"normal", 30}, {"fine", 45}})
        if (d > divisions && smallest >= 2 * fine * divisions / d) {
          better = std::string(" (") + q + " holds them)";
          break;
        }
      run.warnings.push_back(std::to_string(closed + narrow) + " vent(s) narrower than two of the mesh's finest cells (" + num(fine) + " mm here, the smallest vent " +
                             num(smallest) + " mm wide): " + (closed ? std::to_string(closed) + " closed in the run, " : std::string()) +
                             "little or no air goes through them, so the run may come out hotter than the vents' estimate" +
                             (better.empty() ? std::string(" (give cfd.cell_size to resolve them)") : better));
      vents_est["unresolved"] = closed + narrow;
    }
  }
  // Fans in an enclosure: each a disk of air whose two sides the fan's curve sets apart in pressure. On a body that makes
  // heat or has fins (a heatsink), the disk lies against the side the air comes from, the fan's size across; on any other
  // body (one that stands for the fan), the disk is its middle across the air's way, as wide as it, and the body is air.
  struct Disk {
    const Load* load;
    air::Fan fan;
    V centre, normal;
    double radius;
    // The fan's outlet as set (load "outlet": a rectangle on the face its air leaves by, a blower's side): the jump on it
    // rather than on a disk through the middle of the fan's body. Its two in-plane directions, width and height (mm).
    bool plate = false;
    V x{1, 0, 0}, y{0, 1, 0};
    double width = 0, height = 0;
    std::vector<V> tris;  // a profile's triangles (three points each, mm): the jump across its own outline
    double area = 0;      // mm2
  };
  std::vector<Disk> disks;
  // A blower (fan type "blower"): in through its inlet, out through its outlet, both openings drawn on its housing, which
  // stays a solid wall. The air leaves the room's at the inlet and comes back at the outlet; the flow is the one its curve
  // gives against the pressure between the two (found in rounds of the flow's solve).
  struct Blower {
    const Load* load;
    air::Fan fan;
    size_t housing;
    TopoDS_Face in, out;
    V in_n, out_n;  // out of the housing
    double in_area = 0, out_area = 0;  // mm2
    double Q = 0;   // m3/s
    double dp = 0;  // Pa: the outlet's pressure over the inlet's, the last round
    size_t in_body = 0, out_body = 0;  // the bodies the openings are drawn on (a housing in two shells: either)
    std::vector<std::pair<size_t, TopoDS_Shape>> caps;  // each opening plugged (a thin slab behind it, part of its body's surface)
  };
  std::vector<Blower> blowers;
  std::vector<bool> is_air(bodies.size(), false);
  std::set<std::string> heated;  // the bodies heat sources name (never left out)
  for (const Load* h : heats)
    for (const auto& r : h->refs) heated.insert(r.body);
  // A profile's face as triangles (the jump's surface; the faces a blower's opening covers), points in mm.
  auto triangles = [&](const TopoDS_Face& f) {
    std::vector<V> out;
    BRepMesh_IncrementalMesh(f, 0.05, false, 0.2, true);
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(f, loc);
    if (tri.IsNull()) return out;
    for (int k = 1; k <= tri->NbTriangles(); ++k) {
      int n1, n2, n3;
      tri->Triangle(k).Get(n1, n2, n3);
      for (int n : {n1, n2, n3}) {
        const gp_Pnt p = tri->Node(n).Transformed(loc.Transformation());
        out.push_back({p.X(), p.Y(), p.Z()});
      }
    }
    return out;
  };
  auto is_profile = [](const json& o) { return o.is_string() || (o.is_object() && !o.contains("centre")); };
  if (!enclosure.empty())
    for (const Load* l : fan_loads) {
      const json outlet = l->def.value("outlet", json());
      if (is_profile(outlet) && !outlet.is_null()) {
        // Drawn as a profile: its opening (a sketch's closed region or a flat face) is the fan, wherever its model is.
        const air::Fan fan = air::fan_from(l->def.value("fan", json("80x25")));
        V n_out{0, 0, 1};
        const TopoDS_Face out_face = profile_face(doc, scene, outlet, n_out);
        GProp_GProps g;
        BRepGProp::SurfaceProperties(out_face, g);
        const V out_centre{g.CentreOfMass().X(), g.CentreOfMass().Y(), g.CentreOfMass().Z()};
        auto inside = [&](size_t i, const gp_Pnt& p) { return BRepClass3d_SolidClassifier(world[i], p, 1e-6).State() == TopAbs_IN; };
        if (l->def.value("type", std::string("axial")) == "blower") {
          const json inlet = l->def.value("inlet", json());
          if (!is_profile(inlet) || inlet.is_null()) throw Error("blower \"" + l->name + "\": draw its inlet too (where its air comes in)");
          Blower b{l, fan, 0, {}, out_face, {0, 0, 0}, n_out};
          b.in = profile_face(doc, scene, inlet, b.in_n);
          // Its housing: what it is on, else the body whose surface the outlet lies on.
          json on = json::array();
          for (const auto& r : l->refs) on.push_back(r.body);
          std::vector<size_t> parts = parts_of(on);
          // The body each opening is drawn on: the nearest to the opening itself (an opening that is a hole in the housing has
          // its middle in the air, its rim on the housing), among what it is on when that is given.
          auto drawn_on = [&](const TopoDS_Face& f, const char* what) {
            double best = 1e300;
            size_t at = 0;
            for (size_t i = 0; i < bodies.size(); ++i) {
              if (is_wall(bodies[i]) || (!parts.empty() && std::find(parts.begin(), parts.end(), i) == parts.end())) continue;
              const double d = BRepExtrema_DistShapeShape(world[i], f).Value();
              if (d < best) best = d, at = i;
            }
            if (best > 0.5)
              throw Error("blower \"" + l->name + "\": its " + what + " touches no body (the nearest is " + (best < 1e299 ? num(best) + " mm" : "none") +
                          " away): draw it on the blower's own face, its edges projected");
            return at;
          };
          b.out_body = drawn_on(b.out, "outlet");
          b.in_body = drawn_on(b.in, "inlet");
          b.housing = b.out_body;
          Bnd_Box hb;
          BRepBndLib::Add(world[b.out_body], hb);
          BRepBndLib::Add(world[b.in_body], hb);
          double h0[3], h1[3];
          hb.Get(h0[0], h0[1], h0[2], h1[0], h1[1], h1[2]);
          const V hc{0.5 * (h0[0] + h1[0]), 0.5 * (h0[1] + h1[1]), 0.5 * (h0[2] + h1[2])};
          // Out of the housing: each opening's normal turned away from the housing's middle; and each opening plugged by a
          // slab behind it (a cell or two thick) put in its body's surface, so that a hole in the housing is closed there too
          // and the opening is wall the air goes in or out of (what is inside the housing is never reached).
          for (auto [face, normal] : {std::pair<const TopoDS_Face*, V*>{&b.out, &b.out_n}, {&b.in, &b.in_n}}) {
            const size_t on = face == &b.out ? b.out_body : b.in_body;
            const double slab = std::max(0.3, 2 * coarse / std::pow(2.0, level[on]));
            GProp_GProps p;
            BRepGProp::SurfaceProperties(*face, p);
            const gp_Pnt c = p.CentreOfMass();
            if (dot(*normal, sub(V{c.X(), c.Y(), c.Z()}, hc)) < 0) *normal = mul(*normal, -1.0);
            if (l->def.value(face == &b.out ? "flip_outlet" : "flip_inlet", false)) *normal = mul(*normal, -1.0);  // the user's way
            (face == &b.out ? b.out_area : b.in_area) = p.Mass();
            BRepPrimAPI_MakePrism cap(*face, gp_Vec((*normal)[0], (*normal)[1], (*normal)[2]) * -slab);
            if (cap.IsDone()) b.caps.push_back({on, cap.Shape()});
          }
          // What is inside the housing (its impeller, its motor) is shut in by the plugs: left out of the parts. Shut in, not
          // merely within the housing's box (a heatsink in the curve of a scroll housing, a grille recessed into its intake are
          // not): rays from the body's middle every way meet the housing or a plug.
          std::vector<IntCurvesFace_ShapeIntersector> shell;
          for (size_t on : {b.in_body, b.out_body}) {
            shell.emplace_back();
            shell.back().Load(world[on], 1e-6);
          }
          for (const auto& [on, cap] : b.caps) {
            shell.emplace_back();
            shell.back().Load(cap, 1e-6);
          }
          const double far = std::sqrt(hb.SquareExtent()) + 1;
          for (size_t i = 0; i < bodies.size(); ++i) {
            if (i == b.in_body || i == b.out_body || is_wall(bodies[i]) || is_air[i] || heated.count(bodies[i])) continue;
            Bnd_Box ib;
            BRepBndLib::Add(world[i], ib);
            double i0[3], i1[3];
            ib.Get(i0[0], i0[1], i0[2], i1[0], i1[1], i1[2]);
            bool in = true;
            for (int k = 0; k < 3; ++k) in = in && i0[k] >= h0[k] - 0.5 && i1[k] <= h1[k] + 0.5;
            if (!in) continue;
            const gp_Pnt from(0.5 * (i0[0] + i1[0]), 0.5 * (i0[1] + i1[1]), 0.5 * (i0[2] + i1[2]));
            for (int dx = -1; dx <= 1 && in; ++dx)
              for (int dy = -1; dy <= 1 && in; ++dy)
                for (int dz = -1; dz <= 1 && in; ++dz) {
                  if (dx == 0 && dy == 0 && dz == 0) continue;
                  const gp_Lin ray(from, gp_Dir(dx, dy, dz));
                  bool met = false;
                  for (auto& x : shell) {
                    x.Perform(ray, 0, far);
                    met = met || (x.IsDone() && x.NbPnt() > 0);
                  }
                  in = met;
                }
            if (!in) {
              run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" lies within blower \"" + l->name +
                                     "\"'s outline but outside its housing: kept in the run");
              continue;
            }
            is_air[i] = true, level[i] = 0;
            run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" is inside blower \"" + l->name + "\": left out (its housing is solid)");
          }
          b.Q = 0.5 * fan.Qmax;  // a first guess: the rounds find where its curve meets the box
          blowers.push_back(b);
          continue;
        }
        // An axial fan: the jump across the profile, the air its way (its vector, else the profile's normal); the bodies the
        // profile lies in (the fan's own model) are air.
        Disk d{l, fan, out_centre, unit(l->def.value("vector", n_out)), 0};
        if (norm(d.normal) < 0.5) throw Error("fan \"" + l->name + "\": its vector has no length");
        d.plate = true;
        d.tris = triangles(out_face);
        d.area = g.Mass();
        d.radius = std::sqrt(d.area / kPi);
        if (d.tris.empty()) throw Error("fan \"" + l->name + "\": its opening could not be meshed");
        for (size_t i = 0; i < bodies.size(); ++i) {
          if (is_wall(bodies[i])) continue;
          for (size_t k = 0; k < d.tris.size(); k += 3) {
            const V c = mul(add(add(d.tris[k], d.tris[k + 1]), d.tris[k + 2]), 1.0 / 3);
            if (inside(i, gp_Pnt(c[0], c[1], c[2]))) {
              is_air[i] = true, level[i] = 0;
              break;
            }
          }
        }
        disks.push_back(d);
        continue;
      }
      if (l->refs.empty()) throw Error("fan \"" + l->name + "\" is on nothing: draw its opening (outlet) as a profile");
      // What it is on: a block or the fan's own model (a component: its frame, hub and blades as one), or a heatsink.
      json on = json::array();
      for (const auto& r : l->refs) on.push_back(r.body);
      const std::vector<size_t> parts = parts_of(on);
      if (parts.empty()) throw Error("fan \"" + l->name + "\" is on nothing that takes part");
      Disk d{l, air::fan_from(l->def.value("fan", json("80x25"))), {0, 0, 0}, unit(l->def.value("vector", V{1, 0, 0})), 0};
      if (norm(d.normal) < 0.5) throw Error("fan \"" + l->name + "\": its vector has no length");
      if (l->def.value("count", 1) > 1) run.warnings.push_back("fan \"" + l->name + "\": count is for a duct; in an enclosure, add a fan load per fan");
      V u1 = unit(cross(d.normal, std::fabs(d.normal[0]) < 0.9 ? V{1, 0, 0} : V{0, 1, 0})), u2 = unit(cross(d.normal, u1));
      double n0, n1, p0, p1, q0, q1;
      extent_of(parts, d.normal, n0, n1);
      extent_of(parts, u1, p0, p1);
      extent_of(parts, u2, q0, q1);
      bool solid = false;  // a part that makes heat, has fins or is the box's: the fan blows on it
      int finest = 0;
      for (size_t i : parts) {
        for (const Load* h : heats)
          for (const auto& r : h->refs) solid = solid || r.body == bodies[i];
        solid = solid || finned[i] || is_wall(bodies[i]);
        finest = std::max(finest, level[i]);
      }
      const double along = !solid ? 0.5 * (n0 + n1) : n0 - std::max(1.0, 1.5 * coarse / std::pow(2.0, finest));
      d.centre = add(add(mul(d.normal, along), mul(u1, 0.5 * (p0 + p1))), mul(u2, 0.5 * (q0 + q1)));
      d.radius = !solid || d.fan.size <= 0 ? 0.47 * std::min(p1 - p0, q1 - q0) : 0.47 * d.fan.size;
      if (const json o = l->def.value("outlet", json()); o.is_object()) {  // the outlet as set: the jump across that rectangle
        const V x = unit(o.value("x", V{0, 0, 0})), n = unit(o.value("normal", V{0, 0, 0}));
        d.width = o.value("width", 0.0), d.height = o.value("height", 0.0);
        if (norm(x) < 0.5 || norm(n) < 0.5 || d.width <= 0 || d.height <= 0)
          throw Error("fan \"" + l->name + "\": its outlet needs a centre, its normal and x, a width and a height");
        d.plate = true;
        d.centre = o.value("centre", d.centre);
        d.normal = dot(n, d.normal) < 0 ? mul(n, -1.0) : n;  // the jump pushes the air the fan's way
        d.x = x, d.y = unit(cross(d.normal, x));
        d.radius = 0.5 * std::min(d.width, d.height);
      }
      if (!solid)
        for (size_t i : parts) is_air[i] = true, level[i] = 0;
      disks.push_back(d);
    }

  // The air's box: a duct along the air, a little upstream and more downstream, a cell clear of the parts across; or an
  // enclosure in a margin of the room, open on every side. Off the parts' faces by a fraction of a cell, so that no face of
  // theirs lies on a grid plane.
  const double len = hi[0] - lo[0];
  const double big = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
  const double margin = std::max(4 * coarse, cfd.value("padding", 0.15 * big));
  const double up = enclosure.empty() ? std::max(4 * coarse, cfd.value("upstream", 0.25 * len)) : margin;
  const double down = enclosure.empty() ? std::max(4 * coarse, cfd.value("downstream", 0.5 * len)) : margin;
  const double pad = enclosure.empty() ? cfd.value("padding", fine) : margin;
  const double off = 0.37 * coarse;
  const double d0 = lo[0] - up - off, d1 = hi[0] + down + off;
  double a0 = lo[1] - pad - off, a1 = hi[1] + pad + off, b0 = lo[2] - pad - off, b1 = hi[2] + pad + off;
  // cfd.span (mm, a stream only): the box's width across the parts' wider side, centred on them; narrower than a part,
  // the part crosses the box wall to wall and, the walls slipping, the air is two-dimensional (a plate of endless span).
  if (const double span = cfd.value("span", 0.0); span > 0 && enclosure.empty()) {
    if (!stream) throw Error("cfd.span is for a stream (forced convection), not a fan's duct");
    double& s0 = hi[1] - lo[1] >= hi[2] - lo[2] ? a0 : b0;
    double& s1 = &s0 == &a0 ? a1 : b1;
    const double mid = &s0 == &a0 ? 0.5 * (lo[1] + hi[1]) : 0.5 * (lo[2] + hi[2]);
    s0 = mid - 0.5 * span, s1 = mid + 0.5 * span;
  }
  const int nx = std::max(4, int(std::ceil((d1 - d0) / coarse))), ny = std::max(2, int(std::ceil((a1 - a0) / coarse))),
            nz = std::max(2, int(std::ceil((b1 - b0) / coarse)));
  if (double(nx) * ny * nz > cfd.value("max_cells", 3e6)) throw Error("cfd: the air's box would take too many cells at this cell size: set cfd.cell_size larger");
  auto corner = [&](double d, double a, double b) { return add(add(mul(way, d), mul(e1, a)), mul(e2, b)); };

  // ---- the case
  struct Dir {
    fs::path p;
    bool keep = false;
    ~Dir() {
      std::error_code e;
      if (!keep && !p.empty()) fs::remove_all(p, e);
    }
  } dir;
  dir.p = fs::temp_directory_path() / ("opad-cfd-" + new_uuid().substr(0, 12));
  fs::create_directories(dir.p);
  if (const char* k = std::getenv("OPAD_KEEP_CFD"); k && *k) dir.keep = true;
  const fs::path cas = dir.p / "cht", flow = dir.p / "flow";
  run.summary["case_dir"] = dir.keep ? json(path_to_utf8(dir.p)) : json(nullptr);
  // Programs: run with the project directory known (its etc files), or through a full install's wrapper.
  const int timeout = int(st.value("timeout", 7200.0) * 1000);
  // `at`: where the run is when the program starts; `to`: where it is once it is done (its share of the bar, followed through
  // its log: the mesher's phases, a solver's iterations); a program of seconds keeps `at`.
  auto foam_run = [&](const std::string& app, const std::vector<std::string>& args, const fs::path& where, double at, double to = -1) {
    report(at, "OpenFOAM: " + app);
    if (!foam.project.empty()) {
#ifdef _WIN32
      _putenv_s("WM_PROJECT_DIR", path_to_utf8(foam.project).c_str());
#else
      const std::string project = path_to_utf8(foam.project);  // only when it changes: sweep points run at once
      if (const char* now = std::getenv("WM_PROJECT_DIR"); !now || project != now) setenv("WM_PROJECT_DIR", project.c_str(), 1);
#endif
    }
    detail::RunOptions ro;
    ro.output = where / ("log." + app);
    ro.timeout_ms = timeout;
    // The program's log read as it grows (once a second): a run of minutes is seen to move. The mesher says its phase and the
    // iteration in it; a solver its time, against the end time of its controlDict.
    double end_time = 0;
    try {
      const std::string control = read_text_file(where / "system" / "controlDict");
      if (const size_t e = control.find("endTime "); e != std::string::npos) end_time = std::atof(control.c_str() + e + 8);
    } catch (...) {
    }
    double shown = at;
    std::string phase = "OpenFOAM: " + app;
    auto read_at = std::chrono::steady_clock::now();
    double first_time = -1;
    ro.cancelled = [&] {
      const auto now = std::chrono::steady_clock::now();
      if (to > at && now - read_at > std::chrono::seconds(1)) {
        read_at = now;
        std::string tail;
        if (std::ifstream in(ro.output, std::ios::binary | std::ios::ate); in) {
          const std::streamoff size = in.tellg(), take = std::min<std::streamoff>(size, 65536);
          in.seekg(size - take);
          tail.resize(size_t(take));
          in.read(tail.data(), take);
        }
        auto last = [&](const char* word) {
          const size_t i = tail.rfind(word);
          return i == std::string::npos ? -1.0 : std::atof(tail.c_str() + i + std::strlen(word));
        };
        if (app == "snappyHexMesh") {
          // three phases: castellating (refinement), snapping to the parts (morph iterations), adding layers
          const size_t refine = tail.rfind("Refinement phase"), snap = tail.rfind("Morphing phase"), layer = tail.rfind("layer addition phase");
          const double morph = last("Morph iteration "), layers = last("Layer addition iteration ");
          double sub = 0.05;
          if (layer != std::string::npos || layers >= 0) sub = 0.7 + 0.25 * std::min(1.0, (layers + 1) / 10.0), phase = "Meshing the air: adding layers along the parts";
          else if (snap != std::string::npos || morph >= 0) sub = 0.3 + 0.4 * std::min(1.0, (morph + 1) / 10.0), phase = "Meshing the air: fitting it to the parts";
          else if (refine != std::string::npos) sub = 0.15, phase = "Meshing the air: refining around the parts";
          shown = std::max(shown, at + (to - at) * sub);
        } else if (const double t = last("\nTime = "); t > 0 && end_time > 0) {
          if (first_time < 0) first_time = t - 1;
          const double sub = std::clamp((t - first_time) / std::max(1.0, end_time - first_time), 0.0, 1.0);
          shown = std::max(shown, at + (to - at) * sub);
          phase = "OpenFOAM: " + app + ", iteration " + std::to_string(int(t)) + " of " + std::to_string(int(end_time));
        }
      }
      return progress && !progress(shown, phase);
    };
    std::vector<fs::path> a;
    fs::path program;
    if (!foam.wrapper.empty()) program = foam.wrapper, a.push_back(app);
    else program = foam.bin / app;
    for (const auto& s : args) a.push_back(s);
    a.push_back("-case");
    a.push_back(where);
    const int status = detail::run_program(program, a, where, ro);
    std::string log;
    try {
      log = read_text_file(ro.output);
    } catch (...) {
    }
    if (status != 0 || log.find("FOAM FATAL") != std::string::npos) {
      const size_t at_err = log.find("FOAM FATAL");
      throw Error("OpenFOAM's " + app + " failed: " + (at_err == std::string::npos ? "status " + std::to_string(status) : log.substr(at_err, 500)));
    }
    return log;
  };

  // Each part: an STL in metres (snappyHexMesh), its triangles kept for showing the results.
  std::vector<std::string> region;
  std::vector<std::vector<V>> tri_pts(bodies.size());  // three points per triangle, mm
  for (size_t i = 0; i < bodies.size(); ++i) {
    region.push_back(is_air[i] ? std::string() : "s" + std::to_string(i));
    if (is_air[i]) continue;
    BRepMesh_IncrementalMesh(world[i], std::max(0.02, coarse / std::pow(2.0, level[i]) / 4), false, 0.3, true);
    std::ostringstream stl;
    stl.precision(10);
    stl << "solid " << region[i] << "\n";
    for (TopExp_Explorer e(world[i], TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face face = TopoDS::Face(e.Current());
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
      if (tri.IsNull()) continue;
      const bool rev = face.Orientation() == TopAbs_REVERSED;
      for (int t = 1; t <= tri->NbTriangles(); ++t) {
        int n[3];
        tri->Triangle(t).Get(n[0], n[1], n[2]);
        if (rev) std::swap(n[1], n[2]);
        V p[3];
        for (int k = 0; k < 3; ++k) {
          const gp_Pnt q = tri->Node(n[k]).Transformed(loc.Transformation());
          p[k] = {q.X(), q.Y(), q.Z()};
          tri_pts[i].push_back(p[k]);
        }
        const V nn = unit(cross(sub(p[1], p[0]), sub(p[2], p[0])));
        stl << " facet normal " << nn[0] << " " << nn[1] << " " << nn[2] << "\n  outer loop\n";
        for (const auto& q : p) stl << "   vertex " << q[0] * 1e-3 << " " << q[1] * 1e-3 << " " << q[2] * 1e-3 << "\n";
        stl << "  endloop\n endfacet\n";
      }
    }
    for (const Blower& b : blowers)  // a blower's openings plugged: part of its housing's surface (not of its results)
      for (const auto& [on, cap] : b.caps) {
          if (on != i) continue;
          BRepMesh_IncrementalMesh(cap, std::max(0.02, coarse / std::pow(2.0, level[i]) / 4), false, 0.3, true);
          for (TopExp_Explorer e(cap, TopAbs_FACE); e.More(); e.Next()) {
            const TopoDS_Face face = TopoDS::Face(e.Current());
            TopLoc_Location loc;
            const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) continue;
            const bool rev = face.Orientation() == TopAbs_REVERSED;
            for (int t = 1; t <= tri->NbTriangles(); ++t) {
              int n[3];
              tri->Triangle(t).Get(n[0], n[1], n[2]);
              if (rev) std::swap(n[1], n[2]);
              V p[3];
              for (int k = 0; k < 3; ++k) {
                const gp_Pnt q = tri->Node(n[k]).Transformed(loc.Transformation());
                p[k] = {q.X(), q.Y(), q.Z()};
              }
              const V nn = unit(cross(sub(p[1], p[0]), sub(p[2], p[0])));
              stl << " facet normal " << nn[0] << " " << nn[1] << " " << nn[2] << "\n  outer loop\n";
              for (const auto& q : p) stl << "   vertex " << q[0] * 1e-3 << " " << q[1] * 1e-3 << " " << q[2] * 1e-3 << "\n";
              stl << "  endloop\n endfacet\n";
            }
          }
        }
    stl << "endsolid " << region[i] << "\n";
    fs::create_directories(cas / "constant" / "triSurface");
    write_text_file(cas / "constant" / "triSurface" / (region[i] + ".stl"), stl.str());
  }

  // Warm air rising (cfd.buoyancy; on by default in an enclosure without fans): the flow and the air's temperature solved
  // together at each pass between the air and the parts, the walls at the parts' temperatures. Its air is meshed without
  // snapping to the parts (their faces stepped to the cells): on snapped cells the solvers under gravity (Boussinesq or
  // compressible, steady or not) blew up from rounding alone, still air at one temperature included.
  const bool buoyant = !enclosure.empty() && cfd.value("buoyancy", disks.empty());
  // Sealed (cfd.sealed): only the air inside the enclosure, none of the room's; nothing goes in or out.
  const bool sealed = !enclosure.empty() && cfd.value("sealed", false);

  // ---- mesh: the duct, the parts cut out of it as cell zones, then one region each
  report(0.03, "Writing the OpenFOAM case");
  {
    std::ostringstream bm;
    bm << "convertToMeters 0.001;\nvertices (\n";
    for (const V& v : {corner(d0, a0, b0), corner(d1, a0, b0), corner(d1, a1, b0), corner(d0, a1, b0), corner(d0, a0, b1), corner(d1, a0, b1),
                       corner(d1, a1, b1), corner(d0, a1, b1)})
      bm << "  " << vec(v) << "\n";
    bm << ");\nblocks ( hex (0 1 2 3 4 5 6 7) (" << nx << " " << ny << " " << nz << ") simpleGrading (1 1 1) );\nboundary (\n"
       << (enclosure.empty() ? "  inlet { type patch; faces ((0 4 7 3)); }\n  outlet { type patch; faces ((1 2 6 5)); }\n"
                               "  walls { type wall; faces ((0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n"
                             : "  open { type patch; faces ((0 4 7 3) (1 2 6 5) (0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n");
    put(cas / "system" / "blockMeshDict", "dictionary", "blockMeshDict", bm.str());
    std::ostringstream sh;
    // Layers of thin cells along the parts' walls (an air-only, snapped mesh: an enclosure with fans): the air's boundary
    // layers need ten cells across where two or three were (a plate's film came out half as large again as Blasius'
    // layer gives). cfd.layers: how many, 0 for none.
    // Not in still air (a sealed box without fans or gravity): there is no boundary layer, and the thin cells beside an
    // insulating wall kept its coupling from settling (58 % apart after 30 passes).
    const int layers = air_only && !buoyant && !(sealed && disks.empty()) ? cfd.value("layers", 5) : 0;
    sh << "castellatedMesh true; snap " << (buoyant ? "false" : "true") << "; addLayers " << (layers > 0 ? "true" : "false") << ";\ngeometry {\n";
    for (const auto& r : region)
      if (!r.empty()) sh << "  " << r << " { type triSurfaceMesh; file \"" << r << ".stl\"; }\n";
    for (size_t g = 0; g < gap_boxes.size(); ++g) sh << "  gap" << g << " { type searchableBox; min " << vec(mul(gap_boxes[g].lo, 1e-3)) << "; max " << vec(mul(gap_boxes[g].hi, 1e-3))
                                                     << "; }\n";  // m, as the mesh
    sh << "}\ncastellatedMeshControls {\n  maxLocalCells 6000000; maxGlobalCells 12000000; minRefinementCells 0; maxLoadUnbalance 0.1; nCellsBetweenLevels 2;\n"
       << "  features ();\n  refinementSurfaces {\n";
    // In a duct, each part a region of the mesh (its cells kept, conjugate heat in OpenFOAM); in an enclosure only the air is
    // meshed, the parts its walls (their heat in CalculiX, below).
    for (size_t i = 0; i < region.size(); ++i)
      if (!region[i].empty()) {
        sh << "    " << region[i] << " { level (" << level[i] << " " << level[i] << ");";
        if (!air_only) sh << " faceZone " << region[i] << "; cellZone " << region[i] << "; cellZoneInside inside;";
        sh << " }\n";
      }
    // A point in the air: upstream in a duct, in the room's margin by a corner of an enclosure; in a sealed one, in its air.
    V seed = enclosure.empty() ? corner(d0 + 0.5 * (up + off), 0.5 * (a0 + a1), 0.5 * (b0 + b1)) : corner(d0 + 0.5 * margin, a0 + 0.5 * margin, b0 + 0.5 * margin);
    if (sealed) {
      // The first point of a grid over the enclosure's box (all its walls') that no part holds, a cell clear of their faces.
      Bnd_Box eb;
      for (const auto& w : box_walls) BRepBndLib::Add(world[index_of(w)], eb);
      double c[6];
      eb.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
      bool found = false;
      for (int i = 1; i < 12 && !found; ++i)
        for (int j = 1; j < 12 && !found; ++j)
          for (int k = 1; k < 12 && !found; ++k) {
            const gp_Pnt q(c[0] + (c[3] - c[0]) * (i + 0.13) / 12, c[1] + (c[4] - c[1]) * (j + 0.17) / 12, c[2] + (c[5] - c[2]) * (k + 0.19) / 12);
            bool free = true;
            for (size_t b = 0; b < world.size() && free; ++b) {
              if (is_air[b]) continue;
              BRepClass3d_SolidClassifier in(world[b], q, 1e-6);
              if (in.State() != TopAbs_OUT) free = false;
              else if (BRepExtrema_DistShapeShape(world[b], BRepBuilderAPI_MakeVertex(q).Vertex()).Value() < coarse) free = false;
            }
            if (free) seed = {q.X(), q.Y(), q.Z()}, found = true;
          }
      if (!found) throw Error("cfd.sealed: no air found inside the enclosure (is it hollow?)");
    }
    const V inside_air = mul(seed, 1e-3);
    sh << "  }\n  resolveFeatureAngle 30;\n  refinementRegions {";
    for (size_t g = 0; g < gap_boxes.size(); ++g) sh << " gap" << g << " { mode inside; levels ((1e15 " << gap_boxes[g].level << ")); }";
    // cfd.near_wall {distance, cell} (mm): every cell that near a part that small, for the air's boundary layers.
    if (const json nw = cfd.value("near_wall", json()); nw.is_object()) {
      const int L = std::clamp(int(std::ceil(std::log2(coarse / std::max(1e-3, nw.value("cell", fine))) - 0.01)), 0, 8);
      for (const auto& r : region)
        if (!r.empty()) sh << " " << r << " { mode distance; levels ((" << num(nw.value("distance", 2 * fine) * 1e-3) << " " << L << ")); }";
    }
    sh << " }\n  locationInMesh " << vec(inside_air) << ";\n  allowFreeStandingZoneFaces false;\n}\n"
       << "snapControls { nSmoothPatch 3; tolerance 2.0; nSolveIter 50; nRelaxIter 5; nFeatureSnapIter 10; implicitFeatureSnap true; explicitFeatureSnap false; "
          "multiRegionFeatureSnap true; }\n"
       << "addLayersControls { relativeSizes true; layers {" << (layers > 0 ? " \"s[0-9]+.*\" { nSurfaceLayers " + std::to_string(layers) + "; }" : std::string())
       << " } expansionRatio 1.25; finalLayerThickness 0.5; minThickness 0.02; nGrow 0; featureAngle 130; "
          "nRelaxIter 3; nSmoothSurfaceNormals 1; nSmoothNormals 3; nSmoothThickness 10; maxFaceThicknessRatio 0.5; maxThicknessToMedialRatio 0.3; "
          "minMedialAxisAngle 90; nBufferCellsNoExtrude 0; nLayerIter 50; }\n"
       << "meshQualityControls { maxNonOrtho 65; maxBoundarySkewness 20; maxInternalSkewness 4; maxConcave 80; minVol 1e-18; minTetQuality 1e-15; "
          "minArea -1; minTwist 0.02; minDeterminant 0.001; minFaceWeight 0.05; minVolRatio 0.01; minTriangleTwist -1; nSmoothScale 4; "
          "errorReduction 0.75; relaxed { maxNonOrtho 75; } }\nmergeTolerance 1e-6;\n";
    put(cas / "system" / "snappyHexMeshDict", "dictionary", "snappyHexMeshDict", sh.str());
    put(cas / "system" / "controlDict", "dictionary", "controlDict",
        "application chtMultiRegionSimpleFoam; startFrom latestTime; startTime 0; stopAt endTime; endTime 1; deltaT 1; writeControl timeStep; "
        "writeInterval 1; purgeWrite 1; writeFormat ascii; writePrecision 10; writeCompression off; timeFormat general; timePrecision 8; "
        "runTimeModifiable false;\n");
    put(cas / "system" / "fvSchemes", "dictionary", "fvSchemes",
        "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; } laplacianSchemes { default Gauss "
        "linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
    put(cas / "system" / "fvSolution", "dictionary", "fvSolution", "\n");
  }
  const auto t_mesh = std::chrono::steady_clock::now();
  foam_run("blockMesh", {}, cas, 0.05);
  report(0.08, "Meshing the air around the parts (snappyHexMesh)");
  foam_run("snappyHexMesh", {"-overwrite"}, cas, 0.08, 0.3);
  // The air: in a duct, the cells in no part's zone, which splitMeshRegions names domain0; in an enclosure, the whole mesh.
  const std::string fluid = "domain0";
  std::vector<std::string> solids;
  if (!air_only) {
    foam_run("splitMeshRegions", {"-cellZones", "-overwrite"}, cas, 0.3);
    if (!fs::is_directory(cas / "constant" / fluid / "polyMesh")) throw Error("cfd: the mesh has no air around the parts");
    for (size_t i = 0; i < region.size(); ++i)
      if (region[i].empty()) continue;
      else if (fs::is_directory(cas / "constant" / region[i] / "polyMesh")) solids.push_back(region[i]);
      else run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" was too thin for the cell size and is left out: set cfd.cell_size smaller");
  }
  const fs::path air_mesh = !air_only ? cas / "constant" / fluid / "polyMesh" : cas / "constant" / "polyMesh";
  timing["mesh"] = seconds_since(t_mesh);
  // The fans: a face zone of the air's faces each disk crosses, made a pair of baffles the fan's pressure jumps across.
  std::vector<size_t> disk_faces;
  if (!disks.empty()) {
    std::ostringstream ts, cb;
    ts << "actions (\n";
    cb << "internalFacesOnly true;\nbaffles {\n";
    for (size_t k = 0; k < disks.size(); ++k) {
      const std::string f = "fan" + std::to_string(k);
      if (disks[k].plate) {  // a rectangle at any angle: two triangles in constant/triSurface (surfaceName keeps the zone's name)
        const Disk& d = disks[k];
        auto at = [&](double a, double b) { return mul(add(add(d.centre, mul(d.x, a * 0.5 * d.width)), mul(d.y, b * 0.5 * d.height)), 1e-3); };
        const V c00 = at(-1, -1), c10 = at(1, -1), c11 = at(1, 1), c01 = at(-1, 1);
        std::ostringstream stl;
        auto xyz = [](const V& v) { return num(v[0]) + " " + num(v[1]) + " " + num(v[2]); };  // STL: no brackets (vec() is OpenFOAM's)
        auto tri = [&](const V& a, const V& b, const V& c) {
          stl << "facet normal " << xyz(d.normal) << "\n outer loop\n  vertex " << xyz(a) << "\n  vertex " << xyz(b) << "\n  vertex " << xyz(c)
              << "\n endloop\nendfacet\n";
        };
        stl << "solid " << f << "\n";
        if (!d.tris.empty())  // a profile: its own triangles, in metres
          for (size_t t = 0; t + 2 < d.tris.size(); t += 3) tri(mul(d.tris[t], 1e-3), mul(d.tris[t + 1], 1e-3), mul(d.tris[t + 2], 1e-3));
        else
          tri(c00, c10, c11), tri(c00, c11, c01);
        stl << "endsolid " << f << "\n";
        fs::create_directories(cas / "constant" / "triSurface");
        write_text_file(cas / "constant" / "triSurface" / (f + ".stl"), stl.str());
        ts << " { name " << f << "; type faceZoneSet; action new; source searchableSurfaceToFaceZone; surface triSurfaceMesh; surfaceName " << f
           << ".stl; }\n";
      } else {
        ts << " { name " << f << "; type faceZoneSet; action new; source searchableSurfaceToFaceZone; surface searchableDisk; origin "
           << vec(mul(disks[k].centre, 1e-3)) << "; normal " << vec(disks[k].normal) << "; radius " << num(disks[k].radius * 1e-3) << "; }\n";
      }
      cb << "  " << f << " { type faceZone; zoneName " << f << "; patches { master { name " << f << "_in; type cyclic; neighbourPatch " << f
         << "_out; } slave { name " << f << "_out; type cyclic; neighbourPatch " << f << "_in; } } }\n";
    }
    put(cas / "system" / "topoSetDict", "dictionary", "topoSetDict", ts.str() + ");\n");
    put(cas / "system" / "createBafflesDict", "dictionary", "createBafflesDict", cb.str() + "}\n");
    const std::string log = foam_run("topoSet", {}, cas, 0.31);
    for (size_t k = 0; k < disks.size(); ++k) {
      // "faceZoneSet fan0 now size N"
      const std::string key = "fan" + std::to_string(k) + " now size ";
      const size_t at = log.rfind(key);
      const size_t n = at == std::string::npos ? 0 : size_t(std::strtoul(log.c_str() + at + key.size(), nullptr, 10));
      if (n == 0)
        throw Error("fan \"" + disks[k].load->name + "\": " + (disks[k].plate ? "its outlet crosses no air (is it inside a part, or off the air's box?)"
                                                                            : "its disk crosses no air (is it inside a part?)"));
      disk_faces.push_back(n);
    }
    foam_run("createBaffles", {"-overwrite"}, cas, 0.315);
  }
  // The blowers' openings: the faces of their housing's wall that each inlet and outlet covers (on its plane within a cell,
  // inside its outline), made patches of their own: blowerK_in (the room's air leaves into the blower), blowerK_out (it comes
  // back out). Their faces are then no wall of the housing.
  std::vector<std::array<size_t, 2>> blower_faces;
  if (!blowers.empty()) {
    const auto P = read_points(read_text_file(air_mesh / "points"));
    const auto F = read_faces(read_text_file(air_mesh / "faces"));
    const auto walls0 = read_boundary(read_text_file(air_mesh / "boundary"));
    std::ostringstream cp;
    cp << "pointSync false;\npatches (\n";
    fs::create_directories(air_mesh / "sets");
    for (size_t k = 0; k < blowers.size(); ++k) {
      const Blower& b = blowers[k];
      std::array<size_t, 2> count{0, 0};
      for (int side = 0; side < 2; ++side) {
        const std::string wall = region[side == 0 ? b.in_body : b.out_body];
        const double cell = coarse / std::pow(2.0, level[side == 0 ? b.in_body : b.out_body]);
        const TopoDS_Face& face = side == 0 ? b.in : b.out;
        const V n = side == 0 ? b.in_n : b.out_n;
        GProp_GProps g;
        BRepGProp::SurfaceProperties(face, g);
        const V c0{g.CentreOfMass().X(), g.CentreOfMass().Y(), g.CentreOfMass().Z()};
        std::vector<size_t> picked;
        for (const auto& pt : walls0) {
          if (wall.empty() || !(pt.name == wall || pt.name.rfind(wall + "_", 0) == 0)) continue;
          for (size_t f = pt.start; f < pt.start + pt.faces; ++f) {
            V c{0, 0, 0};
            for (int q : F[f]) c = add(c, P[size_t(q)]);
            c = mul(c, 1e3 / double(F[f].size()));  // mm
            const double off = dot(sub(c, c0), n);
            if (std::fabs(off) > (buoyant ? 1.0 : 0.6) * cell) continue;  // unsnapped (warm air rising): its cells stepped
            BRepClass_FaceClassifier where(face, gp_Pnt(c[0] - off * n[0], c[1] - off * n[1], c[2] - off * n[2]), 1e-3);
            if (where.State() == TopAbs_IN || where.State() == TopAbs_ON) picked.push_back(f);
          }
        }
        if (picked.empty())
          throw Error("blower \"" + b.load->name + "\": its " + (side == 0 ? "inlet" : "outlet") +
                      " is not on its housing's surface: draw it on the blower's face (a sketch on that face)");
        const std::string name = "blower" + std::to_string(k) + (side == 0 ? "_in" : "_out");
        std::ostringstream set;
        set << picked.size() << "\n(\n";
        for (size_t f : picked) set << f << "\n";
        set << ")\n";
        write_text_file(air_mesh / "sets" / name, header("faceSet", name, "constant/polyMesh/sets") + set.str());
        cp << " { name " << name << "; patchInfo { type patch; } constructFrom set; set " << name << "; }\n";
        count[size_t(side)] = picked.size();
      }
      blower_faces.push_back(count);
      for (int side = 0; side < 2; ++side)  // a handful of faces: the opening barely there in the mesh (a thin gap before it closed)
        if (count[size_t(side)] < 8)
          run.warnings.push_back("blower \"" + b.load->name + "\": its " + (side == 0 ? "inlet" : "outlet") + " covers only " + std::to_string(count[size_t(side)]) +
                                 " mesh face(s): the run resolves it poorly (a finer quality, or a smaller cfd.cell_size)");
    }
    cp << ");\n";
    put(cas / "system" / "createPatchDict", "dictionary", "createPatchDict", cp.str());
    foam_run("createPatch", {"-overwrite"}, cas, 0.318);
  }
  // Each fan's disk as the mesh has it: the area of the faces it crosses (the solver's mean speed through the fan is over
  // these, not the circle's area: a few percent apart).
  std::vector<double> disk_area(disks.size(), 0.0);
  if (!disks.empty()) {
    const auto P = read_points(read_text_file(air_mesh / "points"));
    const auto F = read_faces(read_text_file(air_mesh / "faces"));
    for (const auto& pt : read_boundary(read_text_file(air_mesh / "boundary")))
      for (size_t k = 0; k < disks.size(); ++k)
        if (pt.name == "fan" + std::to_string(k) + "_in")
          for (size_t f = pt.start; f < pt.start + pt.faces; ++f) {
            V c{0, 0, 0}, area{0, 0, 0};
            for (int q : F[f]) c = add(c, P[size_t(q)]);
            c = mul(c, 1.0 / double(F[f].size()));
            for (size_t q = 0; q < F[f].size(); ++q) area = add(area, mul(cross(sub(P[size_t(F[f][q])], c), sub(P[size_t(F[f][(q + 1) % F[f].size()])], c)), 0.5));
            disk_area[k] += norm(area);
          }
  }

  // Streamlines over the air's cells, with its velocity (m/s) and temperature (K) in each.
  auto trace = [&](FeaResult& res, const Cells& air_cells, const std::vector<double>& Uc, const std::vector<double>& Tc) {
    // Streamlines: from points across the inlet in front of the parts, along the air's velocity (the nearest cell's, in
    // steps of half the finest cell, the midpoint rule) until the outlet, the duct's side, still air or a part.
    std::vector<V> at_mm;
    std::vector<double> cell_mm;  // each air cell's size
    for (size_t c = 0; c < air_cells.centre.size(); ++c)
      at_mm.push_back(mul(air_cells.centre[c], 1e3)), cell_mm.push_back(std::cbrt(std::max(0.0, air_cells.volume[c])) * 1e3);
    const Nearest nearest(at_mm, coarse);
    double mean_speed = 0;
    for (size_t c = 0; c < at_mm.size(); ++c) mean_speed += norm({Uc[3 * c], Uc[3 * c + 1], Uc[3 * c + 2]});
    mean_speed /= std::max<size_t>(1, at_mm.size());
    auto velocity = [&](const V& p, size_t& cell) {
      cell = nearest(p);
      if (norm(sub(at_mm[cell], p)) > 1.2 * cell_mm[cell]) return V{0, 0, 0};  // inside a part
      return V{Uc[3 * cell], Uc[3 * cell + 1], Uc[3 * cell + 2]};
    };
    const int across = cfd.value("streamlines", 10);
    // Seeds: across the inlet in front of the parts; in an enclosure, across each fan's disk just before it.
    std::vector<V> seeds;
    if (enclosure.empty()) {
      const double wa = hi[1] - lo[1], wb = hi[2] - lo[2];
      const int na = std::max(1, int(std::lround(across * std::sqrt(wa / std::max(1e-9, wb))))),
                nb = std::max(1, int(std::lround(across * std::sqrt(wb / std::max(1e-9, wa)))));
      for (int i = 0; i < na; ++i)
        for (int j = 0; j < nb; ++j) seeds.push_back(corner(d0 + 2 * fine, lo[1] + wa * (i + 0.5) / na, lo[2] + wb * (j + 0.5) / nb));
    } else {
      for (const Disk& d : disks) {
        const V u1 = unit(cross(d.normal, std::fabs(d.normal[0]) < 0.9 ? V{1, 0, 0} : V{0, 1, 0})), u2 = unit(cross(d.normal, u1));
        for (int i = 0; i < across; ++i)
          for (int j = 0; j < across; ++j) {
            const double x = -1 + 2 * (i + 0.5) / across, y = -1 + 2 * (j + 0.5) / across;
            if (x * x + y * y <= 1) seeds.push_back(add(add(sub(d.centre, mul(d.normal, fine)), mul(u1, x * d.radius)), mul(u2, y * d.radius)));
          }
      }
    }
    const double reach = norm({d1 - d0, a1 - a0, b1 - b0});
    for (V p : seeds) {
      std::vector<Vec3> line;
      std::vector<double> speed, temp;
      size_t cell = 0;
      for (int step = 0; step < int(16 * reach / fine); ++step) {
        const V u = velocity(p, cell);
        const double sp = norm(u), h = 0.5 * cell_mm[cell];  // half the cell it is in
        if (sp < 1e-3 * mean_speed) break;
        if (step % 2 == 0 || line.empty()) {
          line.push_back(p);
          speed.push_back(sp);
          temp.push_back(Tc[cell] - 273.15);
        }
        size_t c2 = 0;
        const V um = velocity(add(p, mul(u, 0.5 * h / sp)), c2);
        if (norm(um) < 1e-3 * mean_speed) break;
        p = add(p, mul(um, h / norm(um)));
        const double d = dot(p, way), ea = dot(p, e1), eb = dot(p, e2);
        if (d > d1 - 0.5 * fine || d < d0 + (enclosure.empty() ? 0 : 0.5 * fine) || ea < a0 + 0.5 * fine || ea > a1 - 0.5 * fine ||
            eb < b0 + 0.5 * fine || eb > b1 - 0.5 * fine)
          break;
      }
      if (line.size() < 3) continue;
      res.streamlines.push_back(std::move(line));
      res.streamline_speed.push_back(std::move(speed));
      res.streamline_temperature.push_back(std::move(temp));
    }
  };

  // ---- the flow alone (simpleFoam, laminar), on the air's mesh
  const air::Air a = air::properties(ambient);
  json fan_json;
  {
    fs::create_directories(flow / "constant");
    fs::copy(air_mesh, flow / "constant" / "polyMesh", fs::copy_options::recursive);
    const int iters = cfd.value("flow_iterations", 1000);
    put(flow / "system" / "controlDict", "dictionary", "controlDict",
        "application simpleFoam; startFrom startTime; startTime 0; stopAt endTime; endTime " + std::to_string(iters) +
            "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(iters) +
            "; writeFormat ascii; writePrecision 10; timeFormat general; runTimeModifiable false;\n");
    put(flow / "system" / "fvSchemes", "dictionary", "fvSchemes",
        "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,U) bounded Gauss "
        "upwind; div((nuEff*dev2(T(grad(U))))) Gauss linear; } laplacianSchemes { default Gauss linear corrected; } "
        "interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
    put(flow / "system" / "fvSolution", "dictionary", "fvSolution",
        "solvers { p { solver GAMG; smoother GaussSeidel; tolerance 1e-8; relTol 0.01; } U { solver smoothSolver; smoother GaussSeidel; "
        "tolerance 1e-8; relTol 0.1; } }\nSIMPLE { nNonOrthogonalCorrectors 1; consistent no; residualControl { p 1e-4; U 1e-5; } }\n"
        "relaxationFactors { fields { p 0.5; } equations { U 0.7; } }\n");
    put(flow / "constant" / "transportProperties", "dictionary", "transportProperties", "transportModel Newtonian; nu " + num(a.nu) + ";\n");
    put(flow / "constant" / "turbulenceProperties", "dictionary", "turbulenceProperties", "simulationType laminar;\n");
    // The blowers' openings: the room's air out into each blower at its inlet, back at its outlet, at its flow now.
    auto blower_u = [&] {
      std::ostringstream u;
      u.precision(10);
      for (size_t k = 0; k < blowers.size(); ++k)
        u << "\n  blower" << k << "_in { type flowRateOutletVelocity; volumetricFlowRate constant " << blowers[k].Q << "; value uniform (0 0 0); }"
          << "\n  blower" << k << "_out { type flowRateInletVelocity; volumetricFlowRate constant " << blowers[k].Q << "; value uniform (0 0 0); }";
      return u.str();
    };
    // Each blower's flow moved toward where its curve meets the pressure between its outlet and its inlet (the box's
    // resistance at that flow), relaxed, and written into the velocity file the solver goes on from. True while one moves.
    const auto own_air = read_labels(read_text_file(air_mesh / "owner"));
    size_t n_cells = 0;
    for (int o : own_air) n_cells = std::max(n_cells, size_t(o) + 1);
    const auto patches_air = read_boundary(read_text_file(air_mesh / "boundary"));
    auto cell_mean = [&](const std::vector<double>& cells, const std::string& patch) {
      for (const auto& pt : patches_air)
        if (pt.name == patch && pt.faces > 0) {
          double sum = 0;
          for (size_t f = pt.start; f < pt.start + pt.faces; ++f) sum += cells[size_t(own_air[f])];
          return sum / double(pt.faces);
        }
      return 0.0;
    };
    auto blowers_step = [&](const fs::path& at_dir, const std::string& p_name) {
      if (blowers.empty()) return false;
      const auto pc = read_internal(read_text_file(at_dir / p_name), n_cells, 1);
      std::string U = read_text_file(at_dir / "U");
      bool moving = false;
      for (size_t k = 0; k < blowers.size(); ++k) {
        Blower& b = blowers[k];
        const std::string name = "blower" + std::to_string(k);
        b.dp = (cell_mean(pc, name + "_out") - cell_mean(pc, name + "_in")) * a.rho;
        const double next = std::max(0.02 * b.fan.Qmax, b.Q + 0.6 * (air::flow_at(b.fan, b.dp) - b.Q));
        moving = moving || std::fabs(next - b.Q) > 0.02 * b.Q;
        b.Q = next;
        U = set_flow_rate(set_flow_rate(U, name + "_in", b.Q), name + "_out", b.Q);
      }
      write_text_file(at_dir / "U", U);
      return moving;
    };
    std::string u_bc, p_bc;
    if (!enclosure.empty()) {
      // Open to the room on every side (its total pressure the room's), each fan a jump in pressure across its disk: its
      // curve against the mean speed through the disk, kinematic (Pa / rho).
      u_bc = "open { type pressureInletOutletVelocity; value uniform (0 0 0); }\n  \"fan[0-9]+_(in|out)\" { type cyclic; }" + blower_u();
      p_bc = "open { type totalPressure; p0 uniform 0; value uniform 0; }";
      for (size_t k = 0; k < disks.size(); ++k) {
        const air::Fan& fan = disks[k].fan;
        const double A = disk_area[k] > 0 ? disk_area[k] : kPi * std::pow(disks[k].radius * 1e-3, 2);
        std::ostringstream t;
        const int n = fan.curve.size() >= 2 ? int(fan.curve.size()) : 2;
        for (int i = 0; i < n; ++i) {
          const double Q = fan.curve.size() >= 2 ? fan.curve[size_t(i)][0] : fan.Qmax * i / (n - 1);
          t << " (" << num(Q / A) << " " << num(fan.pressure(Q) / a.rho) << ")";
        }
        const std::string f = "fan" + std::to_string(k);
        p_bc += "\n  " + f + "_in { type fan; patchType cyclic; uniformJump true; jumpTable table (" + t.str() + " ); value uniform 0; }\n  " + f +
                "_out { type fan; patchType cyclic; value uniform 0; }";
      }
    } else if (fan_load) {
      const air::Fan fan = air::fan_from(fan_load->def.value("fan", json("80x25")));
      const int count = fan_load->def.value("count", 1);
      // The fan's curve in kinematic pressure (Pa / rho) against the duct's flow, count fans side by side.
      std::ostringstream curve;
      curve << "(";
      const int n = fan.curve.size() >= 2 ? int(fan.curve.size()) : 2;
      for (int i = 0; i < n; ++i) {
        const double Q = fan.curve.size() >= 2 ? fan.curve[size_t(i)][0] : fan.Qmax * i / (n - 1);
        curve << " (" << num(Q * count) << " " << num(fan.pressure(Q) / a.rho) << ")";
      }
      curve << " )\n";
      write_text_file(flow / "constant" / "fanCurve", curve.str());
      u_bc = "inlet { type pressureInletOutletVelocity; value uniform (0 0 0); }\n  outlet { type inletOutlet; inletValue uniform (0 0 0); value uniform (0 0 0); }";
      p_bc = "inlet { type fanPressure; file \"$FOAM_CASE/constant/fanCurve\"; outOfBounds clamp; direction in; p0 uniform 0; value uniform 0; }\n"
             "  outlet { type fixedValue; value uniform 0; }";
      fan_json = {{"name", fan_load->name}, {"fan", fan.to_json()}, {"fans", count}};
    } else {
      // A stream is open air: the box's sides slip (no-slip sides grew their own layers and sped up the air past the part).
      const double U = stream->def.value("velocity", 1.0);
      u_bc = "inlet { type fixedValue; value uniform " + vec(mul(way, U)) + "; }\n  outlet { type inletOutlet; inletValue uniform (0 0 0); value uniform (0 0 0); }"
             "\n  walls { type slip; }";
      p_bc = "inlet { type zeroGradient; }\n  outlet { type fixedValue; value uniform 0; }";
    }
    put(flow / "0" / "U", "volVectorField", "U",
        "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\nboundaryField\n{\n  \".*\" { type noSlip; }\n  " + u_bc + "\n}\n");
    put(flow / "0" / "p", "volScalarField", "p",
        "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type zeroGradient; }\n  " + p_bc + "\n}\n");
    auto mean = [](const std::vector<double>& v) {
      double m = 0;
      for (double x : v) m += x;
      return v.empty() ? 0.0 : m / double(v.size());
    };
    // What the flow did: each fan's flow and pressure and the air through the box (an enclosure), or the duct's flow.
    auto flow_report = [&](const std::string& phi, const std::string& p, int iterations, bool converged) {
      if (!enclosure.empty()) {
        json list = json::array();
        for (size_t k = 0; k < disks.size(); ++k) {
          const std::string f = "fan" + std::to_string(k);
          double Q = 0;
          for (double v : read_patch(phi, f + "_in")) Q += v;  // along the disk's normal: the way the fan blows
          const double jump = (mean(read_patch(p, f + "_out")) - mean(read_patch(p, f + "_in"))) * a.rho;
          if (Q < 0) run.warnings.push_back("fan \"" + disks[k].load->name + "\": the air went through it backwards (" + num(-Q * 3600) + " m3/h)");
          list.push_back({{"name", disks[k].load->name}, {"fan", disks[k].fan.to_json()}, {"flow_m3h", Q * 3600}, {"flow_cfm", Q / 4.719474e-4},
                          {"pressure_Pa", jump}, {"disk", disks[k].plate ? json{{"centre", disks[k].centre}, {"normal", disks[k].normal}, {"x", disks[k].x},
                                                                              {"width_mm", disks[k].width}, {"height_mm", disks[k].height}, {"faces", disk_faces[k]}}
                                                                       : json{{"centre", disks[k].centre}, {"normal", disks[k].normal}, {"radius_mm", disks[k].radius},
                                                                              {"faces", disk_faces[k]}}}});
        }
        for (size_t k = 0; k < blowers.size(); ++k) {
          const Blower& b = blowers[k];
          const double room = air::flow_at(b.fan, b.dp);  // what its curve gives at the pressure the last round found
          if (std::fabs(room - b.Q) > 0.1 * std::max(b.Q, 1e-9))
            run.warnings.push_back("blower \"" + b.load->name + "\": its flow had not settled on its curve (" + num(b.Q * 3600) + " m3/h, the curve " +
                                   num(room * 3600) + " m3/h at " + num(b.dp) + " Pa): more flow iterations (Normal) settle it");
          list.push_back({{"name", b.load->name}, {"type", "blower"}, {"fan", b.fan.to_json()}, {"flow_m3h", b.Q * 3600}, {"flow_cfm", b.Q / 4.719474e-4},
                          {"pressure_Pa", b.dp}, {"inlet", {{"area_mm2", b.in_area}, {"faces", blower_faces[k][0]}, {"normal", b.in_n}}},
                          {"outlet", {{"area_mm2", b.out_area}, {"faces", blower_faces[k][1]}, {"normal", b.out_n},
                                      {"speed_m_s", b.out_area > 0 ? b.Q / (b.out_area * 1e-6) : 0.0}}}});
        }
        double in = 0, out = 0;
        for (double v : read_patch(phi, "open")) (v > 0 ? out : in) += std::fabs(v);
        fan_json = {{"fans", list}, {"vents", {{"air_in_m3h", in * 3600}, {"air_out_m3h", out * 3600}}}, {"iterations", iterations}, {"converged", converged}};
      } else {
        double Q = 0;
        for (double v : read_patch(phi, "inlet")) Q -= v;
        const double pmean = mean(read_patch(p, "inlet"));
        json flow_json = {{"flow_m3h", Q * 3600}, {"flow_cfm", Q / 4.719474e-4}, {"inlet_static_Pa", pmean * a.rho}, {"pressure_Pa", pmean * a.rho},
                          {"iterations", iterations}, {"converged", converged}, {"inlet_velocity_m_s", Q / ((a1 - a0) * (b1 - b0) * 1e-6)}};
        if (!fan_json.is_null())
          for (const auto& [k, v] : flow_json.items()) fan_json[k] = v;
        else
          fan_json = flow_json;
      }
    };
    std::string last, phi, p;
    const auto t_flow = std::chrono::steady_clock::now();
    // A sealed box with no fans and no gravity: its air stands still and only conducts.
    const bool still = sealed && disks.empty() && !buoyant;
    if (still) {
      phi = "FoamFile { version 2.0; format ascii; class surfaceScalarField; object phi; }\ndimensions [0 3 -1 0 0 0 0];\n"
            "internalField uniform 0;\nboundaryField\n{\n  \".*\" { type calculated; value uniform 0; }\n}\n";
    } else if (!buoyant) {
      report(0.32, "Solving the air's flow (simpleFoam)");
      std::string log;
      if (blowers.empty()) {
        log = foam_run("simpleFoam", {}, flow, 0.32, 0.45);
      } else {
        // With blowers: in rounds, each blower's flow set again between them from its curve against the pressure it meets,
        // until none moves (then the rest of the iterations at that flow).
        const int rounds = std::max(2, cfd.value("blower_rounds", 8));
        bool moving = true;
        for (int r = 0, done = 0; r < rounds; ++r) {
          const int upto = r + 1 == rounds || !moving ? iters : std::max(done + 1, iters * (r + 1) / rounds);
          put(flow / "system" / "controlDict", "dictionary", "controlDict",
              "application simpleFoam; startFrom latestTime; startTime 0; stopAt endTime; endTime " + std::to_string(upto) +
                  "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(upto - done) +
                  "; purgeWrite 1; writeFormat ascii; writePrecision 10; timeFormat general; runTimeModifiable false;\n");
          log = foam_run("simpleFoam", {}, flow, 0.32 + 0.13 * done / iters, 0.32 + 0.13 * upto / iters);
          done = upto;
          if (done >= iters || log.find("SIMPLE solution converged") != std::string::npos) break;
          moving = blowers_step(flow / latest_time(flow), "p");
        }
      }
      last = latest_time(flow);
      if (last.empty() || last == "0") throw Error("cfd: simpleFoam wrote no flow");
      const bool converged = log.find("SIMPLE solution converged") != std::string::npos;
      if (!converged) run.warnings.push_back("the air's flow had not settled after " + std::to_string(iters) + " iterations (cfd.flow_iterations)");
      phi = read_text_file(flow / last / "phi"), p = read_text_file(flow / last / "p");
      flow_report(phi, p, std::stoi(last), converged);
    }
    timing["flow"] = seconds_since(t_flow);
    if (air_only) {
      // ---- the heat in an enclosure: the parts in CalculiX (sim/fea.cpp, one bonded mesh: touching parts conduct as one),
      // the air's temperature in OpenFOAM on the frozen flow (scalarTransportFoam, one linear solve), back and forth: the
      // parts' surfaces fix the air's walls; the heat each face then gives the air is its film, against the room's
      // temperature (or, where the air is the warmer, against the air next to it), until the temperatures settle.
      const fs::path heat = buoyant ? flow : dir.p / "heat";  // where the air's temperature is solved
      std::string U;
      if (!buoyant) {
        fs::create_directories(heat / "constant");
        fs::copy(air_mesh, heat / "constant" / "polyMesh", fs::copy_options::recursive);
        U = still ? "FoamFile { version 2.0; format ascii; class volVectorField; object U; }\ndimensions [0 1 -1 0 0 0 0];\n"
                    "internalField uniform (0 0 0);\nboundaryField\n{\n  \".*\" { type fixedValue; value uniform (0 0 0); }\n}\n"
                  : read_text_file(flow / last / "U");
        for (const char* from : {"pressureInletOutletVelocity", "fanPressure"})
          for (size_t at = U.find(from); at != std::string::npos; at = U.find(from)) U.replace(at, std::strlen(from), "fixedValue");
        fs::create_directories(heat / "0");
        write_text_file(heat / "0" / "U", U);
        write_text_file(heat / "0" / "phi", phi);
        put(heat / "constant" / "transportProperties", "dictionary", "transportProperties", "DT DT [0 2 -1 0 0 0 0] " + num(a.alpha) + ";\n");
        put(heat / "system" / "controlDict", "dictionary", "controlDict",
            "application scalarTransportFoam; startFrom startTime; startTime 0; stopAt endTime; endTime 1; deltaT 1; writeControl timeStep; "
            "writeInterval 1; writeFormat ascii; writePrecision 10; timeFormat general; runTimeModifiable false;\n");
        put(heat / "system" / "fvSchemes", "dictionary", "fvSchemes",
            "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,T) bounded Gauss " + heat_scheme + "; } "
            "laplacianSchemes { default Gauss linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
        put(heat / "system" / "fvSolution", "dictionary", "fvSolution",
            "solvers { T { solver PBiCGStab; preconditioner DILU; tolerance 1e-10; relTol 0; } }\nSIMPLE { nNonOrthogonalCorrectors 1; }\n");
      } else {
        // Boussinesq: the air's density falls with its temperature (beta = 1 / T) under gravity (settings.gravity, down).
        const V down = unit(st.value("gravity", V{0, 0, -1}));
        put(flow / "constant" / "g", "uniformDimensionedVectorField", "g", "dimensions [0 1 -2 0 0 0 0];\nvalue " + vec(mul(down, 9.80665)) + ";\n");
        put(flow / "constant" / "transportProperties", "dictionary", "transportProperties",
            "transportModel Newtonian; nu " + num(a.nu) + "; beta " + num(a.beta) + "; TRef " + num(T0) + "; Pr " + num(a.Pr) + "; Prt 0.85;\n");
        put(flow / "system" / "fvSchemes", "dictionary", "fvSchemes",
            "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,U) bounded Gauss upwind; "
            "div(phi,T) bounded Gauss " + heat_scheme + "; div((nuEff*dev2(T(grad(U))))) Gauss linear; } laplacianSchemes { default Gauss linear corrected; } "
            "interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
        put(flow / "system" / "fvSolution", "dictionary", "fvSolution",
            "solvers { p_rgh { solver GAMG; smoother GaussSeidel; tolerance 1e-8; relTol 0.01; } \"(U|T)\" { solver PBiCGStab; preconditioner DILU; "
            "tolerance 1e-8; relTol 0.1; } }\nSIMPLE { momentumPredictor yes; nNonOrthogonalCorrectors 0; pRefCell 0; pRefValue 0; }\n"
            "relaxationFactors { fields { p_rgh 0.7; } equations { U 0.3; T 0.5; } }\n");
        std::string pfan;  // the fans' jumps, on p_rgh
        for (size_t k = 0; k < disks.size(); ++k) {
          const air::Fan& fan = disks[k].fan;
          const double A = disk_area[k] > 0 ? disk_area[k] : kPi * std::pow(disks[k].radius * 1e-3, 2);
          std::ostringstream t;
          const int n = fan.curve.size() >= 2 ? int(fan.curve.size()) : 2;
          for (int i = 0; i < n; ++i) {
            const double Q = fan.curve.size() >= 2 ? fan.curve[size_t(i)][0] : fan.Qmax * i / (n - 1);
            t << " (" << num(Q / A) << " " << num(fan.pressure(Q) / a.rho) << ")";
          }
          const std::string f = "fan" + std::to_string(k);
          pfan += "  " + f + "_in { type fan; patchType cyclic; uniformJump true; jumpTable table (" + t.str() + " ); value uniform 0; }\n  " + f +
                  "_out { type fan; patchType cyclic; value uniform 0; }\n";
        }
        const std::string cyc = "  \"fan[0-9]+_(in|out)\" { type cyclic; }\n";
        put(flow / "0" / "U", "volVectorField", "U",
            "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\nboundaryField\n{\n  \".*\" { type noSlip; }\n  open { type "
            "pressureInletOutletVelocity; value uniform (0 0 0); }\n" + cyc + blower_u() + "\n}\n");
        put(flow / "0" / "p_rgh", "volScalarField", "p_rgh",
            "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type fixedFluxPressure; value uniform 0; }\n  open { type "
            "fixedValue; value uniform 0; }\n" + pfan + "}\n");
        put(flow / "0" / "p", "volScalarField", "p",
            "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type calculated; value uniform 0; }\n" + cyc + "}\n");
        put(flow / "0" / "alphat", "volScalarField", "alphat",
            "dimensions [0 2 -1 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type calculated; value uniform 0; }\n" + cyc + "}\n");
        fs::remove(flow / "0" / "U.orig");
      }
      // The parts' walls: each face's centre, area, normal, the cell behind it and its distance to the face.
      const auto P = read_points(read_text_file(air_mesh / "points"));
      const auto F = read_faces(read_text_file(air_mesh / "faces"));
      const auto own = read_labels(read_text_file(air_mesh / "owner"));
      const auto patches = read_boundary(read_text_file(air_mesh / "boundary"));
      const Cells air_cells = read_cells(air_mesh);
      std::vector<int> fea_body(bodies.size(), -1);  // the study's body index in CalculiX
      json fea_bodies = json::array();
      for (size_t i = 0; i < bodies.size(); ++i)
        if (!is_air[i]) fea_body[i] = int(fea_bodies.size()), fea_bodies.push_back(bodies[i]);
      struct Wall {
        int body;
        V centre;  // mm
        double area, k_d;  // m2, W/m2K: the air's conductance from the face to the cell behind it
        size_t cell;
        V normal;  // out of the air, into the part
      };
      std::vector<std::pair<std::string, std::vector<size_t>>> wall_patches;  // patch -> indices into walls
      std::vector<Wall> walls;
      for (const auto& pt : patches) {
        int body = -1;
        for (size_t i = 0; i < region.size(); ++i)
          if (!region[i].empty() && (pt.name == region[i] || pt.name.rfind(region[i] + "_", 0) == 0)) body = fea_body[i];
        if (body < 0) continue;
        wall_patches.push_back({pt.name, {}});
        for (size_t f = pt.start; f < pt.start + pt.faces; ++f) {
          V c{0, 0, 0}, area{0, 0, 0};
          for (int k : F[f]) c = add(c, P[size_t(k)]);
          c = mul(c, 1.0 / double(F[f].size()));
          for (size_t k = 0; k < F[f].size(); ++k)
            area = add(area, mul(cross(sub(P[size_t(F[f][k])], c), sub(P[size_t(F[f][(k + 1) % F[f].size()])], c)), 0.5));
          const double A = norm(area);
          const size_t cell = size_t(own[f]);
          const double d = A > 0 ? std::fabs(dot(sub(c, air_cells.centre[cell]), mul(area, 1 / A))) : 0;
          wall_patches.back().second.push_back(walls.size());
          walls.push_back({body, mul(c, 1e3), A, a.k / std::max(d, 1e-6), cell, A > 0 ? mul(area, 1 / A) : V{0, 0, 1}});
        }
      }
      if (walls.empty()) throw Error("cfd: the air's mesh has no walls on the parts");
      // Where the air leaves: an enclosure's room margin (open all round), a duct's outlet (its inlet at the room's temperature).
      const std::string leave = enclosure.empty() ? "outlet" : "open";
      std::string T_other = "  \".*\" { type zeroGradient; }\n  " + leave + " { type inletOutlet; inletValue uniform " + num(T0) + "; value uniform " +
                            num(T0) + "; }\n  \"fan[0-9]+_(in|out)\" { type cyclic; }\n" +
                            (enclosure.empty() ? "  inlet { type fixedValue; value uniform " + num(T0) + "; }\n" : std::string());
      std::vector<double> Tair;               // K, each air cell, the last pass
      std::vector<double> Twall(walls.size());  // degC
      std::vector<std::unique_ptr<Nearest>> near_face;
      std::vector<std::vector<size_t>> face_ids;  // per body: indices into the faces CalculiX gives
      std::vector<std::vector<V>> face_centres;
      std::vector<std::unique_ptr<Nearest>> near_wall;
      std::vector<size_t> wall_face, face_wall;  // each air wall face's part face, and each part face's air wall face (or npos)
      std::vector<std::vector<V>> wall_centres;
      std::vector<std::vector<size_t>> wall_ids;
      int passes = 0, buoyant_done = 0;
      double air_seconds = 0;
      // The heat the air carries out of the room's margin (W): out where it flows out, at the temperature of the cell it
      // leaves (upwind; where it flows in it is the room's).
      std::vector<size_t> open_cell;
      for (const auto& pt : patches)
        if (pt.name == leave)
          for (size_t f = pt.start; f < pt.start + pt.faces; ++f) open_cell.push_back(size_t(own[f]));
      auto air_leaving = [&](const std::string& phi_text, const std::vector<double>& T) {
        const auto flux = read_patch(phi_text, leave);
        double q = 0;
        for (size_t k = 0; k < flux.size() && k < open_cell.size(); ++k)
          if (flux[k] > 0) q += a.rho * a.cp * flux[k] * (T[open_cell[k]] - T0);
        return q;
      };
      std::vector<double> h0;  // each face's film once settled on (W/m2K)
      std::vector<double> target, last_r;  // the temperature each film should work against; the last pass's residual
      std::vector<double> last_xt;              // robin: the last pass's targets
      std::vector<std::vector<double>> dR, dX;  // robin: the passes' changes of residual and target (IQN-ILS)
      // cfd.coupling: "robin" (each face's film the conductance to the air cell beside it, against that cell's temperature:
      // what the air takes at that wall, so the two agree as the temperatures settle; stable), or "film" (a film fixed after
      // two passes against the room, its sink moved: fast past a fan, but it swung and never agreed in enclosed air).
      const std::string coupling = cfd.value("coupling", std::string("robin"));
      if (coupling != "robin" && coupling != "film") throw Error("cfd.coupling is robin or film");
      const bool robin = coupling == "robin";
      double omega = robin ? 1.0 : 0.7;
      const AirFilms films = [&](std::vector<AirFace>& faces, int pass) {
        if (pass == 0) {
          h0.assign(faces.size(), 0.0), target.assign(faces.size(), 0.0);
          face_ids.assign(fea_bodies.size(), {}), face_centres.assign(fea_bodies.size(), {});
          wall_ids.assign(fea_bodies.size(), {}), wall_centres.assign(fea_bodies.size(), {});
          for (size_t k = 0; k < faces.size(); ++k) face_ids[size_t(faces[k].body)].push_back(k), face_centres[size_t(faces[k].body)].push_back(faces[k].centre);
          for (size_t w = 0; w < walls.size(); ++w) wall_ids[size_t(walls[w].body)].push_back(w), wall_centres[size_t(walls[w].body)].push_back(walls[w].centre);
          for (size_t b = 0; b < fea_bodies.size(); ++b) {
            near_face.push_back(std::make_unique<Nearest>(face_centres[b], coarse));
            near_wall.push_back(std::make_unique<Nearest>(wall_centres[b], coarse));
          }
          // Each wall face of the air to the nearest face of its part that looks at the air there, and back: by nearness
          // alone a thin wall's (a 1 mm frame, a board) other side, insulated or warmer, was taken as often as not, and the
          // films gave the air several times what it took.
          wall_face.assign(walls.size(), Nearest::npos);
          face_wall.assign(faces.size(), Nearest::npos);
          // A part face whose outside lies beyond the air's box (a plate crossing a duct wall to wall) has no air.
          std::vector<char> in_box(faces.size(), 1);
          for (size_t k = 0; k < faces.size(); ++k) {
            const V q = add(faces[k].centre, mul(faces[k].normal, 0.25 * fine));
            const double d = dot(q, way), ea = dot(q, e1), eb = dot(q, e2);
            in_box[k] = d > d0 && d < d1 && ea > a0 && ea < a1 && eb > b0 && eb < b1;
          }
          for (size_t w = 0; w < walls.size(); ++w) {
            const size_t b = size_t(walls[w].body);
            if (face_ids[b].empty()) continue;
            size_t i = (*near_face[b])(walls[w].centre, [&](size_t n) {
              return in_box[face_ids[b][n]] && dot(faces[face_ids[b][n]].normal, walls[w].normal) < -0.3;
            });
            if (i == Nearest::npos) i = (*near_face[b])(walls[w].centre);
            wall_face[w] = face_ids[b][i];
          }
          for (size_t k = 0; k < faces.size(); ++k) {
            const size_t b = size_t(faces[k].body);
            if (wall_ids[b].empty()) continue;
            if (!in_box[k]) continue;
            const size_t i = (*near_wall[b])(faces[k].centre, [&](size_t n) { return dot(faces[k].normal, walls[wall_ids[b][n]].normal) < -0.3; });
            if (i != Nearest::npos) face_wall[k] = wall_ids[b][i];
          }
        }
        ++passes;
        const auto t_pass = std::chrono::steady_clock::now();
        struct Clock {
          std::chrono::steady_clock::time_point t;
          double& sum;
          ~Clock() { sum += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }
        } clock{t_pass, air_seconds};
        report(std::min(0.9, 0.45 + 0.02 * pass), "The air's temperature, pass " + std::to_string(pass + 1));
        // The walls at the parts' temperatures: each wall face the nearest face CalculiX solved.
        for (size_t w = 0; w < walls.size(); ++w) {
          const size_t b = size_t(walls[w].body);
          Twall[w] = wall_face[w] == Nearest::npos ? ambient : faces[wall_face[w]].T;
        }
        std::ostringstream bc;
        bc.precision(10);
        bc << T_other;
        // A blower gives back the air it took: at its outlet, the mean temperature of the air at its inlet (the last pass's).
        for (size_t k = 0; k < blowers.size(); ++k) {
          const std::string name = "blower" + std::to_string(k);
          const double Tin = Tair.empty() ? T0 : cell_mean(Tair, name + "_in");
          bc << "  " << name << "_out { type fixedValue; value uniform " << Tin << "; }\n";
        }
        for (const auto& [name, ids] : wall_patches) {
          bc << "  " << name << " { type fixedValue; value nonuniform List<scalar> " << ids.size() << " (";
          for (size_t w : ids) bc << " " << Twall[w] + 273.15;
          bc << " ); }\n";
        }
        if (!buoyant) {
          put(heat / "0" / "T", "volScalarField", "T", "dimensions [0 0 0 1 0 0 0];\ninternalField uniform " + num(T0) + ";\nboundaryField\n{\n" + bc.str() + "}\n");
          foam_run("scalarTransportFoam", {}, heat, std::min(0.9, 0.45 + 0.02 * pass));
          Tair = read_internal(read_text_file(heat / "1" / "T"), air_cells.centre.size(), 1);
        } else {
          // Going on from the last pass's flow and temperatures, the walls at the parts' new ones.
          const std::string now = latest_time(flow);
          std::ostringstream internal;
          internal.precision(10);
          if (Tair.empty()) {
            internal << "uniform " << T0;
          } else {
            internal << "nonuniform List<scalar> " << Tair.size() << " (";
            for (double v : Tair) internal << " " << v;
            internal << " )";
          }
          put(flow / now / "T", "volScalarField", "T", "dimensions [0 0 0 1 0 0 0];\ninternalField " + internal.str() + ";\nboundaryField\n{\n" + bc.str() + "}\n");
          // In chunks until the heat the walls give the air leaves the room's margin (within 5 %): before that, the warm air
          // is still filling the box and the films would be the box's warming up, not its steady state.
          for (int chunk = 0; chunk < (sealed ? 1 : cfd.value("buoyant_chunks", 12)); ++chunk) {
            const int more = pass == 0 && chunk == 0 ? cfd.value("buoyant_first", 600) : cfd.value("buoyant_pass", 200);
            buoyant_done += more;
            put(flow / "system" / "controlDict", "dictionary", "controlDict",
                "application buoyantBoussinesqSimpleFoam; startFrom latestTime; startTime 0; stopAt endTime; endTime " + std::to_string(buoyant_done) +
                    "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(more) +
                    "; purgeWrite 1; writeFormat ascii; writePrecision 10; timeFormat general; runTimeModifiable false;\n");
            foam_run("buoyantBoussinesqSimpleFoam", {}, flow, std::min(0.9, 0.45 + 0.02 * pass));
            const std::string now = latest_time(flow);
            const bool blowing = blowers_step(flow / now, "p");  // the blowers' flows against the pressures now
            Tair = read_internal(read_text_file(flow / now / "T"), air_cells.centre.size(), 1);
            double given = 0;
            for (size_t w = 0; w < walls.size(); ++w) given += walls[w].k_d * (Twall[w] + 273.15 - Tair[walls[w].cell]) * walls[w].area;
            const double carried = air_leaving(read_text_file(flow / now / "phi"), Tair);
            if (std::fabs(carried - given) <= 0.05 * std::fabs(given) + 1e-3 && !blowing) break;
          }
        }
        // Each wall face's heat into the air, gathered onto the faces CalculiX has.
        std::vector<double> qA(faces.size(), 0), A(faces.size(), 0), kdA(faces.size(), 0), TcA(faces.size(), 0);
        for (size_t w = 0; w < walls.size(); ++w) {
          const size_t b = size_t(walls[w].body);
          if (face_ids[b].empty()) continue;
          const size_t k = wall_face[w];
          if (k == Nearest::npos) continue;
          const double Tc = Tair[walls[w].cell] - 273.15;
          qA[k] += walls[w].k_d * (Twall[w] - Tc) * walls[w].area;
          A[k] += walls[w].area, kdA[k] += walls[w].k_d * walls[w].area, TcA[k] += walls[w].k_d * Tc * walls[w].area;
        }
        for (size_t k = 0; k < faces.size(); ++k) {
          AirFace& f = faces[k];
          double q, kd, Tc;
          if (A[k] > 0) {
            // Tc weighted by each wall face's conductance: kd (T - Tc) is then exactly the heat those faces give the air.
            q = qA[k] / A[k], kd = kdA[k] / A[k], Tc = TcA[k] / kdA[k];
          } else {  // smaller than the air's cells there: the nearest wall face's, if the air is there at all
            const size_t w = face_wall[k];
            if (w == Nearest::npos || norm(sub(walls[w].centre, f.centre)) > 1.5 * coarse) {
              // No air beside it (a sealed box's outside, a gap too narrow for the cells): insulated.
              f.h = 0, f.sink = ambient, f.q_air = 0;
              continue;
            }
            Tc = Tair[walls[w].cell] - 273.15, kd = walls[w].k_d, q = kd * (Twall[w] - Tc);
          }
          f.q_air = q;
          // The first passes: the face's film against the room's temperature (against the air next to it where that is the
          // warmer). Then that film stays and only the temperature it works against moves (relaxed), so that the film gives
          // the heat the air took: switching faces between the two, or the films chasing the temperatures, kept the
          // passes from settling (a degree up and down after thirty).
          if (robin) {
            f.h = h0[k] = std::clamp(kd, 1.0, 1e6);
            target[k] = Tc;
            if (pass == 0) f.sink = Tc;
          } else if (pass < 2 || h0[k] <= 0) {
            if (f.T - ambient > 0.2 && q > 0) f.h = std::clamp(q / (f.T - ambient), 1.0, 1e4), f.sink = ambient;
            else f.h = std::clamp(kd, 1.0, 1e4), f.sink = Tc;
            if (pass == 1) h0[k] = f.h;
          } else {
            f.h = h0[k];
            target[k] = f.T - q / h0[k];
          }
        }
        // The temperatures the films work against, relaxed by Aitken's factor from the last two passes' residuals.
        if (robin && pass >= 1) {
          // Interface quasi-Newton (IQN-ILS, Degroote et al. 2009): from the passes so far, how the air's temperatures beside
          // the walls answer the sinks given, least squares; the next sinks where that says they agree. One factor for all
          // (Aitken) left a near-adiabatic wall (an insulating frame) creeping a little each pass.
          std::vector<double> x(faces.size()), xt(faces.size()), r(faces.size());
          for (size_t k = 0; k < faces.size(); ++k) x[k] = faces[k].sink, xt[k] = h0[k] > 0 ? target[k] : x[k], r[k] = xt[k] - x[k];
          if (!last_r.empty()) {
            dR.push_back(r), dX.push_back(xt);
            for (size_t k = 0; k < r.size(); ++k) dR.back()[k] -= last_r[k], dX.back()[k] -= last_xt[k];
            if (dR.size() > 20) dR.erase(dR.begin()), dX.erase(dX.begin());
          }
          last_r = r, last_xt = xt;
          std::vector<double> next = xt;
          const size_t m = dR.size();
          if (m > 0) {
            // (V'V + eps) c = -V'r
            std::vector<double> M(m * m), b(m), c(m);
            double tr = 0;
            for (size_t i = 0; i < m; ++i) {
              for (size_t j = 0; j <= i; ++j) {
                double sum = 0;
                for (size_t k = 0; k < r.size(); ++k) sum += dR[i][k] * dR[j][k];
                M[i * m + j] = M[j * m + i] = sum;
              }
              tr += M[i * m + i];
              double sum = 0;
              for (size_t k = 0; k < r.size(); ++k) sum += dR[i][k] * r[k];
              b[i] = -sum;
            }
            for (size_t i = 0; i < m; ++i) M[i * m + i] += 1e-10 * tr + 1e-300;
            // Gaussian elimination with partial pivoting.
            for (size_t col = 0; col < m; ++col) {
              size_t piv = col;
              for (size_t i = col + 1; i < m; ++i)
                if (std::fabs(M[i * m + col]) > std::fabs(M[piv * m + col])) piv = i;
              if (piv != col) {
                for (size_t j = 0; j < m; ++j) std::swap(M[col * m + j], M[piv * m + j]);
                std::swap(b[col], b[piv]);
              }
              for (size_t i = col + 1; i < m; ++i) {
                const double f = M[i * m + col] / M[col * m + col];
                for (size_t j = col; j < m; ++j) M[i * m + j] -= f * M[col * m + j];
                b[i] -= f * b[col];
              }
            }
            for (size_t i = m; i-- > 0;) {
              double sum = b[i];
              for (size_t j = i + 1; j < m; ++j) sum -= M[i * m + j] * c[j];
              c[i] = sum / M[i * m + i];
            }
            for (size_t i = 0; i < m; ++i)
              for (size_t k = 0; k < next.size(); ++k) next[k] += dX[i][k] * c[i];
          }
          for (size_t k = 0; k < faces.size(); ++k)
            if (h0[k] > 0) faces[k].sink = next[k];
        } else if (pass >= 2) {
          std::vector<double> r(faces.size());
          for (size_t k = 0; k < faces.size(); ++k) r[k] = h0[k] > 0 ? target[k] - faces[k].sink : 0.0;
          if (!last_r.empty()) {
            double num = 0, den = 0;
            for (size_t k = 0; k < r.size(); ++k) num += last_r[k] * (r[k] - last_r[k]), den += (r[k] - last_r[k]) * (r[k] - last_r[k]);
            if (den > 0) omega = std::clamp(-omega * num / den, 0.1, robin ? 4.0 : 1.5);
          }
          for (size_t k = 0; k < faces.size(); ++k)
            if (h0[k] > 0) faces[k].sink += omega * r[k];
          last_r = std::move(r);
        }
      };
      json fst = st;
      fst.erase("air");
      fst["cfd"] = cfd;
      fst["bodies"] = fea_bodies;
      const Progress inner = [&](double f, const std::string& phase) { return !progress || progress(0.4 + 0.55 * f, phase); };
      StudyRun solid = run_structural(doc, scene, "thermal", fst, inner, &films);
      // The air: what leaves the box's margin and the heat it carries off; streamlines with its temperatures.
      if (buoyant) {
        const std::string now = latest_time(flow);
        phi = read_text_file(flow / now / "phi");
        U = read_text_file(flow / now / "U");
        flow_report(phi, read_text_file(flow / now / "p_rgh"), buoyant_done, true);
      }
      const auto mass = read_patch(phi, leave);
      double mdot = 0, mt = 0;
      for (size_t k = 0; k < mass.size() && k < open_cell.size(); ++k)
        if (mass[k] > 0) mdot += mass[k], mt += mass[k] * Tair[open_cell[k]];
      const double carried = air_leaving(phi, Tair);
      // How well the air's heat balances: what the walls give it against what leaves the room's margin.
      double given = 0;
      for (size_t w = 0; w < walls.size(); ++w) given += walls[w].k_d * (Twall[w] + 273.15 - Tair[walls[w].cell]) * walls[w].area;
      const double imbalance = std::fabs(given) > 1e-6 ? std::fabs(carried - given) / std::fabs(given) : 0.0;
      if (imbalance > 0.1)
        run.warnings.push_back("the air's heat balance closes to " + std::to_string(int(std::lround(100 * imbalance))) + " %" +
                               (buoyant ? " (warm air rising does not settle fully: a Fine run checks it)" : ""));
      json& to = enclosure.empty() ? fan_json : fan_json["vents"];
      to["outlet_air_C"] = mdot > 0 ? mt / mdot - 273.15 : ambient;
      to["air_rise_C"] = (mdot > 0 ? mt / mdot - 273.15 : ambient) - ambient;
      to["heat_to_air_W"] = carried;
      auto with_air = std::make_shared<FeaResult>(*solid.fea);
      trace(*with_air, air_cells, read_internal(U, air_cells.centre.size(), 3), Tair);
      solid.fea = with_air;
      json summary = solid.summary;
      summary["air"] = "cfd";
      summary["engine"] = "OpenFOAM (snappyHexMesh, simpleFoam, scalarTransportFoam) and CalculiX";
      if (!fins_json.is_null()) fan_json["fins"] = fins_json;
      if (!enclosure.empty()) {
        summary["enclosure"] = enclosure_name(scene, box_walls);
        summary["fans"] = fan_json["fans"];
        summary["vents"] = fan_json["vents"];
        if (!vents_est.is_null()) summary["vents"]["estimate"] = vents_est;
        summary["flow_iterations"] = fan_json["iterations"];
        summary["room_mm"] = {{"size", {d1 - d0, a1 - a0, b1 - b0}}, {"margin", margin}};
      } else {
        if (fan_load) {
          const json& b = summary["bodies"];
          if (const std::string n = scene.node(fan_load->refs.front().body)->name; b.contains(n))
            fan_json["thermal_resistance_C_W"] = (b[n].value("max_temperature_C", ambient) - ambient) / std::max(1e-9, summary.value("heat_W", 0.0));
          summary["fans"] = json::array({fan_json});
        } else {
          summary["stream"] = fan_json;
        }
        summary["duct_mm"] = {{"length", d1 - d0}, {"across", {a1 - a0, b1 - b0}}, {"upstream", up}, {"downstream", down}};
      }
      summary["air_passes"] = passes;
      timing["air_passes"] = air_seconds;
      timing["calculix"] = solid.summary.value("ccx_seconds", 0.0);
      timing["total"] = seconds_since(t_start);
      summary["seconds"] = timing;
      summary["air_balance"] = {{"from_parts_W", given}, {"leaving_W", carried}};
      summary["air_cells"] = air_cells.centre.size();
      summary["cells"] = air_cells.centre.size();
      summary["cell_size_mm"] = fine;
      summary["refinement"] = refinement;
      summary["gap_boxes"] = gap_boxes.size();
      summary["streamlines"] = solid.fea->streamlines.size();
      summary["case_dir"] = run.summary["case_dir"];
      json warnings = solid.summary.value("warnings", json::array());
      for (const auto& w : run.warnings) warnings.push_back(w);
      for (const auto& w : solid.warnings) run.warnings.push_back(w);
      summary["warnings"] = run.warnings;
      solid.summary = summary;
      solid.warnings = run.warnings;
      return solid;
    }
    // The frozen flow for the heat: U as solved, its face fluxes as mass fluxes.
    fs::create_directories(cas / "0" / fluid);
    std::string U = read_text_file(flow / last / "U");
    for (const char* from : {"pressureInletOutletVelocity", "fanPressure"}) {
      for (size_t at = U.find(from); at != std::string::npos; at = U.find(from)) U.replace(at, std::strlen(from), "fixedValue");
    }
    write_text_file(cas / "0" / fluid / "U", U);
    std::string mass = phi;
    {
      const size_t dims = mass.find("[0 3 -1 0 0 0 0]");
      if (dims != std::string::npos) mass.replace(dims, 16, "[1 0 -1 0 0 0 0]");
      // Every real number after the header scaled by the density (counts and brackets stay).
      const size_t body = mass.find("dimensions");
      std::string out = mass.substr(0, body);
      out.reserve(mass.size() + mass.size() / 4);
      bool in_dims = false;  // inside [..]: the dimensions, never scaled
      for (size_t i = body; i < mass.size();) {
        const char c = mass[i];
        if (c == '[') in_dims = true;
        else if (c == ']') in_dims = false;
        const bool start = (std::isdigit(static_cast<unsigned char>(c)) || ((c == '-' || c == '.') && i + 1 < mass.size() && std::isdigit(static_cast<unsigned char>(mass[i + 1])))) &&
                           (i == 0 || std::isspace(static_cast<unsigned char>(mass[i - 1])) || mass[i - 1] == '(');
        if (!start) {
          out += c, ++i;
          continue;
        }
        size_t j = i + 1;
        while (j < mass.size() && (std::isdigit(static_cast<unsigned char>(mass[j])) || mass[j] == '.' || mass[j] == 'e' || mass[j] == 'E' || mass[j] == '-' || mass[j] == '+')) ++j;
        const std::string tok = mass.substr(i, j - i);
        const bool real = tok.find_first_of(".eE") != std::string::npos;
        out += real && !in_dims ? num(std::strtod(tok.c_str(), nullptr) * a.rho) : tok;
        i = j;
      }
      mass = out;
    }
    write_text_file(cas / "0" / fluid / "phi", mass);
  }

  // ---- the heat: conjugate, the flow frozen
  std::map<std::string, Thermal> props;  // conductivity, specific heat; density in rho below
  std::map<std::string, double> rho, power;
  for (size_t i = 0; i < bodies.size(); ++i) {
    Thermal th{50, 490, 0.3};
    double r = 7.85;
    const MaterialChoice c = material_of(doc, scene, bodies[i]);
    bool assumed = true;
    if (!c.id.empty())
      if (const Thermal* t = thermal(c.id)) th = *t, assumed = false;
    if (c.density > 0) r = c.density;
    const json o = st.value("materials", json::object()).value(bodies[i], st.value("materials", json::object()).value("all", json()));
    if (o.is_object() && thermal(o.value("material", std::string()))) {  // a library material for this study (a heatsink's)
      th = *thermal(o.value("material", std::string())), assumed = false;
      if (const Material* lib = material(o.value("material", std::string()))) r = lib->density;
    }
    if (o.is_object()) {
      th.conductivity = o.value("k", th.conductivity), th.specific_heat = o.value("cp", th.specific_heat), r = o.value("density", r);
      assumed = assumed && !o.contains("k");
    }
    if (assumed) run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" has no material with thermal properties: steel assumed");
    props[region[i]] = th, rho[region[i]] = r * 1000;  // kg/m3
  }
  double heat_in = 0;
  for (const Load* l : heats) {
    // Spread over the bodies it names by their volumes.
    std::vector<size_t> on;
    for (const auto& r : l->refs)
      if (const size_t i = size_t(std::find(bodies.begin(), bodies.end(), r.body) - bodies.begin()); i < bodies.size()) on.push_back(i);
    const double P = l->def.value("value", 0.0);
    heat_in += P;
    for (size_t i : on) power[region[i]] += P / double(on.size());
  }
  put(cas / "constant" / "regionProperties", "dictionary", "regionProperties", [&] {
    std::string s = "regions ( fluid (" + fluid + ") solid (";
    for (const auto& r : solids) s += " " + r;
    return s + " ) );\n";
  }());
  put(cas / "constant" / "g", "uniformDimensionedVectorField", "g", "dimensions [0 1 -2 0 0 0 0];\nvalue (0 0 0);\n");
  put(cas / "constant" / fluid / "thermophysicalProperties", "dictionary", "thermophysicalProperties",
      "thermoType { type heRhoThermo; mixture pureMixture; transport const; thermo hConst; equationOfState perfectGas; specie specie; energy "
      "sensibleEnthalpy; }\nmixture { specie { molWeight 28.96; } thermodynamics { Cp " + num(a.cp) + "; Hf 0; } transport { mu " + num(a.mu) +
          "; Pr " + num(a.Pr) + "; } }\n");
  put(cas / "constant" / fluid / "turbulenceProperties", "dictionary", "turbulenceProperties", "simulationType laminar;\n");
  put(cas / "constant" / fluid / "radiationProperties", "dictionary", "radiationProperties", "radiation off; radiationModel none;\n");
  put(cas / "system" / fluid / "fvSchemes", "dictionary", "fvSchemes",
      "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,U) bounded Gauss upwind; "
      "div(phi,K) bounded Gauss upwind; div(phi,h) bounded Gauss " + heat_scheme + "; div(((rho*nuEff)*dev2(T(grad(U))))) Gauss linear; } "
      "laplacianSchemes { default Gauss linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
  put(cas / "system" / fluid / "fvSolution", "dictionary", "fvSolution",
      "solvers { \"rho.*\" { solver PCG; preconditioner DIC; tolerance 0; relTol 0; } p_rgh { solver GAMG; smoother GaussSeidel; tolerance 1e-7; "
      "relTol 0.01; } \"(U|h)\" { solver PBiCGStab; preconditioner DILU; tolerance 1e-9; relTol 0.1; } }\n"
      "SIMPLE { frozenFlow yes; momentumPredictor no; nNonOrthogonalCorrectors 0; pRefCell 0; pRefValue 100000; rhoMin 0.2; rhoMax 2; }\n"
      "relaxationFactors { fields { rho 1; p_rgh 0.7; } equations { U 0.3; h 0.9; } }\n");
  const std::string coupled = " { type compressible::turbulentTemperatureCoupledBaffleMixed; Tnbr T; kappaMethod %K; value uniform " + num(T0) + "; }";
  auto with = [&](std::string s, const char* k) { return s.replace(s.find("%K"), 2, k); };
  auto field = [&](const std::string& reg, const std::string& name, const std::string& cls, const std::string& dims, const std::string& internal,
                   const std::string& bf) {
    write_text_file(cas / "0" / reg / name, header(cls, name, "0/" + reg) + "dimensions " + dims + ";\ninternalField " + internal + ";\nboundaryField\n{\n" + bf + "}\n");
  };
  // The room's air comes in at the ambient temperature: a duct's inlet, or wherever it flows into an enclosure's box.
  const std::string T_in = "inletOutlet; inletValue uniform " + num(T0) + "; value uniform " + num(T0) + "; }\n";
  const std::string fans_cyclic = "  \"fan[0-9]+_(in|out)\" { type cyclic; }\n";
  field(fluid, "T", "volScalarField", "[0 0 0 1 0 0 0]", "uniform " + num(T0),
        "  \".*\" { type zeroGradient; }\n" +
            (enclosure.empty() ? "  inlet { type fixedValue; value uniform " + num(T0) + "; }\n  outlet { type " + T_in : "  open { type " + T_in) +
            "  \"" + fluid + "_to_.*\"" + with(coupled, "fluidThermo") + "\n" + fans_cyclic);
  field(fluid, "p", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000", "  \".*\" { type calculated; value uniform 100000; }\n" + fans_cyclic);
  field(fluid, "p_rgh", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000",
        "  \".*\" { type fixedFluxPressure; value uniform 100000; }\n" +
            std::string(enclosure.empty() ? "  outlet { type fixedValue; value uniform 100000; }\n  inlet { type fixedValue; value uniform 100000; }\n"
                                          : "  open { type fixedValue; value uniform 100000; }\n") +
            fans_cyclic);
  for (const auto& r : solids) {
    const Thermal& th = props[r];
    put(cas / "constant" / r / "thermophysicalProperties", "dictionary", "thermophysicalProperties",
        "thermoType { type heSolidThermo; mixture pureMixture; transport constIso; thermo hConst; equationOfState rhoConst; specie specie; energy "
        "sensibleEnthalpy; }\nmixture { specie { molWeight 50; } transport { kappa " + num(th.conductivity) + "; } thermodynamics { Hf 0; Cp " +
            num(th.specific_heat) + "; } equationOfState { rho " + num(rho[r]) + "; } }\n");
    put(cas / "constant" / r / "radiationProperties", "dictionary", "radiationProperties", "radiation off; radiationModel none;\n");
    put(cas / "system" / r / "fvSchemes", "dictionary", "fvSchemes",
        "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; } laplacianSchemes { default Gauss "
        "linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
    put(cas / "system" / r / "fvSolution", "dictionary", "fvSolution",
        "solvers { \"h.*\" { solver PCG; preconditioner DIC; tolerance 1e-10; relTol 0; } }\nSIMPLE { nNonOrthogonalCorrectors 0; }\n"
        "relaxationFactors { equations { h 1; } }\n");
    if (power[r] > 0)
      put(cas / "system" / r / "fvOptions", "dictionary", "fvOptions",
          "heat { type scalarSemiImplicitSource; active yes; selectionMode all; volumeMode absolute; injectionRateSuSp { h (" + num(power[r]) + " 0); } }\n");
    field(r, "T", "volScalarField", "[0 0 0 1 0 0 0]", "uniform " + num(T0), "  \".*\" { type zeroGradient; }\n  \"" + r + "_to_.*\"" + with(coupled, "solidThermo") + "\n");
    field(r, "p", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000", "  \".*\" { type calculated; value uniform 100000; }\n");
  }
  // In chunks until the parts' temperatures stop moving (a hundredth of a degree over a chunk).
  const int chunk = 150, most = cfd.value("heat_iterations", 3000);
  double before = -1, change = 1e9;
  int done = 0;
  report(0.45, "Solving the heat (chtMultiRegionSimpleFoam)");
  while (done < most) {
    done += chunk;
    put(cas / "system" / "controlDict", "dictionary", "controlDict",
        "application chtMultiRegionSimpleFoam; startFrom latestTime; startTime 0; stopAt endTime; endTime " + std::to_string(done) +
            "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(chunk) +
            "; purgeWrite 1; writeFormat ascii; writePrecision 10; writeCompression off; timeFormat general; timePrecision 8; runTimeModifiable false;\n");
    const std::string log = foam_run("chtMultiRegionSimpleFoam", {}, cas, 0.45 + 0.5 * double(done) / most, 0.45 + 0.5 * double(std::min(done + 1, most)) / most);
    // The hottest the parts got in the chunk's last iteration: each solid region's "Min/max T".
    double hot = -1e300;
    const size_t last_it = log.rfind("\nTime = ");
    for (size_t at = log.find("Solving for solid region", last_it == std::string::npos ? 0 : last_it); at != std::string::npos;
         at = log.find("Solving for solid region", at + 1))
      if (const size_t mm = log.find("Min/max T:", at); mm != std::string::npos) {
        std::istringstream s(log.substr(mm + 10, 80));
        double lo_t = 0, hi_t = 0;
        s >> lo_t >> hi_t;
        hot = std::max(hot, hi_t);
      }
    change = before < 0 ? 1e9 : std::fabs(hot - before);
    before = hot;
    if (change < 0.01) break;
  }
  if (change >= 0.01) run.warnings.push_back("the temperatures were still moving by " + num(change) + " degC over the last " + std::to_string(chunk) + " iterations");

  // ---- results: the parts' temperatures on their surfaces, the air's at the outlet
  report(0.97, "Reading the results");
  const std::string last = latest_time(cas);
  auto res = std::make_shared<FeaResult>();
  res->kind = "thermal";
  res->bodies = bodies;
  res->mesh_size = fine;
  json per_body = json::object();
  double hottest = -1e300;
  V hottest_at{0, 0, 0};
  size_t cells_total = 0;
  for (size_t i = 0; i < bodies.size(); ++i) {
    const std::string& r = region[i];
    if (std::find(solids.begin(), solids.end(), r) == solids.end()) continue;
    const Cells cells = read_cells(cas / "constant" / r / "polyMesh");
    cells_total += cells.centre.size();
    const auto T = read_internal(read_text_file(cas / last / r / "T"), cells.centre.size(), 1);
    std::vector<V> centres_mm;
    for (const auto& c : cells.centre) centres_mm.push_back(mul(c, 1e3));
    const Nearest nearest(centres_mm, 2 * fine);
    // The surface, one node per triangle corner.
    const int base = int(res->nodes.size());
    for (size_t k = 0; k < tri_pts[i].size(); ++k) {
      res->nodes.push_back(tri_pts[i][k]);
      res->temperature.push_back(T[nearest(tri_pts[i][k])] - 273.15);
    }
    for (size_t k = 0; k + 2 < tri_pts[i].size(); k += 3) {
      res->skin.push_back({base + int(k), base + int(k + 1), base + int(k + 2)});
      res->skin_body.push_back(int(i));
    }
    double peak = -1e300, mean = 0, vol = 0;
    size_t at = 0;
    for (size_t c = 0; c < T.size() && c < cells.volume.size(); ++c) {
      if (T[c] > peak) peak = T[c], at = c;
      mean += T[c] * cells.volume[c], vol += cells.volume[c];
    }
    per_body[scene.node(bodies[i])->name] = {{"max_temperature_C", peak - 273.15}, {"mean_temperature_C", vol > 0 ? mean / vol - 273.15 : 0.0},
                                             {"at", centres_mm[at]}, {"conductivity_W_mK", props[r].conductivity}, {"cells", T.size()}};
    if (peak - 273.15 > hottest) hottest = peak - 273.15, hottest_at = centres_mm[at];
  }
  res->displacement.assign(res->nodes.size(), V{0, 0, 0});
  // The air leaving: mass-weighted temperature where it leaves (a duct's outlet, an enclosure's box wherever air flows out),
  // and the heat it carries off.
  {
    const std::string phi = read_text_file(cas / last / fluid / "phi"), T = read_text_file(cas / last / fluid / "T");
    const std::string leave = enclosure.empty() ? "outlet" : "open";
    const auto m = read_patch(phi, leave), t = read_patch(T, leave);
    double mdot = 0, mt = 0, carried = 0;
    for (size_t k = 0; k < m.size() && k < t.size(); ++k) {
      if (m[k] > 0) mdot += m[k], mt += m[k] * t[k];
      carried += m[k] * a.cp * (t[k] - T0);
    }
    const double out_T = mdot > 0 ? mt / mdot - 273.15 : ambient;
    json& to = enclosure.empty() ? fan_json : fan_json["vents"];
    to["outlet_air_C"] = out_T;
    to["air_rise_C"] = out_T - ambient;
    to["heat_to_air_W"] = carried;
    run.summary["to_air_W"] = carried;
    const Cells air_cells = read_cells(cas / "constant" / fluid / "polyMesh");
    cells_total += air_cells.centre.size();
    run.summary["air_cells"] = air_cells.centre.size();

    trace(*res, air_cells, read_internal(read_text_file(cas / "0" / fluid / "U"), air_cells.centre.size(), 3),
          read_internal(read_text_file(cas / last / fluid / "T"), air_cells.centre.size(), 1));
    run.summary["streamlines"] = res->streamlines.size();
  }
  json summary = run.summary;
  summary["air"] = "cfd";
  summary["engine"] = "OpenFOAM (snappyHexMesh, simpleFoam, chtMultiRegionSimpleFoam)";
  summary["case"] = load_case;
  summary["ambient_C"] = ambient;
  summary["max_temperature_C"] = hottest;
  summary["max_temperature_at"] = hottest_at;
  summary["bodies"] = per_body;
  summary["heat_W"] = heat_in;
  summary["cell_size_mm"] = fine;
  summary["cells"] = cells_total;
  summary["heat_iterations"] = done;
  if (!fins_json.is_null()) fan_json["fins"] = fins_json;
  if (!enclosure.empty()) {
    summary["enclosure"] = enclosure_name(scene, box_walls);
    summary["fans"] = fan_json["fans"];
    summary["vents"] = fan_json["vents"];
    if (!vents_est.is_null()) summary["vents"]["estimate"] = vents_est;
    summary["flow_iterations"] = fan_json["iterations"];
    summary["room_mm"] = {{"size", {d1 - d0, a1 - a0, b1 - b0}}, {"margin", margin}};
  } else if (fan_load) {
    if (const auto it = per_body.find(scene.node(fan_load->refs.front().body)->name); it != per_body.end())
      fan_json["thermal_resistance_C_W"] = (it->at("max_temperature_C").get<double>() - ambient) / std::max(1e-9, heat_in);
    summary["fans"] = json::array({fan_json});
  } else {
    summary["stream"] = fan_json;
  }
  if (enclosure.empty()) summary["duct_mm"] = {{"length", d1 - d0}, {"across", {a1 - a0, b1 - b0}}, {"upstream", up}, {"downstream", down}};
  summary["warnings"] = run.warnings;
  run.summary = summary;
  run.t = {0.0};
  run.fea = res;
  return run;
}

}  // namespace opad::sim
