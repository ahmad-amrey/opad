// OPAD_BENCH_PREVIEWPEEK=<prefix> (case preview-peek in tools/bench_cases/design.py, a 30 x 20 x 10 box centred on the
// origin): the report "Ctrl shows the original to allow selecting more". A fillet's preview stands in for the box (the box
// is erased and the preview takes no picks), so once it shows no other edge of the box can be picked. Driven through the
// view's own key and mouse events:
//   - a top edge clicked: the preview shows and the box's other edges cannot be picked (the report);
//   - Ctrl pressed over the view: the box as it is, its edges pickable, the preview out of the way, the picked edge
//     selected on it, the prompt and the panel's hint say so (the hint names the key as the keyboard shows it);
//   - a Ctrl+click on another top edge adds it (two picks); the preview planned for both waits while Ctrl is held;
//   - Ctrl released: that preview (two rounded edges, less volume than one) is back, the box erased again;
//   - Ctrl again and a Ctrl+click on the second edge takes it back (one pick), as Ctrl+click does everywhere;
//   - Enter commits the fillet on the edges picked.
// <prefix>.peek.png (Ctrl held), <prefix>.preview.png (released).
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QTimer>

#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <optional>
#include <set>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "KeyText.hpp"
#include "MainWindow.hpp"
#include "opad/geometry.hpp"

