#include "Viewport.hpp"
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_RenderingParams.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <Graphic3d_TypeOfLimit.hxx>
#include <AIS_AnimationCamera.hxx>
#include <QSettings>
#include <algorithm>

int Viewport::savedRenderQuality() {
  QSettings settings;
  if (!settings.contains("view/qualityV2")) {
    const int old = settings.value("view/quality", 1).toInt();
    settings.setValue("view/qualityV2", old <= 1 ? 0 : old <= 3 ? 1 : 2);
  }
  return std::clamp(settings.value("view/qualityV2").toInt(), 0, 2);
}

void Viewport::setRenderQuality(int level) {
  m_renderQuality = std::clamp(level, 0, 2);
  QSettings().setValue("view/qualityV2", m_renderQuality);
  if (!m_initialised) return;
  auto& p = m_view->ChangeRenderingParams();
  const bool rayTracing = m_renderQuality == 2 && m_viewer->Driver()->InquireLimit(Graphic3d_TypeOfLimit_HasRayTracing);
  p.Method = rayTracing ? Graphic3d_RM_RAYTRACING : Graphic3d_RM_RASTERIZATION;
  p.NbMsaaSamples = rayTracing ? 0 : std::min(4, m_viewer->Driver()->InquireLimit(Graphic3d_TypeOfLimit_MaxMsaa));
  if (m_renderQuality == 2 && !rayTracing) emit hoverChanged(tr("Ray tracing unavailable on this driver; using Studio rendering"));
  p.RenderResolutionScale = m_renderQuality == 1 ? 1.25f : 1.0f;
  p.ShadingModel = m_renderQuality == 0 ? Graphic3d_TypeOfShadingModel_Unlit : Graphic3d_TypeOfShadingModel_Phong;
  p.IsShadowEnabled = m_renderQuality >= 1;
  p.IsReflectionEnabled = false;
  p.IsAntialiasingEnabled = rayTracing;
  p.IsGlobalIlluminationEnabled = false;  // bounded interactive cost; no progressive path-tracing stall
  p.RaytracingDepth = 2;
  setShadows(m_renderQuality >= 1);
  updateDepthBias();
  m_view->Invalidate();
  redrawScene();
}

void Viewport::setSceneBackground(int style) {
  m_sceneBackground = std::clamp(style, 0, 3);
  QSettings().setValue("view/background", m_sceneBackground);
  if (!m_initialised) return;
  QColor c = m_sceneBackground == 2 ? QColor("#ffffff") : m_sceneBackground == 3 ? QColor("#171c24") : m_tokens.vp;
  auto occ = [](const QColor& v) { return Quantity_Color(v.redF(), v.greenF(), v.blueF(), Quantity_TOC_sRGB); };
  m_view->SetBackgroundColor(occ(c));
  if (m_sceneBackground == 1)
    m_view->SetBgGradientColors(occ(QColor("#c7c8c9")), occ(QColor("#66696b")), Aspect_GradientFillMethod_Vertical, false);
  else m_view->SetBgGradientStyle(Aspect_GradientFillMethod_None);
  redrawScene();
}

void Viewport::setTwoDimensional(bool on) {
  const bool entering = on && m_threeDimensionalCamera.IsNull();
  m_twoDimensional = on;
  if (!m_initialised) return;
  finishTrackpadScroll();
  myViewAnimation->Stop();
  ResetViewInput();
  myUI.Reset(); myGL.Reset();
  m_cubeGesture = m_cubeClick = false;
  m_dragOffset = {};
  clearTracking();
  SetAllowRotation(!on);
  SetRotationMode(AIS_RotationMode_BndBoxActive);
  setNavPreset(m_preset);
  if (on) {
    if (entering) {
      m_threeDimensionalCamera = new Graphic3d_Camera(*m_view->Camera());
      const auto d = m_view->Camera()->Direction();
      // Snap to the closest principal plane without an animation that can leak
      // an oblique orientation into drafting input.
      if (std::abs(d.Z()) >= std::max(std::abs(d.X()), std::abs(d.Y())))
        m_view->SetProj(d.Z() < 0 ? V3d_Zpos : V3d_Zneg);
      else if (std::abs(d.X()) >= std::abs(d.Y())) m_view->SetProj(d.X() < 0 ? V3d_Xpos : V3d_Xneg);
      else m_view->SetProj(d.Y() < 0 ? V3d_Ypos : V3d_Yneg);
    }
    setOrthographic(true);
    m_ctx->Deactivate(m_cube);
    m_ctx->Erase(m_cube, false);
  } else {
    if (!m_threeDimensionalCamera.IsNull()) {
      m_view->SetCamera(new Graphic3d_Camera(*m_threeDimensionalCamera));
      m_threeDimensionalCamera.Nullify();
    }
    m_ctx->Display(m_cube, false);
    m_ctx->Activate(m_cube, 0);
  }
  m_ctx->ClearDetected(false);
  ResetPreviousMoveTo();
  redrawScene();
}
