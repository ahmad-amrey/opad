// OPAD_BENCH_ACTIVATE: Activate component (UI-33, app/ActivationArea.cpp) in the running app. Case in
// tools/bench_cases/assembly.py; what the timeline dims is core's ops_in_component (tests/test_design).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>

#include <Bnd_Box.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "AgentBridge.hpp"
#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "opad/geometry.hpp"

namespace {
// One step of the bench: once `ready` holds (polled every 50 ms, at most `ms`), `act` runs with whether it did.
struct Step {
  std::function<bool()> ready;
  std::function<void(bool)> act;
  int ms = 20000;
};

void runSteps(QObject* context, std::shared_ptr<std::vector<Step>> steps, size_t i, std::function<void()> finish) {
  if (i >= steps->size()) return finish();
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [=] {
    const Step& step = (*steps)[i];
    const bool ok = !step.ready || step.ready();
    if (!ok && clock->elapsed() < step.ms) return;
    timer->stop();
    timer->deleteLater();
    step.act(ok);
    runSteps(context, steps, i + 1, finish);
  });
  timer->start(50);
}

const opad::Node* named(const opad::Scene& scene, const std::string& name) {
  for (const auto& [id, n] : scene.nodes)
    if (n.name == name) return &n;
  return nullptr;
}

// Where the browser lists a sketch (the row being made when `id` is empty): the id of the row holding its Sketches folder
// ("" = the document), "?" when it is in none (or, `open`, when that folder is closed).
std::string sketchFolderOwner(BrowserTree* tree, const std::string& id, bool open = true) {
  QTreeWidgetItem* row = nullptr;
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, Qt::UserRole).toString() == "sketch" && (id.empty() ? (*it)->data(0, Qt::UserRole + 8).toBool() : (*it)->data(0, browser::kIdRole).toString().toStdString() == id)) row = *it;
  QTreeWidgetItem* folder = row ? row->parent() : nullptr;
  if (!folder || folder->data(0, browser::kFolderRole).toString() != "sketches" || !folder->parent() || (open && !folder->isExpanded())) return "?";
  return folder->parent()->data(0, browser::kIdRole).toString().toStdString();
}

// The longest the event loop was held while something ran: a 1 ms ticker's worst gap.
struct Ticker {
  QTimer timer;
  QElapsedTimer gap, phase;
  qint64 worst = 0;
  Ticker() {
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, [this] { worst = std::max(worst, gap.restart()); });
  }
  void start() {
    worst = 0;
    gap.start();
    phase.start();
    timer.start(1);
  }
};
}  // namespace

