// OPAD_BENCH_BOXSCAN=<prefix> (UI-43): on an assembly with parts inside others (as1), an iso view fitted, a crossing box
// and a window box over most of the view in the Body filter, then a crossing box in the Face filter, each settle within
// 3 s and select exactly what can be seen in the box. The crossing box kept a 23.5 s background job on as1: once only
// hidden candidates were left, the visibility test picked every pixel of the box. Ground truth: a body (a face) is seen in
// the box when the frame drawn without it (with it in another colour) differs inside the box by more than 4 pixels.
// <prefix>.crossing.png: the view with the crossing box's bodies selected.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>

#include <Prs3d_ShadingAspect.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <functional>
#include <map>
#include <memory>
#include <set>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return done();
}

int differing(const QImage& a, const QImage& b, const QRect& r) {
  int n = 0;
  for (int y = std::max(0, r.top()); y <= std::min(r.bottom(), std::min(a.height(), b.height()) - 1); ++y)
    for (int x = std::max(0, r.left()); x <= std::min(r.right(), std::min(a.width(), b.width()) - 1); ++x) {
      const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
      n += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q)) > 48;
    }
  return n;
}
}  // namespace

bool Viewport::benchBoxScan(const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: box scan: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  m_needFit = false;
  m_view->SetProj(V3d_XposYnegZpos);
  m_view->FitAll(fitBounds(), 0.02, Standard_False);
  m_view->Redraw();
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  const QRect device(int(w * 0.12), int(h * 0.12), int(w * 0.76), int(h * 0.76));  // device pixels
  const QPointF scale = viewScale();
  // What the box selects, and the candidates OCCT's selector finds in it (wholly in it for a window box).
  struct Run { std::set<std::string> taken, candidates; qint64 ms = 0; bool finished = false; };
  auto box = [&](bool crossing) {
    Run run;
    m_ctx->ClearSelected(Standard_False);
    const auto& selector = m_ctx->MainSelector();
    selector->AllowOverlapDetection(crossing);
    selector->Pick(device.left(), device.top(), device.right(), device.bottom(), m_view);
    selector->AllowOverlapDetection(false);
    for (int i = 1; i <= selector->NbPicked(); ++i) {
      const auto owner = selector->Picked(i);
      if (!Handle(OccluderOwner)::DownCast(owner).IsNull() || !Handle(CircleOwner)::DownCast(owner).IsNull()) continue;
      const auto node = m_nodeOf.find(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get());
      const auto sub = Handle(SubShapeOwner)::DownCast(owner);
      if (node != m_nodeOf.end()) run.candidates.insert(node->second + (sub.IsNull() ? std::string() : "/" + std::to_string(sub->index())));
    }
    const QPoint from(qRound(device.left() / scale.x()), qRound(device.top() / scale.y())), to(qRound(device.right() / scale.x()), qRound(device.bottom() / scale.y()));
    UpdateRubberBand(devicePos(crossing ? to : from), devicePos(crossing ? from : to));
    myGL.Selection = myUI.Selection;
    myGL.Selection.Scheme = AIS_SelectionScheme_Replace;
    myGL.Selection.ToApplyTool = true;
    QElapsedTimer t;
    t.start();
    handleSelectionPoly(m_ctx, m_view);
    run.finished = waitUntil([this] { return !m_boxJob; }, 60000);
    run.ms = t.elapsed();
    if (m_boxJob) m_boxJob->cancel();
    for (const auto& ref : selection()) run.taken.insert(ref.body + (ref.kind == opad::Ref::Kind::Body ? std::string() : "/" + std::to_string(ref.index)));
    return run;
  };
  const Run crossing = box(true);
  waitUntil([this] { return !m_subJob && !m_bodyGlowJob; }, 20000);
  m_view->Redraw();
  grabImage().save(prefix + ".crossing.png");
  const Run window = box(false);
  setSelectionFilter(SelFilter::Face);
  waitUntil([this] { return !m_filterJob; }, 30000);
  m_view->Redraw();
  const Run faces = box(true);
  // The ground truth, frame against frame: each candidate drawn in a colour of its own (a face over its body's colour).
  clearSelection();
  waitUntil([this] { return !m_selJob && !m_subJob && !m_bodyGlowJob; }, 20000);
  m_ctx->ClearDetected(Standard_False);
  setSelectionFilter(SelFilter::Body);
  waitUntil([this] { return !m_filterJob; }, 30000);
  const Style style = m_style;  // faces only: a face seen edge-on is all boundary line
  setStyle(Style::Shaded);
  m_view->Redraw();
  const QImage base = grabImage();
  std::map<std::string, int> seen;  // pixels of the box it changes
  auto seenBody = [&](const std::string& id) {
    if (const auto known = seen.find(id); known != seen.end()) return known->second;
    const auto item = m_items.find(id);
    if (item == m_items.end()) return seen[id] = 0;
    m_ctx->Erase(item->second.ais, Standard_False);
    m_view->Redraw();
    const int n = differing(base, grabImage(), device);
    m_ctx->Display(item->second.ais, Standard_False);
    return seen[id] = n;
  };
  auto seenFace = [&](const std::string& ref) {
    if (const auto known = seen.find(ref); known != seen.end()) return known->second;
    const std::string id = ref.substr(0, ref.find('/'));
    const int index = std::stoi(ref.substr(ref.find('/') + 1));
    const auto item = m_items.find(id);
    if (item == m_items.end()) return seen[ref] = 0;
    TopTools_IndexedMapOfShape facesOf;
    TopExp::MapShapes(item->second.located, TopAbs_FACE, facesOf);
    if (index < 0 || index >= facesOf.Extent()) return seen[ref] = 0;
    Handle(AIS_Shape) paint = new AIS_Shape(facesOf(index + 1));  // the face in magenta, in place, drawn over its own mesh
    paint->SetLocalTransformation(item->second.ais->LocalTransformation());
    paint->Attributes()->SetAutoTriangulation(Standard_False);
    paint->SetColor(Quantity_NOC_MAGENTA1);
    if (const auto body = Handle(BodyShape)::DownCast(item->second.ais); !body.IsNull() && body->prs() && body->prs()->closed)  // drawn culled
      paint->Attributes()->ShadingAspect()->Aspect()->SetFaceCulling(Graphic3d_TypeOfBackfacingModel_BackCulled);
    Standard_Integer mode = 0;
    Standard_ShortReal factor = 0, units = 0;
    item->second.ais->PolygonOffsets(mode, factor, units);  // the body's own: the same depths, drawn after it (LEQUAL)
    paint->SetPolygonOffsets(mode, factor, units);
    m_ctx->Display(paint, AIS_Shaded, -1, Standard_False);
    m_view->Redraw();
    const int n = differing(base, grabImage(), device);
    m_ctx->Remove(paint, Standard_False);
    return seen[ref] = n;
  };
  // Seen over 64 pixels (an 8 x 8 patch): taken; not seen at all: not taken; a sliver in between is not judged.
  auto judge = [&](const QString& what, const Run& run, bool face) {
    if (run.candidates.size() > 300) {  // a big model (the Engine): the frames would take minutes; within its budget, that is all
      require(run.finished && run.ms < 15000, QString("%1: %2 candidates, %3 taken in %4 ms").arg(what).arg(run.candidates.size()).arg(run.taken.size()).arg(run.ms));
      return;
    }
    int surely = 0, hidden = 0, missed = 0, wrong = 0, slivers = 0, taken = 0;
    QString details;
    for (const auto& c : run.candidates) {
      const int n = face ? seenFace(c) : seenBody(c);
      const bool took = run.taken.count(c) > 0;
      surely += n > 64;
      hidden += n == 0;
      slivers += n > 0 && n <= 64;
      taken += n > 0 && n <= 64 && took;
      if ((n > 64 && !took) || (n == 0 && took)) details += QString(" %1 %4 (%2 px%3)").arg(QString::fromStdString(c)).arg(n).arg(took ? ", taken" : "").arg(m_doc->nodeName(c.substr(0, c.find('/'))));
      missed += n > 64 && !took;
      wrong += n == 0 && took;
    }
    require(run.finished && run.ms < 3000 && missed == 0 && wrong == 0 && surely > 0 && hidden > 0,
            QString("%1: of %2 candidates %3 seen in the box, %4 not seen and %5 slivers (%6 of them taken); %7 missed, %8 not seen taken; "
                    "%9 ms%10").arg(what).arg(run.candidates.size()).arg(surely).arg(hidden).arg(slivers).arg(taken).arg(missed).arg(wrong).arg(run.ms).arg(details));
  };
  judge("crossing box, bodies", crossing, false);
  judge("window box, bodies", window, false);
  judge("crossing box, faces", faces, true);
  setStyle(style);
  m_view->Redraw();
  myUI.Reset();
  return all;
}

OPAD_BENCH(OPAD_BENCH_BOXSCAN, boxscan) {
  Viewport* v = w.m_viewport;
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(&w);
  auto shown = std::make_shared<bool>(false);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, v, timer, clock, value, shown] {
    if (!w.m_loadJob && v->displayedCount() == 0 && !std::exchange(*shown, true)) v->isolate(w.m_doc->scene.roots);  // the Engine's root is hidden
    const bool ready = !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() > 0 && !w.m_jobs->busy();
    if (!ready && clock->elapsed() < 120000) return;
    timer->stop();
    timer->deleteLater();
    trace::log(QString("bench: box scan: the model is displayed (%1 bodies) %2").arg(v->displayedCount()).arg(ready ? "PASS" : "FAIL"));
    const bool ok = ready && v->benchBoxScan(value);
    QCoreApplication::exit(ok ? 0 : 2);
  });
  timer->start(50);
  return true;
}
