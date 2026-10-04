// Select other (UI-128) in the running app; case in tools/bench_cases/viewer.py.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QStyleHints>
#include <QMouseEvent>
#include <QMenu>
#include <QTimer>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <functional>
#include <map>
#include <sstream>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"

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

std::string brep(const TopoDS_Shape& shape) {
  std::ostringstream out;
  BRepTools::Write(shape, out);
  return out.str();
}

QStringList labels(const std::vector<Viewport::PickCandidate>& candidates) {
  QStringList out;
  for (const auto& c : candidates) out << c.label;
  return out;
}

QList<QAction*> rows(QMenu* menu) {  // the candidates' rows, without the section
  QList<QAction*> out;
  for (QAction* a : menu ? menu->actions() : QList<QAction*>())
    if (a->data().isValid()) out << a;
  return out;
}
}  // namespace

// OPAD_BENCH_SELECTOTHER=<prefix> on an empty document: a 20 mm box, a twin box in the same place and a pin standing through
// them, a lone box beside them, seen from the top. Over the pin, in the Body filter, the list holds the pin first and both boxes; Alt+click opens it
// as a menu, hovering the second row hovers that box in the view, choosing it selects it though the pin is in front. In the
// Face filter the faces behind the pin's top are listed and one is chosen; Tab and Shift+Tab hover the next and previous face
// under the resting pointer and a click takes the hovered one. In the Edge filter over a box's edge only edges are listed
// (never the faces that stand in front of edges). In the Distance tool a row is the tool's pick. A plain press held still
// for the press-and-hold time opens the list without a modifier. <prefix>.menu.png / .preview.png.
OPAD_BENCH(OPAD_BENCH_SELECTOTHER, selectother) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: select other: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  Viewport* v = w.m_viewport;
  const QString prefix = value;
  w.m_doc->run("import_brep", {{"brep", brep(BRepPrimAPI_MakeBox(20, 20, 10).Shape())}, {"name", "Box"}});
  w.m_doc->run("import_brep", {{"brep", brep(BRepPrimAPI_MakeBox(20, 20, 10).Shape())}, {"name", "Twin"}});
  w.m_doc->run("import_brep", {{"brep", brep(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(10, 10, 0), gp_Dir(0, 0, 1)), 3, 20).Shape())}, {"name", "Pin"}});
  w.m_doc->run("import_brep", {{"brep", brep(BRepPrimAPI_MakeBox(gp_Pnt(40, 0, 0), 10, 10, 10).Shape())}, {"name", "Lone"}});
  if (!until([v] { return v->displayedCount() >= 4 && v->remainingBodies() == 0; }, 30000)) {
    require(false, "the four bodies are displayed");
    return QCoreApplication::exit(2), true;
  }
  std::map<QString, std::string> ids;
  for (const auto& id : w.m_doc->scene.all_bodies()) ids[w.m_doc->nodeName(id)] = id;
  auto filter = [&w, v](const char* id, Viewport::SelFilter f) {
    bool applied = v->selectionFilter() == f;
    auto once = QObject::connect(v, &Viewport::filterApplied, v, [&applied] { applied = true; });
    if (!applied) w.action(id)->trigger();
    until([&applied] { return applied; }, 10000);
    QObject::disconnect(once);
  };
  filter("select.bodies", Viewport::SelFilter::Body);
  v->standardView("top");
  v->fitAll();
  v->benchHoverAt(QPointF(4, 4));
  const QPointF overPin = v->widgetPoint({10, 10, 20});

  // Bodies: the pin, then both boxes behind it.
  auto candidates = v->pickCandidates(overPin);
  QStringList names = labels(candidates);
  require(names.size() == 3 && names.value(0) == "Pin" && names.contains("Box") && names.contains("Twin"), "over the pin, nearest first: " + names.join(", "));
  v->benchClickAt(overPin, Qt::AltModifier);
  QMenu* menu = v->findChild<QMenu*>("selectOther");
  require(menu && rows(menu).size() == 3 && rows(menu).value(0)->text() == "Pin", "Alt+click opens the list as a menu");
  if (!menu) return QCoreApplication::exit(2), true;
  require(v->selection().empty(), "and selects nothing by itself");
  const QString behind = rows(menu).value(1)->text();
  emit menu->hovered(rows(menu).value(1));
  v->benchHoverAt(overPin);  // the frame after: the status text names what is hovered
  require(v->hoverText() == behind, "hovering the second row hovers " + behind + " in the view: " + v->hoverText());
  menu->grab().save(prefix + ".menu.png");
  v->grabImage().save(prefix + ".preview.png");
  rows(menu).value(1)->trigger();
  menu->close();
  auto selected = v->selection();
  require(selected.size() == 1 && selected.front().body == ids[behind] && selected.front().kind == opad::Ref::Kind::Body, "choosing it selects " + behind + " though the pin is in front");
  v->clearSelection();

  // The same list from the view's context menu (a right click, no modifier).
  QStringList offered;
  QTimer::singleShot(300, v, [&w, &offered] {  // while the context menu runs its own loop
    for (QMenu* m : w.findChildren<QMenu*>())
      if (m->isVisible())
        for (QAction* a : m->actions())
          if (a->objectName() == "select.other") {
            m->close();
            a->trigger();
          }
    if (QMenu* list = w.m_viewport->findChild<QMenu*>("selectOther")) {
      for (QAction* a : rows(list)) offered << a->text();
      list->close();
    }
  });
  for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
    QMouseEvent e(type, overPin, v->mapToGlobal(overPin), Qt::RightButton, type == QEvent::MouseButtonPress ? Qt::RightButton : Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(v, &e);
  }
  until([&offered] { return !offered.isEmpty(); }, 3000);
  require(offered == names, "a right click offers Select other..., which lists the same: " + offered.join(", "));

  // A plain press held still opens the same list; its release is no click. A press that moves on is a drag and a quick
  // click a click, a held press on nothing stays a click (it clears the selection).
  const int hold = QGuiApplication::styleHints()->mousePressAndHoldInterval();
  auto send = [v](QEvent::Type type, const QPointF& at, Qt::MouseButtons buttons) {
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(v, &e);
  };
  auto listed = [v] { return v->findChild<QMenu*>("selectOther"); };
  v->benchHoverAt(overPin);
  QElapsedTimer held;
  held.start();
  send(QEvent::MouseButtonPress, overPin, Qt::LeftButton);
  const bool notAtOnce = !listed();
  until([&] { return listed() != nullptr; }, hold + 2000);
  menu = listed();
  require(notAtOnce && menu && held.elapsed() >= hold - 50 && rows(menu).size() == 3 && rows(menu).value(0)->text() == "Pin",
          QString("a press held still opens the list after %1 ms (the hold is %2 ms)").arg(held.elapsed()).arg(hold));
  send(QEvent::MouseButtonRelease, overPin, Qt::NoButton);
  v->benchHoverAt(overPin);
  require(menu && listed() == menu && v->selection().empty(), "its release leaves the list open and selects nothing");
  if (menu) {
    const QString last = rows(menu).value(2)->text();
    rows(menu).value(2)->trigger();
    menu->close();
    selected = v->selection();
    require(selected.size() == 1 && selected.front().body == ids[last], "the held list's last row selects " + last);
  }
  v->clearSelection();
  send(QEvent::MouseButtonPress, overPin, Qt::LeftButton);
  send(QEvent::MouseMove, overPin + QPointF(12, 0), Qt::LeftButton);
  until([] { return false; }, hold + 300);
  require(!listed(), "a press that moves on opens no list");
  send(QEvent::MouseButtonRelease, overPin + QPointF(12, 0), Qt::NoButton);
  v->clearSelection();
  v->benchClickAt(overPin);
  until([] { return false; }, hold + 300);
  selected = v->selection();
  require(!listed() && selected.size() == 1 && selected.front().body == ids["Pin"], "a quick click selects the pin and opens no list");
  v->benchHoverAt(QPointF(4, 4));
  send(QEvent::MouseButtonPress, QPointF(4, 4), Qt::LeftButton);
  until([] { return false; }, hold + 300);
  send(QEvent::MouseButtonRelease, QPointF(4, 4), Qt::NoButton);
  v->benchHoverAt(QPointF(4, 4));
  require(!listed() && v->selection().empty(), "a press held on nothing opens no list and its release clears the selection");
  const QPointF overLone = v->widgetPoint({45, 5, 10});
  v->benchHoverAt(overLone);
  send(QEvent::MouseButtonPress, overLone, Qt::LeftButton);
  until([] { return false; }, hold + 300);
  send(QEvent::MouseButtonRelease, overLone, Qt::NoButton);
  v->benchHoverAt(overLone);
  selected = v->selection();
  require(!listed() && selected.size() == 1 && selected.front().body == ids["Lone"], "a press held on one thing alone opens no list and its release selects it");
  v->clearSelection();

  // Faces: those behind the pin's top, one of them chosen.
  filter("select.faces", Viewport::SelFilter::Face);
  v->benchHoverAt(overPin);
  candidates = v->pickCandidates(overPin);
  names = labels(candidates);
  const bool allFaces = std::all_of(candidates.begin(), candidates.end(), [](const auto& c) { return c.ref.kind == opad::Ref::Kind::Face; });
  require(candidates.size() >= 4 && allFaces && names.value(0).startsWith("Pin") && candidates.front().depth <= candidates.back().depth, "faces over the pin, nearest first: " + names.join(", "));
  if (candidates.size() < 3) return QCoreApplication::exit(2), true;
  require(v->choosePickCandidate(1) && v->selection().size() == 1 && v->selection().front().str() == candidates[1].ref.str(), "a face behind is chosen: " + names.value(1));
  v->clearSelection();
  until([v] { return v->selection().empty(); }, 2000);

  // Tab: the next face under the resting pointer, Shift+Tab back, a click takes the hovered one. (The face chosen above
  // stays hovered while the pointer rests there: moved off and back first.)
  v->benchHoverAt(QPointF(4, 4));
  v->benchHoverAt(overPin);
  const QString front = v->hoverText();
  auto tab = [v](bool back) {
    QKeyEvent key(QEvent::KeyPress, back ? Qt::Key_Backtab : Qt::Key_Tab, back ? Qt::ShiftModifier : Qt::NoModifier);
    const bool taken = QCoreApplication::sendEvent(v, &key) && key.isAccepted();
    v->benchHoverAt(v->widgetPoint({10, 10, 20}));
    return taken;
  };
  const bool first = tab(false);
  const QString second = v->hoverText();
  tab(false);
  const QString third = v->hoverText();
  tab(true);
  require(first && front == names.value(0) && second == names.value(1) && third == names.value(2) && v->hoverText() == second,
          QString("Tab hovers %1 then %2, Shift+Tab back to %1 (from %3)").arg(second, third, front));
  v->benchClickAt(overPin);
  selected = v->selection();
  require(selected.size() == 1 && selected.front().str() == candidates[1].ref.str(), "a click takes the face Tab hovered: " + (selected.empty() ? QString("nothing") : QString::fromStdString(selected.front().str())));
  v->benchHoverAt(QPointF(4, 4));  // moving on picks again: the front face over the pin
  v->benchHoverAt(overPin);
  require(v->hoverText() == front, "once the pointer moves the nearest is hovered again: " + v->hoverText());
  v->clearSelection();

  // Edges: never the faces standing in front of edges (UI-31).
  filter("select.edges", Viewport::SelFilter::Edge);
  const QPointF overEdge = v->widgetPoint({20, 10, 10});
  v->benchHoverAt(overEdge);
  candidates = v->pickCandidates(overEdge);
  names = labels(candidates);
  const bool allEdges = std::all_of(candidates.begin(), candidates.end(), [](const auto& c) { return c.ref.kind == opad::Ref::Kind::Edge; });
  require(candidates.size() >= 2 && allEdges, "over a box's edge only edges are listed: " + names.join(", "));

  // A guided tool takes the chosen row as its pick.
  filter("select.faces", Viewport::SelFilter::Face);
  w.startTool("distance");
  v->benchHoverAt(overPin);
  v->benchClickAt(overPin, Qt::AltModifier);
  menu = v->findChild<QMenu*>("selectOther");
  const auto toolRows = rows(menu);
  if (toolRows.size() > 1) toolRows[1]->trigger();
  if (menu) menu->close();
  require(w.m_toolPicks.size() == 1 && toolRows.size() > 1 && w.refLabel(w.m_toolPicks.front()) == toolRows[1]->text(), "in the Distance tool the chosen row is the first pick: " +
          (w.m_toolPicks.empty() ? QString("none") : w.refLabel(w.m_toolPicks.front())));
  w.cancelTool();
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
