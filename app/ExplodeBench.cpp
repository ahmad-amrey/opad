// OPAD_BENCH_EXPLODE: exploded views (UI-36, app/ExplodeArea.cpp) in the running app. Cases in
// tools/bench_cases/assembly.py; the layout itself is core's (tests/test_explode).
#include <QCoreApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "ExplodeArea.hpp"
#include "ExplodePanel.hpp"
#include "MainWindow.hpp"
#include "opad/explode.hpp"
#include "opad/inspect.hpp"

namespace {
// One step: once `ready` holds (polled every 50 ms, at most `ms`), `act` runs with whether it did.
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

// The longest the event loop was held: a 1 ms ticker's worst gap.
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

const opad::Node* named(const opad::Scene& scene, const std::string& name) {
  for (const auto& [id, n] : scene.nodes)
    if (n.name == name) return &n;
  return nullptr;
}

double length(const std::array<double, 3>& v) { return std::hypot(v[0], v[1], v[2]); }
bool same(const std::array<double, 3>& a, const std::array<double, 3>& b) { return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 1e-6; }
QString vec(const std::array<double, 3>& v) { return QString("(%1, %2, %3)").arg(v[0], 0, 'f', 1).arg(v[1], 0, 'f', 1).arg(v[2], 0, 'f', 1); }

// Clicks a decorator badge of a browser row: the first one whose icon is `icon`, found by sweeping the row.
bool clickBadge(BrowserTree* tree, const std::string& id, const QString& icon, QString* tooltip = nullptr) {
  auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
  for (QTreeWidgetItemIterator it(tree); *it; ++it) {
    if ((*it)->data(0, browser::kIdRole).toString().toStdString() != id) continue;
    for (QTreeWidgetItem* up = (*it)->parent(); up; up = up->parent()) up->setExpanded(true);
    tree->scrollToItem(*it);
    const QModelIndex index = tree->indexFromItem(*it);
    const QRect row = tree->visualRect(index);
    const browser::Decoration d = delegate->decoration(index);
    for (int x = row.right(); x > row.left(); x -= 2) {
      QRect at;
      const browser::Badge* b = delegate->badgeAt(d, index, row, QPoint(x, row.center().y()), &at);
      if (!b || b->icon != icon || !b->clicked) continue;
      if (tooltip) *tooltip = b->tooltip;
      for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, QPointF(at.center()), QPointF(tree->viewport()->mapToGlobal(at.center())), Qt::LeftButton,
                      type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tree->viewport(), &e);
      }
      return true;
    }
  }
  return false;
}
}  // namespace

