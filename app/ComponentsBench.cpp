// OPAD_BENCH_COMPONENTS: components without dialogs (UI-34, app/ComponentsArea.cpp) in the running app. Cases in
// tools/bench_cases/assembly.py; one step for several commands is tests/test_app_document, the Edit look test_body_look.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QCheckBox>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPointer>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "AssemblyWidgets.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Ribbon.hpp"

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

void key(QWidget* widget, int code, const QString& text = QString()) {
  for (const QEvent::Type type : {QEvent::KeyPress, QEvent::KeyRelease}) {
    QKeyEvent e(type, code, Qt::NoModifier, text);
    QCoreApplication::sendEvent(widget, &e);
  }
}

void click(QWidget* widget) {
  const QPointF at = QRectF(widget->rect()).center();
  for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
    QMouseEvent e(type, at, widget->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &e);
  }
}

void type(QWidget* widget, const QString& text) {
  for (const QChar c : text) key(widget, c.toUpper().unicode(), QString(c));
}

// The rename editor open on a browser row, if any.
QLineEdit* renaming(BrowserTree* tree, const std::string& id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, browser::kIdRole).toString().toStdString() == id && (*it)->data(0, Qt::UserRole).toString() != "document")
      return qobject_cast<QLineEdit*>(tree->indexWidget(tree->indexFromItem(*it)));
  return nullptr;
}

// The longest the event loop was held while something ran: a 1 ms ticker's worst gap.
struct Ticker {
  QTimer timer;
  QElapsedTimer gap;
  qint64 worst = 0;
  Ticker() {
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, [this] { worst = std::max(worst, gap.restart()); });
  }
  void start() {
    worst = 0;
    gap.start();
    timer.start(1);
  }
};

bool samePlace(const opad::Mat4& a, const opad::Mat4& b) {
  for (size_t i = 0; i < 16; ++i)
    if (std::abs(a.m[i] - b.m[i]) > 1e-9) return false;
  return true;
}
}  // namespace

