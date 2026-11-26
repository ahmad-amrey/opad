// Viewport, point clouds (opad/point_cloud.hpp, BodyPrs::isCloud): a cloud is drawn as points from arrays its mesh worker
// built, a uniform sample of BodyPrs::cloudBudget points when it is bigger. Once the camera is still (the zoom refinement's
// timer, ViewportRefine.cpp) its points are drawn as wide as their spacing looks at this zoom, and a cloud drawn as a
// sample whose visible part is a fraction of it gets that part drawn at full density, found and filled on a worker (level of
// detail: an overview of tens of millions of points, every point zoomed in). The colour mode (view/cloudColor) and a
// region of points (the region tool's selection, an overlay in Topmost) are drawn the same way, from arrays made off the
// UI thread.
#include "Viewport.hpp"

#include <AIS_InteractiveObject.hxx>
#include <AIS_RubberBand.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>

#include <algorithm>
#include <cmath>

#include "Jobs.hpp"
#include "opad/geometry.hpp"

namespace {
// The region overlay: points in one colour, a little wider than the cloud's, never pickable.
class CloudRegionPrs : public AIS_InteractiveObject {
 public:
  CloudRegionPrs(Handle(Graphic3d_ArrayOfPoints) points, const Quantity_Color& colour, float size) : m_points(std::move(points)), m_colour(colour), m_size(size) {}

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    if (m_points.IsNull()) return;
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_POINT, m_colour, m_size));
    g->AddPrimitiveArray(m_points);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  Handle(Graphic3d_ArrayOfPoints) m_points;
  Quantity_Color m_colour;
  float m_size;
};

// World -> normalised device coordinates of a camera, row-major (the caller divides by w).
std::array<double, 16> ndcOf(const Handle(Graphic3d_Camera)& camera, const opad::Mat4& world) {
  const Graphic3d_Mat4d proj = camera->ProjectionMatrix(), view = camera->OrientationMatrix();
  const Graphic3d_Mat4d pv = proj * view;
  std::array<double, 16> m{};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += pv.GetValue(r, k) * (k < 3 ? world.at(k, c) : (c == 3 ? 1.0 : 0.0));
      m[size_t(r * 4 + c)] = s;
    }
  return m;
}
}  // namespace

std::shared_ptr<const BodyPrs> Viewport::cloudPrs(const std::string& node, opad::Mat4* world) const {
  const auto item = m_items.find(node);
  if (item == m_items.end()) return nullptr;
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_meshMu));
  const auto p = m_prs.find(item->second.key);
  if (p == m_prs.end() || !p->second || !p->second->isCloud()) return nullptr;
  if (world) *world = item->second.world;
  return p->second;
}

size_t Viewport::benchCloudDrawn(const std::string& node) const {
  const auto item = m_items.find(node);
  if (item == m_items.end()) return 0;
  const Handle(BodyShape) body = Handle(BodyShape)::DownCast(item->second.ais);
  if (body.IsNull() || !m_ctx->IsDisplayed(body)) return 0;
  if (const auto& shown = body->displayPrs(); shown && shown->isCloud()) return shown->cloudDrawn;
  return body->prs() && body->prs()->isCloud() ? body->prs()->cloudDrawn : 0;
}

std::array<double, 16> Viewport::worldToNdc(const opad::Mat4& world) const { return ndcOf(m_view->Camera(), world); }

QPointF Viewport::widgetToNdc(const QPointF& at) const {
  const double w = std::max(1, width()), h = std::max(1, height());
  return {2 * at.x() / w - 1, 1 - 2 * at.y() / h};
}

void Viewport::setCloudColorMode(int mode) {
  BodyPrs::cloudColorMode = std::clamp(mode, 0, 4);
  // Every cloud drawn again in the new colours: its whole sample (a refinement comes back by itself at the next stop).
  m_cloudShown.clear();
  for (const auto& [id, item] : m_items) {
    const Handle(BodyShape) body = Handle(BodyShape)::DownCast(item.ais);
    if (!body.IsNull() && body->prs() && body->prs()->isCloud()) m_cloudRecolour.insert(item.key);
  }
  m_cloudCamera = {};
  m_refineTimer.start();
}

