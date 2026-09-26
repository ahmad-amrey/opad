// Viewport, zoom refinement: bodies are meshed once for the whole-model view (deflectionForBox), which shows as
// chords when zoomed in far (an extruded DXF profile no longer followed its curves). Once the camera has been still
// for a moment, visible bodies whose chords exceed about a pixel get finer drawing arrays from a worker; picking,
// ordinals and highlights keep the base mesh. A budget bounds the triangles per body, per pass and kept in total.
#include "Viewport.hpp"

#include <BRepBuilderAPI_Copy.hxx>

#include <algorithm>
#include <cmath>

#include "Jobs.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"

namespace {
constexpr double kBodyTriangles = 1.5e6;   // one refined body
constexpr double kPassTriangles = 4e6;     // one worker pass
constexpr double kKeptTriangles = 12e6;    // every refined body kept, least recently seen go first
constexpr size_t kPassBodies = 8;
}  // namespace

void Viewport::scheduleRefinement() {
  const auto state = m_view->Camera()->WorldViewProjState();
  if (m_refineCamera == state) return;
  m_refineCamera = state;
  m_refineTimer.start();  // restarted by every frame that moves the camera: refine once it is still
}

void Viewport::refineVisible() {
  if (!m_initialised || m_refineJob || m_items.empty() || m_doc->loading) return;
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  if (w <= 0 || h <= 0) return;
  const auto camera = m_view->Camera();
  // World size of one backing pixel: everywhere in orthographic, at the focus distance in perspective.
  const double pixel = camera->IsOrthographic() ? camera->Scale() / h : 2 * camera->Distance() * std::tan(camera->FOVy() * M_PI / 360) / h;
  if (!(pixel > 0) || !std::isfinite(pixel)) return;
  ++m_refineClock;
  struct Want {
    std::string key;
    double deflection, area, triangles;
    TopoDS_Shape shape;
    Bnd_Box box;
  };
  std::map<std::string, Want> wants;
  for (const auto& [id, item] : m_items) {
    if (Handle(BodyShape)::DownCast(item.ais).IsNull()) continue;
    if (!(item.world.is_identity() || opad::mat_is_rigid(item.world))) continue;  // drawn from a transformed copy
    std::shared_ptr<BodyPrs> base;
    {
      std::lock_guard<std::mutex> lock(m_meshMu);
      if (auto p = m_prs.find(item.key); p != m_prs.end()) base = p->second;
    }
    if (!base || base->triangles.IsNull() || !(base->deflection > 0) || base->box.IsVoid()) continue;
    auto refined = m_refined.find(item.key);
    if (refined != m_refined.end()) refined->second.used = m_refineClock;
    const double current = refined != m_refined.end() ? refined->second.deflection : base->deflection;
    if (current <= pixel) continue;  // its chords are within a pixel already
    // How much of the view it covers: its box projected and clipped to the window.
    const Bnd_Box box = base->box.Transformed(opad::trsf_from_mat(item.world));
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
    for (int c = 0; c < 8; ++c) {
      Standard_Integer px = 0, py = 0;
      m_view->Convert(c & 1 ? x1 : x0, c & 2 ? y1 : y0, c & 4 ? z1 : z0, px, py);
      left = std::min(left, double(px)); right = std::max(right, double(px));
      top = std::min(top, double(py)); bottom = std::max(bottom, double(py));
    }
    const double area = std::max(0.0, std::min(right, double(w)) - std::max(left, 0.0)) * std::max(0.0, std::min(bottom, double(h)) - std::max(top, 0.0));
    if (area <= 0) continue;
    // Chords grow about as 1/deflection (edges) and faster on curved faces: aim for half a pixel within the budget.
    const double triangles = std::max(1.0, double(base->triangleCount()));
    const double deflection = std::max({pixel * 0.5, 1e-4, base->deflection * triangles / kBodyTriangles});
    if (deflection >= current * 0.7) continue;  // not worth a re-mesh
    auto& want = wants[item.key];
    if (want.key.empty()) want = {item.key, deflection, 0, triangles * base->deflection / deflection, opad::body_shape(m_doc->doc, item.key), base->box};
    want.area += area;
  }
  if (wants.empty()) return;
  std::vector<Want> order;
  for (auto& [key, want] : wants) order.push_back(std::move(want));
  std::sort(order.begin(), order.end(), [](const Want& a, const Want& b) { return a.area > b.area; });
  auto pass = std::make_shared<std::vector<Want>>();
  double budget = kPassTriangles;
  for (auto& want : order) {
    if (pass->size() >= kPassBodies || (!pass->empty() && want.triangles > budget)) break;
    budget -= want.triangles;
    pass->push_back(std::move(want));
  }
  auto results = std::make_shared<std::vector<std::shared_ptr<BodyPrs>>>(pass->size());
  const void* cache = m_activeCache;
  if (trace::enabled()) trace::log(QStringLiteral("refine: %1 bodies at %2 mm (pixel %3 mm)").arg(pass->size()).arg(pass->front().deflection).arg(pixel));
  QElapsedTimer started;
  started.start();
  m_refineJob = m_jobs->async(tr("Refining the view"), [pass, results](Progress p) {
    for (size_t i = 0; i < pass->size(); ++i) {
      if (p.cancelled()) return;
      const auto& want = (*pass)[i];
      // A copy: the cached shape keeps the base triangulation that picking and highlights are built on.
      TopoDS_Shape copy = BRepBuilderAPI_Copy(want.shape, Standard_True, Standard_False).Shape();
      BodyPrs::meshForDisplay(copy, want.deflection);
      auto prs = BodyPrs::build(copy, want.box, true);
      prs->deflection = want.deflection;
      (*results)[i] = std::move(prs);
    }
  }, [this, pass, results, cache, started](bool ok, const QString&) {
    m_refineJob = nullptr;
    if (!ok || cache != m_activeCache) return;
    QElapsedTimer applying;
    applying.start();
    size_t triangles = 0;
    for (size_t i = 0; i < pass->size(); ++i) {
      const auto& prs = (*results)[i];
      if (!prs || prs->triangles.IsNull()) continue;
      const auto& key = (*pass)[i].key;
      triangles += prs->triangleCount();
      m_refined[key] = Refined{prs->deflection, prs, m_refineClock};
      for (auto& [id, item] : m_items)
        if (item.key == key)
          if (auto body = Handle(BodyShape)::DownCast(item.ais); !body.IsNull() && body->setDisplayPrs(prs)) m_ctx->RecomputePrsOnly(body, Standard_False);
    }
    // Keep a bounded amount: the bodies seen least recently go back to their base mesh.
    double kept = 0;
    for (const auto& [key, refined] : m_refined) kept += double(refined.prs->triangleCount());
    while (kept > kKeptTriangles && m_refined.size() > 1) {
      auto oldest = std::min_element(m_refined.begin(), m_refined.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      kept -= double(oldest->second.prs->triangleCount());
      for (auto& [id, item] : m_items)
        if (item.key == oldest->first)
          if (auto body = Handle(BodyShape)::DownCast(item.ais); !body.IsNull() && body->setDisplayPrs(nullptr)) m_ctx->RecomputePrsOnly(body, Standard_False);
      m_refined.erase(oldest);
    }
    redrawScene();
    if (trace::enabled())
      trace::log(QStringLiteral("refine: done in %1 ms (%2 triangles, applied in %3 ms, %4 bodies kept)").arg(started.elapsed()).arg(triangles).arg(applying.elapsed()).arg(m_refined.size()));
    m_refineTimer.start();  // the next candidates, if any are left
  });
}