// OPAD_BENCH_ACTIVATE=<prefix> on two components, Housing and Lid, a box in each, and a sketch at the root. The Lid's radio
// in the browser activates it without selecting it; the Housing's box and the root sketch are ghosted at the theme's
// ghost opacity and not pickable (a click there selects nothing, the same click on the Lid's box does), the Lid's box is
// drawn as it is; the chip names the Lid, the browser bolds it and dims the Housing, the timeline dims the Housing's
// ops; a guided tool picks the ghost as a reference, and after it the ghost is unpickable again; F frames the Lid; a
// sketch made through the plane picker (ghosts pickable while choosing) and the sketch editor, a box made through the
// feature panel and an import all land in the Lid (no reparent op); the context menu offers Activate for a body of
// another component and Activate root; an undone active component hands activation back to the root; the chip's click
// activates the root and everything is drawn and picked as before; the Lid activated, saved and opened again is active
// again. With the Lid active: <prefix>.ghost.png (the view),
// <prefix>.browser.png, <prefix>.chips.png, <prefix>.timeline.png and <prefix>.ribbon.png (Design > Assemble); with its
// sketch made and selected, <prefix>.sketches.png (the browser).
OPAD_BENCH(OPAD_BENCH_ACTIVATE, activate) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: activate: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  struct State {
    std::string housing, lid, boxA, boxB, sketch, temp;
    int ax = 0, ay = 0, bx = 0, by = 0;
    size_t sketches = 0, features = 0, ops = 0;
    QTemporaryDir dir;
  };
  auto s = std::make_shared<State>();
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  DesignController* design = w.m_design;
  const QString prefix = value;
  auto idle = [v, &w] { return !v->looksPending() && !w.m_displayJob && w.m_meshRemaining == 0; };
  auto picked = [v] {
    std::vector<std::string> ids;
    for (const auto& r : v->selection()) ids.push_back(r.body);
    return ids;
  };
  auto centre = [doc, v](const std::string& id) {
    const Bnd_Box box = opad::node_world_bbox(doc->doc, doc->scene, id);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    return v->widgetPoint({(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2});
  };
  auto steps = std::make_shared<std::vector<Step>>();
  auto& list = *steps;
  if (doc->scene.all_bodies().size() > 16) {
    // A big assembly (the Engine): a component holding about half of the bodies activated and the root again, each with no
    // event-loop gap over 250 ms (the ghosts applied by the sliced look job, the timeline's scope, the browser).
    for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
      if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
    auto ticker = std::make_shared<Ticker>();
    auto settled = [&w, v, doc, idle] {
      int expected = 0;
      for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
      return idle() && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0;
    };
    list.push_back({settled, [=](bool shown) {
                      const size_t total = doc->scene.all_bodies().size();
                      size_t best = 0, under = 0;
                      for (const auto& [id, n] : doc->scene.nodes)
                        if (n.kind == opad::Node::Kind::Component && !n.parent.empty()) {
                          const size_t in = doc->scene.bodies_under(id).size();
                          if (std::min(in, total - in) > best) best = std::min(in, total - in), under = in, s->lid = id;
                        }
                      require(shown && !s->lid.empty(), QString("%1 bodies displayed; %2 activated, %3 bodies in it").arg(v->displayedCount()).arg(doc->nodeName(s->lid)).arg(under));
                      ticker->start();
                      doc->setActiveComponent(s->lid);
                    }, 220000});
    list.push_back({[v] { return !v->looksPending(); }, [=](bool done) {
                      ticker->timer.stop();
                      size_t ghosts = 0, inside = 0;
                      for (const auto& id : doc->scene.all_bodies()) (v->shownLook(id).ghost ? ghosts : inside) += 1;
                      require(done && ticker->worst < 250 && ghosts > 0 && inside > 0,
                              QString("activated in %1 ms, %2 ghosts, %3 inside, worst event-loop gap %4 ms").arg(ticker->phase.elapsed()).arg(ghosts).arg(inside).arg(ticker->worst));
                      // The hover hint's pick (one per resting mouse): a 5 x 5 grid over the view.
                      v->fitAll();
                      v->benchPickAt(0, 0);
                      qint64 slowest = 0;
                      int found = 0;
                      for (int j = 1; j <= 5; ++j)
                        for (int i = 1; i <= 5; ++i) {
                          QElapsedTimer pick;
                          pick.start();
                          found += !v->ghostAt(QPointF(v->width() * i / 6.0, v->height() * j / 6.0)).empty();
                          slowest = std::max(slowest, pick.elapsed());
                        }
                      require(slowest < 50, QString("a ghost's hover pick: slowest of 25 %1 ms (%2 on a ghost)").arg(slowest).arg(found));
                      ticker->start();
                      doc->setActiveComponent({});
                    }, 60000});
    list.push_back({[v] { return !v->looksPending(); }, [=](bool done) {
                      ticker->timer.stop();
                      const auto bodies = doc->scene.all_bodies();
                      require(done && ticker->worst < 250 && std::none_of(bodies.begin(), bodies.end(), [v](const std::string& id) { return v->shownLook(id).ghost; }),
                              QString("the root again in %1 ms, no ghosts, worst event-loop gap %2 ms").arg(ticker->phase.elapsed()).arg(ticker->worst));
                    }, 60000});
    runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
    return true;
  }
  // The document, made through the command layer (as opad-cli would): two components with a box made in each (the
  // feature's component) and a sketch at the root; then drawn and pickable.
  list.push_back({{}, [=](bool) {
                    const std::string housing = doc->run("component", {{"name", "Housing"}}).value("id", "");
                    const std::string lid = doc->run("component", {{"name", "Lid"}}).value("id", "");
                    doc->run("feature", {{"kind", "box"}, {"component", housing}, {"inputs", {{"x", "15 mm"}, {"y", "10 mm"}, {"length", "30 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}});
                    doc->run("feature", {{"kind", "box"}, {"component", lid}, {"inputs", {{"x", "50 mm"}, {"y", "10 mm"}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "5 mm"}}}});
                    doc->run("sketch", {{"name", "Root sketch"}, {"geometry", opad::json::parse(R"({"shapes":[{"kind":"rect2","picks":[[-30,-10],[-20,0]]}]})")}});
                  }});
  auto rootSketch = [doc] {
    for (const auto& sketch : doc->scene.sketches)
      if (sketch.name == "Root sketch") return sketch.id;
    return std::string();
  };
  list.push_back({[doc, v, idle, rootSketch] {
                    const auto bodies = doc->scene.all_bodies();
                    return idle() && bodies.size() == 2 && v->displayedCount() == 2 && !rootSketch().empty() && !v->benchLookState(rootSketch()).empty();
                  },
                  [=](bool shown) {
                    const opad::Node* housing = named(doc->scene, "Housing");
                    const opad::Node* lid = named(doc->scene, "Lid");
                    s->housing = housing ? housing->id : "";
                    s->lid = lid ? lid->id : "";
                    for (const auto& b : doc->scene.all_bodies()) (doc->scene.node(b)->parent == s->housing ? s->boxA : s->boxB) = b;
                    s->sketch = rootSketch();
                    v->fitAll();
                    const bool points = v->benchBodyPoint(s->boxA, s->ax, s->ay) && v->benchBodyPoint(s->boxB, s->bx, s->by);
                    require(shown && housing && lid && !s->boxA.empty() && !s->boxB.empty() && points && doc->activeComponent().empty(),
                            QString("two components with a box each, both picked (at %1,%2 and %3,%4 of %5x%6 px), the root active")
                                .arg(s->ax).arg(s->ay).arg(s->bx).arg(s->by).arg(v->width() * v->displayScale()).arg(v->height() * v->displayScale()));
                  }});
  // The Lid's radio in the browser.
  list.push_back({{}, [=, &w](bool) {
                    BrowserTree* tree = w.m_browser->tree();
                    QTreeWidgetItem* row = nullptr;
                    for (QTreeWidgetItemIterator it(tree); *it; ++it)
                      if ((*it)->data(0, browser::kIdRole).toString().toStdString() == s->lid) row = *it;
                    auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
                    const QModelIndex index = row ? tree->indexFromItem(row) : QModelIndex();
                    const QRect rect = tree->visualRect(index);
                    QRect lead;
                    const browser::Badge* radio = delegate && index.isValid() ? delegate->badgeAt(delegate->decoration(index), index, rect, QPoint(rect.left() + browser::kSwatchX + 5, rect.center().y()), &lead) : nullptr;
                    require(radio && radio->icon == "radioOff" && radio->clicked, "the Lid's row has an activation radio, off");
                    if (!radio) return;
                    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                      QMouseEvent e(type, QPointF(lead.center()), QPointF(tree->viewport()->mapToGlobal(lead.center())), Qt::LeftButton,
                                    type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
                      QCoreApplication::sendEvent(tree->viewport(), &e);
                    }
                    require(doc->activeComponent() == s->lid && w.m_browser->selectedIds().empty(), "the radio's click activates the Lid and selects nothing");
                  }});
  // Alt+click on a component row activates it too, the document row the root, and selects nothing; the breadcrumb leads
  // to the active component and marks it where a selection's path passes it.
  list.push_back({{}, [=, &w](bool) {
                    BrowserTree* tree = w.m_browser->tree();
                    auto altClick = [tree](const std::string& id) {
                      for (QTreeWidgetItemIterator it(tree); *it; ++it)
                        if ((*it)->data(0, browser::kIdRole).toString().toStdString() == id && ((*it)->data(0, Qt::UserRole).toString() == "document") == id.empty()) {
                          const QRect rect = tree->visualRect(tree->indexFromItem(*it));
                          const QPoint at(rect.left() + browser::kNameX + 8, rect.center().y());
                          for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                            QMouseEvent e(type, QPointF(at), QPointF(tree->viewport()->mapToGlobal(at)), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::AltModifier);
                            QCoreApplication::sendEvent(tree->viewport(), &e);
                          }
                          return true;
                        }
                      return false;
                    };
                    auto crumb = [&w] {
                      for (QLabel* label : w.m_browser->findChildren<QLabel*>())
                        if (label->textFormat() == Qt::RichText) return label->text();
                      return QString();
                    };
                    const QString idle = crumb();
                    require(idle.contains(BrowserPanel::tr("Active:")) && idle.contains("Lid"), "the breadcrumb with nothing selected: " + idle);
                    w.m_browser->selectIds({s->boxB});
                    const QString through = crumb();
                    w.m_browser->selectIds({});
                    v->clearSelection();
                    require(through.contains(QString("Lid<span")) && through.contains(BrowserPanel::tr("(active)")) && !through.contains(BrowserPanel::tr("Active:")), "a body of the Lid selected, its path marks the Lid: " + through);
                    const bool housing = altClick(s->housing) && doc->activeComponent() == s->housing && tree->selectedItems().isEmpty();
                    const bool root = altClick({}) && doc->activeComponent().empty();
                    require(housing && root && altClick(s->lid) && doc->activeComponent() == s->lid && tree->selectedItems().isEmpty(),
                            "Alt+click activates the Housing, the document row the root, the Lid again; nothing selected");
                  }});
  // Ghosts: drawn, not picked, the rest of the window following.
  list.push_back({idle, [=, &w](bool) {
                    const double alpha = v->tokens().ghost.alphaF();
                    const auto a = v->benchLookState(s->boxA), b = v->benchLookState(s->boxB), sk = v->benchLookState(s->sketch);
                    require(v->shownLook(s->boxA).ghost && std::abs(a.value("transparency", 0.0) - (1 - alpha)) < 1e-6 && a.value("activated", -1) == 0 && a.value("displayed", false),
                            QString("the Housing's box is a ghost at %1 opacity, not pickable: %2").arg(alpha, 0, 'f', 2).arg(QString::fromStdString(a.dump())));
                    require(!v->shownLook(s->boxB).ghost && b.value("transparency", 1.0) == 0.0 && b.value("activated", 0) > 0, "the Lid's box is drawn and picked as it is");
                    require(v->shownLook(s->sketch).ghost && sk.value("activated", -1) == 0, "the root's sketch is ghosted too");
                    require(v->benchPickAt(s->ax, s->ay) != s->boxA && v->benchPickAt(s->bx, s->by) == s->boxB && v->hoverName(s->boxA).endsWith(Viewport::tr(" (inactive)")),
                            "picking passes the ghost, finds the Lid's box");
                    QLabel* chip = nullptr;
                    for (QLabel* label : w.m_chips->findChildren<QLabel*>())
                      if (label->isVisibleTo(w.m_chips) && label->text().contains("Lid")) chip = label;
                    require(chip && chip->objectName() == "chipSel", "the chip names the active component: " + (chip ? chip->text() : QString()));
                    BrowserTree* tree = w.m_browser->tree();
                    auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
                    browser::Decoration lidRow, housingRow, documentRow;
                    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                      const std::string id = (*it)->data(0, browser::kIdRole).toString().toStdString();
                      const browser::Decoration d = delegate->decoration(tree->indexFromItem(*it));
                      if (id == s->lid) lidRow = d;
                      else if (id == s->housing) housingRow = d;
                      else if ((*it)->data(0, Qt::UserRole).toString() == "document") documentRow = d;
                    }
                    const bool pill = lidRow.badges.size() == 1 && lidRow.badges.front().color == &Tokens::sel && housingRow.badges.isEmpty();
                    require(lidRow.lead.icon == "radioOn" && lidRow.bold && pill && !lidRow.dim && housingRow.lead.icon == "radioOff" && housingRow.dim && documentRow.lead.icon == "radioOff" && !documentRow.dim,
                            "browser: the Lid's radio on, its name bold with an 'active' pill, the Housing dimmed, the document's radio off");
                    const auto& dimmed = w.m_timeline->dimmedOps();
                    const opad::Node* na = doc->scene.node(s->boxA);
                    const opad::Node* nb = doc->scene.node(s->boxB);
                    require(na && nb && dimmed.count(doc->scene.node(s->housing)->source_op) && dimmed.count(na->source_op) && !dimmed.count(doc->scene.node(s->lid)->source_op) && !dimmed.count(nb->source_op),
                            QString("timeline: the Housing's ops dimmed, the Lid's not (%1 of %2 dimmed)").arg(dimmed.size()).arg(doc->doc.ops.size()));
                    require(w.action("assembly.activateRoot")->isEnabled() && !w.action("assembly.activate")->isEnabled(), "Activate root enabled, Activate not (nothing selected)");
                    const opad::json live = w.m_agent->liveState();
                    require(live.contains("active_component") && live["active_component"].value("id", "") == s->lid && live["active_component"].value("name", "") == "Lid",
                            "MCP live_state reports the active component: " + QString::fromStdString(live.value("active_component", opad::json()).dump()));
                    v->grabImage().save(prefix + ".ghost.png");
                    w.m_browser->grab().save(prefix + ".browser.png");
                    w.m_chips->grab().save(prefix + ".chips.png");
                    w.m_timeline->grab().save(prefix + ".timeline.png");
                    w.setWorkspace("design");  // Design > Assemble > Components: Activate, Activate root
                    w.m_ribbon->setCurrentTab(int(w.m_ribbon->tabIds().indexOf("design.assemble")));
                    int buttons = 0;
                    if (RibbonPage* page = w.m_ribbon->page("design.assemble"))
                      for (QToolButton* b : page->findChildren<QToolButton*>())
                        buttons += b->defaultAction() == w.action("assembly.activate") || b->defaultAction() == w.action("assembly.activateRoot");
                    w.m_ribbon->grab().save(prefix + ".ribbon.png");
                    require(buttons == 2, QString("the Assemble tab has Activate and Activate root (%1 buttons)").arg(buttons));
                    w.action("assembly.activeVisibility")->trigger();  // off: the rest drawn and picked as it is
                  }});
  // Active component visibility and the inactive opacity.
  list.push_back({[=] { return idle() && !v->shownLook(s->boxA).ghost; }, [=, &w](bool plain) {
                    const auto a = v->benchLookState(s->boxA);
                    QLabel* chip = nullptr;
                    for (QLabel* label : w.m_chips->findChildren<QLabel*>())
                      if (label->isVisibleTo(w.m_chips) && label->text().contains("Lid")) chip = label;
                    require(plain && a.value("transparency", 1.0) == 0.0 && a.value("activated", 0) > 0 && v->benchPickAt(s->ax, s->ay) == s->boxA && chip && doc->activeComponent() == s->lid,
                            "visibility off: the Housing's box drawn and picked as it is, the Lid still active and named");
                    w.action("assembly.activeVisibility")->trigger();
                    QMenu* opacity = w.findChild<QMenu*>("assembly.inactiveOpacity");
                    for (QAction* preset : opacity ? opacity->actions() : QList<QAction*>())
                      if (std::abs(preset->data().toDouble() - 0.1) < 1e-9) preset->trigger();
                  }});
  list.push_back({[=] { return idle() && v->shownLook(s->boxA).ghost; }, [=, &w](bool ghost) {
                    const auto a = v->benchLookState(s->boxA);
                    const QMenu* chipMenu = w.findChild<QMenu*>("activationChipMenu");
                    const QMenu* opacity = w.findChild<QMenu*>("assembly.inactiveOpacity");
                    auto menus = [](QAction* a) {  // the menus it is in: at least the Design menu and the chip's
                      int n = 0;
                      for (QObject* o : a ? a->associatedObjects() : QList<QObject*>()) n += qobject_cast<QMenu*>(o) != nullptr;
                      return n;
                    };
                    require(ghost && std::abs(a.value("transparency", 0.0) - 0.9) < 0.01 && a.value("activated", -1) == 0 && w.action("assembly.activeVisibility")->isChecked() &&
                                chipMenu && opacity && chipMenu->actions() == QList<QAction*>({w.action("assembly.activateRoot"), w.action("assembly.activeVisibility"), opacity->menuAction()}) &&
                                menus(w.action("assembly.activeVisibility")) >= 2 && menus(opacity->menuAction()) >= 2,
                            QString("visibility on at 10 %: the Housing's box a ghost at transparency %1, not pickable (%2 modes); the chip's menu (%3 entries) and the Design menu have both (%4, %5 menus)")
                                .arg(a.value("transparency", 0.0)).arg(a.value("activated", -1)).arg(chipMenu ? chipMenu->actions().size() : -1)
                                .arg(menus(w.action("assembly.activeVisibility"))).arg(opacity ? menus(opacity->menuAction()) : -1));
                    for (QAction* preset : opacity ? opacity->actions() : QList<QAction*>())
                      if (preset->data().toDouble() == 0) preset->trigger();  // the theme's again
                  }});
  // A ghost under the resting mouse is named in the status bar with the way to activate its component; the right-click
  // menu there offers it; a double click on a ghost activates its component.
  auto mouse = [v](QEvent::Type type, int x, int y, Qt::MouseButton button) {
    const QPointF at(x / v->displayScale(), y / v->displayScale());
    QMouseEvent e(type, at, v->mapToGlobal(at), button, type == QEvent::MouseButtonRelease || type == QEvent::MouseMove ? Qt::NoButton : button, Qt::NoModifier);
    QCoreApplication::sendEvent(v, &e);
  };
  list.push_back({[=] { return idle() && std::abs(v->benchLookState(s->boxA).value("transparency", 0.0) - (1 - v->tokens().ghost.alphaF())) < 1e-6; }, [=](bool themed) {
                    require(themed, "the Theme preset: the ghost at the theme's opacity again");
                    mouse(QEvent::MouseMove, s->ax, s->ay, Qt::NoButton);
                  }});
  list.push_back({[=, &w] { return w.m_statusHover->text().contains("Housing"); }, [=, &w](bool named) {
                    const QString hint = w.m_statusHover->text();
                    require(named && hint.contains(doc->nodeName(s->boxA)), "resting on the Housing's ghost, the status names it and its component: " + hint);
                    mouse(QEvent::MouseMove, s->bx, s->by, Qt::NoButton);
                  }, 3000});
  list.push_back({[=, &w] { return w.m_statusHover->text().isEmpty(); }, [=, &w](bool cleared) {
                    require(cleared, "on the Lid's box (no ghost) the hint goes");
                    QStringList entries;
                    QAction* housing = nullptr;
                    {
                      const QSignalBlocker quiet(v);  // the menu this click opens is asked for below, not shown
                      mouse(QEvent::MouseButtonPress, s->ax, s->ay, Qt::RightButton);
                      mouse(QEvent::MouseButtonRelease, s->ax, s->ay, Qt::RightButton);
                    }
                    QMenu menu;
                    for (AreaController* area : w.m_areas) area->contextMenu(SelectionContext(), menu);
                    for (QAction* a : menu.actions()) {
                      entries << (a->objectName().isEmpty() ? a->text() : a->objectName());
                      if (a->objectName().isEmpty() && a->text().contains("Housing")) housing = a;
                    }
                    if (housing) housing->trigger();
                    require(housing && doc->activeComponent() == s->housing, "right-click on the Housing's ghost offers to activate it: " + entries.join(", "));
                  }});
  list.push_back({[=] { return idle() && v->shownLook(s->boxB).ghost; }, [=](bool ghost) {
                    mouse(QEvent::MouseButtonDblClick, s->bx, s->by, Qt::LeftButton);
                    require(ghost && doc->activeComponent() == s->lid, "a double click on the Lid's box, a ghost now, activates the Lid again");
                  }});
  list.push_back({[=] { return idle() && !v->shownLook(s->boxB).ghost; }, [=](bool) {
                    v->benchClickAt(s->bx, s->by);  // the positive control: the same click on the Lid's box selects it
                  }});
  list.push_back({[=] { return picked() == std::vector<std::string>{s->boxB}; }, [=](bool selected) {
                    require(selected, "a click on the Lid's box selects it");
                    v->benchClickAt(s->ax, s->ay);
                  }});
  list.push_back({[=] { return picked().empty(); }, [=, &w](bool cleared) {
                    require(cleared, "a click on the ghost selects nothing (the selection is refused)");
                    w.startTool("distance");
                  }});
  // References: a guided tool picks the ghost.
  list.push_back({[=] { return v->ghostsPickable() && !v->looksPending(); }, [=, &w](bool) {
                    require(v->benchPickAt(s->ax, s->ay) == s->boxA && v->benchLookState(s->boxA).value("activated", 0) > 0 && v->shownLook(s->boxA).ghost,
                            "while a guided tool picks, the ghost is picked as a reference (still drawn as a ghost)");
                    w.cancelTool();
                  }});
  list.push_back({[=] { return !v->ghostsPickable() && !v->looksPending(); }, [=, &w](bool) {
                    require(v->benchPickAt(s->ax, s->ay) != s->boxA, "the tool closed: the ghost is not picked again");
                    // F with nothing selected frames the active component.
                    w.action("view.fit")->trigger();
                    const QPoint middle = v->rect().center(), lid = centre(s->boxB), housing = centre(s->boxA);
                    require((lid - middle).manhattanLength() < v->width() / 10 && (housing - middle).manhattanLength() > (lid - middle).manhattanLength() + v->width() / 10,
                            QString("F frames the Lid: its box at (%1, %2), the middle (%3, %4)").arg(lid.x()).arg(lid.y()).arg(middle.x()).arg(middle.y()));
                    s->sketches = doc->scene.sketches.size();
                    design->startSketch();
                  }});
  // A sketch: plane picker (ghosts pickable meanwhile), editor, Finish.
  list.push_back({[=] { return design->planePicker()->active(); }, [=](bool active) {
                    require(active && v->ghostsPickable(), "choosing the sketch plane: ghosts are pickable (a face of another component is a plane too)");
                    design->planePicker()->choose({{"base", "xy"}});
                  }});
  list.push_back({[=] { return design->planePicker()->positioning(); }, [=](bool ok) {
                    require(ok, "the XY plane chosen");
                    design->planePicker()->apply();
                  }});
  list.push_back({[=] { return design->sketchActive(); }, [=, &w](bool ok) {
                    require(ok && !v->ghostsPickable(), "the sketch editor is open; ghosts not pickable any more");
                    require(sketchFolderOwner(w.m_browser->tree(), {}) == s->lid, "the browser lists the sketch being made in the Lid's own Sketches folder");
                    SketchEditor* sketch = design->sketch();
                    sketch->setTool("rect");
                    for (const auto& [u, x] : std::vector<std::pair<double, double>>{{45, 2}, {55, 12}}) {
                      sketch->sketchMove(u, x, Qt::NoModifier, false);
                      sketch->sketchPress(u, x, Qt::NoModifier);
                      sketch->sketchRelease(u, x, Qt::NoModifier);
                    }
                    sketch->setTool("select");
                    design->finishSketch();
                  }});
  list.push_back({[=] { return doc->scene.sketches.size() > s->sketches && !design->sketchActive() && !doc->designBusy && !v->looksPending(); }, [=, &w](bool made) {
                    const opad::SketchItem* sketch = made ? &doc->scene.sketches.back() : nullptr;
                    const opad::Op* op = sketch ? doc->doc.find_op(sketch->id) : nullptr;
                    require(sketch && sketch->component == s->lid && op && op->data.value("component", "") == s->lid && !v->shownLook(sketch->id).ghost && v->shownLook(s->sketch).ghost,
                            "the new sketch is made in the Lid (its op says so), drawn as it is; the root's stays a ghost");
                    BrowserTree* tree = w.m_browser->tree();
                    const std::string in = sketch ? sketchFolderOwner(tree, sketch->id) : "?", root = sketchFolderOwner(tree, s->sketch, false);
                    require(in == s->lid && root.empty(), QString("browser: the new sketch in the Lid's Sketches folder (open), the root's in the document's (%1, %2)")
                                                              .arg(QString::fromStdString(in == s->lid ? "Lid" : in), QString::fromStdString(root)));
                    if (sketch) w.m_browser->selectIds({sketch->id});
                    w.m_browser->grab().save(prefix + ".sketches.png");
                    QString crumb;
                    for (QLabel* label : w.m_browser->findChildren<QLabel*>())
                      if (label->textFormat() == Qt::RichText) crumb = label->text();
                    w.m_browser->selectIds({});
                    require(sketch && crumb.contains("Lid<span") && crumb.contains(QString::fromStdString(sketch->name)), "the sketch selected, the breadcrumb goes through the Lid: " + crumb);
                    s->features = doc->scene.features.size();
                    design->startFeature("box");
                    FeaturePanel* form = design->featurePanel();
                    form->setValue("x", "50 mm");
                    form->setValue("y", "30 mm");
                    s->ops = doc->doc.ops.size();
                    emit form->accepted();
                  }});
  // A box through the feature panel.
  list.push_back({[=] { return doc->scene.features.size() > s->features && !design->featureActive() && !doc->designBusy; }, [=](bool made) {
                    const opad::Feature* box = made ? &doc->scene.features.back() : nullptr;
                    const auto bodies = box ? box->result.value("bodies", opad::json::array()) : opad::json::array();
                    const opad::Node* body = bodies.empty() ? nullptr : doc->scene.node(bodies[0].value("id", ""));
                    require(box && box->component == s->lid && body && body->parent == s->lid && doc->doc.ops.size() == s->ops + 1 && doc->doc.ops.back().type == "feature",
                            QString("the new box is made in the Lid, its body under it, one op and no reparent (%1 ops)").arg(doc->doc.ops.size() - s->ops));
                    QFile obj(s->dir.filePath("block.obj"));
                    obj.open(QIODevice::WriteOnly);
                    obj.write("v 70 0 0\nv 80 0 0\nv 80 10 0\nv 70 10 0\nv 70 0 10\nv 80 0 10\nv 80 10 10\nv 70 10 10\n"
                              "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8\n");
                    obj.close();
                    s->ops = doc->doc.ops.size();
                    doc->startImport(obj.fileName());
                  }});
  // An import, the context menu, falling back to the root.
  list.push_back({[=] { return !doc->loading && doc->doc.ops.size() > s->ops; }, [=, &w](bool imported) {
                    const opad::Op& op = doc->doc.ops.back();
                    require(imported && op.type == "import" && op.data.value("parent", "") == s->lid, "an import goes into the Lid");
                    SelectionContext context;
                    context.ids = {s->boxA};
                    QMenu menu;
                    for (AreaController* area : w.m_areas) area->contextMenu(context, menu);
                    QStringList entries;
                    for (QAction* a : menu.actions()) entries << a->objectName();
                    require(entries.contains("assembly.activate") && entries.contains("assembly.activateRoot"), "context menu on the Housing's box: " + entries.join(' '));
                    s->temp = doc->run("component", {{"name", "Temp"}}).value("id", "");
                    doc->setActiveComponent(s->temp);
                    const bool moved = doc->activeComponent() == s->temp;
                    doc->undo();
                    require(moved && doc->activeComponent().empty() && !doc->scene.node(s->temp), "the active component undone: the root is active");
                    w.m_browser->selectIds({s->boxB});  // a body: its component is what Activate activates
                    QAction* activate = w.action("assembly.activate");
                    const bool enabled = activate->isEnabled();
                    if (enabled) activate->trigger();  // disabled, it would only say why in a message box
                    require(enabled && doc->activeComponent() == s->lid, "Activate on a selected body activates its component, the Lid");
                    w.m_browser->selectIds({});
                    v->clearSelection();
                  }});
  // The chip: back to the root, everything as before.
  list.push_back({idle, [=, &w](bool) {
                    QLabel* chip = nullptr;
                    for (QLabel* label : w.m_chips->findChildren<QLabel*>())
                      if (label->isVisibleTo(w.m_chips) && label->text().contains("Lid")) chip = label;
                    if (chip) {
                      QMouseEvent e(QEvent::MouseButtonRelease, QPointF(4, 4), QPointF(chip->mapToGlobal(QPoint(4, 4))), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                      QCoreApplication::sendEvent(chip, &e);
                    }
                    require(chip && doc->activeComponent().empty() && !chip->isVisibleTo(w.m_chips), "the chip's click activates the root and the chip goes");
                  }});
  list.push_back({idle, [=, &w](bool) {
                    bool restored = true;
                    for (const auto& id : doc->scene.all_bodies()) restored = restored && !v->shownLook(id).ghost && v->shownLook(id) == v->bodyLook(id);
                    const auto a = v->benchLookState(s->boxA);
                    v->fitAll();
                    int x = 0, y = 0;
                    require(restored && a.value("transparency", 1.0) == 0.0 && a.value("activated", 0) > 0 && v->benchBodyPoint(s->boxA, x, y) && !v->shownLook(s->sketch).ghost &&
                                w.m_timeline->dimmedOps().empty() && !w.action("assembly.activateRoot")->isEnabled() && !w.m_agent->liveState().contains("active_component"),
                            "the root active: no ghosts, the Housing's box picked again, the timeline undimmed, live_state without an active component");
                    // Remembered per document: the Lid activated again, saved, opened again.
                    w.m_browser->selectIds({s->lid});
                    if (QAction* activate = w.action("assembly.activate"); activate->isEnabled()) activate->trigger();
                    w.m_browser->selectIds({});
                    doc->saveAs(s->dir.filePath("again.opad"));
                    doc->startOpen(s->dir.filePath("again.opad"));
                  }});
  list.push_back({[doc] { return !doc->loading && !doc->activeComponent().empty(); }, [=, &w](bool restored) {
                    QLabel* chip = nullptr;
                    for (QLabel* label : w.m_chips->findChildren<QLabel*>())
                      if (label->isVisibleTo(w.m_chips) && label->text().contains("Lid")) chip = label;
                    require(restored && doc->activeComponent() == s->lid && chip, "opened again, the Lid it was left in is active again");
                  }});
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
