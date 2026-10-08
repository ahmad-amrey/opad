// Perspective zoom at the pointer, also into a part (the report "camera zoom as I am inside the part: imagine trying to
// select the upper face of a slot in a box with its upper face removed"). OCCT's own zoom asks the selection's picker for
// the depth under the pointer: in the Edges or Vertices filter, in a sketch (bodies unpickable), over a locked or inactive
// body or into an opening it finds nothing there and zooms at the window's middle or towards the camera's centre, which
// it only ever nears: the zoom slowed to a crawl and stopped, away from the pointer. Found, it never came closer than a
// fixed 1 mm, a crawl on a large model and a jump on a small one. Here every step flies the eye along the pointer's ray
// (what is under the pointer stays there) by a share of the distance to what is drawn there, never less than a floor:
//   - the depth is the navigation picker's (every displayed body, whatever picks), else a sketch's or a drawing's plane
//     under the pointer, else (an opening, empty space) the distance to the model's middle, at least a tenth of its size;
//   - the floor is 0.5 % of the size of the body under the pointer (of the model in empty space) and three times the
//     nearest the near plane comes, so the surface being neared stays in front of it until the eye passes through it:
//     the steps never shrink to nothing, and a surface reached or an opening is gone through.
// Orthographic zoom stays OCCT's: a scale at the pointer, which never stalls and clips nothing (its depth range is the
// whole model). Bench: OPAD_BENCH_CAVITYZOOM (ViewportZoomBench.cpp).
#include <Bnd_Box.hxx>
#include <Graphic3d_Camera.hxx>
#include <SelectMgr_ViewerSelector.hxx>
#include <V3d_View.hxx>

#include <algorithm>
#include <cmath>

#include "BodyShape.hpp"
#include "Viewport.hpp"

void Viewport::handleCameraActions(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view, const AIS_WalkDelta& walk) {
  if (myToAllowZooming && !myGL.ZoomActions.IsEmpty() && !view->Camera()->IsOrthographic() && !view->Camera()->IsStereo()) {
    NCollection_Sequence<Aspect_ScrollDelta> rest;  // a zoom with no pointer (none in OPAD today) stays OCCT's
    for (NCollection_Sequence<Aspect_ScrollDelta>::Iterator it(myGL.ZoomActions); it.More(); it.Next()) {
      if (it.Value().HasPoint()) zoomAlongRay(view, it.Value().Point, it.Value().Delta);
      else rest.Append(it.Value());
    }
    myGL.ZoomActions = rest;
  }
  AIS_ViewController::handleCameraActions(ctx, view, walk);
}

void Viewport::zoomAlongRay(const Handle(V3d_View)& view, const Graphic3d_Vec2i& pixel, double delta) {
  if (delta == 0.0 || view->Window().IsNull()) return;
  AbortViewAnimation();
  const Handle(Graphic3d_Camera)& camera = view->Camera();
  Standard_Integer w = 0, h = 0;
  view->Window()->Size(w, h);
  if (w <= 0 || h <= 0) return;
  // The pixel's ray, as V3d_View::Convert maps pixels (the y axis up from the bottom row).
  const gp_Pnt eye = camera->Eye();
  const gp_Pnt through = camera->UnProject(gp_Pnt(2.0 * pixel.x() / w - 1.0, 2.0 * (h - 1 - pixel.y()) / h - 1.0, 0.0));
  gp_Vec ray(eye, through);
  if (ray.SquareMagnitude() < 1e-24) return;
  ray.Normalize();
  const gp_Vec ahead(camera->Direction());
  if (ray.Dot(ahead) <= 0) return;

  Bnd_Box scene = view->View()->MinMaxValues();
  const double sceneSize = scene.IsVoid() ? camera->Distance() : std::sqrt(scene.SquareExtent());
  const gp_Pnt middle = scene.IsVoid() ? camera->Center() : gp_Pnt((scene.CornerMin().XYZ() + scene.CornerMax().XYZ()) / 2);
  // What is drawn under the pointer: the nearest displayed body (whatever the filter picks), with its own size.
  double depth = -1, size = sceneSize;
  if (!m_navSelector.IsNull()) {
    const Handle(V3d_View) picking = view == m_view ? navView() : view;  // A's view shares this camera: what is drawn there
    m_navSelector->Pick(pixel.x(), pixel.y(), picking);
    for (int i = 1; i <= m_navSelector->NbPicked() && depth < 0; ++i) {
      Bnd_Box box;
      if (navDrawn(m_navSelector->Picked(i)->Selectable().get(), picking, &box).IsNull()) continue;
      const double d = gp_Vec(eye, m_navSelector->PickedPoint(i)).Dot(ray);
      if (d <= 0) continue;
      depth = d;
      if (!box.IsVoid()) size = std::sqrt(box.SquareExtent());
    }
  }
  if (depth < 0) {  // a sketch's plane, or a drawing's inside its bounds
    bool inside = false;
    const gp_Pnt onPlane = drawingPlanePoint(QPointF(pixel.x() / devicePixelRatioF(), pixel.y() / devicePixelRatioF()), inside);
    if (inside && gp_Vec(eye, onPlane).Dot(ray) > 0) depth = gp_Vec(eye, onPlane).Dot(ray);
  }
  if (depth < 0) depth = std::max(eye.Distance(middle), 0.1 * sceneSize);  // an opening, or nothing at all
  // The nearest the near plane comes (Graphic3d_Camera::ZFitAll: 5.97e-4 of the depth of the model's middle, at most the
  // distance to its farthest corner): three times that keeps the surface neared in front of it.
  double farthest = sceneSize;
  if (!scene.IsVoid()) {
    const gp_Pnt lo = scene.CornerMin(), hi = scene.CornerMax();
    farthest = 0;
    for (int i = 0; i < 8; ++i) farthest = std::max(farthest, eye.Distance(gp_Pnt(i & 1 ? hi.X() : lo.X(), i & 2 ? hi.Y() : lo.Y(), i & 4 ? hi.Z() : lo.Z())));
  }
  const double floor = std::max({0.005 * size, 3.0 * 5.97e-4 * farthest, 1e-6 * sceneSize, 1e-9});

  // OCCT's scale for a scroll: a delta of 100 doubles (or halves) the view.
  const double k = std::abs(delta) / 100.0 + 1.0;
  const double reach = std::max(depth, floor);
  const double move = delta > 0 ? reach * (1.0 - 1.0 / k) : -reach * (k - 1.0);
  if (delta < 0 && eye.Distance(middle) > 1e7 * std::max(sceneSize, 1e-9)) return;  // far enough out
  const gp_Pnt to = eye.Translated(ray * move);
  // The camera's centre at the depth of what is under the pointer (the next orbit or zoom without a pick turns or flies
  // towards it), never behind the floor.
  const double centre = std::max((depth - move) * ray.Dot(ahead), floor);
  camera->SetEyeAndCenter(to, to.Translated(ahead * centre));
  view->Invalidate();
}
