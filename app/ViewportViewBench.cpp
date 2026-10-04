// Benches of the view's looks and navigation (T2 viewer): OPAD_BENCH_TRANSPARENCY (UI-39), OPAD_BENCH_HIGHLIGHT (UI-38),
// OPAD_BENCH_NAVIGATE (UI-47).
// Cases in tools/bench_cases/viewer.py; Qt events stay within the hidden window.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>

#include <AIS_RubberBand.hxx>
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

// A sketch's wire in 3D (its line in the sketch blue, close to the selection colour): hovered it is picked; selected, it is
// drawn like a picked edge, thick in the selection colour over a halo, not outlined (on a line an outline would be white,
// the hover's colour).
bool Viewport::benchWireHighlight(const std::string& sketch, const QString& prefix) {
  const QString theme = m_tokens.dark ? "dark" : "light";
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: highlight %1: sketch wire: %2 %3").arg(theme, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  QElapsedTimer clock;  // leaving the sketch made everything pickable again: a sliced job
  clock.start();
  while ((m_filterJob || m_lookJob) && clock.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  const auto wire = m_sketchWires.find(sketch);
  if (!require(wire != m_sketchWires.end(), "the finished sketch is drawn as a wire")) return false;
  standardView("top");
  m_view->Redraw();
  const QPointF mid(widgetPoint({25, -40, 0}));
  moveTo(devicePos(mid));
  require(m_ctx->HasDetected() && m_ctx->DetectedInteractive() == wire->second.ais, "hovered, it is picked");
  m_ctx->SelectDetected(AIS_SelectionScheme_Replace);
  OnSelectionChanged(m_ctx, m_view);
  m_ctx->ClearDetected(Standard_False);
  const auto glow = m_bodyGlows.find(wire->second.ais.get());
  require(glow != m_bodyGlows.end() && glow->second->style().edge.IsEqual(occ(m_tokens.selected3d)) &&
              glow->second->style().haloWidth > glow->second->style().edgeWidth && glow->second->style().edgeWidth >= highlight::kHoverEdgeWidth,
          "selected, it glows in the selection colour over a halo, as thick as a picked edge, not outlined");
  m_view->Redraw();
  const QImage frame = grabImage();
  frame.save(prefix + "." + theme + ".wire.png");
  const int thick = across(frame, this, mid, 12, hued);
  require(thick >= 3 * displayScale(), QString("%1 px of it across the line").arg(thick));
  m_ctx->ClearSelected(Standard_False);
  applySelectionLayers();
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
    w.m_design->finishSketch();
    until([&w] { return !w.m_design->sketchActive() && !w.m_doc->scene.sketches.empty(); });
    const std::string wire = w.m_doc->scene.sketches.empty() ? std::string() : w.m_doc->scene.sketches.back().id;
    until([v, &wire] { return !v->looksPending(); });
    for (const bool dark : {true, false}) {
      w.applyTheme(dark);
      ok = v->benchWireHighlight(wire, value) && ok;
    }
    w.applyTheme(true);
    return ok;
  });
  return true;
}

