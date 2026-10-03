// Benches of the view's looks and navigation (T2 viewer): OPAD_BENCH_TRANSPARENCY (UI-39), OPAD_BENCH_HIGHLIGHT (UI-38).
// Cases in tools/bench_cases/viewer.py; Qt events stay within the hidden window.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
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
#include "DesignController.hpp"
#include "Highlight.hpp"
#include "MainWindow.hpp"
#include "NavCube.hpp"
#include "PlanePicker.hpp"
#include "SketchEditor.hpp"
#include "Viewport.hpp"
#include "opad/design/sketch_geom.hpp"

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

QString rgb(const QColor& c) { return QString("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue()); }
bool hued(const QColor& c) { return c.blue() - std::max(c.red(), c.green() / 2) > 35; }  // the selection blue, also blended
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }

// A widget point's pixel in a frame grabbed at device resolution.
QColor pixel(const QImage& frame, const QWidget* view, const QPointF& at) {
  const int x = qRound(at.x() * frame.width() / view->width()), y = qRound(at.y() * frame.height() / view->height());
  return frame.rect().contains(x, y) ? frame.pixelColor(x, y) : QColor();
}

// The pixels of a vertical run of `reach` widget points either side of `at` that pass `test`.
int across(const QImage& frame, const QWidget* view, const QPointF& at, double reach, const std::function<bool(const QColor&)>& test) {
  const double scale = double(frame.height()) / view->height();
  const int x = qRound(at.x() * scale), y = qRound(at.y() * scale), r = qRound(reach * scale);
  int n = 0;
  for (int dy = -r; dy <= r; ++dy)
    if (frame.rect().contains(x, y + dy) && test(frame.pixelColor(x, y + dy))) ++n;
  return n;
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

// In the current theme, on a grey box and a blue one (close to the selection colour) in an iso view: (1) a hovered body
// glows white, the selected one is hued and unlike its hover; (2) the blue one selected gets an outline (its edges drawn
// thicker in a colour off its own); (3) a face is hued, an edge thicker than its hover in a halo, a vertex hued; (4) the
// cube's TOP side in a top view is drawn as selected, its hover white. <prefix>.<theme>.*.png.
bool Viewport::benchHighlight(const QString& prefix) {
  bool all = true;
  const QString theme = m_tokens.dark ? "dark" : "light";
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: highlight %1: %2 %3").arg(theme, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  std::string grey, blue;  // the body of least chroma, and the one nearest the selection colour (under kCloseDeltaE)
  double chroma = 1e9, nearest = highlight::kCloseDeltaE;
  for (const auto& [id, item] : m_items) {
    const QColor c = shownColor(id);
    if (const double d = theme::deltaE(c, m_tokens.selected3d); d < nearest) nearest = d, blue = id;
    if (const double s = c.hsvSaturationF(); s < chroma) chroma = s, grey = id;
  }
  if (!require(!grey.empty() && !blue.empty(), "a grey body and one in the selection colour are displayed")) return false;
  const double scale = displayScale();
  auto box = [this](const std::string& id) {
    Bnd_Box b;
    m_items.at(id).ais->BoundingBox(b);
    return b;
  };
  auto at = [this](double x, double y, double z) { return QPointF(widgetPoint({x, y, z})); };
  auto filter = [this](SelFilter f) {
    m_ctx->ClearSelected(Standard_False);
    m_ctx->ClearDetected(Standard_False);
    applySelectionLayers();
    refreshSubHighlight();
    m_filter = f;
    for (auto& [id, item] : m_items) activateSelection(item.ais);
  };
  auto frame = [this] {
    m_view->Redraw();
    m_view->RedrawImmediate();
    return grabImage();
  };
  auto hover = [this](const QPointF& p) {
    moveTo(devicePos(p));
    return m_ctx->HasDetected();
  };
  auto click = [this](const QPointF& p) {  // what is under p becomes the selection, as a click hands it over
    moveTo(devicePos(p));
    if (!m_ctx->HasDetected()) return false;
    m_ctx->SelectDetected(AIS_SelectionScheme_Replace);
    OnSelectionChanged(m_ctx, m_view);
    m_ctx->ClearDetected(Standard_False);
    return true;
  };
  const QPointF away(4, height() - 4);
  myViewAnimation->Stop();
  standardView("iso");
  m_view->FitAll(fitBounds(), 0.2, Standard_False);
  const Bnd_Box g = box(grey), b = box(blue);
  const gp_Pnt gl = g.CornerMin(), gh = g.CornerMax();
  const QPointF top = at((gl.X() + gh.X()) / 2, (gl.Y() + gh.Y()) / 2, gh.Z());

  // (1) Body: plain, hovered, selected.
  filter(SelFilter::Body);
  hover(away);
  const QColor plain = pixel(frame(), this, top);
  require(hover(top) && m_ctx->DetectedInteractive() == m_items.at(grey).ais, "the grey body's top is hovered");
  QImage shot = frame();
  const QColor hovered = pixel(shot, this, top);
  shot.save(prefix + "." + theme + ".hover.png");
  require(!hued(hovered) && hovered.lightness() > plain.lightness() + 15, QString("hover glows white: %1 over %2").arg(rgb(hovered), rgb(plain)));
  require(click(top) && m_bodyGlows.count(m_items.at(grey).ais.get()), "a click selects it, with its glow");
  hover(away);
  shot = frame();
  shot.save(prefix + "." + theme + ".body.png");
  const QColor selected = pixel(shot, this, top);
  require(hued(selected) && channelGap(selected, hovered) > 40, QString("the selection is hued and unlike the hover: %1").arg(rgb(selected)));
  const auto& greyGlow = m_bodyGlows.at(m_items.at(grey).ais.get())->style();
  require(greyGlow.edge.IsEqual(occ(m_tokens.selected3d)) && !highlight::closeToSelection(m_tokens, shownColor(grey)),
          "a grey body's edges in the selection colour, no outline");

  // (2) The blue body: outlined. Its edges are drawn in a colour off its own, thicker than its plain edges.
  const gp_Pnt bl = b.CornerMin(), bh = b.CornerMax();
  const QPointF blueEdge = at((bl.X() + bh.X()) / 2, bl.Y(), bh.Z());  // the front top edge in an iso view
  const QColor blueColour = shownColor(blue), outline = highlight::outlineFor(blueColour);
  auto outlined = [&](const QColor& c) { return channelGap(c, outline) < 40; };
  m_ctx->ClearSelected(Standard_False);
  applySelectionLayers();
  const int plainEdge = across(frame(), this, blueEdge, 12, outlined);
  opad::Ref blueRef;
  blueRef.body = blue;
  selectRefs({blueRef});
  shot = frame();
  shot.save(prefix + "." + theme + ".outline.png");
  const int outlineEdge = across(shot, this, blueEdge, 12, outlined);
  const auto glow = m_bodyGlows.find(m_items.at(blue).ais.get());
  require(glow != m_bodyGlows.end() && glow->second->style().edge.IsEqual(occ(outline)) && theme::deltaE(outline, blueColour) > 40,
          QString("a body in the selection colour is outlined in %1").arg(outline.name()));
  require(outlineEdge >= 2 * scale && outlineEdge > plainEdge, QString("its outline is drawn: %1 px across the front edge, %2 plain").arg(outlineEdge).arg(plainEdge));

  // (3) Sub-shapes of the grey body: a face, an edge (thicker than its hover, in a halo), a vertex.
  filter(SelFilter::Face);
  require(click(top) && !m_subHl.IsNull() && !m_subHl->m_triangles.empty(), "a face is selected");
  hover(away);
  shot = frame();
  require(hued(pixel(shot, this, top)), QString("the face is hued: %1").arg(rgb(pixel(shot, this, top))));
  filter(SelFilter::Edge);
  const QPointF edge = at((gl.X() + gh.X()) / 2, gl.Y(), gh.Z());
  require(hover(edge) && !Handle(SubShapeOwner)::DownCast(m_ctx->DetectedOwner()).IsNull(), "the front top edge is hovered");
  shot = frame();
  const int hoverWidth = across(shot, this, edge, 12, [](const QColor& c) { return std::min({c.red(), c.green(), c.blue()}) > 225; });
  require(click(edge) && !m_subHl.IsNull() && !m_subHl->m_segments.empty(), "the edge is selected");
  hover(away);
  shot = frame();
  shot.save(prefix + "." + theme + ".edge.png");
  const int selectedWidth = across(shot, this, edge, 12, hued);
  const auto& edgeStyle = m_subHl->style();
  require(edgeStyle.edgeWidth >= highlight::kHoverEdgeWidth && edgeStyle.haloWidth > edgeStyle.edgeWidth,
          QString("an edge is %1 px in a %2 px halo").arg(edgeStyle.edgeWidth).arg(edgeStyle.haloWidth));
  require(selectedWidth > hoverWidth && selectedWidth >= 3 * scale, QString("the selected edge is %1 px across, its hover %2").arg(selectedWidth).arg(hoverWidth));
  filter(SelFilter::Vertex);
  const QPointF corner = at(gh.X(), gl.Y(), gh.Z());
  require(click(corner) && !m_subHl.IsNull() && !m_subHl->m_points.empty(), "a vertex is selected");
  hover(away);
  require(across(frame(), this, corner, 4, hued) >= 2, "the vertex is hued");
  filter(SelFilter::Body);

  // (4) The cube: the side a standard view looks at is drawn as selected; its hover glows white.
  const QPointF cube = cubeCentre(), onSide = cube + QPointF(-18, 18);  // off the label
  standardView("top");
  updateCubeSide();
  shot = frame();
  shot.save(prefix + "." + theme + ".cube.png");
  const QColor side = pixel(shot, this, onSide);
  require(Handle(NavCube)::DownCast(m_cube)->currentSide() == V3d_Zpos && hued(side), QString("a top view draws the cube's TOP as selected: %1").arg(rgb(side)));
  standardView("iso");
  updateCubeSide();
  const QColor rest = pixel(frame(), this, cube);
  require(Handle(NavCube)::DownCast(m_cube)->currentSide() == -1, "an iso view looks at no side");
  require(hover(cube) && m_ctx->DetectedInteractive() == m_cube, "the cube is hovered");
  shot = frame();
  const QColor cubeHover = pixel(shot, this, cube);
  require(!hued(cubeHover) && std::min({cubeHover.red(), cubeHover.green(), cubeHover.blue()}) > 200 && cubeHover.lightness() > rest.lightness(),
          QString("the cube's hover glows white: %1 over %2").arg(rgb(cubeHover), rgb(rest)));
  m_ctx->ClearDetected(Standard_False);
  m_ctx->ClearSelected(Standard_False);
  applySelectionLayers();
  refreshSubHighlight();
  redrawScene();
  return all;
}

namespace {
// A line in a sketch on XY in front of the boxes: selected, it is drawn in the selection colour over a halo; hovered
// (nothing selected), it glows white. <prefix>.<theme>.sketch.png.
bool sketchRoles(Viewport* v, SketchEditor* sketch, const QString& prefix) {
  const Tokens& t = v->tokens();
  const QString theme = t.dark ? "dark" : "light";
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: highlight %1: sketch: %2 %3").arg(theme, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const double scale = v->displayScale();
  const QPointF mid(v->widgetPoint({25, -40, 0}));
  sketch->sketchPress(25, -40, Qt::NoModifier);
  sketch->sketchRelease(25, -40, Qt::NoModifier);
  sketch->sketchMove(25, -90, Qt::NoModifier, false);
  QImage frame = v->grabImage();
  frame.save(prefix + "." + theme + ".sketch.png");
  const int selected = across(frame, v, mid, 14, hued);
  require(selected >= 4 * scale, QString("a selected line is hued over its halo: %1 px across").arg(selected));
  sketch->sketchPress(25, -90, Qt::NoModifier);  // nothing there: the selection is cleared
  sketch->sketchRelease(25, -90, Qt::NoModifier);
  sketch->sketchMove(25, -40, Qt::NoModifier, false);
  frame = v->grabImage();
  frame.save(prefix + "." + theme + ".sketch-hover.png");
  const QColor glow = highlight::hoverHalo(t);
  const int hovered = across(frame, v, mid, 14, [&](const QColor& c) { return channelGap(c, glow) < 12 && channelGap(c, t.vp) >= 15; });
  require(hovered >= 3 * scale && !hued(pixel(frame, v, mid)), QString("a hovered line glows white: %1 px of the glow across").arg(hovered));
  sketch->sketchMove(25, -90, Qt::NoModifier, false);
  return all;
}
}  // namespace

// Both themes: the view's roles, then a sketch's.
OPAD_BENCH(OPAD_BENCH_HIGHLIGHT, highlight) {
  Viewport* v = w.m_viewport;
  whenDisplayed(&w, v, [&w, v, value] {
    bool ok = true;
    for (const bool dark : {true, false}) {
      w.applyTheme(dark);
      ok = v->benchHighlight(value) && ok;
    }
    QElapsedTimer clock;
    auto until = [&clock](const std::function<bool()>& done) {
      clock.restart();
      while (!done() && clock.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    };
    PlanePicker* picker = w.m_design->planePicker();
    w.m_design->startSketch();  // the XY plane, its origin where it is
    picker->choose({{"base", "xy"}});
    until([picker] { return picker->positioning(); });
    picker->apply();
    until([&w] { return w.m_design->sketchActive(); });
    SketchEditor* sketch = w.m_design->sketch();
    if (!w.m_design->sketchActive()) {
      trace::log("bench: highlight: a sketch on XY is open FAIL");
      return false;
    }
    v->lookAt(opad::design::base_frame("xy"), true, false);
    auto settle = [&] { until([sketch] { return !sketch->busy(); }); };
    sketch->setTool("line");
    for (const double x : {-10.0, 60.0}) {
      sketch->sketchPress(x, -40, Qt::NoModifier);
      sketch->sketchRelease(x, -40, Qt::NoModifier);
      settle();
    }
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    sketch->sketchKey(&escape);
    settle();
    sketch->setTool("select");
    settle();
    for (const bool dark : {true, false}) {
      w.applyTheme(dark);
      ok = sketchRoles(v, sketch, value) && ok;
    }
    w.applyTheme(true);
    return ok;
  });
  return true;
}

OPAD_BENCH(OPAD_BENCH_TRANSPARENCY, transparency) {
  Viewport* v = w.m_viewport;
  whenDisplayed(&w, v, [v, value] { return v->benchTransparency(value); });
  return true;
}
