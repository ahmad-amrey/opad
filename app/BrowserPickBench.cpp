// The browser as a place to pick and to rename: a component renamed in its row shows its new name at once, Rename on a
// component chosen in the browser renames the component (not the bodies under it, which the view shows selected), and a
// feature's bodies input takes a row clicked in the browser (a component: every body under it). Case in
// tools/bench_cases/assembly.py.
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTreeWidgetItemIterator>

#include <functional>

#include "BenchRegistry.hpp"
#include "BrowserDelegate.hpp"
#include "BrowserPanel.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "MainWindow.hpp"

namespace {
bool settle(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  QCoreApplication::processEvents();
  return done();
}

QTreeWidgetItem* row(QTreeWidget* tree, const std::string& id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, browser::kIdRole).toString().toStdString() == id) return *it;
  return nullptr;
}

// Types a name into the row's editor and presses Enter, as the user does.
bool typeName(BrowserTree* tree, const QString& name) {
  auto* editor = qobject_cast<QLineEdit*>(tree->renameEditor());
  if (!editor) return false;
  editor->setText(name);
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(editor, &enter);
  QCoreApplication::processEvents();
  return true;
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_BROWSER_PICK, browserPick) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: browser pick: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] { QCoreApplication::exit(all ? 0 : 2); return true; };
  AppDocument* doc = w.m_doc;
  BrowserPanel* browser = w.m_browser;
  BrowserTree* tree = browser->tree();
  std::string comp, other;
  try {
    comp = doc->run("component", {{"name", "Component"}}).value("id", "");
    other = doc->run("component", {{"name", "Elsewhere"}}).value("id", "");
    for (const char* name : {"Lid", "Base"})
      doc->run("feature", {{"kind", "box"}, {"component", comp}, {"body_name", name}, {"inputs", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}}});
    doc->run("feature", {{"kind", "box"}, {"component", other}, {"inputs", {{"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}}}});
  } catch (const std::exception& e) {
    require(false, QString("the parts made: %1").arg(QString::fromUtf8(e.what())));
    return finish();
  }
  settle([doc] { return !doc->designBusy; }, 60000);
  w.m_browserOverlay->reveal(true);
  QApplication::setActiveWindow(&w);
  settle([&] { return row(tree, comp) != nullptr; }, 10000);
  auto bodiesIn = [doc](const std::string& component) {
    std::vector<std::string> out;
    if (const opad::Node* n = doc->node(component))
      for (const auto& c : n->children)
        if (const opad::Node* b = doc->node(c); b && b->kind == opad::Node::Kind::Body) out.push_back(c);
    return out;
  };
  const std::vector<std::string> bodies = bodiesIn(comp);
  auto bodyNames = [doc, &bodies] {
    QStringList names;
    for (const auto& b : bodies) names << doc->nodeName(b);
    return names.join(", ");
  };
  const QString namesBefore = bodyNames();

  // 1. Renamed in its row: the row shows the new name (it kept showing "Component").
  browser->startRename(comp);
  QCoreApplication::processEvents();
  const bool typed = typeName(tree, "Enclosure");
  settle([&] { return doc->nodeName(comp) == "Enclosure"; }, 3000);
  QTreeWidgetItem* it = row(tree, comp);
  require(typed && doc->nodeName(comp) == "Enclosure" && it && it->data(0, browser::kNameRole).toString() == "Enclosure",
          QString("renamed in its row, the component's row shows the new name: \"%1\"").arg(it ? it->data(0, browser::kNameRole).toString() : QString()));

  // 2. Chosen in the browser, Rename is the component's (the view shows its bodies selected).
  browser->setSelectedIds({comp});
  w.onBrowserSelection({comp});
  settle([&] { return !w.m_viewport->selection().empty(); }, 5000);
  const std::vector<std::string> current = w.currentNodeIds();
  require(current == std::vector<std::string>{comp}, QString("the component chosen in the browser is what the edit commands act on (%1 ids, the view holding %2)")
                                                          .arg(current.size()).arg(w.m_viewport->selection().size()));
  w.action("edit.rename")->trigger();
  QCoreApplication::processEvents();
  const bool editing = tree->renameEditor() != nullptr;
  typeName(tree, "Case");
  settle([&] { return doc->nodeName(comp) == "Case"; }, 3000);
  require(editing && doc->nodeName(comp) == "Case" && bodyNames() == namesBefore && row(tree, comp)->data(0, browser::kNameRole).toString() == "Case",
          QString("Rename again renames the component, its bodies keep their names (%1; editor %2, named \"%3\")").arg(bodyNames()).arg(editing).arg(doc->nodeName(comp)));

  // 3. Move: its Bodies input takes a component clicked in the browser, every body under it.
  w.m_design->startFeature("move");
  FeaturePanel* form = w.m_design->featurePanel();
  settle([&] { return w.m_design->featureActive() && form->activeInput() == "bodies"; }, 5000);
  browser->setSelectedIds({comp});
  emit browser->selectionChanged({comp});  // what a click on the row sends
  settle([&] { const opad::json p = form->picks("bodies"); return p.is_array() && p.size() == bodies.size(); }, 3000);
  const opad::json picks = form->picks("bodies");
  std::set<std::string> picked;
  if (picks.is_array())
    for (const auto& p : picks) picked.insert(p.value("body", ""));
  require(picked == std::set<std::string>(bodies.begin(), bodies.end()) && bodies.size() == 2,
          QString("Move's Bodies input takes the component clicked in the browser: %1 of its %2 bodies").arg(picked.size()).arg(bodies.size()));
  // A body's row adds nothing new and replaces nothing it should not: another component's box alone.
  const std::vector<std::string> otherBodies = bodiesIn(other);
  emit browser->selectionChanged(otherBodies);
  settle([&] { const opad::json p = form->picks("bodies"); return p.is_array() && p.size() == 1; }, 3000);
  const opad::json one = form->picks("bodies");
  require(one.is_array() && one.size() == 1 && !otherBodies.empty() && one[0].value("body", "") == otherBodies[0],
          "a body's row picks that body");
  w.m_design->escape();
  return finish();
}
