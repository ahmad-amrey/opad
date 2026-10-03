// Benches of the view's looks and navigation (T2 viewer): OPAD_BENCH_TRANSPARENCY (UI-39). Cases in
// tools/bench_cases/viewer.py; Qt events stay within the hidden window.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Graphic3d_MaterialAspect.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
// Runs `run` once every body of the document is displayed (or after a minute, reported), then quits with its outcome.
void whenDisplayed(QObject* context, Viewport* v, std::function<bool()> run) {
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(context);
  QObject::connect(timer, &QTimer::timeout, context, [v, timer, clock, run] {
    const bool ready = !v->pumpJob() && v->remainingBodies() == 0 && v->displayedCount() > 0;
    if (!ready && clock->elapsed() < 60000) return;
    timer->stop();
    timer->deleteLater();
    if (!ready) trace::log("bench: view: the model is displayed FAIL");
    QCoreApplication::exit(ready && run() ? 0 : 2);
  });
  timer->start(50);
}

int channelGap(const QColor& a, const QColor& b) {
  return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
}
}  // namespace

// Two boxes, red and blue at half opacity, overlap in a top view; each quality draws them in one display order and then
// in the other. Unordered blending (the control) gives the overlap the colour of the last one drawn; OIT the same both
// ways, with both colours in it. <prefix>.oit.png: the Studio frame.
bool Viewport::benchTransparency(const QString& prefix) {
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: transparency: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!m_initialised) return require(false, "the view is up");
  const Handle(Graphic3d_Camera) camera = new Graphic3d_Camera(*m_view->Camera());
  const int quality = m_renderQuality;
  std::vector<Handle(AIS_Shape)> hidden;
  for (const auto& [id, item] : m_items)
    if (m_ctx->IsDisplayed(item.ais)) {
      hidden.push_back(item.ais);
      m_ctx->Erase(item.ais, Standard_False);
    }
  auto glass = [](const TopoDS_Shape& shape, const Quantity_Color& color) {
    BRepMesh_IncrementalMesh mesh(shape, 0.1);
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    Handle(AIS_Shape) ais = new BodyShape(shape, BodyPrs::build(shape, box));
    Graphic3d_MaterialAspect matte(Graphic3d_NameOfMaterial_Plastified);
    matte.SetSpecularColor(Quantity_NOC_BLACK);
    ais->SetMaterial(matte);
    ais->SetColor(color);
    ais->SetTransparency(0.5);
    return ais;
  };
  const Handle(AIS_Shape) red = glass(BRepPrimAPI_MakeBox(gp_Pnt(-14, -10, 0), 18, 20, 10).Shape(), Quantity_Color(0.9, 0.1, 0.1, Quantity_TOC_sRGB));
  const Handle(AIS_Shape) blue = glass(BRepPrimAPI_MakeBox(gp_Pnt(-4, -10, 2), 18, 20, 10).Shape(), Quantity_Color(0.1, 0.2, 0.9, Quantity_TOC_sRGB));
  m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
  m_view->Camera()->SetEyeAndCenter(gp_Pnt(0, 0, 100), gp_Pnt(0, 0, 6));
  m_view->Camera()->SetUp(gp::DY());
  m_view->Camera()->SetScale(60);
  QImage frame;
  auto overlap = [&](bool swapped) {
    m_ctx->Remove(red, Standard_False);
    m_ctx->Remove(blue, Standard_False);
    for (const auto& shape : swapped ? std::vector{blue, red} : std::vector{red, blue}) m_ctx->Display(shape, AIS_Shaded, -1, Standard_False);
    m_view->Redraw();
    frame = grabImage();
    const QPoint at = widgetPoint({0, 0, 12});
    return frame.pixelColor(qRound(at.x() * double(frame.width()) / width()), qRound(at.y() * double(frame.height()) / height()));
  };
  for (const int level : {0, 1}) {
    setRenderQuality(level);
    require(m_view->RenderingParams().TransparencyMethod == Graphic3d_RTM_BLEND_OIT, QString("quality %1 rasterises translucency with OIT").arg(level));
    m_view->ChangeRenderingParams().TransparencyMethod = Graphic3d_RTM_BLEND_UNORDERED;
    const QColor u1 = overlap(false), u2 = overlap(true);
    m_view->ChangeRenderingParams().TransparencyMethod = Graphic3d_RTM_BLEND_OIT;
    const QColor o1 = overlap(false), o2 = overlap(true);
    auto name = [](const QColor& c) { return QString("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue()); };
    require(channelGap(u1, u2) > 20, QString("quality %1: unordered blending depends on the order (control): %2 then %3").arg(level).arg(name(u1), name(u2)));
    require(channelGap(o1, o2) <= 6 && o1.red() > 40 && o1.blue() > 40,
            QString("quality %1: OIT overlap %2 either way (%3 swapped), both colours in it").arg(level).arg(name(o1), name(o2)));
    if (level == 1 && !prefix.isEmpty()) frame.save(prefix + ".oit.png");
  }
  m_ctx->Remove(red, Standard_False);
  m_ctx->Remove(blue, Standard_False);
  for (const auto& shape : hidden) m_ctx->Display(shape, Standard_False);
  setRenderQuality(quality);
  m_view->SetCamera(camera);
  redrawScene();
  return all;
}

OPAD_BENCH(OPAD_BENCH_TRANSPARENCY, transparency) {
  Viewport* v = w.m_viewport;
  whenDisplayed(&w, v, [v, value] { return v->benchTransparency(value); });
  return true;
}
