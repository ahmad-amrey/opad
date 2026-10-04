#include "Viewport.hpp"
#include "Jobs.hpp"
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_RenderingParams.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <Graphic3d_TypeOfLimit.hxx>
#include <AIS_AnimationCamera.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <QSettings>
#include <algorithm>

namespace {
// Theme background and Studio quality are the defaults since TODO 10. Earlier builds wrote their own defaults (Studio
// gradient, Draft) back at every start, so a stored value cannot tell a choice from a default: those move once, and
// whatever is chosen afterwards stays.
void migrateViewDefaults(QSettings& settings) {
  if (settings.value("view/defaultsVersion", 0).toInt() >= 2) return;
  if (settings.value("view/background", 0).toInt() == 1) settings.setValue("view/background", 0);
  if (settings.contains("view/qualityV2") && settings.value("view/qualityV2").toInt() == 0) settings.setValue("view/qualityV2", 1);
  settings.setValue("view/defaultsVersion", 2);
}
}  // namespace

int Viewport::savedRenderQuality() {
  QSettings settings;
  if (!settings.contains("view/qualityV2") && settings.contains("view/quality")) {
    const int old = settings.value("view/quality").toInt();
    settings.setValue("view/qualityV2", old <= 1 ? 0 : old <= 3 ? 1 : 2);
  }
  migrateViewDefaults(settings);
  return std::clamp(settings.value("view/qualityV2", 1).toInt(), 0, 2);
}

int Viewport::savedSceneBackground() {
  QSettings settings;
  migrateViewDefaults(settings);
  return std::clamp(settings.value("view/background", 0).toInt(), 0, 3);
}
void Viewport::setHoverFade(bool enabled,double seconds) {
  m_hoverFadeEnabled=enabled;m_hoverFadeSeconds=std::clamp(seconds,.1,60.0);
  QSettings().setValue("view/hoverFade",enabled);QSettings().setValue("view/hoverFadeSeconds",m_hoverFadeSeconds);
  resetHoverFade();
}
void Viewport::resetHoverFade() {
  m_hoverFadeObject.Nullify();m_hoverFadeTimer.stop();m_hoverAge.invalidate();
  // Keep the detected owner: clicks must still select an object whose hover faded.
  // Restore its presentation in the next frame, even if the pointer stays still.
  m_hoverFadeRestorePending=true;
  if(m_initialised){ResetPreviousMoveTo();requestRedraw();}
}
void Viewport::trackHoverFade() {
  if(!m_initialised || !m_ctx->HasDetected()){m_hoverFadeTimer.stop();return;}
  const auto object=m_ctx->DetectedInteractive();
  // Visiting another object (including helpers) releases the old suppression.
  // Empty space alone does not: leaving and returning to the same object keeps it faded.
  if(object!=m_hoverFadeObject){m_hoverFadeObject=object;m_hoverAge.restart();m_hoverFadeTimer.stop();}
  if(object.IsNull() || !m_nodeOf.count(object.get())){
    m_hoverFadeRestorePending=false;m_hoverFadeTimer.stop();return;
  }
  if(m_hoverFadeEnabled && !m_hoverFadeTimer.isActive()) {
    const double remaining=m_hoverFadeSeconds*1000-m_hoverAge.elapsed();
    // Sleep until the fade starts; only its 400 ms animation needs extra frames.
    if(remaining>0)m_hoverFadeTimer.start(std::max(1,int(remaining)));
    else if(remaining>-400)m_hoverFadeTimer.start(40);
  }
  updateHoverFade();
}
void Viewport::updateHoverFade() {
  if(!m_initialised || !m_ctx->HasDetected() || m_ctx->DetectedInteractive()!=m_hoverFadeObject){m_hoverFadeTimer.stop();return;}
  const double fade=m_hoverFadeEnabled?std::clamp((m_hoverAge.elapsed()-m_hoverFadeSeconds*1000)/400.0,0.0,1.0):0.0;
  if(fade<=0 && !m_hoverFadeRestorePending)return;
  m_hoverFadeRestorePending=false;
  const auto manager=m_ctx->MainPrsMgr();manager->ClearImmediateDraw();
  if(fade<1) {
    const auto base=m_ctx->HighlightStyle(m_filter==SelFilter::Body?Prs3d_TypeOfHighlight_Dynamic:Prs3d_TypeOfHighlight_LocalDynamic);
    Handle(Prs3d_Drawer) style=base;
    if(fade>0) {
      style=new Prs3d_Drawer;style->SetLink(base);style->SetColor(base->Color());
      style->SetTransparency(float(base->Transparency()+(1-base->Transparency())*fade));
      style->SetDisplayMode(base->DisplayMode());style->SetZLayer(base->ZLayer());
      auto shading=new Prs3d_ShadingAspect;shading->SetAspect(new Graphic3d_AspectFillArea3d(*base->ShadingAspect()->Aspect()));
      const double transparency=base->ShadingAspect()->Transparency();
      shading->SetTransparency(transparency+(1-transparency)*fade);style->SetShadingAspect(shading);
    }
    manager->BeginImmediateDraw();m_ctx->DetectedOwner()->HilightWithColor(manager,style,style->DisplayMode());manager->EndImmediateDraw(m_viewer);
  }
  // Only alter presentations here. The controller renders after this check, so
  // a new OCCT MoveTo highlight can never leak into a visible frame first.
  m_view->InvalidateImmediate();
  if(fade>=1)m_hoverFadeTimer.stop();
}

