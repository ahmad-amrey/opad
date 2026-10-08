// OPAD_BENCH_LOCK: Lock (UI-37, app/LockArea.cpp) in the running app. Cases in tools/bench_cases/assembly.py; the
// composition is tests/test_body_look, the refusals and the DXF layer flags are core's (test_design, test_dxf).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidgetItemIterator>

#include <Bnd_Box.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
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

QTreeWidgetItem* rowOf(BrowserTree* tree, const std::string& id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, browser::kIdRole).toString().toStdString() == id && (*it)->data(0, Qt::UserRole).toString() != "document") return *it;
  return nullptr;
}

// The lock badge of a browser row (null if it has none), its rect in `rect`.
const browser::Badge* lockBadge(BrowserTree* tree, const std::string& id, browser::Decoration& d, QRect* rect = nullptr) {
  QTreeWidgetItem* row = rowOf(tree, id);
  auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
  if (!row || !delegate) return nullptr;
  tree->scrollToItem(row);
  const QModelIndex index = tree->indexFromItem(row);
  d = delegate->decoration(index);
  const QRect r = tree->visualRect(index);
  for (int x = r.right(); x > r.left() + browser::kNameX; --x)
    if (const browser::Badge* b = delegate->badgeAt(d, index, r, QPoint(x, r.center().y()), rect); b && b->icon == "lock") return b;
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

void click(QWidget* widget, const QPoint& at) {
  for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
    QMouseEvent e(type, QPointF(at), QPointF(widget->mapToGlobal(at)), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &e);
  }
}
}  // namespace

