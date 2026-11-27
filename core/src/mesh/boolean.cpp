// Mesh Booleans and plane cuts through Manifold (opad/mesh_edit.hpp). Manifold takes closed, oriented 2-manifolds only, so
// the input is checked first and a refusal says what is wrong; an open mesh is cut by our own clipper instead (no caps).
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <unordered_map>

#include <manifold/manifold.h>

#include "opad/mesh_edit.hpp"
#include "opad/util.hpp"

namespace opad::meshops {
namespace {

// Calls ctx.Cancel() once `cancel` says so, polled every 30 ms while an evaluation runs on this thread.
class Watch {
 public:
  Watch(const Cancel& cancel, manifold::ExecutionContext& ctx) {
    if (!cancel) return;
    m_thread = std::thread([this, cancel, ctx]() mutable {
      while (!m_done.load()) {
        if (cancel()) {
          ctx.Cancel();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
    });
  }
  ~Watch() {
    m_done = true;
    if (m_thread.joinable()) m_thread.join();
  }

 private:
  std::atomic<bool> m_done{false};
  std::thread m_thread;
};

const char* why(manifold::Manifold::Error e) {
  using E = manifold::Manifold::Error;
  switch (e) {
    case E::NonFiniteVertex: return "a vertex is not a finite number";
    case E::NotManifold: return "it is not a closed surface (open or non-manifold edges)";
    case E::VertexOutOfBounds: return "a triangle names a missing vertex";
    case E::ResultTooLarge: return "the result is too large";
    case E::Cancelled: return "cancelled";
    default: return "the mesh is not valid";
  }
}

manifold::Manifold manifold_of(const Mesh& m, manifold::ExecutionContext& ctx) {
  manifold::MeshGL64 gl;
  gl.numProp = 3;
  gl.vertProperties = m.xyz;
  gl.triVerts.assign(m.tri.begin(), m.tri.end());
  gl.Merge();  // vertices a hair apart on open edges (a solid meshed on a copy) become shared
  manifold::Manifold out = ctx.FromMeshGL(gl);
  if (out.Status() == manifold::Manifold::Error::Cancelled) throw Error("cancelled");
  return out;
}

manifold::Manifold to_manifold(const Mesh& m, const char* side, manifold::ExecutionContext& ctx) {
  manifold::Manifold out = manifold_of(m, ctx);
  if (out.Status() != manifold::Manifold::Error::NoError) {
    const Report r = check(m, {false, 0});
    std::string detail;
    if (r.open_edges) detail += ", " + std::to_string(r.open_edges) + " open edges in " + std::to_string(r.holes) + " holes";
    if (r.non_manifold_edges) detail += ", " + std::to_string(r.non_manifold_edges) + " non-manifold edges";
    throw Error(std::string(side) + ": " + why(out.Status()) + detail + ". Repair the mesh first (Repair mesh closes holes and fixes its edges)");
  }
  return out;
}

Mesh from_manifold(const manifold::Manifold& m) {
  const manifold::MeshGL64 gl = m.GetMeshGL64();
  Mesh out;
  const size_t np = gl.numProp;
  const size_t nv = np ? gl.vertProperties.size() / np : 0;
  out.xyz.resize(nv * 3);
  for (size_t v = 0; v < nv; ++v)
    for (int k = 0; k < 3; ++k) out.xyz[v * 3 + k] = gl.vertProperties[v * np + k];
  out.tri.assign(gl.triVerts.begin(), gl.triVerts.end());
  // Property seams split vertices (mergeFromVert -> mergeToVert): one vertex again, so the mesh is welded.
  if (!gl.mergeFromVert.empty()) {
    std::vector<uint32_t> to(nv);
    for (size_t v = 0; v < nv; ++v) to[v] = static_cast<uint32_t>(v);
    for (size_t i = 0; i < gl.mergeFromVert.size(); ++i) to[gl.mergeFromVert[i]] = static_cast<uint32_t>(gl.mergeToVert[i]);
    for (auto& t : out.tri) t = to[t];
    out = weld(out, 0);
  }
  return out;
}

manifold::Manifold evaluated(manifold::Manifold m, manifold::ExecutionContext& ctx, const Cancel& cancel) {
  Watch watch(cancel, ctx);
  const auto status = m.WithContext(ctx).Status();
  if (status == manifold::Manifold::Error::Cancelled) throw Error("cancelled");
  if (status != manifold::Manifold::Error::NoError) throw Error(std::string("the mesh operation failed: ") + why(status));
  return m;
}

}  // namespace

Mesh boolean(const Mesh& a, const Mesh& b, Op op, const Cancel& cancel) {
  manifold::ExecutionContext ctx;
  Watch watch(cancel, ctx);
  const manifold::Manifold A = to_manifold(a, "the target", ctx), B = to_manifold(b, "a tool", ctx);
  const manifold::OpType type = op == Op::Union ? manifold::OpType::Add : op == Op::Subtract ? manifold::OpType::Subtract : manifold::OpType::Intersect;
  const manifold::Manifold r = A.Boolean(B, type);
  const auto status = r.WithContext(ctx).Status();
  if (status == manifold::Manifold::Error::Cancelled) throw Error("cancelled");
  if (status != manifold::Manifold::Error::NoError) throw Error(std::string("the mesh Boolean failed: ") + why(status));
  return from_manifold(r);
}

Mesh union_all(const std::vector<Mesh>& meshes, const Cancel& cancel) {
  manifold::ExecutionContext ctx;
  std::vector<manifold::Manifold> parts;
  {
    Watch watch(cancel, ctx);
    for (const auto& m : meshes) parts.push_back(to_manifold(m, "a body", ctx));
  }
  manifold::ExecutionContext eval;
  return from_manifold(evaluated(manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add), eval, cancel));
}

// ---------------------------------------------------------------- open meshes: clipped triangle by triangle
namespace {

struct Clipper {
  const Mesh& m;
  std::array<double, 3> o, n;
  std::vector<double> d;  // signed distance per vertex
  // A cut point per crossed edge, shared by the triangles on either side (keyed by the edge's ends, smaller first).
  std::unordered_map<uint64_t, uint32_t> cutAbove, cutBelow;
  Mesh above, below;
  std::vector<int64_t> inAbove, inBelow;

