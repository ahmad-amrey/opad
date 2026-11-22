// Viewport, zoom refinement: bodies are meshed once for the whole-model view (deflectionForBox), which shows as
// chords when zoomed in far (an extruded DXF profile no longer followed its curves). Once the camera has been still
// for a moment, visible bodies whose chords exceed about a pixel get finer drawing arrays from a worker; picking,
// ordinals and highlights keep the base mesh. A budget bounds the triangles per body, per pass and kept in total.
// Compare's parts (the other version's bodies) and its bodies with coloured faces are refined the same way.
#include "Viewport.hpp"

#include <BRepBuilderAPI_Copy.hxx>

#include <algorithm>
#include <cmath>
#include <functional>

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

std::vector<Handle(BodyShape)> Viewport::refineTargets(const std::string& key) const {
  std::vector<Handle(BodyShape)> out;
  const std::string tint = refineTintKey(""), compare = refineComparePrefix();
  if (key.rfind(compare, 0) == 0) {
    const size_t k = std::stoul(key.substr(compare.size()));
    if (k < m_compareParts.size())
      if (const auto body = Handle(BodyShape)::DownCast(m_compareParts[k].second); !body.IsNull()) out.push_back(body);
  } else if (key.rfind(tint, 0) == 0) {
    const std::string id = key.substr(tint.size());
    const auto item = m_items.find(id);
    const auto painted = m_faceTints.find(id);
    if (item != m_items.end() && painted != m_faceTints.end() && painted->second.ais == item->second.ais.get())
      if (const auto body = Handle(BodyShape)::DownCast(item->second.ais); !body.IsNull()) out.push_back(body);
  } else {
    for (const auto& [id, item] : m_items)
      if (item.key == key && !faceTinted(id))
        if (const auto body = Handle(BodyShape)::DownCast(item.ais); !body.IsNull()) out.push_back(body);
  }
  return out;
}

std::shared_ptr<const BodyPrs> Viewport::refineFallback(const std::string& key) const {
  const std::string tint = refineTintKey("");
  if (key.rfind(tint, 0) != 0) return nullptr;
  const auto painted = m_faceTints.find(key.substr(tint.size()));
  return painted == m_faceTints.end() ? nullptr : painted->second.arrays;
}

std::shared_ptr<const BodyPrs> Viewport::refineBase(const std::string& key) const {
  if (key.rfind(refineTintKey(""), 0) == 0) return refineFallback(key);
  if (key.rfind(refineComparePrefix(), 0) == 0) {
    const auto targets = refineTargets(key);
    return targets.empty() ? nullptr : targets.front()->prs();
  }
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_meshMu));
  const auto p = m_prs.find(key);
  return p == m_prs.end() ? nullptr : p->second;
}

void Viewport::dropRefined(const std::string& prefix) {
  for (auto it = m_refined.begin(); it != m_refined.end();) it = it->first.rfind(prefix, 0) == 0 ? m_refined.erase(it) : std::next(it);
}