void Viewport::setRenderQuality(int level) {
  m_renderQuality = std::clamp(level, 0, 2);
  QSettings().setValue("view/qualityV2", m_renderQuality);
  if (!m_initialised) return;
  m_degraded = false;
  m_qualityTimer.stop();
  auto& p = m_view->ChangeRenderingParams();
  const bool rayTracing = m_renderQuality == 2 && m_viewer->Driver()->InquireLimit(Graphic3d_TypeOfLimit_HasRayTracing);
  p.Method = rayTracing ? Graphic3d_RM_RAYTRACING : Graphic3d_RM_RASTERIZATION;
  p.NbMsaaSamples = rayTracing ? 0 : std::min(4, m_viewer->Driver()->InquireLimit(Graphic3d_TypeOfLimit_MaxMsaa));
  if (m_renderQuality == 2 && !rayTracing) emit hoverChanged(tr("Ray tracing unavailable on this driver; using Studio rendering"));
  applyQuality();
  outlineBodies();  // put back if it was lowered
  p.ShadingModel = m_renderQuality == 0 ? Graphic3d_TypeOfShadingModel_Unlit : Graphic3d_TypeOfShadingModel_Phong;
  p.IsReflectionEnabled = false;
  p.IsAntialiasingEnabled = rayTracing;
  p.IsGlobalIlluminationEnabled = false;  // bounded interactive cost; no progressive path-tracing stall
  p.RaytracingDepth = 2;
  // Translucent things (a body's opacity, ghosts, the selection's tints) blend order-independently when rasterised
  // (UI-39): unordered blending gave where two overlap the colour of whichever was displayed last.
  p.TransparencyMethod = Graphic3d_RTM_BLEND_OIT;
  setShadows(m_renderQuality >= 1);
  updateDepthBias();
  m_view->Invalidate();
  redrawScene();
}

void Viewport::applyQuality() {
  auto& p = m_view->ChangeRenderingParams();
  const bool rayTracing = p.Method == Graphic3d_RM_RAYTRACING;
  p.RenderResolutionScale = m_degraded ? (rayTracing ? 0.5f : 1.0f) : m_renderQuality == 1 ? 1.25f : 1.0f;
  p.IsShadowEnabled = m_renderQuality >= 1 && !m_degraded;
}

void Viewport::setAdaptiveQuality(bool on) {
  m_adaptive = on;
  QSettings().setValue("view/adaptive", on);
  if (!on) restoreQuality();
}

// Moving under a gesture, the wheel, a trackpad or an animation; a camera set at once (Fit, a typed view) is a single frame.
// Only the resolution scale and the shadows change: MSAA would reallocate the frame buffers, and the shader variants are
// kept after the first change.
void Viewport::degradeWhileNavigating() {
  const auto camera = m_view->Camera()->WorldViewProjState();
  if (camera == m_qualityCamera) return;
  m_qualityCamera = camera;
  if (m_degraded) return m_qualityTimer.start();
  const bool navigating = PressedMouseButtons() != Aspect_VKeyMouse_NONE || (!myViewAnimation.IsNull() && !myViewAnimation->IsStopped()) ||
                          m_trackpadMode != TrackpadMode::None || (m_wheelClock.isValid() && m_wheelClock.elapsed() < 300);
  // Draft has nothing to lower but the silhouettes of Shaded + edges.
  if (!m_adaptive || (m_renderQuality == 0 && m_style != Style::ShadedEdges) || m_fullFrameMs < kSmoothFrameMs || !navigating) return;
  m_degraded = true;
  applyQuality();
  outlineBodies();
  m_qualityTimer.start();
  if (trace::enabled()) trace::log(QStringLiteral("quality: lowered while navigating (a full frame took %1 ms)").arg(m_fullFrameMs));
}

void Viewport::restoreQuality() {
  m_qualityTimer.stop();
  if (!m_degraded || !m_initialised) return;
  m_degraded = false;
  applyQuality();
  outlineBodies();
  m_view->Invalidate();
  requestRedraw();
  if (trace::enabled()) trace::log(QStringLiteral("quality: full again"));
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
  if (m_style == Style::HiddenLine || m_style == Style::HiddenEdges) setStyle(m_style);  // faces in the background's colour
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
      if(m_sketchInput)lookAt(m_sketchFrame,false,false);
      else if (std::abs(d.Z()) >= std::max(std::abs(d.X()), std::abs(d.Y())))
        m_view->SetProj(d.Z() < 0 ? V3d_Zpos : V3d_Zneg);
      else if (std::abs(d.X()) >= std::abs(d.Y())) m_view->SetProj(d.X() < 0 ? V3d_Xpos : V3d_Xneg);
      else m_view->SetProj(d.Y() < 0 ? V3d_Ypos : V3d_Yneg);
      // SetProj keeps where the world origin was on screen, so a model far from it left the view: keep the view centre.
      if (!m_sketchInput) {
        const gp_Pnt center = m_threeDimensionalCamera->Center();
        const auto camera = m_view->Camera();
        camera->SetEyeAndCenter(camera->Eye().Translated(gp_Vec(camera->Center(), center)), center);
      }
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
  // The grid lies in the plane 2D mode looks at (a sketch keeps its own plane), and in 2D mode it never ends.
  if (!m_sketchInput) {
    const gp_Dir d = m_view->Camera()->Direction();
    const double ax = std::abs(d.X()), ay = std::abs(d.Y()), az = std::abs(d.Z());
    if (!on || az >= std::max(ax, ay)) m_viewer->SetPrivilegedPlane(gp_Ax3(gp::Origin(), gp::DZ(), gp::DX()));
    else if (ax >= ay) m_viewer->SetPrivilegedPlane(gp_Ax3(gp::Origin(), gp::DX(), gp::DY()));
    else m_viewer->SetPrivilegedPlane(gp_Ax3(gp::Origin(), gp::DY(), gp::DZ()));
  }
  updateGridExtent();
  m_ctx->ClearDetected(false);
  ResetPreviousMoveTo();
  redrawScene();
}