OPAD_BENCH(OPAD_BENCH_PREVIEWPEEK, previewpeek) {
  struct State {
    size_t step = 0;
    int wait = 0;
    std::string box;
    int first = -1, second = -1;
    double oneEdge = 0;
  };
  auto st = std::make_shared<State>();
  for (const auto& f : w.m_doc->scene.features)
    for (const auto& body : f.result.value("bodies", opad::json::array()))
      if (f.kind == "box") st->box = body.value("id", "");
  if (st->box.empty()) {
    trace::log("bench: preview-peek FAIL: the document needs a box");
    QCoreApplication::exit(2);
    return true;
  }
  w.setWorkspace("design");
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  Viewport* view = w.m_viewport;
  MainWindow* win = &w;
  const QString prefix = value;
  auto require = [](bool ok, const std::string& why) {
    if (!ok) throw opad::Error(why);
  };
  auto pass = [](const QString& what) { trace::log("bench: preview-peek: " + what + " PASS"); };
  auto shape = [win, st] { return opad::node_world_shape(win->m_doc->doc, win->m_doc->scene, st->box); };
  auto volumeOf = [](const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
  };
  auto previewVolume = [design, volumeOf] {
    double v = 0;
    if (const auto plan = design->readyPreview())
      for (const auto& c : plan->changed)
        if (!c.removed && c.shape && !c.shape->IsNull()) v += volumeOf(*c.shape);
    return v;
  };
  auto topEdges = [shape] {  // straight edges at the top, z = 10
    std::set<int> out;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape(), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
      BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
      if (c.GetType() == GeomAbs_Line && std::abs(c.Value(c.FirstParameter()).Z() - 10) < 1e-6 && std::abs(c.Value(c.LastParameter()).Z() - 10) < 1e-6) out.insert(i - 1);
    }
    return out;
  };
  // Where in the view an edge of the box is picked (device pixels), if anywhere.
  auto find = [view, st](std::set<int> among) -> std::optional<QPoint> {
    int x = 0, y = 0;
    if (!view->benchPickPoint([st, among](const std::string& c, const opad::Ref& r) { return c.empty() && r.body == st->box && r.kind == opad::Ref::Kind::Edge && among.count(r.index); }, x, y, false))
      return std::nullopt;
    return QPoint(x, y);
  };
  // A click as the mouse sends it at a device pixel, every event with these modifiers (Ctrl stays held through it).
  auto click = [view](const QPoint& device, Qt::KeyboardModifiers modifiers) {
    view->benchClickAt(QPointF(device.x() / view->displayScale(), device.y() / view->displayScale()), modifiers, true);
  };
  auto ctrl = [view, win](bool down) {
    QApplication::setActiveWindow(win);
    QKeyEvent e(down ? QEvent::KeyPress : QEvent::KeyRelease, Qt::Key_Control, down ? Qt::ControlModifier : Qt::NoModifier);
    QApplication::sendEvent(view, &e);
  };
  auto key = [view, win](int code) {
    QApplication::setActiveWindow(win);
    QWidget* to = QApplication::focusWidget();
    QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
    QApplication::sendEvent(to ? to : static_cast<QWidget*>(view), &press);
  };
  auto picks = [form] {
    const opad::json p = form->picks("edges");
    std::set<int> out;
    if (p.is_array())
      for (const auto& e : p) out.insert(e.value("index", -1));
    return out;
  };
  auto planned = [design](size_t edges) {  // the preview on hand is the plan for these edges
    const opad::json in = design->readyPreviewInputs();
    return design->readyPreview() && in.is_object() && in.contains("edges") && in["edges"].is_array() && in["edges"].size() == edges;
  };
  auto boxShown = [view, st] { return !view->previewStandsIn(st->box) || view->previewPeek(); };
  auto waitFor = [st](bool ok, const std::string& why) {
    if (ok) return st->wait = 0, true;
    if (++st->wait > 100) throw opad::Error(why);
    return false;
  };

  std::vector<std::function<bool()>> steps = {
      [=] {
        view->standardView("iso");
        view->fitAll();
        design->startFeature("fillet");
        return true;
      },
      [=] {
        const bool edges = view->selectionFilter() == Viewport::SelFilter::Edge;
        const auto at = edges ? find(topEdges()) : std::nullopt;
        if (!waitFor(at.has_value(), edges ? "a top edge is not found in the view" : "fillet's Edges switch the view to edges")) return false;
        click(*at, Qt::NoModifier);
        return true;
      },
      [=] {
        if (!waitFor(picks().size() == 1 && planned(1) && view->previewShown(), "one edge picked shows no fillet preview")) return false;
        st->first = *picks().begin();
        st->oneEdge = previewVolume();
        std::set<int> others = topEdges();
        others.erase(st->first);
        require(view->previewStandsIn(st->box) && !find(others).has_value(), "with the preview shown the box's other edges are still pickable");
        pass("one top edge picked: the preview stands in for the box, whose other edges cannot be picked");
        const QString hint = keys::fixedText("ctrl");
        bool hinted = false;
        for (QLabel* label : form->findChildren<QLabel*>()) hinted = hinted || (label->isVisible() && label->text().contains(hint) && label->text().contains("pick more"));
        require(hinted, "the panel's hint does not say that Ctrl shows the bodies to pick more");
        pass("the panel's hint says that holding " + keys::plain(QKeySequence(Qt::Key_Control)) + " shows the bodies to pick more");
        ctrl(true);
        return true;
      },
      [=] {
        std::set<int> others = topEdges();
        others.erase(st->first);
        const auto at = find(others);
        const auto selected = view->selection();
        require(view->previewPeek() && !view->previewShown() && boxShown() && at.has_value(), "Ctrl held: the box is not shown as it is, pickable, without the preview");
        require(selected.size() == 1 && selected[0].body == st->box && selected[0].index == st->first, "Ctrl held: the picked edge is not selected on the box");
        require(win->m_promptText.contains(keys::fixedText("ctrl")), "Ctrl held: the prompt does not say so: " + win->m_promptText.toStdString());
        if (!prefix.isEmpty()) view->grabImage().save(prefix + ".peek.png");
        pass("Ctrl held: the box as it is with its edge picked, the preview out of the way, the prompt says so");
        click(*at, Qt::ControlModifier);
        return true;
      },
      [=] {
        if (!waitFor(picks().size() == 2 && planned(2), "a Ctrl+click on another top edge did not add it")) return false;
        const std::set<int> two = picks();
        st->second = *std::find_if(two.begin(), two.end(), [st](int i) { return i != st->first; });
        require(view->previewPeek() && !view->previewShown() && boxShown(), "the preview for both edges did not wait for Ctrl's release");
        pass("a Ctrl+click adds another edge; its preview waits while Ctrl is held");
        ctrl(false);
        return true;
      },
      [=] {
        require(!view->previewPeek() && view->previewShown() && view->previewStandsIn(st->box) && planned(2) && previewVolume() < st->oneEdge - 1e-3,
                "Ctrl released: the preview of both edges is not back in place of the box");
        if (!prefix.isEmpty()) view->grabImage().save(prefix + ".preview.png");
        pass("Ctrl released: the preview of both edges is back, the box erased again");
        ctrl(true);
        return true;
      },
      [=] {
        int x = 0, y = 0;
        require(view->benchPickPoint([st](const std::string& c, const opad::Ref& r) { return c.empty() && r.body == st->box && r.kind == opad::Ref::Kind::Edge && r.index == st->second; }, x, y, false),
                "Ctrl held again: the second edge is not pickable");
        require(view->selection().size() == 2, "Ctrl held again: both picks are not selected on the box");
        click(QPoint(x, y), Qt::ControlModifier);
        return true;
      },
      [=] {
        if (!waitFor(picks() == std::set<int>{st->first} && planned(1), "a Ctrl+click on a picked edge did not take it back")) return false;
        pass("a Ctrl+click on a picked edge takes it back, as Ctrl+click does everywhere");
        ctrl(false);
        require(view->previewShown() && !view->previewPeek(), "released, the preview is not back");
        view->setFocus();
        key(Qt::Key_Return);
        return true;
      },
      [=] {
        if (!waitFor(!design->featureActive() && !win->m_doc->scene.features.empty() && win->m_doc->scene.features.back().kind == "fillet", "Enter did not commit the fillet")) return false;
        const opad::json edges = win->m_doc->scene.features.back().inputs.value("edges", opad::json());
        require(edges.is_array() && edges.size() == 1 && !view->previewPeek() && view->previewBodyCount() == 0, "the committed fillet is not on the one edge left");
        pass("Enter commits the fillet on the edges picked");
        return true;
      },
  };
  auto* timer = new QTimer(&w);
  timer->setInterval(60);
  QObject::connect(timer, &QTimer::timeout, &w, [st, steps, timer] {
    try {
      if (st->step < steps.size()) {
        if (steps[st->step]()) ++st->step;
        return;
      }
      timer->stop();
      trace::log("bench: preview-peek: PASS");
      QCoreApplication::exit(0);
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: preview-peek: %1 FAIL").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
