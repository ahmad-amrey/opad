// Viewport, small parts hidden while the view moves (UI-134): a board's hundreds of resistors and capacitors cost frames
// while orbiting and say nothing at that moment. From the first frame the camera moves, bodies whose box's longest side is
// under the filter's size go out through the Navigation look layer (erased, their presentations kept), and come back once
// the camera has been still for 300 ms. Selected bodies stay; the boxes are the cached view boxes (no geometry is walked).
#include "Viewport.hpp"

#include <algorithm>

#include "opad/geometry.hpp"

void Viewport::setSmallPartFilter(double mm) {
  if (!m_smallTimer.isSingleShot()) {
    m_smallTimer.setSingleShot(true);
    m_smallTimer.setInterval(300);
    connect(&m_smallTimer, &QTimer::timeout, this, &Viewport::restoreSmallParts);
  }
  m_smallParts = mm > 0 ? mm : 0;
  if (m_initialised) m_smallCamera = m_view->Camera()->WorldViewProjState();  // turning it on is no move
  if (m_smallHidden) restoreSmallParts();
}

void Viewport::cameraMoving() {
  if (!(m_smallParts > 0) || !m_initialised || m_doc->loading || m_needFit) return;  // a load fitting batch by batch is no navigation
  const auto state = m_view->Camera()->WorldViewProjState();
  if (m_smallCamera == state) return;  // a frame that did not move the camera (a hover, a selection)
  m_smallCamera = state;
  m_smallTimer.start();
  if (m_smallHidden) return;
  std::map<std::string, LookDelta> hidden;
  LookDelta off;
  off.visible = false;
  for (const auto& [id, item] : m_items) {
    if (m_ctx->IsSelected(item.ais)) continue;
    auto size = m_partSizes.find(item.key);
    if (size == m_partSizes.end()) {
      const Bnd_Box b = opad::body_bbox(m_doc->doc, item.key);  // cached by the mesh worker for every displayed body
      double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
      if (!b.IsVoid()) b.Get(x0, y0, z0, x1, y1, z1);
      size = m_partSizes.emplace(item.key, b.IsVoid() ? 1e300 : std::max({x1 - x0, y1 - y0, z1 - z0}) - 2 * b.GetGap()).first;
    }
    if (size->second < m_smallParts) hidden.emplace(id, off);
  }
  m_smallCount = static_cast<int>(hidden.size());
  if (hidden.empty()) return;
  m_smallHidden = true;
  setLookLayer(LookSource::Navigation, std::move(hidden));
}

void Viewport::restoreSmallParts() {
  m_smallTimer.stop();
  if (m_initialised) m_smallCamera = m_view->Camera()->WorldViewProjState();
  if (!m_smallHidden) return;
  m_smallHidden = false;
  clearLookLayer(LookSource::Navigation);
}