// OPAD_BENCH_LOCK=<prefix>. On a document of its own (a box in a Housing component, a box at the root): Lock on the root
// box is one step and turns the command into Unlock; the box is faded (half its opacity) and not picked (a click passes
// it), the other still is; its browser row has a lock badge; resting on it names it in the status bar, the right-click
// menu there offers Unlock; a guided tool picks it as a reference and afterwards it is not picked again; Unlock from that
// menu brings it back. The Housing locked: its box faded too, its row's badge dim and naming the Housing, a click on that
// badge unlocks the Housing; Unlock on the box alone frees the Housing and says so. On a drawing (a DXF with a locked
// layer, opened in viewer mode): the locked layer's lines faded towards the background and not picked, the other layer's
// picked, the badge on the layer row unlocks it. <prefix>.view.png and <prefix>.browser.png with the box locked
// (<prefix>.drawing.png for the drawing).
OPAD_BENCH(OPAD_BENCH_LOCK, lock) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: lock: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  struct State {
    std::string housing, boxA, boxB, walls, plain;
    int ax = 0, ay = 0, bx = 0, by = 0;
    size_t ops = 0;
    QString refused;  // what a feature planned on the worker said
  };
  auto s = std::make_shared<State>();
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  const QString prefix = value;
  auto idle = [v, &w] { return !v->looksPending() && !w.m_displayJob && w.m_meshRemaining == 0; };
  auto mouse = [v](QEvent::Type type, int x, int y, Qt::MouseButton button) {
    const QPointF at(x / v->displayScale(), y / v->displayScale());
    QMouseEvent e(type, at, v->mapToGlobal(at), button, type == QEvent::MouseButtonRelease || type == QEvent::MouseMove ? Qt::NoButton : button, Qt::NoModifier);
    QCoreApplication::sendEvent(v, &e);
  };
  auto rightClickMenu = [v, mouse, &w](int x, int y) {  // the areas' entries of the menu a right-click there opens
    {
      const QSignalBlocker quiet(v);  // the window's menu is not opened (exec), its entries are asked for below
      mouse(QEvent::MouseButtonPress, x, y, Qt::RightButton);
      mouse(QEvent::MouseButtonRelease, x, y, Qt::RightButton);
    }
    auto menu = std::make_shared<QMenu>();
    for (AreaController* area : w.m_areas) area->contextMenu(SelectionContext(), *menu);
    return menu;
  };
  auto unlockAt = [](QMenu& menu) {
    for (QAction* a : menu.actions())
      if (a->objectName() == "lockUnlockAt") return a;
    return static_cast<QAction*>(nullptr);
  };
  QAction* lock = w.action("design.lock");
  auto steps = std::make_shared<std::vector<Step>>();
  auto& list = *steps;
  const auto bodies = doc->scene.all_bodies();
  if (bodies.size() > 16) {
    // A big assembly (the Engine): a component holding about half of the bodies locked and unlocked, each with no
    // event-loop gap over 250 ms (the Lock layer applied by the sliced look job); a locked body's hover pick under 50 ms.
    for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
      if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
    auto ticker = std::make_shared<Ticker>();
    auto settled = [v, doc, idle] {
      int expected = 0;
      for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
      return idle() && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0;
    };
    list.push_back({settled, [=, &w](bool shown) {
                      const size_t total = doc->scene.all_bodies().size();
                      size_t best = 0;
                      for (const auto& [id, n] : doc->scene.nodes)
                        if (n.kind == opad::Node::Kind::Component && !n.parent.empty()) {
                          const size_t in = doc->scene.bodies_under(id).size();
                          if (std::min(in, total - in) > best) best = std::min(in, total - in), s->housing = id;
                        }
                      require(shown && !s->housing.empty(), QString("%1 bodies displayed; %2 holds %3").arg(v->displayedCount()).arg(doc->nodeName(s->housing)).arg(best));
                      w.m_browser->selectIds({s->housing});
                      ticker->start();
                      QElapsedTimer clock;
                      clock.start();
                      lock->trigger();
                      s->ax = static_cast<int>(clock.elapsed());
                    }, 220000});
    auto faded = [doc, v, s](bool on) {
      const auto under = doc->scene.bodies_under(s->housing);
      return !v->looksPending() && std::all_of(under.begin(), under.end(), [&](const std::string& b) {
        const auto state = v->benchLookState(b);
        return state.empty() || (state.value("activated", -1) == 0) == on;
      });
    };
    list.push_back({[=] { return faded(true); }, [=, &w](bool done) {
                      ticker->timer.stop();
                      require(done && ticker->worst < 250 && doc->scene.node(s->housing)->locked,
                              QString("locked in %1 ms, its bodies faded and not picked, worst event-loop gap %2 ms").arg(s->ax).arg(ticker->worst));
                      v->fitNodes({s->housing});
                      v->benchFlush();  // the frame that fits the picker's depth range to the view
                      qint64 slowest = 0;
                      int found = 0;
                      for (int j = 1; j <= 5; ++j)
                        for (int i = 1; i <= 5; ++i) {
                          QElapsedTimer pick;
                          pick.start();
                          const std::string at = v->drawnAt(QPointF(v->width() * i / 6.0, v->height() * j / 6.0));
                          found += !at.empty() && doc->scene.effectively_locked(at);
                          slowest = std::max(slowest, pick.elapsed());
                        }
                      require(slowest < 50, QString("a locked body's hover pick, framed: slowest of 25 %1 ms (%2 on a locked body, the rest in front of it)").arg(slowest).arg(found));
                      ticker->start();
                      QElapsedTimer clock;
                      clock.start();
                      lock->trigger();  // Unlock: the component is still selected
                      s->ax = static_cast<int>(clock.elapsed());
                      w.m_browser->selectIds({});
                    }, 60000});
    list.push_back({[=] { return faded(false); }, [=](bool done) {
                      ticker->timer.stop();
                      require(done && ticker->worst < 250 && !doc->scene.node(s->housing)->locked,
                              QString("unlocked in %1 ms, picked again, worst event-loop gap %2 ms").arg(s->ax).arg(ticker->worst));
                    }, 60000});
    runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
    return true;
  }
  if (!bodies.empty() && std::all_of(bodies.begin(), bodies.end(), [doc](const std::string& id) { return doc->scene.node(id)->representation == "drawing2d"; })) {
    // A drawing with a locked layer (Walls) and a plain one, viewed: the layer's state from the file.
    list.push_back({[=] { return idle() && v->displayedCount() == static_cast<int>(doc->scene.all_bodies().size()); }, [=, &w](bool shown) {
                      for (const auto& [id, n] : doc->scene.nodes)
                        if (n.layer.is_object()) (n.name == "Walls" ? s->walls : s->plain) = id;
                      const auto wallBodies = doc->scene.bodies_under(s->walls), plainBodies = doc->scene.bodies_under(s->plain);
                      require(shown && doc->browse && !wallBodies.empty() && !plainBodies.empty() && doc->scene.node(s->walls)->locked,
                              "a drawing in viewer mode with its Walls layer locked by the file and a plain layer");
                      if (wallBodies.empty() || plainBodies.empty()) return;
                      v->standardView("top");
                      v->fitAll();
                      const std::string wb = wallBodies.front(), pb = plainBodies.front();
                      auto middle = [doc, v](const std::string& id, int& x, int& y) {  // of the line, where it is drawn (the drawing is centred)
                        double x0, y0, z0, x1, y1, z1;
                        opad::node_world_bbox(doc->doc, doc->scene, id).Get(x0, y0, z0, x1, y1, z1);
                        const QPoint at = v->widgetPoint({(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2});
                        x = qRound(at.x() * v->displayScale()), y = qRound(at.y() * v->displayScale());
                      };
                      middle(wb, s->ax, s->ay);
                      middle(pb, s->bx, s->by);
                      const auto state = v->benchLookState(wb);
                      const QColor bg = v->tokens().vp;
                      const auto want = looks::mix(doc->scene.node(wb)->color, {bg.redF(), bg.greenF(), bg.blueF()}, 0.5);
                      bool towards = state.contains("color");
                      for (int i = 0; i < 3 && towards; ++i) towards = std::abs(state["color"][i].get<double>() - want[i]) < 2e-3;
                      require(towards && state.value("activated", -1) == 0 && v->benchLookState(pb).value("activated", 0) > 0,
                              "the locked layer's lines are faded towards the background and not picked, the plain layer's are: " + QString::fromStdString(state.dump()));
                      require(v->benchPickAt(s->ax, s->ay) != wb && v->benchPickAt(s->bx, s->by) == pb, "picking passes the locked lines and finds the plain ones");
                      v->grabImage().save(prefix + ".drawing.png");
                      browser::Decoration d;
                      QRect rect;
                      const browser::Badge* badge = lockBadge(w.m_browser->tree(), s->walls, d, &rect);
                      require(badge && badge->color == &Tokens::locked && badge->clicked, "the Walls layer's row has a lock badge");
                      if (badge) click(w.m_browser->tree()->viewport(), rect.center());
                      require(doc->scene.node(s->walls) && !doc->scene.node(s->walls)->locked && !doc->isDirty(), "a click on the badge unlocks the layer (a view change in viewer mode)");
                    }});
    list.push_back({[=] { return idle() && v->benchLookState(doc->scene.bodies_under(s->walls).front()).value("activated", 0) > 0; }, [=](bool picked) {
                      require(picked && v->benchPickAt(s->ax, s->ay) == doc->scene.bodies_under(s->walls).front(), "unlocked, the Walls lines are picked again");
                    }});
    runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
    return true;
  }
  // The document, made through the command layer: a box in a Housing component and a box at the root.
  list.push_back({{}, [=](bool) {
                    s->housing = doc->run("component", {{"name", "Housing"}}).value("id", "");
                    doc->run("feature", {{"kind", "box"}, {"component", s->housing}, {"inputs", {{"x", "15 mm"}, {"y", "10 mm"}, {"length", "30 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}});
                    doc->run("feature", {{"kind", "box"}, {"inputs", {{"x", "50 mm"}, {"y", "10 mm"}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "5 mm"}}}});
                  }});
  list.push_back({[=] { return idle() && doc->scene.all_bodies().size() == 2 && v->displayedCount() == 2; }, [=, &w](bool shown) {
                    for (const auto& b : doc->scene.all_bodies()) (doc->scene.node(b)->parent == s->housing ? s->boxA : s->boxB) = b;
                    v->fitAll();
                    const bool points = v->benchBodyPoint(s->boxA, s->ax, s->ay) && v->benchBodyPoint(s->boxB, s->bx, s->by);
                    require(shown && !s->boxA.empty() && !s->boxB.empty() && points, "a box in the Housing and one at the root, both picked");
                    w.m_browser->selectIds({s->boxB});
                    const bool offered = lock->isEnabled() && lock->data().toString() == "lock";
                    s->ops = doc->doc.ops.size();
                    const QStringList steps = doc->undoLabels();
                    lock->trigger();
                    require(offered && doc->scene.node(s->boxB)->locked && doc->doc.ops.size() == s->ops + 1 && doc->undoLabels().size() == steps.size() + 1 &&
                                doc->undoLabel() == AppDocument::tr("lock") && lock->data().toString() == "unlock",
                            "Lock on the selected box: one op, one undo step, the command says Unlock now: " + lock->text());
                    w.m_browser->selectIds({});
                    v->clearSelection();
                  }});
  list.push_back({idle, [=, &w](bool) {
                    const auto b = v->benchLookState(s->boxB), a = v->benchLookState(s->boxA);
                    require(std::abs(b.value("transparency", 0.0) - 0.5) < 1e-6 && b.value("activated", -1) == 0 && b.value("displayed", false) && !v->shownLook(s->boxB).ghost,
                            "the locked box is drawn at half its opacity and not pickable: " + QString::fromStdString(b.dump()));
                    require(a.value("transparency", 1.0) == 0.0 && a.value("activated", 0) > 0, "the other box is drawn and picked as it is");
                    require(v->benchPickAt(s->bx, s->by) != s->boxB && v->benchPickAt(s->ax, s->ay) == s->boxA && v->hoverName(s->boxB).endsWith(Viewport::tr(" (locked)")),
                            "picking passes the locked box, finds the other; a locked body's hover name says so");
                    browser::Decoration d, other;
                    const browser::Badge* badge = lockBadge(w.m_browser->tree(), s->boxB, d);
                    const browser::Badge* none = lockBadge(w.m_browser->tree(), s->boxA, other);
                    require(badge && badge->color == &Tokens::locked && badge->clicked && !badge->tooltip.isEmpty() && !none, "the locked box's browser row has a lock badge, the other box's none");
                    v->grabImage().save(prefix + ".view.png");
                    w.m_browser->grab().save(prefix + ".browser.png");
                    v->benchClickAt(s->ax, s->ay);  // the positive control: a click on the other box selects it
                  }});
  auto picked = [v](const std::string& id) {
    const auto refs = v->selection();
    return std::any_of(refs.begin(), refs.end(), [&](const opad::Ref& r) { return r.body == id; });
  };
  list.push_back({[=] { return picked(s->boxA); }, [=](bool selected) {
                    require(selected, "a click on the other box selects it");
                    v->clearSelection();
                    v->benchClickAt(s->bx, s->by);
                  }});
  list.push_back({{}, [=](bool) {
                    require(!picked(s->boxB), "a click on the locked box does not select it");
                    v->clearSelection();
                    mouse(QEvent::MouseMove, s->bx, s->by, Qt::NoButton);
                  }});
  auto hinted = [doc, s] { return QCoreApplication::translate("Lock", "%1 is locked · right-click to unlock").arg(doc->nodeName(s->boxB)); };
  list.push_back({[=, &w] { return w.m_statusHover->text() == hinted(); }, [=, &w](bool named) {
                    const QString hint = w.m_statusHover->text();
                    require(named, "resting on the locked box names it and how to unlock it: " + hint);
                    mouse(QEvent::MouseMove, s->ax, s->ay, Qt::NoButton);
                  }, 3000});
  list.push_back({[=, &w] { return w.m_statusHover->text() != hinted(); }, [=, &w](bool cleared) {
                    require(cleared, "on the other box the hint goes: " + w.m_statusHover->text());
                    auto menu = rightClickMenu(s->bx, s->by);
                    QAction* unlock = unlockAt(*menu);
                    require(unlock && unlock->text().contains(doc->nodeName(s->boxB)), "the right-click menu on the locked box offers to unlock it");
                    auto none = rightClickMenu(s->ax, s->ay);
                    require(!unlockAt(*none), "the right-click menu on the other box does not");
                    w.startTool("distance");
                  }});
  // References: a guided tool picks the locked box (measured, snapped to).
  list.push_back({[=] { return v->ghostsPickable() && !v->looksPending(); }, [=, &w](bool) {
                    require(v->benchPickAt(s->bx, s->by) == s->boxB && v->benchLookState(s->boxB).value("activated", 0) > 0 && std::abs(v->benchLookState(s->boxB).value("transparency", 0.0) - 0.5) < 1e-6,
                            "while a guided tool picks, the locked box is picked as a reference (still faded)");
                    w.cancelTool();
                  }});
  list.push_back({[=] { return !v->ghostsPickable() && !v->looksPending(); }, [=](bool) {
                    require(v->benchPickAt(s->bx, s->by) != s->boxB, "the tool closed: the locked box is not picked again");
                    auto menu = rightClickMenu(s->bx, s->by);
                    if (QAction* unlock = unlockAt(*menu)) unlock->trigger();
                    require(!doc->scene.node(s->boxB)->locked && doc->undoLabel() == AppDocument::tr("unlock"), "Unlock from the right-click menu unlocks it, one step");
                  }});
  list.push_back({[=] { return idle() && v->benchLookState(s->boxB).value("activated", 0) > 0; }, [=, &w](bool picked) {
                    require(picked && v->benchLookState(s->boxB).value("transparency", 1.0) == 0.0 && v->benchPickAt(s->bx, s->by) == s->boxB, "unlocked: drawn and picked as before");
                    w.m_browser->selectIds({s->housing});
                    lock->trigger();
                    w.m_browser->selectIds({});
                    require(doc->scene.node(s->housing)->locked && !doc->scene.node(s->boxA)->locked, "the Housing locked (its box is not, it is held by it)");
                  }});
  // Refused in the shown language naming what holds the lock: a browser drop (through run) and a feature planned on a worker.
  auto lockedWith = [doc, s](const char* text) {
    return AppDocument::tr(text).arg(QString::fromStdString(doc->scene.node(s->boxA)->name), QString::fromStdString(doc->scene.node(s->housing)->name));
  };
  list.push_back({idle, [=, &w](bool) {
                    QString said;
                    const auto heard = QObject::connect(doc, &AppDocument::message, &w, [&said](const QString& text) { said = text; });
                    emit w.m_browser->tree()->reparentRequested({s->boxA}, std::string(), -1);
                    QObject::disconnect(heard);
                    require(said == lockedWith("“%1” is locked with “%2”: unlock “%2” before moving it") && doc->scene.node(s->boxA)->parent == s->housing,
                            "dropping the Housing's box on the root is refused naming the Housing: " + said);
                    const opad::json move = {{"op", "feature"}, {"kind", "move"}, {"name", "Move"}, {"inputs", {{"bodies", opad::json::array({s->boxA})}, {"dx", "0 mm"}, {"dy", "0 mm"}, {"dz", "5 mm"}, {"rotate", false}, {"copy", false}}}};
                    w.m_design->applyOps({move}, "move", [s](bool ok, const QString& error) { s->refused = ok ? QString("applied") : error; });
                  }});
  list.push_back({[s] { return !s->refused.isEmpty(); }, [=](bool done) {
                    require(done && s->refused == lockedWith("“%1” is locked with “%2”: unlock “%2” before moving it"), "moving it with a feature is refused the same way (placed, not rebuilt): " + s->refused);
                  }});
  // A locked component: what it holds is locked with it.
  list.push_back({[=] { return idle() && v->benchLookState(s->boxA).value("activated", -1) == 0; }, [=, &w](bool held) {
                    require(held && std::abs(v->benchLookState(s->boxA).value("transparency", 0.0) - 0.5) < 1e-6 && v->benchPickAt(s->ax, s->ay) != s->boxA,
                            "the Housing's box is faded and not picked with it");
                    browser::Decoration d;
                    QRect rect;
                    const browser::Badge* badge = lockBadge(w.m_browser->tree(), s->boxA, d, &rect);
                    require(badge && badge->color == &Tokens::fg3 && badge->tooltip.contains("Housing"), "the box's row has a dim lock badge naming the Housing: " + (badge ? badge->tooltip : QString()));
                    if (badge) click(w.m_browser->tree()->viewport(), rect.center());
                    require(!doc->scene.node(s->housing)->locked, "a click on that badge unlocks the Housing");
                  }});
  list.push_back({[=] { return idle() && v->benchLookState(s->boxA).value("activated", 0) > 0; }, [=, &w](bool picked) {
                    require(picked && v->benchPickAt(s->ax, s->ay) == s->boxA, "the Housing's box is picked again");
                    w.m_browser->selectIds({s->housing});
                    lock->trigger();
                    w.m_browser->selectIds({s->boxA});  // held by the Housing: Unlock frees the Housing
                    const bool offered = lock->isEnabled() && lock->data().toString() == "unlock";
                    lock->trigger();
                    const QString said = w.statusBar()->currentMessage();
                    require(offered && !doc->scene.node(s->housing)->locked && said.contains("Housing"), "Unlock on the box frees the Housing that held it and says so: " + said);
                    w.m_browser->selectIds({});
                  }});
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