void Viewport::updateClouds() {
  if (!m_initialised || m_cloudJob || m_doc->loading) return;
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  if (w <= 0 || h <= 0) return;
  const auto camera = m_view->Camera();
  const double pixel = camera->IsOrthographic() ? camera->Scale() / h : 2 * camera->Distance() * std::tan(camera->FOVy() * M_PI / 360) / h;
  if (!(pixel > 0) || !std::isfinite(pixel)) return;
  // Point sizes: as wide as the drawn points' spacing looks now (a sample's points stand further apart).
  bool resized = false;
  struct Want {
    std::string key;
    std::shared_ptr<const BodyPrs> base;
    std::array<double, 16> ndc;
    bool recolour = false;
  };
  std::vector<Want> wants;
  std::set<std::string> seen;
  for (const auto& [id, item] : m_items) {
    const Handle(BodyShape) body = Handle(BodyShape)::DownCast(item.ais);
    if (body.IsNull() || !body->prs() || !body->prs()->isCloud() || !m_ctx->IsDisplayed(body)) continue;
    const auto& base = body->prs();
    const auto& shown = body->displayPrs() && body->displayPrs()->isCloud() ? body->displayPrs() : base;
    const size_t total = base->cloudData->size();
    const double fraction = total ? double(std::max<size_t>(shown->cloudDrawn, 1)) / double(total) : 1;
    // Seen from the refinement's own part: its points are every point there.
    const double spacing = base->cloudSpacing / std::sqrt(shown == base ? fraction : 1.0);
    resized = body->setPointSize(float(std::clamp(spacing / pixel * 1.1, 1.0, 6.0))) || resized;
    if (!seen.insert(item.key).second) continue;
    const bool recolour = m_cloudRecolour.count(item.key) > 0;
    if (recolour || base->cloudDrawn < total) wants.push_back({item.key, base, ndcOf(camera, item.world), recolour});
  }
  if (resized) redrawScene();
  const auto state = camera->WorldViewProjState();
  if (wants.empty() || (m_cloudCamera == state && m_cloudRecolour.empty())) return;
  m_cloudCamera = state;
  m_cloudRecolour.clear();
  auto pass = std::make_shared<std::vector<Want>>(std::move(wants));
  auto results = std::make_shared<std::vector<std::shared_ptr<BodyPrs>>>(pass->size());
  auto whole = std::make_shared<std::vector<char>>(pass->size(), 0);
  const int mode = BodyPrs::cloudColorMode.load();
  const void* cache = m_activeCache;
  QElapsedTimer started;
  started.start();
  m_jobs->backgroundNext();
  m_cloudJob = m_jobs->async(tr("Drawing the points"), [pass, results, whole, mode](Progress p) {
    for (size_t i = 0; i < pass->size(); ++i) {
      if (p.cancelled()) return;
      const auto& want = (*pass)[i];
      const auto& points = *want.base->cloudData;
      // The points in view (inside the window, in front of the eye).
      const opad::cloud::Region visible = opad::cloud::in_polygon(points, want.ndc, {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, [&] { return p.cancelled(); });
      const size_t count = visible.count();
      auto prs = std::make_shared<BodyPrs>();
      prs->box = want.base->box;
      prs->cloudData = want.base->cloudData;
      prs->cloudSpacing = want.base->cloudSpacing;
      if (count >= size_t(0.8 * double(points.size())) || count == 0) {
        // Most of it in view: the whole cloud's sample (in the colours asked for, when that is why it is here).
        (*whole)[i] = 1;
        if (!want.recolour) continue;
        prs->cloudPoints = BodyPrs::cloudArray(points, nullptr, mode, BodyPrs::cloudBudget, &prs->cloudDrawn);
      } else {
        const std::vector<uint32_t> idx = visible.indices();
        prs->cloudPoints = BodyPrs::cloudArray(points, &idx, mode, BodyPrs::cloudBudget, &prs->cloudDrawn);
      }
      (*results)[i] = std::move(prs);
    }
  }, [this, pass, results, whole, cache, started](bool ok, const QString&) {
    m_cloudJob = nullptr;
    if (!ok || cache != m_activeCache) return;
    bool changed = false;
    size_t drawn = 0;
    for (size_t i = 0; i < pass->size(); ++i) {
      const auto& want = (*pass)[i];
      const auto& prs = (*results)[i];
      for (const auto& [id, item] : m_items) {
        if (item.key != want.key) continue;
        const Handle(BodyShape) body = Handle(BodyShape)::DownCast(item.ais);
        if (body.IsNull() || body->prs() != want.base) continue;  // meshed anew meanwhile
        // Whole in view and drawn in the base colours: back to the base sample.
        const std::shared_ptr<const BodyPrs> next = prs ? prs : ((*whole)[i] && BodyPrs::cloudColorMode.load() == 0 ? nullptr : body->displayPrs());
        if (body->setDisplayPrs(next)) {
          m_ctx->RecomputePrsOnly(body, Standard_False);
          changed = true;
        }
        if (prs) drawn += prs->cloudDrawn;
      }
    }
    if (changed) {
      applySelectionLayers();
      redrawScene();
      m_refineTimer.start();  // the point size for what is drawn now
    }
    if (trace::enabled()) trace::log(QStringLiteral("cloud: %1 clouds drawn again in %2 ms (%3 points)").arg(pass->size()).arg(started.elapsed()).arg(drawn));
  }, JobKind::Background);
}

void Viewport::showCloudRegion(const std::string& node, std::shared_ptr<const std::vector<uint32_t>> indices) {
  if (const auto old = m_cloudRegion.find(node); old != m_cloudRegion.end()) {
    m_ctx->Remove(old->second, Standard_False);
    m_cloudRegion.erase(old);
  }
  opad::Mat4 world;
  const auto prs = cloudPrs(node, &world);
  if (!prs || !indices || indices->empty()) return redrawScene();
  // The array on a worker: a region can be millions of points.
  auto array = std::make_shared<Handle(Graphic3d_ArrayOfPoints)>();
  const std::string id = node;
  const double spacing = prs->cloudSpacing;
  m_jobs->async(tr("Showing the region"), [prs, indices, array](Progress) {
    *array = BodyPrs::cloudArray(*prs->cloudData, indices.get(), 4, BodyPrs::cloudBudget);
  }, [this, id, array, world, prs, spacing](bool ok, const QString&) {
    if (!ok || array->IsNull()) return;
    opad::Mat4 now;
    if (cloudPrs(id, &now) != prs) return;
    if (const auto old = m_cloudRegion.find(id); old != m_cloudRegion.end()) m_ctx->Remove(old->second, Standard_False);
    Handle(CloudRegionPrs) overlay = new CloudRegionPrs(*array, Quantity_Color(1.0, 0.55, 0.1, Quantity_TOC_sRGB), 4.0f);
    if (!now.is_identity() && opad::mat_is_rigid(now)) overlay->SetLocalTransformation(opad::trsf_from_mat(now));
    overlay->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(overlay, 0, -1, Standard_False);
    m_cloudRegion[id] = overlay;
    (void)spacing;
    redrawScene();
  }, JobKind::Background);
}

bool Viewport::pickCloudPoint(const QPointF& at, opad::Vec3& point) {
  if (!m_initialised) return false;
  gp_Pnt p;
  const QPointF s = viewScale();
  if (!navigationPoint(Graphic3d_Vec2i(int(at.x() * s.x()), int(at.y() * s.y())), p)) return false;
  point = {p.X(), p.Y(), p.Z()};
  return true;
}

void Viewport::setCloudLasso(const QPolygonF& widget, bool closed) {
  if (!m_initialised) return;
  if (widget.size() < 2) {
    if (!m_cloudLasso.IsNull()) m_ctx->Remove(m_cloudLasso, Standard_False);
    m_cloudLasso.Nullify();
    return redrawScene();
  }
  // The polygon drawn as OCCT's own rubber band (2D, over everything), in device pixels from the bottom left.
  if (m_cloudLasso.IsNull()) {
    m_cloudLasso = new AIS_RubberBand(Quantity_Color(1.0, 0.55, 0.1, Quantity_TOC_sRGB), Aspect_TOL_SOLID, Quantity_Color(1.0, 0.55, 0.1, Quantity_TOC_sRGB), 0.85, 1.5);
    Handle(AIS_RubberBand)::DownCast(m_cloudLasso)->SetFilling(false);
  }
  Handle(AIS_RubberBand) band = Handle(AIS_RubberBand)::DownCast(m_cloudLasso);
  band->ClearPoints();
  const QPointF s = viewScale();
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  for (const QPointF& p : widget) band->AddPoint(Graphic3d_Vec2i(int(p.x() * s.x()), h - int(p.y() * s.y())));
  if (closed) band->AddPoint(Graphic3d_Vec2i(int(widget.first().x() * s.x()), h - int(widget.first().y() * s.y())));
  if (!m_ctx->IsDisplayed(band)) m_ctx->Display(band, 0, -1, Standard_False, AIS_DS_Displayed);
  else m_ctx->Redisplay(band, Standard_False);
  redrawScene();
}