  Clipper(const Mesh& mesh, const std::array<double, 3>& origin, const std::array<double, 3>& normal) : m(mesh), o(origin), n(normal) {
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len <= 0) throw Error("the cutting plane has no normal");
    for (auto& c : n) c /= len;
    d.resize(m.vertices());
    for (size_t v = 0; v < m.vertices(); ++v)
      d[v] = (m.xyz[v * 3] - o[0]) * n[0] + (m.xyz[v * 3 + 1] - o[1]) * n[1] + (m.xyz[v * 3 + 2] - o[2]) * n[2];
    inAbove.assign(m.vertices(), -1);
    inBelow.assign(m.vertices(), -1);
  }
  static uint32_t add(Mesh& out, const double* p) {
    out.xyz.insert(out.xyz.end(), {p[0], p[1], p[2]});
    return static_cast<uint32_t>(out.vertices() - 1);
  }
  uint32_t vertex(bool up, uint32_t v) {
    auto& map = up ? inAbove : inBelow;
    if (map[v] < 0) map[v] = add(up ? above : below, &m.xyz[size_t(v) * 3]);
    return static_cast<uint32_t>(map[v]);
  }
  uint32_t cut(bool up, uint32_t a, uint32_t b) {
    const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
    auto& map = up ? cutAbove : cutBelow;
    if (auto it = map.find(key); it != map.end()) return it->second;
    const uint32_t lo = std::min(a, b), hi = std::max(a, b);  // the same point from either triangle
    const double t = d[lo] / (d[lo] - d[hi]);
    double p[3];
    for (int k = 0; k < 3; ++k) p[k] = m.xyz[size_t(lo) * 3 + k] + t * (m.xyz[size_t(hi) * 3 + k] - m.xyz[size_t(lo) * 3 + k]);
    const uint32_t id = add(up ? above : below, p);
    map.emplace(key, id);
    return id;
  }
  // The polygon of triangle (a, b, c) on one side, fanned into triangles.
  void side(bool up, uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t v[3] = {a, b, c};
    uint32_t poly[4];
    int count = 0;
    for (int i = 0; i < 3; ++i) {
      const uint32_t p = v[i], q = v[(i + 1) % 3];
      const bool pin = up ? d[p] >= 0 : d[p] < 0, qin = up ? d[q] >= 0 : d[q] < 0;
      if (pin) poly[count++] = vertex(up, p);
      if (pin != qin) poly[count++] = cut(up, p, q);
    }
    Mesh& out = up ? above : below;
    for (int i = 1; i + 1 < count; ++i) out.tri.insert(out.tri.end(), {poly[0], poly[i], poly[i + 1]});
  }
  void run(const Cancel& cancel) {
    for (size_t t = 0; t < m.triangles(); ++t) {
      if ((t & 0xFFFF) == 0 && cancel && cancel()) throw Error("cancelled");
      const uint32_t a = m.tri[t * 3], b = m.tri[t * 3 + 1], c = m.tri[t * 3 + 2];
      const bool ua = d[a] >= 0, ub = d[b] >= 0, uc = d[c] >= 0;
      if (ua && ub && uc) above.tri.insert(above.tri.end(), {vertex(true, a), vertex(true, b), vertex(true, c)});
      else if (!ua && !ub && !uc) below.tri.insert(below.tri.end(), {vertex(false, a), vertex(false, b), vertex(false, c)});
      else {
        side(true, a, b, c);
        side(false, a, b, c);
      }
    }
  }
};

}  // namespace

Cut split_by_plane(const Mesh& m, const std::array<double, 3>& origin, const std::array<double, 3>& normal, const Cancel& cancel) {
  const double len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
  if (len <= 0) throw Error("the cutting plane has no normal");
  Cut out;
  manifold::ExecutionContext ctx;
  // A closed mesh is Manifold's to cut (and cap); one it refuses (open, a scan) is clipped here.
  if (const manifold::Manifold M = manifold_of(m, ctx); M.Status() == manifold::Manifold::Error::NoError) {
    const manifold::vec3 nrm(normal[0] / len, normal[1] / len, normal[2] / len);
    const double offset = (origin[0] * nrm.x + origin[1] * nrm.y + origin[2] * nrm.z);
    auto [up, down] = M.SplitByPlane(nrm, offset);
    manifold::ExecutionContext a, b;
    out.above = from_manifold(evaluated(up, a, cancel));
    out.below = from_manifold(evaluated(down, b, cancel));
    out.capped = true;
    return out;
  }
  Clipper clip(m, origin, normal);
  clip.run(cancel);
  out.above = std::move(clip.above);
  out.below = std::move(clip.below);
  out.capped = false;
  return out;
}

}  // namespace opad::meshops