// OPAD_BENCH_COMPONENTS=<prefix>. On a Housing component placed 30 mm up (a box in it), an empty Lid and two boxes at the
// root: New component makes "Component 1", activates it and opens its rename in the browser with a ticked Activate box
// under the name (taking no focus): a click on it activates the root again and turns Activate new components off, another
// turns both back (remembered for the document), the typed name is one step more and the box goes with the editor; with
// Activate new components off the next one goes into the active one, nothing else is activated, its box unticked. Component
// from selection (Ctrl+G) on the two root boxes is one undo step, puts them into a new component at the root where they
// were, opens its rename; on a box of the Housing and a root box it adds a transform for the Housing's box only (both stay
// put); a locked box makes it refuse and take everything back. Move to component… lists the root (current, not chosen)
// and every component, narrows to the Lid as "lid" is typed, Enter moves the box (one step); into the Housing it keeps
// the box where it is; a component's own entry is not offered to itself; nothing matching says so; Esc closes. A box
// dropped on the Housing in the browser, and back to the root, keeps its place too (a transform in the same step). Opacity:
// the right-click menu's slider dragged to 40 % shows it live (the document unchanged, no op), let go writes one step
// for the bodies under the Housing; the Opacity command's popup writes once the keys rest, Esc drops what is pending.
// The Design menu has Component from selection, Design > Assemble drops it down from New component. <prefix>.rename.png
// (the browser), .picker.png, .menu.png, .popup.png, .ribbon.png.
OPAD_BENCH(OPAD_BENCH_COMPONENTS, components) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: components: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  struct State {
    std::string housing, lid, boxA, boxB, boxC, made;
    size_t ops = 0, steps = 0;
    opad::Mat4 worldA, worldB, worldC;
    std::shared_ptr<QMenu> menu;  // the right-click menu's entries, kept while its slider is dragged
    QPointer<OpacitySlider> slider;
  };
  auto s = std::make_shared<State>();
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  BrowserTree* tree = w.m_browser->tree();
  const QString prefix = value;
  auto idle = [v, &w] { return !v->looksPending() && !w.m_displayJob && w.m_meshRemaining == 0; };
  auto mark = [doc, s] {
    s->ops = doc->doc.ops.size();
    s->steps = doc->undoLabels().size();
  };
  auto oneStep = [doc, s] { return doc->undoLabels().size() == s->steps + 1; };
  auto componentNamed = [](int n) { return QCoreApplication::translate("Components", "Component %1").arg(n); };
  auto byName = [doc](const QString& name) {
    for (const auto& [id, n] : doc->scene.nodes)
      if (QString::fromStdString(n.name) == name) return id;
    return std::string();
  };
  auto picker = [&w] { return w.findChild<ComponentPicker*>(); };
  auto toggle = [&w] { return w.findChild<ActivateToggle*>(); };
  auto steps = std::make_shared<std::vector<Step>>();
  auto& list = *steps;
  if (doc->scene.all_bodies().size() > 16) {
    // A big assembly (the Engine): Move to component… over all its components and the opacity slider on a component with
    // about half of the bodies, timed; the event loop never held over 250 ms while the slider is dragged.
    for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
      if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
    auto ticker = std::make_shared<Ticker>();
    auto settled = [&w, v, doc, idle] {
      int expected = 0;
      for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
      return idle() && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0;
    };
    list.push_back({settled, [=, &w](bool shown) {
                      const size_t total = doc->scene.all_bodies().size();
                      size_t best = 0, components = 0;
                      for (const auto& [id, n] : doc->scene.nodes)
                        if (n.kind == opad::Node::Kind::Component) {
                          ++components;
                          const size_t in = doc->scene.bodies_under(id).size();
                          if (!n.parent.empty() && std::min(in, total - in) > best) best = std::min(in, total - in), s->housing = id;
                        }
                      require(shown && !s->housing.empty(), QString("%1 bodies displayed, %2 components; %3 holds %4 bodies").arg(v->displayedCount()).arg(components).arg(doc->nodeName(s->housing)).arg(doc->scene.bodies_under(s->housing).size()));
                      s->boxA = doc->scene.bodies_under(s->housing).front();
                      w.m_browser->selectIds({s->boxA});
                      QElapsedTimer clock;
                      clock.start();
                      w.action("design.reparent")->trigger();
                      const qint64 first = clock.elapsed();
                      ComponentPicker* p = picker();
                      if (p) key(p->filter(), Qt::Key_Escape);
                      clock.restart();
                      w.action("design.reparent")->trigger();  // again: the window is made
                      const qint64 again = clock.elapsed();
                      qint64 slowest = 0;
                      const QString letters = doc->nodeName(s->housing).left(4);
                      for (const QChar c : letters) {
                        clock.restart();
                        if (p) type(p->filter(), QString(c));
                        slowest = std::max(slowest, clock.elapsed());
                      }
                      const int listed = p ? p->shown() : -1;
                      if (p) key(p->filter(), Qt::Key_Escape);
                      require(p && first < 250 && again < 100 && slowest < 50 && listed > 0 && !p->isVisible(),
                              QString("Move to component… over %1 components opened in %2 ms (again in %3 ms); each letter of '%4' %5 ms at most, %6 left")
                                  .arg(components).arg(first).arg(again).arg(letters).arg(slowest).arg(listed));
                      SelectionContext context;
                      context.ids = {s->housing};
                      s->menu = std::make_shared<QMenu>();
                      for (AreaController* area : w.m_areas) area->contextMenu(context, *s->menu);
                      for (QAction* a : s->menu->actions())
                        if (auto* action = qobject_cast<QWidgetAction*>(a)) s->slider = qobject_cast<OpacitySlider*>(action->defaultWidget());
                      mark();
                      ticker->start();
                      if (s->slider) {
                        s->slider->slider()->setSliderDown(true);
                        s->slider->slider()->setValue(50);
                      }
                    }, 220000});
    list.push_back({[=] { return !v->looksPending() && std::abs(v->benchLookState(s->boxA).value("transparency", 0.0) - 0.5) < 1e-6; }, [=](bool live) {
                      ticker->timer.stop();
                      require(live && ticker->worst < 250 && doc->doc.ops.size() == s->ops,
                              QString("the slider dragged to 50 %: %1 bodies drawn at it, nothing written, worst event-loop gap %2 ms").arg(doc->scene.bodies_under(s->housing).size()).arg(ticker->worst));
                      QElapsedTimer clock;
                      clock.start();
                      if (s->slider) s->slider->slider()->setSliderDown(false);
                      const qint64 written = clock.elapsed();
                      const bool step = oneStep();
                      clock.restart();
                      doc->run("appearance", {{"target", s->boxA}, {"visible", true}});  // one op, for comparison
                      const qint64 single = clock.elapsed();
                      require(step && std::abs(doc->scene.node(s->boxA)->opacity - 0.5) < 1e-9,
                              QString("let go: written for %1 bodies in %2 ms (one appearance op alone takes %3 ms: the document is resolved again either way)")
                                  .arg(doc->scene.bodies_under(s->housing).size()).arg(written).arg(single));
                      if (s->menu) emit s->menu->aboutToHide();
                      s->menu.reset();
                    }, 60000});
    runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
    return true;
  }
  list.push_back({{}, [=](bool) {
                    s->housing = doc->run("component", {{"name", "Housing"}}).value("id", "");
                    doc->run("transform", {{"target", s->housing}, {"matrix", opad::Mat4::translation(0, 0, 30).to_json()}});
                    s->lid = doc->run("component", {{"name", "Lid"}}).value("id", "");
                    doc->run("feature", {{"kind", "box"}, {"component", s->housing}, {"inputs", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}});
                    doc->run("feature", {{"kind", "box"}, {"inputs", {{"x", "40 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}}});
                    doc->run("feature", {{"kind", "box"}, {"inputs", {{"x", "60 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "5 mm"}}}});
                  }});
  // New component: named, activated, renamed in place.
  list.push_back({[=] { return idle() && doc->scene.all_bodies().size() == 3 && v->displayedCount() == 3; }, [=, &w](bool shown) {
                    for (const auto& b : doc->scene.all_bodies()) {
                      const opad::Node* n = doc->scene.node(b);
                      (n->parent == s->housing ? s->boxA : n->local.is_identity() && s->boxB.empty() ? s->boxB : s->boxC) = b;
                    }
                    s->worldA = doc->scene.world(s->boxA), s->worldB = doc->scene.world(s->boxB), s->worldC = doc->scene.world(s->boxC);
                    require(shown && !s->boxA.empty() && !s->boxB.empty() && !s->boxC.empty() && w.action("assembly.activateNew")->isChecked(),
                            "a box in the Housing (30 mm up), two at the root; Activate new components on");
                    mark();
                    w.action("design.newcomponent")->trigger();
                    s->made = byName(componentNamed(1));
                    const opad::Node* made = doc->scene.node(s->made);
                    QLineEdit* editor = renaming(tree, s->made);
                    require(made && made->kind == opad::Node::Kind::Component && made->parent.empty() && oneStep() && doc->activeComponent() == s->made && editor,
                            "New component: 'Component 1' at the root, one step, active, its name being edited in the browser");
                    ActivateToggle* box = toggle();
                    const QRect under = box && editor ? box->geometry() : QRect(), name = editor ? editor->geometry() : QRect();
                    require(box && box->isVisibleTo(tree) && box->parentWidget() == tree->viewport() && box->box()->isChecked() && box->box()->focusPolicy() == Qt::NoFocus &&
                                under.top() > name.bottom() && std::abs(under.right() - name.right()) <= 1,
                            QString("an Activate box, ticked, under the name's end (%1,%2 under %3,%4), taking no focus").arg(under.right()).arg(under.top()).arg(name.right()).arg(name.bottom()));
                    w.m_browser->grab().save(prefix + ".rename.png");
                    if (box) click(box->box());
                    require(box && !box->box()->isChecked() && doc->activeComponent().empty() && !w.action("assembly.activateNew")->isChecked() &&
                                !QSettings().value("assembly/activateNew", true).toBool() && doc->rememberedComponent().empty() && renaming(tree, s->made) == editor && box->isVisibleTo(tree),
                            "a click on it: the root active again, Activate new components off (and its setting), the name still being edited");
                    if (box) click(box->box());
                    require(box && box->box()->isChecked() && doc->activeComponent() == s->made && w.action("assembly.activateNew")->isChecked() &&
                                QSettings().value("assembly/activateNew", false).toBool() && doc->rememberedComponent() == s->made && renaming(tree, s->made) == editor && doc->undoLabels().size() == s->steps + 1,
                            "another click: active again (remembered for the document), Activate new components on; no step for either");
                    if (editor) {
                      editor->setText("Bracket");
                      key(editor, Qt::Key_Return);  // the delegate writes it a moment later
                    }
                  }});
  list.push_back({[=] { return doc->nodeName(s->made) == "Bracket"; }, [=, &w](bool named) {
                    require(named && doc->undoLabels().size() == s->steps + 2 && !renaming(tree, s->made) && toggle() && !toggle()->isVisibleTo(tree), "the typed name is written (one step more), the editor closes and the Activate box with it");
                    w.action("assembly.activateNew")->trigger();  // off
                    mark();
                    w.action("design.newcomponent")->trigger();
                    const std::string inner = byName(componentNamed(1));
                    const opad::Node* n = doc->scene.node(inner);
                    QLineEdit* second = renaming(tree, inner);
                    require(n && n->parent == s->made && doc->activeComponent() == s->made && oneStep() && second && toggle()->isVisibleTo(tree) && !toggle()->box()->isChecked(),
                            "Activate new components off: the next one goes into the active Bracket, which stays active; its rename opens, Activate unticked");
                    if (second) key(second, Qt::Key_Escape);
                    require(doc->nodeName(inner) == componentNamed(1) && !renaming(tree, inner) && !toggle()->isVisibleTo(tree), "Esc keeps the name; the Activate box goes");
                    w.action("assembly.activateNew")->trigger();  // on again
                    w.action("assembly.activateRoot")->trigger();
                    w.m_browser->selectIds({});
                  }});
  // Component from selection.
  list.push_back({idle, [=, &w](bool) {
                    QAction* group = w.action("design.componentFromSelection");
                    w.m_browser->selectIds({s->boxB, s->boxC});
                    const bool offered = group->isEnabled() && group->shortcut() == QKeySequence("Ctrl+G");
                    mark();
                    group->trigger();
                    const std::string made = doc->scene.node(s->boxB)->parent;
                    const opad::Node* g = doc->scene.node(made);
                    require(offered && g && g->parent.empty() && doc->scene.node(s->boxC)->parent == made && oneStep() &&
                                doc->undoLabel() == QCoreApplication::translate("Components", "component from selection") && doc->activeComponent().empty(),
                            QString("Ctrl+G on the two root boxes: a new component '%1' at the root holds them, one undo step, nothing activated").arg(g ? QString::fromStdString(g->name) : QString()));
                    require(samePlace(doc->scene.world(s->boxB), s->worldB) && samePlace(doc->scene.world(s->boxC), s->worldC) && doc->doc.ops.size() == s->ops + 3 && renaming(tree, made),
                            "both stay where they were (no transform), and the new component's name is being edited");
                    if (QLineEdit* editor = renaming(tree, made)) key(editor, Qt::Key_Escape);
                    doc->undo();
                    const bool back = !doc->scene.node(made) && doc->scene.node(s->boxB)->parent.empty() && doc->doc.ops.size() == s->ops;
                    doc->redo();
                    require(back && doc->scene.node(s->boxB)->parent == made, "one undo takes it all back, redo brings it again");
                    doc->undo();
                    // Across components: the Housing is placed 30 mm up, so its box needs a transform to stay put.
                    w.m_browser->selectIds({s->boxA, s->boxB});
                    mark();
                    group->trigger();
                    const std::string across = doc->scene.node(s->boxA)->parent;
                    const opad::Node* a = doc->scene.node(across);
                    int transforms = 0;
                    for (size_t i = s->ops; i < doc->doc.ops.size(); ++i) transforms += doc->doc.ops[i].type == "transform";
                    require(a && a->parent.empty() && doc->scene.node(s->boxB)->parent == across && oneStep() && transforms == 1 &&
                                samePlace(doc->scene.world(s->boxA), s->worldA) && samePlace(doc->scene.world(s->boxB), s->worldB),
                            QString("a Housing box and a root box: a new component at the root (their common parent), one step, %1 transform (the Housing's box), both in place").arg(transforms));
                    if (QLineEdit* editor = renaming(tree, across)) key(editor, Qt::Key_Escape);
                    doc->undo();
                    // A locked box: refused, nothing kept.
                    doc->run("appearance", {{"target", s->boxB}, {"locked", true}});
                    w.m_browser->selectIds({s->boxB, s->boxC});
                    mark();
                    group->trigger();  // the refusal is a message box (dismissed by the bench)
                    require(doc->doc.ops.size() == s->ops && doc->undoLabels().size() == s->steps && doc->scene.node(s->boxB)->parent.empty() && doc->scene.node(s->boxC)->parent.empty(),
                            "with a locked box it is refused and takes the new component back");
                    doc->undo();  // the lock
                    w.m_browser->selectIds({s->boxC});
                    mark();
                    w.action("design.reparent")->trigger();
                  }});
  // Move to component…: the list over the view.
  list.push_back({[=] { return picker() && picker()->isVisible(); }, [=, &w](bool open) {
                    ComponentPicker* p = picker();
                    size_t components = 0;
                    for (const auto& [id, n] : doc->scene.nodes) components += n.kind == opad::Node::Kind::Component;
                    QListWidget* rows = p ? p->list() : nullptr;
                    const bool rootCurrent = rows && rows->count() > 0 && !(rows->item(0)->flags() & Qt::ItemIsEnabled);
                    require(open && p->shown() == static_cast<int>(components + 1) && rootCurrent && rows->currentRow() == 1,
                            QString("Move to component… lists the root (where the box is: not chosen) and all %1 components, the first other one current").arg(components));
                    if (p) p->grab().save(prefix + ".picker.png");
                    if (p) type(p->filter(), "lid");
                    require(p && p->shown() == 1 && p->list()->currentItem() && p->list()->currentItem()->text() == "Lid", "typing 'lid' leaves the Lid, current");
                    if (p) key(p->filter(), Qt::Key_Return);
                    require(p && !p->isVisible() && doc->scene.node(s->boxC)->parent == s->lid && oneStep() && doc->undoLabel() == AppDocument::tr("move") && samePlace(doc->scene.world(s->boxC), s->worldC),
                            "Enter moves the box into the Lid: one step, where it was; the list closes");
                    w.action("design.reparent")->trigger();
                  }});
  list.push_back({[=] { return picker() && picker()->isVisible(); }, [=, &w](bool open) {
                    ComponentPicker* p = picker();
                    const bool fresh = open && p->filter()->text().isEmpty();
                    mark();
                    type(p->filter(), "hous");
                    key(p->filter(), Qt::Key_Return);
                    const opad::Node* c = doc->scene.node(s->boxC);
                    require(fresh && c->parent == s->housing && oneStep() && samePlace(doc->scene.world(s->boxC), s->worldC) && !samePlace(c->local, opad::Mat4{}),
                            "into the Housing (30 mm up) it stays where it is: its own placement written in the same step");
                    // The browser's drop (BrowserTree::dropEvent asks for it): onto the Housing, then back to the root's top.
                    mark();
                    emit tree->reparentRequested({s->boxB}, s->housing, -1);
                    const opad::Node* b = doc->scene.node(s->boxB);
                    require(b->parent == s->housing && oneStep() && doc->doc.ops.size() == s->ops + 2 && doc->doc.ops.back().type == "transform" && samePlace(doc->scene.world(s->boxB), s->worldB),
                            "a box dropped on the Housing in the browser stays where it is: a reparent and its placement, one step");
                    mark();
                    emit tree->reparentRequested({s->boxB}, std::string(), 0);
                    b = doc->scene.node(s->boxB);
                    require(b->parent.empty() && doc->scene.roots.front() == s->boxB && oneStep() && doc->doc.ops.size() == s->ops + 2 && samePlace(doc->scene.world(s->boxB), s->worldB) && b->local.is_identity(1e-9),
                            "dropped back at the top of the root: in place again, its placement the identity");
                    w.m_browser->selectIds({s->housing});
                    w.action("design.reparent")->trigger();
                    bool self = false;
                    for (int i = 0; i < p->list()->count(); ++i) self = self || p->list()->item(i)->text() == "Housing";
                    type(p->filter(), "zzz");
                    const bool none = p->shown() == 0 && p->list()->count() == 1;
                    key(p->filter(), Qt::Key_Escape);
                    require(!self && none && !p->isVisible(), "the Housing is not offered to itself; nothing matching says so; Esc closes the list");
                  }});
  // Opacity: the right-click menu's slider, then the command's popup.
  list.push_back({idle, [=, &w](bool) {
                    SelectionContext context;
                    context.ids = {s->housing};
                    s->menu = std::make_shared<QMenu>();
                    QMenu& menu = *s->menu;
                    for (AreaController* area : w.m_areas) area->contextMenu(context, menu);
                    QWidgetAction* action = nullptr;
                    QStringList entries;
                    for (QAction* a : menu.actions()) {
                      entries << a->objectName();
                      if (a->objectName() == "opacitySliderAction") action = qobject_cast<QWidgetAction*>(a);
                    }
                    auto* slider = action ? qobject_cast<OpacitySlider*>(action->defaultWidget()) : nullptr;
                    require(slider && entries.contains("design.reparent") && entries.contains("design.componentFromSelection"),
                            "the right-click menu has the opacity slider, Move to component… and Component from selection: " + entries.join(' '));
                    if (!slider) return;
                    menu.adjustSize();
                    menu.grab().save(prefix + ".menu.png");
                    mark();
                    s->slider = slider;
                    slider->slider()->setSliderDown(true);  // dragged
                    slider->slider()->setValue(40);
                  }});
  list.push_back({[=] { return !v->looksPending() && std::abs(v->benchLookState(s->boxA).value("transparency", 0.0) - 0.6) < 1e-6; }, [=, &w](bool live) {
                    require(live && doc->scene.node(s->boxA)->opacity == 1.0 && doc->doc.ops.size() == s->ops,
                            "dragging the slider to 40 % draws the Housing's box at 40 % at once; nothing written yet");
                    OpacitySlider* slider = s->slider;
                    if (!slider) return;
                    slider->slider()->setSliderDown(false);  // let go
                    const auto under = doc->scene.bodies_under(s->housing);
                    require(under.size() == 2 && std::all_of(under.begin(), under.end(), [doc](const std::string& b) { return std::abs(doc->scene.node(b)->opacity - 0.4) < 1e-9; }) &&
                                oneStep() && doc->doc.ops.size() == s->ops + under.size(),
                            "let go: one appearance step for the two bodies under the Housing (its box and the one moved into it)");
                    emit s->menu->aboutToHide();  // the menu goes
                    s->menu.reset();
                    w.m_browser->selectIds({s->boxB});
                    mark();
                    w.action("design.opacity")->trigger();
                  }});
  list.push_back({[=, &w] { return w.findChild<OpacityPopup*>() != nullptr; }, [=, &w](bool open) {
                    OpacityPopup* popup = w.findChild<OpacityPopup*>();
                    require(open && popup->isVisible() && std::abs(popup->slider()->value() - 1.0) < 1e-9, "the Opacity command opens its slider over the view at the box's opacity");
                    popup->grab().save(prefix + ".popup.png");
                    key(popup->slider()->slider(), Qt::Key_PageDown);  // 90 %
                  }});
  list.push_back({[=] { return doc->doc.ops.size() > s->ops; }, [=, &w](bool written) {
                    OpacityPopup* popup = w.findChild<OpacityPopup*>();
                    require(written && std::abs(doc->scene.node(s->boxB)->opacity - 0.9) < 1e-9 && oneStep() && popup && popup->isVisible(),
                            "a key changes it and once it rests it is written, one step; the popup stays");
                    if (!popup) return;
                    mark();
                    key(popup->slider()->slider(), Qt::Key_PageDown);  // 80 %, not written yet
                    key(popup, Qt::Key_Escape);
                  }});
  list.push_back({[=, &w] { return !w.findChild<OpacityPopup*>() && !v->looksPending(); }, [=, &w](bool closed) {
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    require(closed && doc->doc.ops.size() == s->ops && std::abs(doc->scene.node(s->boxB)->opacity - 0.9) < 1e-9 &&
                                std::abs(v->benchLookState(s->boxB).value("transparency", 0.0) - 0.1) < 1e-6,
                            "Esc drops what was pending: nothing written, drawn at 90 % again");
                    // The Design menu (after New component) and Design > Assemble.
                    QMenu* design = nullptr;
                    for (QAction* a : w.menuBar()->actions())
                      if (a->menu() && a->menu()->actions().contains(w.action("design.newcomponent"))) design = a->menu();
                    const QList<QAction*> items = design ? design->actions() : QList<QAction*>();
                    const qsizetype at = items.indexOf(w.action("design.newcomponent"));
                    require(at >= 0 && at + 2 < items.size() && items[at + 1] == w.action("design.componentFromSelection") && items[at + 2] == w.action("assembly.activateNew") &&
                                w.action("design.reparent")->text() == QCoreApplication::translate("Components", "Move to component…"),
                            "the Design menu: New component, Component from selection, Activate new components; Reparent is Move to component…");
                    w.setWorkspace("design");
                    w.m_ribbon->setCurrentTab(int(w.m_ribbon->tabIds().indexOf("design.assemble")));
                    bool dropped = false;
                    if (RibbonPage* page = w.m_ribbon->page("design.assemble"))
                      for (QToolButton* b : page->findChildren<QToolButton*>())
                        dropped = dropped || (b->defaultAction() == w.action("design.newcomponent") && b->menu() && b->menu()->actions().contains(w.action("design.componentFromSelection")));
                    w.m_ribbon->grab().save(prefix + ".ribbon.png");
                    require(dropped, "Design > Assemble: Component from selection drops down from New component");
                    w.m_browser->selectIds({});
                  }});
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