void Viewport::refineVisible() {
  // Not in the wireframe (UI-48): the refinement meshes faces finer, and a wireframe draws no face.
  if (!m_initialised || m_refineJob || (m_items.empty() && m_compareParts.empty()) || m_doc->loading || m_style == Style::Wireframe) return;
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
    std::shared_ptr<const opad::FaceColors> colors;
    std::shared_ptr<const BodyPrs> base;  // what it was drawn from: the result lands only while that is still so
  };
  std::map<std::string, Want> wants;
  // One drawn object: `base` the arrays it was drawn from before any refinement, `placed` where it stands, `shape` the
  // geometry to mesh again (read only when it is wanted).
  auto consider = [&](const std::string& key, const std::shared_ptr<const BodyPrs>& base, const gp_Trsf& placed, const std::function<TopoDS_Shape()>& shape) {
    if (!base || base->triangles.IsNull() || !(base->deflection > 0) || base->box.IsVoid()) return;
    auto refined = m_refined.find(key);
    if (refined != m_refined.end()) refined->second.used = m_refineClock;
    const double current = refined != m_refined.end() ? refined->second.deflection : base->deflection;
    if (current <= pixel) return;  // its chords are within a pixel already
    // How much of the view it covers: its box projected and clipped to the window.
    const Bnd_Box box = base->box.Transformed(placed);
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
    if (area <= 0) return;
    // Chords grow about as 1/deflection (edges) and faster on curved faces: aim for half a pixel within the budget.
    const double triangles = std::max(1.0, double(base->triangleCount()));
    const double deflection = std::max({pixel * 0.5, 1e-4, base->deflection * triangles / kBodyTriangles});
    if (deflection >= current * 0.7) return;  // not worth a re-mesh
    auto& want = wants[key];
    if (want.key.empty()) {
      TopoDS_Shape geometry;
      try {
        geometry = shape();
      } catch (const std::exception&) {
      }
      if (geometry.IsNull()) return (void)wants.erase(key);
      want = {key, deflection, 0, triangles * base->deflection / deflection, geometry, base->box, base->faceColors, base};
    }
    want.area += area;
  };
  for (const auto& [id, item] : m_items) {
    const Handle(BodyShape) body = Handle(BodyShape)::DownCast(item.ais);
    if (body.IsNull()) continue;
    if (!(item.world.is_identity() || opad::mat_is_rigid(item.world))) continue;  // drawn from a transformed copy
    const gp_Trsf placed = opad::trsf_from_mat(item.world);
    if (const auto painted = m_faceTints.find(id); painted != m_faceTints.end()) {
      // Compare's coloured faces: their own arrays refined, with the same colours.
      if (painted->second.ais == item.ais.get() && painted->second.arrays)
        consider(refineTintKey(id), painted->second.arrays, placed, [this, key = item.key] { return opad::body_shape(m_doc->doc, key); });
      continue;
    }
    std::shared_ptr<const BodyPrs> base;
    {
      std::lock_guard<std::mutex> lock(m_meshMu);
      if (auto p = m_prs.find(item.key); p != m_prs.end()) base = p->second;
    }
    consider(item.key, base, placed, [this, key = item.key] { return opad::body_shape(m_doc->doc, key); });
  }
  // Compare's parts (the other version's bodies, drawn from arrays of their own): refined like the document's.
  for (size_t k = 0; k < m_compareParts.size(); ++k) {
    const Handle(BodyShape) body = Handle(BodyShape)::DownCast(m_compareParts[k].second);
    if (body.IsNull() || !m_ctx->IsDisplayed(body)) continue;
    consider(refineCompareKey(k), body->prs(), body->LocalTransformation(), [body] { return body->Shape(); });
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
  m_jobs->backgroundNext();
  m_refineJob = m_jobs->async(tr("Refining the view"), [pass, results](Progress p) {
    for (size_t i = 0; i < pass->size(); ++i) {
      if (p.cancelled()) return;
      const auto& want = (*pass)[i];
      // A copy: the cached shape keeps the base triangulation that picking and highlights are built on.
      TopoDS_Shape copy = BRepBuilderAPI_Copy(want.shape, Standard_True, Standard_False).Shape();
      BodyPrs::meshForDisplay(copy, want.deflection);
      auto prs = BodyPrs::build(copy, want.box, true, want.colors);
      prs->indexSubShapes(copy);  // a selected or hovered face or edge drawn from these triangles too
      prs->deflection = want.deflection;
      (*results)[i] = std::move(prs);
    }
  }, [this, pass, results, cache, started](bool ok, const QString&) {
    m_refineJob = nullptr;
    if (!ok || cache != m_activeCache) return;
    QElapsedTimer applying;
    applying.start();
    size_t triangles = 0;
    // A selected body's glow is made from the arrays it is drawn with: when those change it is made again.
    bool glowsStale = false, swapped = false;
    auto dropGlow = [this, &glowsStale, &swapped](const Handle(AIS_InteractiveObject)& ais) {
      swapped = true;
      if (auto glow = m_bodyGlows.find(ais.get()); glow != m_bodyGlows.end()) {
        m_ctx->Remove(glow->second, Standard_False);
        m_bodyGlows.erase(glow);
        glowsStale = true;
      }
    };
    for (size_t i = 0; i < pass->size(); ++i) {
      const auto& prs = (*results)[i];
      if (!prs || prs->triangles.IsNull()) continue;
      const auto& key = (*pass)[i].key;
      // Gone or drawn from other arrays meanwhile (Compare run again: part k is another body; a tint recoloured; meshed anew).
      if (refineBase(key) != (*pass)[i].base) continue;
      const auto targets = refineTargets(key);
      if (targets.empty()) continue;
      triangles += prs->triangleCount();
      m_refined[key] = Refined{prs->deflection, prs, m_refineClock};
      for (const auto& body : targets)
        if (body->setDisplayPrs(prs)) { m_ctx->RecomputePrsOnly(body, Standard_False); dropGlow(body); }
    }
    // Keep a bounded amount: the bodies seen least recently go back to their base mesh.
    double kept = 0;
    for (const auto& [key, refined] : m_refined) kept += double(refined.prs->triangleCount());
    while (kept > kKeptTriangles && m_refined.size() > 1) {
      auto oldest = std::min_element(m_refined.begin(), m_refined.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      kept -= double(oldest->second.prs->triangleCount());
      const auto fallback = refineFallback(oldest->first);
      for (const auto& body : refineTargets(oldest->first))
        if (body->setDisplayPrs(fallback)) { m_ctx->RecomputePrsOnly(body, Standard_False); dropGlow(body); }
      m_refined.erase(oldest);
    }
    if (glowsStale) applySelectionLayers();
    if (swapped && (!m_subHl.IsNull() || m_subJob)) refreshSubHighlight();  // selected faces and edges: from the arrays drawn
    if (swapped && (!m_candidateHl.IsNull() || m_candidateJob)) showCandidateRefs(m_candidateRefs, true);  // a hovered step's faces too
    if (swapped && m_style == Style::HiddenEdges) scheduleEdgeOverlay();  // its rims are the arrays drawn
    redrawScene();
    if (trace::enabled())
      trace::log(QStringLiteral("refine: done in %1 ms (%2 triangles, applied in %3 ms, %4 bodies kept)").arg(started.elapsed()).arg(triangles).arg(applying.elapsed()).arg(m_refined.size()));
    m_refineTimer.start();  // the next candidates, if any are left
  }, JobKind::Background);
}
