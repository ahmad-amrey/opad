// Bodies and components on the clipboard (TODO 11 UI-129): the clipboard bench. Case in tools/bench_cases/sketch.py.
#include <BRepPrimAPI_MakeBox.hxx>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMimeData>
#include <QTabBar>
#include <QTimer>

#include <cmath>
#include <memory>

#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "MainWindow.hpp"
#include "Units.hpp"
#include "opad/clipboard.hpp"
#include "opad/geometry.hpp"

namespace {
struct Checks {
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: clipboard: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
};

void waitFor(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(20);
}

// A key pressed where the keyboard is (not spontaneous: Qt offers it to the shortcuts first, as it does a typed key).
void press(QWidget* to, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QKeyEvent event(QEvent::KeyPress, key, modifiers);
  QApplication::sendEvent(to, &event);
}

struct Steps {
  std::vector<std::function<void()>> list;
  void next() {
    if (list.empty()) return;
    auto step = list.front();
    list.erase(list.begin());
    step();
  }
};

double centreX(const AppDocument* doc, const std::string& id) {
  const Bnd_Box b = opad::node_world_bbox(doc->doc, doc->scene, id);
  if (b.IsVoid()) return NAN;
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return (x0 + x1) / 2;
}

opad::json clipboardJson() {
  const QMimeData* mime = QApplication::clipboard()->mimeData();
  if (!mime || !mime->hasFormat(SketchEditor::kClipMime)) return {};
  const QByteArray bytes = mime->data(SketchEditor::kClipMime);
  return opad::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}
}  // namespace