// OPAD_BENCH_EXPLODE=<prefix>. An enclosure made through the command layer: a shell and a lid, a PCB subassembly (board,
// chip, a small capacitor) and a Screws component with four screws. Exploded view (its command) opens the panel and plays
// the parts out (the lid rising frame by frame, the chip following), level 1 moving the PCB whole; level 2 splits it (the
// capacitor riding on the board); the PCB's browser badge keeps it whole again at level 2; level 1 with Explode its parts on
// the Screws spreads every screw; the slider at 50 % has the first level out and the screws still in their folder; a click
// on the board selects the PCB's unit; the lid's handle dragged up moves it while the mouse moves, a digit typed over the
// view sets its travel (typed values); the distance tool measures lid to shell where they are drawn and is not pinned;
// two screws grouped move as one, ungrouped apart; Save as view writes a view op with the explode, Collapse puts every
// part back, View > Named views explodes it again; Update view appends an edit; a feature started collapses the view and
// leaving it opens the view again. <prefix>.view.png, .panel.png, .browser.png, .chips.png, .ribbon.png. On a big model
// (the Engine): laying out, level 2 and 60 ticks from 0 to 1, each timed, no event-loop gap over 250 ms.
OPAD_BENCH(OPAD_BENCH_EXPLODE, explode) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: explode: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Explode* area = w.findChild<Explode*>();
  if (!area) {
    require(false, "the Explode area is registered");
    QCoreApplication::exit(2);
    return true;
  }
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  ExplodePanel* form = area->form();
  const QString prefix = value;
  auto idle = [v, &w] { return !v->looksPending() && !w.m_displayJob && w.m_meshRemaining == 0; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto& list = *steps;
  auto ticker = std::make_shared<Ticker>();
  auto laidOut = [area, v] { return area->isOn() && !area->layingOut() && !area->units().empty() && !area->playing() && !v->looksPending(); };

  if (doc->scene.all_bodies().size() > 16) {
    // A big assembly (the Engine): laying out, level 2, then 60 ticks from 0 to 1 (each from setT until every body shows
    // it), and collapsing, with no event-loop gap over 250 ms.
    for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
      if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
    auto settled = [&w, v, doc, idle] {
      int expected = 0;
      for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
      return idle() && v->displayedCount() + v->skippedCount() >= expected && v->displayedCount() > 0;
    };
    list.push_back({settled, [=, &w](bool shown) {
                      require(shown, QString("%1 bodies displayed").arg(v->displayedCount()));
                      ticker->start();
                      w.action("assembly.explode")->trigger();
                    }, 240000});
    list.push_back({laidOut, [=, &w](bool done) {
                      ticker->timer.stop();
                      require(done && ticker->worst < 250 && area->t() > 0.99,
                              QString("laid out and played out in %1 ms: %2 units, worst event-loop gap %3 ms").arg(ticker->phase.elapsed()).arg(area->units().size()).arg(ticker->worst));
                      ticker->start();
                      area->setLevels(2);
                    }, 60000});
    list.push_back({laidOut, [=, &w](bool done) {
                      ticker->timer.stop();
                      require(done && ticker->worst < 250, QString("level 2 in %1 ms: %2 units, worst event-loop gap %3 ms").arg(ticker->phase.elapsed()).arg(area->units().size()).arg(ticker->worst));
                      // 60 ticks, each timed from setT until the looks job has moved every body.
                      struct Ticks {
                        int i = 0;
                        qint64 worst = 0, total = 0;
                        QElapsedTimer clock;
                        QMetaObject::Connection applied;
                        bool done = false;
                      };
                      auto ticks = std::make_shared<Ticks>();
                      auto next = std::make_shared<std::function<void()>>();
                      *next = [=] {
                        if (ticks->i > 60) {
                          QObject::disconnect(ticks->applied);
                          ticks->done = true;
                          return;
                        }
                        ticks->clock.start();
                        area->setT(ticks->i++ / 60.0);
                        if (!v->looksPending()) QTimer::singleShot(0, v, [=] { (*next)(); });  // nothing moved (the first)
                      };
                      ticks->applied = QObject::connect(v, &Viewport::looksApplied, v, [=] {
                        const qint64 ms = ticks->clock.elapsed();
                        ticks->worst = std::max(ticks->worst, ms);
                        ticks->total += ms;
                        QTimer::singleShot(0, v, [=] { (*next)(); });
                      });
                      ticker->start();
                      (*next)();
                      auto* poll = new QTimer(v);
                      QObject::connect(poll, &QTimer::timeout, v, [=] {
                        if (!ticks->done) return;
                        poll->stop();
                        poll->deleteLater();
                        ticker->timer.stop();
                        require(ticker->worst < 250 && ticks->worst < 100,
                                QString("60 ticks over %1 bodies: worst %2 ms, mean %3 ms (setT to every body moved), worst event-loop gap %4 ms")
                                    .arg(v->displayedCount()).arg(ticks->worst).arg(ticks->total / 61.0, 0, 'f', 1).arg(ticker->worst));
                        ticker->start();
                        area->setOn(false);
                      });
                      poll->start(20);
                    }, 60000});
    list.push_back({[=] { return !area->isOn() && !area->playing() && !v->looksPending(); }, [=, &w](bool off) {
                      ticker->timer.stop();
                      size_t moved = v->shownOffsets().size();
                      require(off && moved == 0 && ticker->worst < 250, QString("collapsed: %1 bodies off their place, worst event-loop gap %2 ms").arg(moved).arg(ticker->worst));
                    }, 120000});
    runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
    return true;
  }

  struct State {
    std::string enclosure, pcb, screws, shell, lid, board, chip, cap, view;
    std::vector<std::string> screw;
    std::vector<double> rising;  // the lid's height while it plays out
    std::set<QString> chips;     // the chip's texts meanwhile
    double spacing = 0;
    size_t ops = 0;
    int x = 0, y = 0;
  };
  auto s = std::make_shared<State>();
  auto offset = [v](const std::string& id) { const BodyLook look = v->shownLook(id); return look.offset; };
  auto chipText = [area] { return area->chip()->isVisible() ? area->chip()->text() : QString(); };
  // The enclosure, through the command layer as opad-cli would make it.
  list.push_back({{}, [=, &w](bool) {
                    s->enclosure = doc->run("component", {{"name", "Enclosure"}}).value("id", "");
                    s->pcb = doc->run("component", {{"name", "PCB"}, {"parent", s->enclosure}}).value("id", "");
                    s->screws = doc->run("component", {{"name", "Screws"}, {"parent", s->enclosure}}).value("id", "");
                    auto box = [doc](const std::string& component, const char* name, double z, double x, double y, double l, double wd, double h) {
                      doc->run("feature", {{"kind", "box"}, {"name", name}, {"component", component},
                                           {"inputs", {{"plane", {{"origin", {0, 0, z}}, {"normal", {0, 0, 1}}}}, {"x", std::to_string(x) + " mm"}, {"y", std::to_string(y) + " mm"},
                                                       {"length", std::to_string(l) + " mm"}, {"width", std::to_string(wd) + " mm"}, {"height", std::to_string(h) + " mm"}}}});
                    };
                    box(s->enclosure, "Shell", 0, 50, 30, 100, 60, 20);
                    box(s->enclosure, "Lid", 20, 50, 30, 100, 60, 5);
                    box(s->pcb, "Board", 5, 50, 30, 90, 50, 1.6);
                    box(s->pcb, "Chip", 6.6, 45, 30, 10, 10, 2);
                    box(s->pcb, "Cap", 6.6, 20, 20, 2, 1.2, 1);
                    const double corners[4][2] = {{4, 4}, {96, 4}, {4, 56}, {96, 56}};
                    for (int i = 0; i < 4; ++i)
                      doc->run("feature", {{"kind", "cylinder"}, {"name", "Screw " + std::to_string(i + 1)}, {"component", s->screws},
                                           {"inputs", {{"plane", {{"origin", {0, 0, -2}}, {"normal", {0, 0, 1}}}}, {"x", std::to_string(corners[i][0]) + " mm"},
                                                       {"y", std::to_string(corners[i][1]) + " mm"}, {"diameter", "3 mm"}, {"height", "12 mm"}}}});
                  }});
  list.push_back({[=] { return idle() && v->displayedCount() == 9; }, [=, &w](bool shown) {
                    const opad::Scene& scene = doc->scene;
                    auto body = [&](const std::string& component, const std::string& name) {
                      for (const auto& id : scene.bodies_under(component))
                        if (scene.node(id)->name == name) return id;
                      return std::string();
                    };
                    s->shell = body(s->enclosure, "Shell");
                    s->lid = body(s->enclosure, "Lid");
                    s->board = body(s->pcb, "Board");
                    s->chip = body(s->pcb, "Chip");
                    s->cap = body(s->pcb, "Cap");
                    s->screw = scene.bodies_under(s->screws);
                    require(shown && !s->lid.empty() && !s->cap.empty() && s->screw.size() == 4 && !area->isOn() && !w.action("assembly.explodeOff")->isEnabled(),
                            QString("the enclosure: 9 bodies in Enclosure, PCB and Screws (%1 shown); explode off").arg(v->displayedCount()));
                    v->fitAll();
                    w.action("assembly.explode")->trigger();  // the panel opens and the parts play out
                  }});
  // Playing out: the lid rises frame by frame.
  list.push_back({[=] {
                    if (area->playing()) {
                      s->rising.push_back(offset(s->lid)[2]);
                      s->chips.insert(chipText());
                    }
                    return laidOut() && area->t() > 0.999;
                  },
                  [=, &w](bool out) {
                    std::vector<double> distinct;
                    for (double z : s->rising)
                      if (distinct.empty() || std::abs(z - distinct.back()) > 1e-6) distinct.push_back(z);
                    const bool monotonic = std::is_sorted(distinct.begin(), distinct.end());
                    require(out && area->panel()->isVisible() && distinct.size() >= 4 && monotonic && s->chips.size() >= 3 && v->showsAll(),
                            QString("Exploded view opens the panel and plays out: the lid at %1 heights on the way up, the chip read %2 texts; the exploded parts in view")
                                .arg(distinct.size()).arg(s->chips.size()));
                    const auto lid = offset(s->lid), board = offset(s->board), chip = offset(s->chip), cap = offset(s->cap), shell = offset(s->shell);
                    require(lid[2] > 5 && same(board, chip) && same(board, cap) && length(board) > 0 && !same(board, lid),
                            QString("level 1: the lid up %1, the PCB whole %2 (board, chip and capacitor together), the shell %3").arg(lid[2], 0, 'f', 1).arg(vec(board), vec(shell)));
                    QStringList levels;
                    for (QToolButton* b : form->levelButtons()) levels << b->text() + (b->isChecked() ? "*" : "");
                    require(levels == QStringList({"1*", "2", ExplodePanel::tr("All")}) && chipText().contains("100") && area->trailCount() >= 3,
                            QString("the level control (%1) apart from the distance; the chip reads '%2'; %3 trail lines").arg(levels.join(' '), chipText()).arg(area->trailCount()));
                    for (QToolButton* b : form->levelButtons())
                      if (b->text() == "2") b->click();
                  }});
  list.push_back({laidOut, [=, &w](bool done) {
                    const auto board = offset(s->board), chip = offset(s->chip), cap = offset(s->cap);
                    require(done && area->spec().levels == 2 && !same(board, chip) && same(board, cap),
                            QString("level 2 splits the PCB: the chip %1 off the board %2, the capacitor riding on the board").arg(vec(chip), vec(board)));
                    QString tip;
                    const bool clicked = clickBadge(w.m_browser->tree(), s->pcb, "explodeLevel", &tip);
                    require(clicked && area->spec().keep.count(s->pcb), "the PCB's browser badge (" + tip + ") keeps it together");
                  }});
  list.push_back({laidOut, [=, &w](bool done) {
                    const auto board = offset(s->board), chip = offset(s->chip), cap = offset(s->cap);
                    require(done && same(board, chip) && same(board, cap) && length(board) > 0, "kept: the PCB moves whole at level 2 " + vec(board));
                    w.m_browser->grab().save(prefix + ".browser.png");
                    area->setLevels(1);
                    w.m_browser->selectIds({s->screws});
                  }});
  list.push_back({[=, &w] { return laidOut() && w.action("assembly.explodeSplit")->isEnabled(); }, [=, &w](bool enabled) {
                    SelectionContext context = w.selectionContext();
                    QMenu menu;
                    for (AreaController* a : w.m_areas) a->contextMenu(context, menu);
                    QStringList entries;
                    for (QAction* a : menu.actions())
                      if (!a->objectName().isEmpty()) entries << a->objectName();
                    require(enabled && entries.contains("assembly.explodeKeep") && entries.contains("assembly.explodeSplit"), "the Screws' context menu: " + entries.join(' '));
                    QAction* split = w.action("assembly.explodeSplit");
                    split->setChecked(false);
                    split->trigger();  // checkable: on, as a click in the menu turns it
                    require(area->spec().split.count(s->screws) > 0, "Explode its parts on the Screws");
                    w.m_browser->selectIds({});
                  }});
  list.push_back({laidOut, [=, &w](bool done) {
                    std::set<std::string> spread;
                    for (const auto& id : s->screw) spread.insert(vec(offset(id)).toStdString());
                    require(done && spread.size() == 4 && area->spec().levels == 1, QString("level 1 with the Screws split: the four screws at %1 different places").arg(spread.size()));
                    form->slider()->setValue(500);
                  }});
  list.push_back({[=] { return !v->looksPending(); }, [=, &w](bool) {
                    // Level by level: at 50 % the top level is out, the screws have only moved with their folder.
                    std::set<std::string> spread;
                    for (const auto& id : s->screw) spread.insert(vec(offset(id)).toStdString());
                    require(std::abs(area->t() - 0.5) < 1e-9 && chipText().contains("50") && spread.size() == 1 && offset(s->lid)[2] > 5 && form->distanceBox()->text().startsWith("50"),
                            QString("the slider at 50 %: the lid out, the screws together (%1 places), the chip '%2'").arg(spread.size()).arg(chipText()));
                    form->slider()->setValue(1000);
                  }});
  list.push_back({[=] { return !v->looksPending(); }, [=, &w](bool) {
                    v->fitAll();
                    s->x = s->y = 0;
                    require(v->benchBodyPoint(s->board, s->x, s->y), QString("the board is picked where it is drawn (%1, %2)").arg(s->x).arg(s->y));
                    v->benchClickAt(s->x, s->y);
                  }});
  // A click on a part of a larger unit selects that unit: the board -> the PCB.
  list.push_back({[=, &w] { return w.m_browser->selectedIds() == std::vector<std::string>{s->pcb} && area->dragUnit() >= 0; }, [=, &w](bool unit) {
                    require(unit && area->units()[static_cast<size_t>(area->dragUnit())].id == s->pcb && area->handle()->isVisible(),
                            "a click on the board selects the PCB, its unit, and shows its drag handle");
                    w.m_browser->selectIds({s->lid});
                  }});
  // The lid's handle: dragged up, the lid follows the mouse.
  list.push_back({[=] { return area->handle()->isVisible() && area->dragUnit() >= 0 && area->units()[static_cast<size_t>(area->dragUnit())].id == s->lid && !v->looksPending(); }, [=, &w](bool shown) {
                    const auto& u = area->units()[static_cast<size_t>(area->dragUnit())];
                    const double before = opad::explode_travel(u, area->spec(), u.dir), z0 = offset(s->lid)[2];
                    const auto o = offset(s->lid);
                    const QPoint tip = v->widgetPoint({u.centre[0] + o[0], u.centre[1] + o[1], u.centre[2] + o[2]});
                    auto mouse = [v](QEvent::Type type, QPoint at, Qt::MouseButtons buttons) {
                      QMouseEvent e(type, QPointF(at), QPointF(v->mapToGlobal(at)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
                      QCoreApplication::sendEvent(v, &e);
                    };
                    mouse(QEvent::MouseButtonPress, tip, Qt::LeftButton);
                    const bool dragging = area->handle()->dragging();
                    std::vector<double> travel;
                    for (int step = 1; step <= 3; ++step) {
                      mouse(QEvent::MouseMove, tip - QPoint(0, 20 * step), Qt::LeftButton);
                      travel.push_back(opad::explode_travel(area->units()[static_cast<size_t>(area->dragUnit())], area->spec(), u.dir));
                    }
                    mouse(QEvent::MouseButtonRelease, tip - QPoint(0, 60), Qt::NoButton);
                    require(shown && dragging && travel.size() == 3 && travel[0] > before && travel[1] > travel[0] && travel[2] > travel[1] && area->spec().offsets.count(s->lid),
                            QString("dragging the lid's arrow up: its travel %1 -> %2, %3, %4 mm while the mouse moves (z %5)")
                                .arg(before, 0, 'f', 1).arg(travel.empty() ? 0 : travel[0], 0, 'f', 1).arg(travel.size() > 1 ? travel[1] : 0, 0, 'f', 1).arg(travel.size() > 2 ? travel[2] : 0, 0, 'f', 1).arg(z0, 0, 'f', 1));
                    s->spacing = travel.empty() ? 0 : travel.back();
                  }});
  list.push_back({[=] { return !v->looksPending(); }, [=, &w](bool) {
                    const auto& u = area->units()[static_cast<size_t>(area->dragUnit())];
                    require(std::abs(offset(s->lid)[2] - (opad::explode_travel(u, area->spec(), {0, 0, 1}))) < 1e-6, QString("the lid drawn where the drag left it: z %1").arg(offset(s->lid)[2], 0, 'f', 1));
                    // Typed: a digit over the view starts the value (as DynamicInput does), the box takes an expression.
                    QKeyEvent key(QEvent::KeyPress, Qt::Key_7, Qt::NoModifier, "7");
                    QCoreApplication::sendEvent(v, &key);
                    const double seven = opad::explode_travel(area->units()[static_cast<size_t>(area->dragUnit())], area->spec(), u.dir);
                    auto* box = area->handle()->findChild<QLineEdit*>("dimensionValue");
                    if (box) {
                      box->setText("12.5 mm + 10 mm");
                      emit box->textEdited(box->text());
                      emit box->returnPressed();
                    }
                    const double typed = opad::explode_travel(area->units()[static_cast<size_t>(area->dragUnit())], area->spec(), u.dir);
                    require(box && std::abs(seven - 7) < 1e-6 && std::abs(typed - 22.5) < 1e-6, QString("typed over the view: 7 -> %1 mm, '12.5 mm + 10 mm' -> %2 mm").arg(seven).arg(typed));
                    if (box) box->clearFocus();
                  }});
  // Measuring where the parts are drawn: lid to shell.
  list.push_back({[=] { return !v->looksPending(); }, [=, &w](bool) {
                    w.m_browser->selectIds({});
                    v->clearSelection();
                    v->grabImage().save(prefix + ".view.png");
                    w.startTool("distance");
                  }});
  list.push_back({[=, &w] { return v->ghostsPickable() && !w.m_jobs->busy(); }, [=, &w](bool) {
                    int x = 0, y = 0;
                    if (v->benchBodyPoint(s->lid, x, y)) v->benchClickAt(x, y);
                  }});
  list.push_back({[=, &w] { return v->selection().size() == 1 && !w.m_jobs->busy(); }, [=, &w](bool) {
                    int x = 0, y = 0;
                    if (v->benchBodyPoint(s->shell, x, y)) v->benchClickAt(x, y);
                  }});
  list.push_back({[=, &w] { return !w.m_lastMeasure.is_null(); }, [=, &w](bool measured) {
                    opad::Ref lid, shell;
                    lid.body = s->lid;
                    shell.body = s->shell;
                    const double drawn = opad::measure_distance(doc->doc, opad::exploded_scene(doc->scene, v->shownOffsets()), lid, shell).value("value", -1.0);
                    const double assembled = opad::measure_distance(doc->doc, doc->scene, lid, shell).value("value", -1.0);
                    const double got = measured ? w.m_lastMeasure.value("value", -2.0) : -2;
                    require(measured && std::abs(got - drawn) < 1e-6 && drawn > assembled + 5 && w.m_lastMeasure.value("exploded", false) && !w.action("inspect.pin")->isEnabled(),
                            QString("the distance tool measures lid to shell where they are drawn: %1 mm (exploded %2, assembled %3), not pinned").arg(got).arg(drawn).arg(assembled));
                    w.cancelTool();
                    w.m_browser->selectIds({s->screw[0], s->screw[1]});
                  }});
  // Groups.
  list.push_back({[=, &w] { return w.action("assembly.explodeGroup")->isEnabled(); }, [=, &w](bool enabled) {
                    w.action("assembly.explodeGroup")->trigger();
                    require(enabled && area->spec().groups.size() == 1, "two screws selected: Group (explode as one)");
                  }});
  list.push_back({laidOut, [=, &w](bool done) {
                    require(done && area->unitOf(s->screw[0]) == area->unitOf(s->screw[1]) && same(offset(s->screw[0]), offset(s->screw[1])) && !same(offset(s->screw[0]), offset(s->screw[2])),
                            "grouped: the two screws move as one, the others apart");
                    const bool enabled = w.action("assembly.explodeUngroup")->isEnabled();
                    w.action("assembly.explodeUngroup")->trigger();
                    require(enabled && area->spec().groups.empty(), "Ungroup");
                    w.m_browser->selectIds({});
                  }});
  // Save as view, collapse, the view again from View > Named views.
  list.push_back({laidOut, [=, &w](bool done) {
                    require(done && !same(offset(s->screw[0]), offset(s->screw[1])), "ungrouped: the screws apart again");
                    s->ops = doc->doc.ops.size();
                    s->view = area->saveView("Enclosure exploded");
                    const opad::ViewBookmark* view = doc->scene.views.empty() ? nullptr : &doc->scene.views.back();
                    const opad::json e = view ? view->explode : opad::json();
                    require(!s->view.empty() && view && view->id == s->view && doc->doc.ops.size() == s->ops + 1 && e.is_object() && e.value("levels", -1) == 1 &&
                                e.value("keep", opad::json::array()).dump().find(s->pcb) != std::string::npos && e.value("split", opad::json::array()).dump().find(s->screws) != std::string::npos &&
                                e.value("offsets", opad::json::object()).contains(s->lid) && form->updateButton()->isVisibleTo(form),
                            "Save as view: one view op with the explode (level 1, the PCB kept, the Screws split, the lid's drag): " + QString::fromStdString(e.dump()).left(160));
                    w.m_chips->grab().save(prefix + ".chips.png");
                    area->panel()->grab().save(prefix + ".panel.png");
                    w.setWorkspace("design");
                    w.m_ribbon->setCurrentTab(int(w.m_ribbon->tabIds().indexOf("design.assemble")));
                    auto buttons = [&w](const QString& tab, const QStringList& ids) {
                      int n = 0;
                      if (RibbonPage* page = w.m_ribbon->page(tab))
                        for (QToolButton* b : page->findChildren<QToolButton*>())
                          n += b->defaultAction() && ids.contains(b->defaultAction()->objectName());
                      return n;
                    };
                    const int assemble = buttons("design.assemble", {"assembly.explode"});
                    w.m_ribbon->grab().save(prefix + ".ribbon.png");
                    w.m_ribbon->setCurrentTab(int(w.m_ribbon->tabIds().indexOf("design.view")));
                    const int viewTab = buttons("design.view", {"assembly.explode", "assembly.explodePlay", "assembly.explodeOff"});
                    require(assemble == 1 && viewTab == 3, QString("Design > Assemble has Exploded view (%1), Design > View its group with Play and Collapse (%2)").arg(assemble).arg(viewTab));
                    w.action("assembly.explodeOff")->trigger();
                  }});
  list.push_back({[=] { return !area->isOn() && !area->playing() && !v->looksPending(); }, [=, &w](bool off) {
                    require(off && v->shownOffsets().empty() && area->trailCount() == 0 && !area->chip()->isVisible(), "Collapse: every part back in place, no trail lines, no chip");
                    QMenu* views = w.findChild<QMenu*>("views");
                    QAction* entry = nullptr;
                    for (QAction* a : views ? views->actions() : QList<QAction*>())
                      if (a->data().toString().toStdString() == s->view) entry = a;
                    if (entry) entry->trigger();
                    require(entry != nullptr, "View > Named views lists the exploded view");
                  }});
  list.push_back({[=] { return laidOut() && area->t() > 0.999; }, [=, &w](bool out) {
                    require(out && area->viewId() == s->view && area->spec().keep.count(s->pcb) && area->spec().offsets.count(s->lid) && offset(s->lid)[2] > 5,
                            "the named view explodes again as saved (the PCB kept, the lid's drag)");
                    s->ops = doc->doc.ops.size();
                    form->updateButton()->click();
                  }});
  list.push_back({{}, [=, &w](bool) {
                    const opad::Op& op = doc->doc.ops.back();
                    require(doc->doc.ops.size() == s->ops + 1 && op.type == "edit" && op.data.value("target", "") == s->view && op.data["set"].contains("explode"), "Update view appends one edit of the view");
                    w.m_design->startFeature("box");
                  }});
  // A feature edit collapses the view; leaving it opens the view again.
  list.push_back({[=, &w] { return w.m_design->featureActive() && area->t() == 0 && !area->playing() && !v->looksPending(); }, [=, &w](bool collapsed) {
                    require(collapsed && v->shownOffsets().empty() && area->isOn(), "a feature started: the view collapses while it is edited");
                    w.m_design->escape();
                  }});
  list.push_back({[=, &w] { return !w.m_design->featureActive() && area->t() > 0.999 && !area->playing() && !v->looksPending(); }, [=, &w](bool back) {
                    require(back && offset(s->lid)[2] > 5, "the feature left: the view explodes again");
                  }});
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