// (1) The zoom window: Z's mode frames a dragged rectangle (its centre comes to the view's centre, the zoom by its width), a
// click zooms in twice there, Esc and a right click leave it untouched. (2) Fit, Home, a standard view and the previous
// view animate from where the camera is to exactly where the instant move goes. (3) Previous and next view walk the views
// the camera rested at; a new view drops what was ahead. (4) CAD 2D: the middle button pans, Shift+middle too (it orbits
// in the Fusion preset: the control), nothing orbits. (5) A right click on the cube asks for its menu, elsewhere for the
// context menu. (6) In 2D mode the view twists: a turn, a typed angle, untwisted. <prefix>.zoom-band.png: the rectangle
// being dragged. The document's Home is the window part's (its commands write view ops).
bool Viewport::benchNavigation(const QString& prefix) {
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: navigate: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!require(m_initialised && !m_items.empty(), "a model is displayed")) return false;
  auto mouse = [this](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent e(type, at, mapToGlobal(at), button, buttons, mods);
    QCoreApplication::sendEvent(this, &e);
    paintEvent(nullptr);  // a hidden window has no paint cycle: the controller's input is applied here
  };
  auto drag = [&](Qt::MouseButton button, const QPointF& from, const QPointF& to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    mouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton, mods);
    mouse(QEvent::MouseButtonPress, from, button, button, mods);
    for (int i = 1; i <= 4; ++i) mouse(QEvent::MouseMove, from + (to - from) * (i / 4.0), Qt::NoButton, button, mods);
    mouse(QEvent::MouseButtonRelease, to, button, Qt::NoButton, mods);
  };
  auto direction = [this] { return m_view->Camera()->Direction(); };
  myViewAnimation->Stop();
  m_forceAnimate = false;
  standardView("iso");
  fitAll();
  m_view->Redraw();
  const QPointF centre(width() / 2.0, height() / 2.0);

  // (1) Zoom window.
  const ViewState start = viewState();
  startZoomWindow();
  require(m_zoomWindow && cursor().shape() == Qt::CrossCursor, "the zoom window waits for a rectangle, cross cursor");
  const QPointF a(width() * 0.40, height() * 0.38), b(width() * 0.60, height() * 0.52), middle = (a + b) / 2;
  Standard_Real px = 0, py = 0, pz = 0;
  m_view->Convert(devicePos(middle).x(), devicePos(middle).y(), px, py, pz);  // the point under the rectangle's centre
  mouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
  mouse(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
  require(m_ctx->IsDisplayed(myRubberBand), "the rectangle is drawn while dragging");
  m_view->Redraw();
  grabImage().save(prefix + ".zoom-band.png");
  mouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
  const ViewState zoomed = viewState();
  const QPointF there(widgetPoint({px, py, pz}));
  require(!m_zoomWindow && !m_ctx->IsDisplayed(myRubberBand) && cursor().shape() != Qt::CrossCursor, "the drag ends it, the rectangle goes");
  require(std::abs(zoomed.scale / start.scale - 0.2) < 0.03, QString("it zooms by the rectangle's width: %1 of the height before").arg(zoomed.scale / start.scale));
  require(QLineF(there, centre).length() < 6, QString("what was at the rectangle's centre is at the view's: %1 px off").arg(QLineF(there, centre).length()));
  startZoomWindow();
  QKeyEvent escape(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
  QCoreApplication::sendEvent(this, &escape);
  require(!m_zoomWindow && escape.isAccepted() && viewState().same(zoomed), "Esc leaves it at the shortcut stage, the view as it was");
  int menus = 0;
  const auto counted = connect(this, &Viewport::contextMenuRequested, this, [&menus] { ++menus; });
  startZoomWindow();
  mouse(QEvent::MouseButtonPress, centre, Qt::RightButton, Qt::RightButton);
  mouse(QEvent::MouseButtonRelease, centre, Qt::RightButton, Qt::NoButton);
  require(!m_zoomWindow && menus == 0 && viewState().same(zoomed), "a right click leaves it, no context menu");
  startZoomWindow();
  mouse(QEvent::MouseButtonPress, centre, Qt::LeftButton, Qt::LeftButton);
  mouse(QEvent::MouseButtonRelease, centre, Qt::LeftButton, Qt::NoButton);
  require(std::abs(viewState().scale / zoomed.scale - 0.5) < 0.02, "a click zooms in twice there");

  // (2) Animated moves end exactly where the instant ones go.
  m_forceAnimate = true;
  auto animated = [&](const QString& what, const std::function<void(bool)>& move) {
    const Handle(Graphic3d_Camera) from = new Graphic3d_Camera(*m_view->Camera());
    const ViewState origin = viewState();
    move(false);
    const ViewState instant = viewState();
    m_view->Camera()->Copy(from);
    move(true);
    const bool running = !myViewAnimation->IsStopped(), startsHere = viewState().same(origin);
    myViewAnimation->Update(0.12);
    const ViewState partway = viewState();
    myViewAnimation->Update(10.0);
    myViewAnimation->Stop();
    require(running && startsHere && !partway.same(origin) && !partway.same(instant) && viewState().same(instant),
            QString("%1 animates, partway at 0.12 s, and ends where the instant move goes").arg(what));
  };
  standardView("iso");
  animated("a front view", [this](bool on) { standardView("front", on); });
  animated("Home", [this](bool on) { home(on); });
  m_view->Camera()->SetScale(m_view->Camera()->Scale() / 6);
  animated("Fit all", [this](bool on) { fitAll(on); });
  m_forceAnimate = false;

  // (3) Previous and next view.
  m_history.clear();
  standardView("top");
  settleView();
  const ViewState top = viewState();
  standardView("front");
  settleView();
  const ViewState front = viewState();
  standardView("right");
  const ViewState right = viewState();  // not rested: Previous records it first
  require(previousView() && viewState().same(front), "Previous goes back to the front view, the right one recorded");
  require(previousView() && viewState().same(top), "and to the top view");
  require(!previousView() && viewState().same(top), "nothing before the first");
  require(nextView() && viewState().same(front) && nextView() && viewState().same(right) && !nextView(), "Next walks forward to the last");
  require(previousView() && viewState().same(front), "back once more");
  standardView("iso");
  settleView();
  require(!nextView() && m_history.size() == 3, "a new view drops what was ahead");
  require(previousView() && viewState().same(front), "and goes back to where it left");

  // (4) The CAD 2D preset: no orbit on any button.
  const NavPreset preset = m_preset;
  standardView("iso");
  fitAll();
  setNavPreset(NavPreset::Fusion);
  gp_Dir d0 = direction();
  drag(Qt::MiddleButton, centre, centre + QPointF(60, 30), Qt::ShiftModifier);
  require(!direction().IsParallel(d0, 1e-3), "Fusion: Shift+middle orbits (the control)");
  standardView("iso");
  fitAll();
  setNavPreset(NavPreset::Cad2D);
  bool orbits = false;
  for (AIS_MouseGestureMap::Iterator it(ChangeMouseGestureMap()); it.More(); it.Next())
    orbits = orbits || it.Value() == AIS_MouseGesture_RotateOrbit || it.Value() == AIS_MouseGesture_RotateView;
  require(!orbits && ChangeMouseGestureMap().Find(Aspect_VKeyMouse_MiddleButton) == AIS_MouseGesture_Pan, "CAD 2D binds no orbit, the middle button pans");
  d0 = direction();
  const gp_Pnt c0 = m_view->Camera()->Center();
  drag(Qt::MiddleButton, centre, centre + QPointF(60, 30));
  const gp_Pnt c1 = m_view->Camera()->Center();
  drag(Qt::MiddleButton, centre, centre + QPointF(60, 30), Qt::ShiftModifier);
  require(direction().IsParallel(d0, 1e-9) && c0.Distance(c1) > pixelSize() * 20, "CAD 2D: a middle drag pans, Shift+middle does not orbit");
  setNavPreset(preset);

  // (5) The cube's menu: which menu a right click asks for (signals held: the window's context menu is modal).
  {
    const QSignalBlocker held(this);
    m_view->Redraw();
    mouse(QEvent::MouseButtonPress, cubeCentre(), Qt::RightButton, Qt::RightButton);
    const bool onCube = m_cubeMenu;
    mouse(QEvent::MouseButtonRelease, cubeCentre(), Qt::RightButton, Qt::NoButton);
    mouse(QEvent::MouseButtonPress, QPointF(8, height() - 8), Qt::RightButton, Qt::RightButton);
    const bool elsewhere = m_cubeMenu;
    mouse(QEvent::MouseButtonRelease, QPointF(8, height() - 8), Qt::RightButton, Qt::NoButton);
    require(onCube && !elsewhere && !m_cubeMenu, "a right click on the cube asks for the cube's menu, elsewhere for the context menu");
  }
  disconnect(counted);

  // (6) The 2D twist.
  setTwoDimensional(true);
  const gp_Dir flat = direction();
  require(std::abs(twistAngle()) < 1e-6, "2D mode starts untwisted");
  rollView(90);
  require(std::abs(twistAngle() - 90) < 1e-6 && direction().IsEqual(flat, 1e-9), QString("a turn twists the 2D view: %1°").arg(twistAngle()));
  twistView(30);
  require(std::abs(twistAngle() - 30) < 1e-6 && direction().IsEqual(flat, 1e-9), QString("a typed twist: %1°").arg(twistAngle()));
  twistView(0);
  require(std::abs(twistAngle()) < 1e-6 && m_view->Camera()->Up().IsEqual(naturalUp(), 1e-9), "untwisted, the plan's Y is up again");
  setTwoDimensional(false);
  standardView("iso");
  fitAll();
  return all;
}

// The viewport's part, then the window's: Z through its command and Esc, a middle double click fitting everything through
// Fit all, the CAD 2D preset chosen and given up from the menu (saved), the cube's menu, the turn buttons twisting in 2D.
OPAD_BENCH(OPAD_BENCH_NAVIGATE, navigate) {
  Viewport* v = w.m_viewport;
  whenDisplayed(&w, v, [&w, v, value] {
    bool ok = v->benchNavigation(value);
    auto require = [&ok](bool pass, const QString& what) {
      trace::log(QString("bench: navigate: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
      ok = ok && pass;
    };
    QAction* zoom = w.action("view.zoomWindow");
    zoom->trigger();
    const bool started = v->zoomWindowActive() && zoom->isChecked();
    QKeyEvent escape(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(w.m_viewport, &escape);
    require(started && !v->zoomWindowActive() && !zoom->isChecked(), "Zoom window (Z) starts the mode, its button pressed until Esc");
    // A middle double click: Fit all.
    v->fitAll();
    const opad::json fitted = v->cameraJson();
    opad::json closer = fitted;
    closer["scale"] = fitted["scale"].get<double>() / 5;
    v->setCameraJson(closer);
    const QPointF centre(v->width() / 2.0, v->height() / 2.0);
    QMouseEvent twice(QEvent::MouseButtonDblClick, centre, v->mapToGlobal(centre), Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(v, &twice);
    require(std::abs(v->cameraJson()["scale"].get<double>() / fitted["scale"].get<double>() - 1) < 1e-6, "a middle double click fits everything");
    // CAD 2D from the menu, saved; another preset unticks it.
    w.action("nav.cad2d")->trigger();
    require(v->navPreset() == Viewport::NavPreset::Cad2D && w.m_settings.value("ui/nav").toString() == "CAD2D" && w.action("nav.cad2d")->isChecked() &&
                !w.action("nav.fusion")->isChecked(), "Navigation: CAD 2D is chosen and saved");
    w.action("nav.fusion")->trigger();
    require(v->navPreset() == Viewport::NavPreset::Fusion && !w.action("nav.cad2d")->isChecked() && w.m_settings.value("ui/nav").toString() == "Fusion",
            "another preset unticks it");
    // Animate view changes, a setting.
    w.action("view.animate")->trigger();
    const bool off = !v->animateViews() && !w.m_settings.value("view/animate").toBool();
    w.action("view.animate")->trigger();
    require(off && v->animateViews() && w.m_settings.value("view/animate").toBool(), "Animate view changes turns off and on, saved");
    // The cube's menu.
    const QPointF cube = v->cubeCentre();
    v->grabImage();
    QMouseEvent press(QEvent::MouseButtonPress, cube, v->mapToGlobal(cube), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(v, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, cube, v->mapToGlobal(cube), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(v, &release);
    QMenu* menu = w.findChild<QMenu*>("cubeMenu");
    QStringList ids;
    if (menu)
      for (QAction* a : menu->actions()) ids << a->objectName();
    require(menu && ids.contains("view.home") && ids.contains("view.setHome") && ids.contains("view.resetHome") && ids.contains("view.top") &&
                ids.contains("view.previous") && ids.contains("view.ortho") && !w.action("view.resetHome")->isEnabled(),
            QString("the cube's menu: %1 (Reset Home off: Home is the default)").arg(ids.join(' ')));
    if (menu) menu->close();
    // The document's Home: a view op marked home, gone to exactly, undone and redone, set again (the last wins), reset in
    // one step (planned on a worker: the boxes have a design history) and undone, in 2D only along its own plane.
    {
      AppDocument* doc = w.m_doc;
      auto same = [](const opad::json& a, const opad::json& b) {
        const double tolerance = 1e-6 * std::max(1.0, a.value("scale", 1.0));
        for (const char* key : {"eye", "target", "up"})
          for (int i = 0; i < 3; ++i)
            if (std::abs(a[key][i].get<double>() - b[key][i].get<double>()) > (key[0] == 'u' ? 1e-9 : tolerance)) return false;
        return std::abs(a["scale"].get<double>() / b["scale"].get<double>() - 1) < 1e-9;
      };
      auto looking = [v] {
        const opad::Vec3 d = v->viewDirection();
        return gp_Dir(d[0], d[1], d[2]);
      };
      auto homes = [doc] { return std::count_if(doc->scene.views.begin(), doc->scene.views.end(), [](const opad::ViewBookmark& b) { return b.home; }); };
      auto zoomed = [v](const QString& view, double factor) {
        v->standardView(view);
        opad::json camera = v->cameraJson();
        camera["scale"] = camera["scale"].get<double>() / factor;
        v->setCameraJson(camera);
        return v->cameraJson();
      };
      require(w.m_commands.editsDocument("view.setHome") && w.m_commands.editsDocument("view.resetHome"),
              "Set and Reset Home edit the document (in viewer mode they ask to save as OPAD first)");
      const opad::json front = zoomed("front", 2);
      const size_t before = doc->doc.ops.size();
      w.action("view.setHome")->trigger();
      const opad::Op& set = doc->doc.ops.back();
      w.updateCommands();
      require(doc->doc.ops.size() == before + 1 && set.type == "view" && set.data.value("home", false) && set.data.value("name", "") == "Home" && homes() == 1 &&
                  v->customHome() && doc->isDirty() && w.action("view.resetHome")->isEnabled(),
              "Set current view as Home appends one view op marked home, the document is changed, Reset Home is on");
      QStringList named;
      if (w.m_viewsMenu)
        for (QAction* a : w.m_viewsMenu->actions()) named << a->text();
      require(!named.contains("Home"), QString("the Home is not a named view: %1").arg(named.join(", ")));
      v->standardView("top");
      w.action("view.home")->trigger();
      require(same(v->cameraJson(), front), "Home (H) goes back to it exactly: the direction, the target and the zoom");
      doc->undo();
      w.action("view.home")->trigger();
      require(!v->customHome() && looking().IsEqual(gp_Dir(-1, 1, -1), 1e-9), "undone, Home is the iso view again");
      doc->redo();
      v->standardView("top");
      w.action("view.home")->trigger();
      require(v->customHome() && same(v->cameraJson(), front), "redone, Home is the front view again");
      const opad::json right = zoomed("right", 3);
      w.action("view.setHome")->trigger();
      v->standardView("top");
      w.action("view.home")->trigger();
      require(homes() == 2 && same(v->cameraJson(), right), "set again, the last one is Home");
      const opad::Scene saved = opad::resolve(opad::Document::parse(doc->doc.serialize()));
      require(saved.views.size() == 2 && saved.views[0].home && saved.views[1].home && same(saved.views[1].camera, right), "both are in the saved text, marked home");
      // In 2D mode a Home along another direction keeps the plane (and fits); one along the plane is gone to.
      v->standardView("top");
      v->setTwoDimensional(true);
      const gp_Dir plan = looking();
      w.action("view.home")->trigger();
      require(looking().IsEqual(plan, 1e-9), "in 2D a Home looking from the right keeps the plan");
      v->setTwoDimensional(false);
      const size_t steps = doc->doc.ops.size();
      w.action("view.resetHome")->trigger();
      QElapsedTimer clock;
      clock.start();
      while ((doc->designBusy || homes() > 0) && clock.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      w.updateCommands();
      w.action("view.home")->trigger();
      require(homes() == 0 && doc->doc.ops.size() == steps + 2 && !v->customHome() && !w.action("view.resetHome")->isEnabled() &&
                  looking().IsEqual(gp_Dir(-1, 1, -1), 1e-9),
              "Reset Home tombstones both in one step, Home is the iso view, Reset Home is off");
      doc->undo();
      require(homes() == 2 && same(v->homeCamera(), right), "one undo brings both back");
      doc->undo(2);
      require(homes() == 0, "undone to the start, no Home");
    }
    // The turn buttons twist the 2D view.
    w.action("view.2d")->setChecked(true);
    require(!w.m_rollLeft->isHidden() && !w.m_rollRight->isHidden(), "the turn buttons stay in 2D mode");
    w.action("view.rollleft")->trigger();
    require(std::abs(v->twistAngle() - 90) < 1e-6, "Turn 90° left twists the 2D view");
    w.action("view.untwist")->trigger();
    require(std::abs(v->twistAngle()) < 1e-6, "Untwist view");
    w.action("view.2d")->setChecked(false);
    return ok;
  });
  return true;
}

OPAD_BENCH(OPAD_BENCH_TRANSPARENCY, transparency) {
  Viewport* v = w.m_viewport;
  whenDisplayed(&w, v, [v, value] { return v->benchTransparency(value); });
  return true;
}
