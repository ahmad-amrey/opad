// The origin of an empty design document and the click handlers that measure on a worker (UI-51); cases in
// tools/bench_cases/viewer.py.
#include <QAction>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QTreeWidget>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>

#include <cmath>
#include <functional>
#include <sstream>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SectionPanel.hpp"
#include "SketchEditor.hpp"
#include "opad/inspect.hpp"

namespace {
bool until(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return done();
}

// The label of the candidate picking finds at a world point, "" if none.
QString candidateAt(Viewport* v, const opad::Vec3& world) {
  for (const auto& c : v->pickCandidates(v->widgetPoint(world)))
    if (!c.candidate.empty()) return c.label;
  return {};
}
}  // namespace

// OPAD_BENCH_DESIGNORIGIN=<prefix> on an empty document, grid setting off. The Design workspace shows the origin's axes, its
// three planes (picked where they are) and the grid; Review shows none. The XY plane picked first and New sketch: the sketch
// is on XY at once, the origin goes while it is open and comes back when it closes empty; a body ends it, and Origin planes
// and axes brings the planes back over it shown only, never picked. Then on that box:
// Properties on a face opens at once and fills its area from a worker; Pick face for the section is inspected on a worker
// too. <prefix>.origin.png.
OPAD_BENCH(OPAD_BENCH_DESIGNORIGIN, designorigin) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: design origin: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  Viewport* v = w.m_viewport;
  const bool gridSetting = w.action("view.grid")->isChecked();
  w.setWorkspace("design");
  until([v] { return v->originGuide(); }, 5000);
  v->standardView("iso");
  v->fitAll();
  v->benchHoverAt(QPointF(4, 4));
  require(!gridSetting && v->originGuide() && !v->benchGridBox().IsVoid(), "the empty Design document shows the origin and the grid (its setting off)");
  require(!v->benchGridEcho(), "with no star on the grid node under the pointer (OCCT's grid echo is off)");
  // Points of each plane that no other plane hides in the iso view (the nearest candidate is the one picked).
  const QString xy = candidateAt(v, {25, 25, 0}), xz = candidateAt(v, {25, 0, 25}), yz = candidateAt(v, {0, 25, 25});
  require(xy == "XY plane" && xz == "XZ plane" && yz == "YZ plane", QString("its planes are picked where they are: %1, %2, %3").arg(xy, xz, yz));
  v->benchHoverAt(QPointF(v->width() / 2.0, v->height() - 6.0));
  v->grabImage().save(value + ".origin.png");
  w.setWorkspace("review");
  require(!v->originGuide() && v->benchGridBox().IsVoid() && candidateAt(v, {25, 25, 0}).isEmpty(), "Review shows none of it");
  w.setWorkspace("design");
  until([v] { return v->originGuide(); }, 5000);
  // The XY plane picked first, then New sketch: on XY at once.
  int index = -1;
  const auto under = v->pickCandidates(v->widgetPoint({25, 25, 0}));
  for (size_t i = 0; i < under.size() && index < 0; ++i)
    if (under[i].label == "XY plane") index = static_cast<int>(i);
  require(index >= 0 && v->choosePickCandidate(index) && v->selectedCandidates() == std::vector<std::string>{opad::json{{"base", "xy"}}.dump()}, "the XY plane is picked");
  w.action("design.sketch")->trigger();
  until([&w] { return w.m_design->sketchActive(); }, 10000);
  const opad::json plane = w.m_design->sketch()->plane();
  require(w.m_design->sketchActive() && plane.value("base", "") == "xy", "New sketch draws on it at once: " + QString::fromStdString(plane.dump()));
  require(!v->originGuide(), "the origin goes while the sketch is open");
  w.m_design->cancelSketch();
  until([&w, v] { return !w.m_design->sketchActive() && v->originGuide(); }, 5000);
  require(!w.m_design->sketchActive() && v->originGuide() && candidateAt(v, {25, 25, 0}) == "XY plane", "and comes back when it closes with nothing drawn");
  std::ostringstream text;
  BRepTools::Write(BRepPrimAPI_MakeBox(gp_Pnt(-20, -20, 0), 40, 40, 10).Shape(), text);
  w.m_doc->run("import_brep", {{"brep", text.str()}, {"name", "Block"}});
  until([v] { return v->displayedCount() == 1 && v->remainingBodies() == 0; }, 20000);
  require(!v->originGuide() && v->benchGridBox().IsVoid() && candidateAt(v, {25, 25, 0}).isEmpty(), "a body ends it");
  // Origin planes and axes keeps them with the model, shown only: a click there reaches what lies behind (a face on XY).
  QAction* show = w.action("design.showOrigin");
  show->trigger();
  until([v] { return v->originGuide(); }, 5000);
  const QString over = candidateAt(v, {25, 25, 0});  // (the grid box would count the axes: infinite structures too)
  require(show->isChecked() && v->originGuide() && over.isEmpty(),
          "Origin planes and axes with a model: the planes shown and never picked: " + (over.isEmpty() ? QString("nothing there") : over));
  show->trigger();  // the setting as it was
  until([v] { return !v->originGuide(); }, 5000);

  // Click handlers that walk the body run on a worker: Properties on a face, the section's Pick face.
  const std::string body = w.m_doc->scene.all_bodies().front();
  opad::Ref top;
  top.body = body;
  top.kind = opad::Ref::Kind::Face;
  for (int i = 0; i < 6; ++i) {
    opad::Ref r = top;
    r.index = i;
    const auto info = opad::inspect_ref(w.m_doc->doc, w.m_doc->scene, r);
    if (info.contains("normal") && info["normal"][2].get<double>() > 0.5) top = r;
  }
  auto rows = [&w] {
    QStringList keys;
    const QTreeWidget* table = w.m_props->table();
    for (const auto* item : table->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive)) keys << item->text(0);
    return keys;
  };
  w.selectionMoved({top});
  w.action("inspect.properties")->trigger();
  const bool deferred = w.m_propsJob != nullptr && !rows().contains("Area");
  until([&w] { return !w.m_propsJob; }, 20000);
  require(deferred && w.m_propsPanel->isVisible() && rows().contains("Area"), "Properties on a face opens at once and fills its area from a worker: " + rows().join(", "));
  w.m_propsPanel->hide();
  const bool sectionBefore = w.m_section->enabled();
  w.sectionFromFace(top);
  const bool sectionDeferred = w.m_sectionJob != nullptr && w.m_section->enabled() == sectionBefore;
  until([&w] { return !w.m_sectionJob; }, 20000);
  const opad::Vec3 o = w.m_section->origin(), n = w.m_section->normal();
  require(sectionDeferred && w.m_section->enabled() && std::abs(n[2] - 1) < 1e-9 && std::abs(o[2] - 10) < 0.05,  // the slider's step
          QString("Pick face sets the section from a worker: origin z %1, normal z %2").arg(o[2]).arg(n[2]));
  w.action("inspect.section")->setChecked(false);
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}

// OPAD_BENCH_ORBITPIVOT=<prefix> on a drawing of 100,000 lines (design note E): the orbit pivot of a press away from the
// drawing, the nearest point of its curves on screen, is found run by run (UI-51) in a few milliseconds and is the point a
// scan of every segment finds.
OPAD_BENCH(OPAD_BENCH_ORBITPIVOT, orbitpivot) {
  Viewport* v = w.m_viewport;
  const bool shown = until([&w, v] { return !w.m_loadJob && v->displayedCount() > 0 && v->remainingBodies() == 0; }, 120000);
  const bool ok = shown && v->benchOrbitPivot(value);
  if (!shown) trace::log("bench: orbit pivot: the drawing is displayed FAIL");
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