// OPAD_BENCH_CLIPBOARD_BODIES=<prefix> on a document with one body (UI-129), the keys pressed over the view: Ctrl+C with the body
// selected puts it on the clipboard (application/x-opad+json, this document, the BREP along); Ctrl+V opens Move / copy with
// Make a copy on and the body picked, X set to put the copy beside it; Enter makes it there (a new body entry, one undo
// step). Ctrl+Shift+V puts the same part again beside the first paste (the same entry: two instances), selected, one undo
// step named so; undo takes it away, redo brings it back. A clip from another document (a cube) pastes where it was there, its entry
// added on a worker, one undo step. <prefix>.move-panel.png: the Move / copy panel Ctrl+V opened; <prefix>.assemble.png: the
// ribbon's Assemble tab with its Clipboard group. Ctrl+X on one of two bodies one op made copies it and deletes nothing.
OPAD_BENCH(OPAD_BENCH_CLIPBOARD_BODIES, clipboardBodies) {
  auto check = std::make_shared<Checks>();
  auto steps = std::make_shared<Steps>();
  auto next = [steps] { QTimer::singleShot(0, qApp, [steps] { steps->next(); }); };
  auto finish = [check] { QCoreApplication::exit(check->all ? 0 : 2); };
  MainWindow* window = &w;
  AppDocument* doc = w.m_doc;
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  Viewport* view = w.m_viewport;
  const std::string body = doc->scene.all_bodies().empty() ? std::string() : doc->scene.all_bodies().front();
  const std::string key = body.empty() ? std::string() : doc->scene.node(body)->body_key;
  auto step = std::make_shared<double>(0);  // how far the first paste went
  auto keyboard = [window, view] {
    QApplication::setActiveWindow(window);
    view->setFocus();
    return view;
  };
  (*check)(!body.empty() && !doc->browse, "an editable document with a body");
  if (body.empty()) {
    finish();
    return true;
  }
  w.setWorkspace("design");

  steps->list.push_back([=] {  // Ctrl+C
    QApplication::clipboard()->clear();
    window->m_browser->selectIds({body});
    press(keyboard(), Qt::Key_C, Qt::ControlModifier);
    waitFor(window, [] { return !clipboardJson().is_null(); }, 10000, [=](bool ok) {
      const opad::json clip = clipboardJson();
      (*check)(ok && clip.value("format", "") == opad::kNodesClipFormat && clip.value("document", "") == doc->doc.header.uuid && clip["nodes"].size() == 1 &&
                   clip["nodes"][0].value("id", "") == body && clip["bodies"].size() == 1 && clip["bodies"][0].value("key", "") == key,
               "Ctrl+C with the body selected puts it on the clipboard as application/x-opad+json, with its entry");
      next();
    });
  });
  steps->list.push_back([=] {  // Ctrl+V: Move / copy, copy on, beside it
    press(keyboard(), Qt::Key_V, Qt::ControlModifier);
    waitFor(window, [design, form] { return design->featureActive() && form->spec() && form->spec()->kind == "move"; }, 10000, [=](bool ok) {
      const opad::json picks = form->picks("bodies");
      const auto dx = units::parse(units::Kind::Length, form->valueText("dx"));
      *step = dx.value_or(0);
      (*check)(ok && form->inputs().value("copy", false) && picks.is_array() && picks.size() == 1 && picks[0].value("body", "") == body && dx && *dx >= 30,
               "Ctrl+V opens Move / copy with Make a copy on, the body picked, X " + form->valueText("dx") + " to put it beside");
      if (value != "1") window->m_featurePanel->grab().save(value + ".move-panel.png");
      const size_t features = doc->scene.features.size(), entries = doc->doc.body_count();
      const QStringList undo = doc->undoLabels();
      press(keyboard(), Qt::Key_Return);
      waitFor(window, [design, doc, features] { return !design->featureActive() && doc->scene.features.size() > features && !doc->designBusy; }, 30000, [=](bool made) {
        const auto bodies = doc->scene.all_bodies();
        const std::string copy = bodies.size() == 2 ? (bodies[0] == body ? bodies[1] : bodies[0]) : std::string();
        const double moved = copy.empty() ? NAN : centreX(doc, copy) - centreX(doc, body);
        (*check)(made && !copy.empty() && std::abs(moved - dx.value_or(0)) < 1e-3 && doc->scene.node(copy)->body_key != key && doc->doc.body_count() == entries + 1,
                 QString("Enter makes the copy %1 mm along X as a new body").arg(moved));
        (*check)(doc->undoLabels().size() == undo.size() + 1, "one undo step: " + doc->undoLabel());
        next();
      });
    });
  });
  steps->list.push_back([=] {  // Ctrl+Shift+V: the same part again
    const size_t nodes = doc->scene.nodes.size(), entries = doc->doc.body_count();
    const QStringList undo = doc->undoLabels();
    press(keyboard(), Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    waitFor(window, [doc, nodes] { return doc->scene.nodes.size() == nodes + 1; }, 10000, [=](bool ok) {
      std::string instance;
      for (const auto& b : doc->scene.all_bodies())
        if (b != body && doc->scene.node(b)->body_key == key) instance = b;
      const double moved = instance.empty() ? NAN : centreX(doc, instance) - centreX(doc, body);
      (*check)(ok && !instance.empty() && doc->doc.body_count() == entries && doc->scene.instance_count.at(key) == 2 && std::abs(moved - 2 * *step) < 1e-3,
               QString("Ctrl+Shift+V puts the same part again (the same entry, two instances) %1 mm along X, beside the first paste").arg(moved));
      (*check)(doc->undoLabels().size() == undo.size() + 1 && doc->undoLabel() == QObject::tr("Paste as linked instances"), "one undo step: " + doc->undoLabel());
      waitFor(window, [window, instance] { return window->selectionContext().ids == std::vector<std::string>{instance}; }, 5000, [=](bool selected) {
        (*check)(selected, "the pasted instance is selected");
        doc->undo();
        const bool undone = !doc->scene.node(instance) && doc->scene.instance_count.at(key) == 1;
        doc->redo();
        (*check)(undone && doc->scene.node(instance), "undo takes it away, redo brings it back");
        next();
      });
    });
  });
  steps->list.push_back([=] {  // a clip from another document
    opad::Document other = opad::Document::create();
    const std::string cube = opad::new_uuid();
    const std::string cubeKey = other.add_body(opad::brep_from_shape(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 40), 5, 5, 5).Shape()), {{"name", "Cube"}});
    other.append({{"op", "import"}, {"source", "cube.step"}, {"units", "mm"}, {"nodes", {{{"type", "body"}, {"id", cube}, {"name", "Cube"}, {"key", cubeKey}}}}});
    auto* mime = new QMimeData;
    mime->setData(SketchEditor::kClipMime, QByteArray::fromStdString(opad::copy_nodes(other, opad::resolve(other), {cube}).dump()));
    QApplication::clipboard()->setMimeData(mime);
    const QStringList undo = doc->undoLabels();
    press(keyboard(), Qt::Key_V, Qt::ControlModifier);
    waitFor(window, [doc, cubeKey] { return doc->doc.has_body(cubeKey) && !doc->designBusy; }, 20000, [=](bool ok) {
      std::string pasted;
      for (const auto& b : doc->scene.all_bodies())
        if (doc->scene.node(b)->body_key == cubeKey) pasted = b;
      const opad::Node* n = pasted.empty() ? nullptr : doc->scene.node(pasted);
      (*check)(ok && n && n->name == "Cube" && n->parent.empty() && std::abs(centreX(doc, pasted) - 2.5) < 1, "a cube copied in another document pastes where it was there, its entry added");
      (*check)(doc->undoLabels().size() == undo.size() + 1 && doc->undoLabel() == QObject::tr("Paste"), "one undo step: " + doc->undoLabel());
      (*check)(window->m_timeline->describe(doc->doc.ops.back()) == QObject::tr("Paste %1").arg("Cube"), "the timeline calls it " + window->m_timeline->describe(doc->doc.ops.back()));
      QTabBar* tabs = window->m_ribbon->tabBar();
      for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == QObject::tr("Assemble")) window->m_ribbon->setCurrentTab(i);
      if (value != "1") window->m_ribbon->grab().save(value + ".assemble.png");
      next();
    });
  });
  steps->list.push_back([=] {  // Ctrl+X on one of two bodies one op made: copied, not cut (Delete would take both)
    opad::Document other = opad::Document::create();
    std::vector<std::string> ids;
    std::vector<std::string> keys;
    opad::json nodes = opad::json::array();
    for (int i = 0; i < 2; ++i) {
      ids.push_back(opad::new_uuid());
      keys.push_back(other.add_body(opad::brep_from_shape(BRepPrimAPI_MakeBox(gp_Pnt(100 + 10 * i, 0, 0), 5, 5, 5 + i).Shape()), {{"name", "Pair"}}));
      nodes.push_back({{"type", "body"}, {"id", ids.back()}, {"name", i ? "Pair B" : "Pair A"}, {"key", keys.back()}});
    }
    other.append({{"op", "import"}, {"source", "pair.step"}, {"units", "mm"}, {"nodes", nodes}});
    auto* mime = new QMimeData;
    mime->setData(SketchEditor::kClipMime, QByteArray::fromStdString(opad::copy_nodes(other, opad::resolve(other), ids).dump()));
    QApplication::clipboard()->setMimeData(mime);
    press(keyboard(), Qt::Key_V, Qt::ControlModifier);
    waitFor(window, [doc, keys] { return doc->doc.has_body(keys[0]) && doc->doc.has_body(keys[1]) && !doc->designBusy; }, 20000, [=](bool pasted) {
      std::string a, b;
      for (const auto& id : doc->scene.all_bodies()) {
        if (doc->scene.node(id)->name == "Pair A") a = id;
        if (doc->scene.node(id)->name == "Pair B") b = id;
      }
      (*check)(pasted && !a.empty() && !b.empty() && doc->scene.node(a)->source_op == doc->scene.node(b)->source_op, "two bodies pasted by one op");
      const size_t ops = doc->doc.ops.size();
      QApplication::clipboard()->clear();
      window->m_browser->selectIds({a});
      press(keyboard(), Qt::Key_X, Qt::ControlModifier);
      waitFor(window, [] { return !clipboardJson().is_null(); }, 10000, [=](bool copied) {
        const opad::json clip = clipboardJson();
        (*check)(copied && clip["nodes"].size() == 1 && doc->scene.node(a) && doc->scene.node(b) && doc->doc.ops.size() == ops,
                 "Ctrl+X on one of them copies it but deletes nothing: Delete would tombstone the op and take the other too");
        finish();
      });
    });
  });
  next();
  return true;
}
