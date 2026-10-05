// What the coordinate readout of the status bar (CoordinateReadout, UI-112) asks of the view.
#include "Viewport.hpp"

#include <AIS_InteractiveContext.hxx>
#include <StdSelect_ViewerSelector3d.hxx>

bool Viewport::detectedPoint(opad::Vec3& p) const {
  if (!m_initialised || !m_ctx->HasDetected() || m_ctx->MainSelector()->NbPicked() < 1 || !m_nodeOf.count(m_ctx->DetectedInteractive().get())) return false;
  const gp_Pnt hit = m_ctx->MainSelector()->PickedPoint(1);
  p = {hit.X(), hit.Y(), hit.Z()};
  return true;
}

bool Viewport::benchDetect(int x, int y) {
  if (!m_initialised) return false;
  m_view->Redraw();  // the picker clips to the z range of the last frame
  m_ctx->MoveTo(x, y, m_view, Standard_False);
  return m_ctx->HasDetected();
}
