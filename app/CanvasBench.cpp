// OPAD_BENCH_CANVAS=<prefix> (UI-70), on a document holding a box beside the XZ plane (case "canvas" in
// tools/bench_cases/assets.py): a JPEG inserted as a canvas on XZ through the placer at a typed width; a corner dragged by the
// real mouse handlers' path (the canvas drawn where the drag puts it at every step, nothing written until let go, then one
// transform op, the opposite corner fixed, undo and redo); Shift on a corner stretching it, Picture proportions; digits typed in the view landing in the panel's X (never the
// filter shortcut) and applied with Enter; Calibrate by two clicks on the picture and a typed distance; Align to model onto two
// vertices of the box; lock (no handles, a drag moves nothing); flip (the picture drawn mirrored), show through and
// selectable; Trace to sketch; Replace (same node, width kept); a sketch's backdrop turned into a canvas; a picture dropped
// onto the window with the box's top face selected (the placer on the face, centred on it). Frames:
// <prefix>.png (handles), .panel.png, .place.png, .flipped.png.
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QUrl>
#include <QTreeWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>

#include <cmath>
#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "CanvasArea.hpp"
#include "CanvasEditor.hpp"
#include "DesignController.hpp"
#include "DrawingPlacer.hpp"
#include "BrowserPanel.hpp"
#include "MainWindow.hpp"
#include "PropertiesPanel.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"
#include "opad/canvas.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace {
using Step = std::function<void(std::function<void()> next)>;

void waitFor(QObject* context, std::function<bool()> ready, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, ready, ms, then] {
    const bool ok = ready();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(50);
}

double gap(const opad::Vec3& a, const opad::Vec3& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
bool same(const opad::Mat4& a, const opad::Mat4& b, double tol = 1e-6) {
  for (size_t i = 0; i < 12; ++i)
    if (std::fabs(a.m[i] - b.m[i]) > tol * std::max(1.0, std::fabs(a.m[i]))) return false;
  return true;
}
void mouse(QWidget* widget, QEvent::Type type, const QPointF& at, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  QMouseEvent e(type, at, widget->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
  QApplication::sendEvent(widget, &e);
}
// Where the picture's blue block is in a frame: the mean position of its pixels, and how many.
QPointF blueCentre(const QImage& frame, int& count) {
  double x = 0, y = 0;
  count = 0;
  for (int j = 0; j < frame.height(); ++j)
    for (int i = 0; i < frame.width(); ++i)
      if (const QRgb c = frame.pixel(i, j); qBlue(c) > 150 && qRed(c) < 90 && qGreen(c) < 110) x += i, y += j, ++count;
  return count ? QPointF(x / count, y / count) : QPointF();
}
}  // namespace

// OPAD_BENCH_CANVAS_PERF=<prefix> on the Engine (not a gui_benches case: run it with OPAD_TRACE on Engine V8-XT Turbo.opad and
// read the stalls): a 12 MP JPEG inserted as a canvas on XY among the Engine's bodies (unhidden in memory, never saved), the
// model's snap points found, a 20-step drag with snapping and its release, X typed; each step's time is logged.
OPAD_BENCH(OPAD_BENCH_CANVAS_PERF, canvasPerf) {
  static bool running = false;
  if (running) return true;
  running = true;
  CanvasArea* area = w.findChild<CanvasArea*>();
  AppDocument* doc = w.m_doc;
  Viewport* view = w.m_viewport;
  const QString photo = value + ".photo.jpg";
  {
    QImage pixels(4000, 3000, QImage::Format_RGB32);
    pixels.fill(Qt::white);
    QPainter p(&pixels);
    p.setBrush(Qt::black);
    for (int i = 0; i < 40; ++i) p.drawEllipse(QRectF(100 * i, 70 * i, 300, 200));
    p.end();
    pixels.save(photo, "JPG", 90);
  }
  std::vector<std::string> hidden;  // collected first: every run() rebuilds the scene being iterated
  for (const auto& [id, node] : doc->scene.nodes)
    if (!node.visible) hidden.push_back(id);
  QElapsedTimer clock;
  clock.start();
  for (const auto& id : hidden) doc->run("appearance", {{"target", id}, {"visible", true}});
  trace::log(QString("bench: canvas perf: %1 nodes unhidden in memory in %2 ms").arg(hidden.size()).arg(clock.restart()));
  waitFor(area, [&w, view] { return w.m_meshRemaining == 0 && view->displayedCount() > 0; }, 300000, [=, &w](bool) {
    DrawingPlacer* placer = area->placer();
    QObject::connect(placer, &DrawingPlacer::ready, area, [=] {
      placer->setImageWidth(500);
      auto t = std::make_shared<QElapsedTimer>();
      t->start();
      placer->panel()->findChild<QPushButton*>("primary")->click();
      waitFor(area, [=] { return area->editor()->active() && view->showsPicture(area->editor()->canvas()) && !doc->loading; }, 120000, [=](bool shown) {
        trace::log(QString("bench: canvas perf: placed and shown in %1 ms").arg(t->restart()));
        if (!shown) {
          trace::log("bench: canvas perf FAIL: not shown");
          return QCoreApplication::exit(2);
        }
        CanvasEditor* editor = area->editor();
        waitFor(area, [editor] { return editor->modelPoints() > 0; }, 120000, [=](bool) {
          trace::log(QString("bench: canvas perf: %1 snap points of the model found in %2 ms").arg(editor->modelPoints()).arg(t->restart()));
          view->benchDesignShot(value + ".png");
          QPointF grip;
          editor->gripPoint(CanvasEditor::Grip::Move, grip);
          mouse(view, QEvent::MouseButtonPress, grip, Qt::LeftButton);
          qint64 worst = 0;
          for (int i = 1; i <= 20; ++i) {
            QElapsedTimer step;
            step.start();
            mouse(view, QEvent::MouseMove, grip + QPointF(6 * i, -4 * i), Qt::LeftButton);
            worst = std::max(worst, step.elapsed());
          }
          t->restart();
          mouse(view, QEvent::MouseButtonRelease, grip + QPointF(120, -80), Qt::NoButton);
          const qint64 release = t->restart();
          area->field(0)->setText("50");
          area->field(0)->setModified(true);
          emit area->field(0)->returnPressed();
          const qint64 typed = t->restart();
          trace::log(QString("bench: canvas perf: drag steps worst %1 ms, let go (one transform op, the scene replayed) %2 ms, X typed %3 ms").arg(worst).arg(release).arg(typed));
          trace::log(QString("bench: canvas perf %1").arg(worst < 50 ? "PASS" : "FAIL"));
          area->finish();
          QCoreApplication::exit(worst < 50 ? 0 : 2);
        });
      });
    }, Qt::SingleShotConnection);
    area->placeOn(photo, opad::Frame(), 0, 0, false);
  });
  return true;
}

OPAD_BENCH(OPAD_BENCH_CANVAS, canvas) {
  static bool running = false;  // the registry asks again after every load: the canvas's own import too
  if (running) return true;
  running = true;
  const QString prefix = value;
  CanvasArea* area = w.findChild<CanvasArea*>();
  if (!area) {
    trace::log("bench: canvas FAIL: no canvas area");
    QCoreApplication::exit(2);
    return true;
  }
  struct State {
    std::string canvas;
    opad::Mat4 world;
    size_t ops = 0;
    std::vector<opad::Vec3> picks;
    bool ok = true;
  };
  auto st = std::make_shared<State>();
  Viewport* view = w.m_viewport;
  AppDocument* doc = w.m_doc;
  CanvasEditor* editor = area->editor();
  auto check = [st](bool ok, const QString& what) {
    trace::log(QString("bench: canvas: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    st->ok = st->ok && ok;
    return ok;
  };
  auto canvasWorld = [doc, st] { return doc->scene.world(st->canvas); };
  // The picture: white, a black disc in its top left quarter, a blue block bottom right (its top half dark for the trace).
  const QString photo = prefix + ".photo.jpg", square = prefix + ".square.png";
  {
    QImage pixels(400, 200, QImage::Format_RGB32);
    pixels.fill(Qt::white);
    QPainter p(&pixels);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    p.drawEllipse(QRectF(30, 20, 70, 70));
    p.setBrush(QColor(20, 40, 200));
    p.drawRect(260, 120, 110, 60);
    p.end();
    QImage other(300, 300, QImage::Format_RGB32);
    other.fill(QColor(30, 30, 30));
    if (!pixels.save(photo, "JPG", 92) || !other.save(square, "PNG")) {
      trace::log("bench: canvas FAIL: the pictures could not be written");
      QCoreApplication::exit(2);
      return true;
    }
  }
  const opad::Frame xz = opad::design::resolve_plane(doc->doc, doc->scene, {{"base", "xz"}});
  std::vector<Step> steps;
  // 1. Insert on XZ through the placer, 200 mm wide.
  steps.push_back([=, &w](std::function<void()> next) {
    DrawingPlacer* placer = area->placer();
    QObject::connect(placer, &DrawingPlacer::ready, area, [=] {
      check(placer->picture() && std::fabs(placer->imageWidth() - 400 * 25.4 / 96) < 1 , "the placer shows the picture at its own size");
      placer->setImageWidth(200);
      placer->panel()->grab().save(prefix + ".place.png");
      st->ops = doc->doc.ops.size();
      placer->panel()->findChild<QPushButton*>("primary")->click();  // Place
      waitFor(area, [=] { return editor->active() && view->showsPicture(editor->canvas()) && !doc->loading; }, 20000, [=](bool ok) {
        if (!check(ok, "Place imports the canvas, shown with its picture, its editor open")) return QCoreApplication::exit(2);
        st->canvas = editor->canvas();
        const opad::CanvasPlace p = opad::canvas_place(doc->scene, st->canvas);
        const opad::json& op = doc->doc.ops.back().data;
        check(doc->doc.ops.size() == st->ops + 1 && op["op"] == "import" && op.contains("canvas") && std::fabs(p.width - 200) < 1e-6 && p.on_plane &&
                  std::fabs(p.x) < 1e-9 && std::fabs(p.y) < 1e-9 && std::fabs(p.plane.normal()[1] - xz.normal()[1]) < 1e-12,
              "one import op: 200 mm wide, centred on the XZ plane's origin");
        check(area->panel()->isVisible() && area->field(2)->text().startsWith("200"), "the canvas panel shows its width");
        next();
      });
    }, Qt::SingleShotConnection);
    area->placeOn(photo, xz, 0, 0, false);
  });
  // 2. Drag the top right corner: drawn where the drag is at every step, nothing written until let go; then one op.
  steps.push_back([=](std::function<void()> next) {
    QElapsedTimer clock;
    clock.start();
    view->benchDesignShot(prefix + ".fit.png");  // a frame: the view is laid out for the clicks
    trace::log(QString("bench: canvas: frame in %1 ms").arg(clock.restart()));
    st->world = canvasWorld();
    st->ops = doc->doc.ops.size();
    const opad::CanvasPlace before = editor->place();
    const auto corners = opad::canvas_points(st->world, before.body_w, before.body_h);
    QPointF at;
    editor->gripPoint(CanvasEditor::Grip::Corner2, at);
    mouse(view, QEvent::MouseMove, at, Qt::NoButton);
    mouse(view, QEvent::MouseButtonPress, at, Qt::LeftButton);
    bool live = true;
    for (int i = 1; i <= 6; ++i) {
      mouse(view, QEvent::MouseMove, at + QPointF(8 * i, -5 * i), Qt::LeftButton);
      opad::Mat4 shown;
      live = live && view->shownPlacement(st->canvas, shown) && same(shown, opad::canvas_world(editor->place())) && !same(shown, st->world) &&
             doc->doc.ops.size() == st->ops && editor->dragging();
    }
    trace::log(QString("bench: canvas: 6 drag steps in %1 ms").arg(clock.restart()));
    check(live, "the drag shows the canvas where it goes at every step, nothing written meanwhile");
    mouse(view, QEvent::MouseButtonRelease, at + QPointF(48, -30), Qt::NoButton);
    const opad::CanvasPlace after = opad::canvas_place(doc->scene, st->canvas);
    const auto moved = opad::canvas_points(canvasWorld(), after.body_w, after.body_h);
    check(doc->doc.ops.size() == st->ops + 1 && doc->doc.ops.back().type == "transform" && after.width > before.width + 1 && gap(moved[0], corners[0]) < 1e-6 &&
              std::fabs(after.height / after.width - before.height / before.width) < 1e-9,
          QString("letting go writes one transform op: %1 -> %2 mm wide about the fixed opposite corner").arg(before.width).arg(after.width));
    trace::log(QString("bench: canvas: let go in %1 ms").arg(clock.restart()));
    view->grabImage().save(prefix + ".png");
    trace::log(QString("bench: canvas: grabbed in %1 ms").arg(clock.restart()));
    doc->undo();
    const bool undone = same(canvasWorld(), st->world);
    doc->redo();
    check(undone && !same(canvasWorld(), st->world), "undo puts it back, redo again");
    // Esc during a drag: claimed from the window's shortcuts, the canvas back where it was, nothing written.
    const size_t ops = doc->doc.ops.size();
    QPointF grip;
    editor->gripPoint(CanvasEditor::Grip::Move, grip);
    mouse(view, QEvent::MouseButtonPress, grip, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, grip + QPointF(40, 10), Qt::LeftButton);
    const bool moving = editor->dragging();
    QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    over.ignore();
    QApplication::sendEvent(view, &over);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(view, &esc);
    mouse(view, QEvent::MouseButtonRelease, grip + QPointF(40, 10), Qt::NoButton);
    opad::Mat4 shown;
    check(moving && over.isAccepted() && !editor->dragging() && view->shownPlacement(st->canvas, shown) && same(shown, canvasWorld()) && doc->doc.ops.size() == ops,
          "Esc during a drag puts the canvas back and writes nothing");
    next();
  });
  // 2b. Shift on a corner stretches it (free aspect): drawn stretched at every step through its own rectangle at that aspect,
  // one transform op when let go, the opposite corner fixed; Picture proportions puts the picture's proportions back.
  steps.push_back([=](std::function<void()> next) {
    st->world = canvasWorld();
    st->ops = doc->doc.ops.size();
    const opad::CanvasPlace before = editor->place();
    const auto corners = opad::canvas_points(st->world, before.body_w, before.body_h);
    QPointF at;
    editor->gripPoint(CanvasEditor::Grip::Corner2, at);
    mouse(view, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::ShiftModifier);
    bool live = true;
    for (int i = 1; i <= 5; ++i) {
      mouse(view, QEvent::MouseMove, at + QPointF(12 * i, -i), Qt::LeftButton, Qt::ShiftModifier);
      opad::Mat4 shown;
      live = live && view->shownPlacement(st->canvas, shown) && same(shown, opad::canvas_world(editor->place())) && editor->place().stretched() &&
             doc->doc.ops.size() == st->ops;
    }
    check(live && view->showsPicture(st->canvas), "Shift on a corner: drawn stretched at every step, its picture on it, nothing written meanwhile");
    mouse(view, QEvent::MouseButtonRelease, at + QPointF(60, -5), Qt::NoButton, Qt::ShiftModifier);
    const opad::CanvasPlace after = opad::canvas_place(doc->scene, st->canvas);
    const auto moved = opad::canvas_points(canvasWorld(), after.body_w, after.body_h);
    opad::Mat4 shown;
    check(doc->doc.ops.size() == st->ops + 1 && doc->doc.ops.back().type == "transform" && after.stretched() && gap(moved[0], corners[0]) < 1e-6 &&
              after.height / after.width < before.height / before.width - 0.01 && view->shownPlacement(st->canvas, shown) && same(shown, canvasWorld()) &&
              view->showsPicture(st->canvas),
          QString("let go: one transform op, %1 x %2 mm -> %3 x %4 mm about the fixed corner, drawn as the document has it")
              .arg(before.width).arg(before.height).arg(after.width).arg(after.height));
    view->grabImage().save(prefix + ".stretched.png");
    QPushButton* proportions = area->panel()->findChild<QPushButton*>("canvasProportions");
    check(proportions && proportions->isVisible(), "the panel offers Picture proportions while it is stretched");
    // Width typed alone keeps the stretch; then the picture's proportions back.
    area->field(2)->setText("150");
    area->field(2)->setModified(true);
    emit area->field(2)->returnPressed();
    const opad::CanvasPlace typed = opad::canvas_place(doc->scene, st->canvas);
    check(std::fabs(typed.width - 150) < 1e-6 && std::fabs(typed.height / typed.width - after.height / after.width) < 1e-9, "a width typed alone keeps its proportions, stretched");
    if (proportions) proportions->click();
    const opad::CanvasPlace back = opad::canvas_place(doc->scene, st->canvas);
    check(!back.stretched() && std::fabs(back.width - 150) < 1e-6 && opad::mat_is_rigid(canvasWorld()) && view->shownPlacement(st->canvas, shown) && same(shown, canvasWorld()) &&
              !proportions->isVisible(),
          "Picture proportions: its height from its width again, drawn as a similarity");
    doc->undo();
    check(opad::canvas_place(doc->scene, st->canvas).stretched() && view->shownPlacement(st->canvas, shown) && same(shown, canvasWorld()), "undo: stretched again, drawn so");
    doc->redo();
    next();
  });
  // 3. Digits typed in the view go into X (not the Body filter of key 1); Enter applies them.
  steps.push_back([=](std::function<void()> next) {
    view->setFocus();
    const auto filter = view->selectionFilter();
    st->ops = doc->doc.ops.size();
    bool claimed = true;
    for (const QString& t : {QString("1"), QString("2"), QString("0")}) {  // as the window delivers them: the shortcut stage first
      const int key = Qt::Key_0 + t.toInt();
      QKeyEvent over(QEvent::ShortcutOverride, key, Qt::NoModifier, t);
      over.ignore();
      QApplication::sendEvent(view, &over);
      claimed = claimed && over.isAccepted();
      QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, t);
      QApplication::sendEvent(view, &press);
    }
    QLineEdit* x = area->field(0);
    trace::log(QString("bench: canvas: typed X \"%1\", claimed %2, filter %3 -> %4").arg(x->text()).arg(claimed).arg(int(filter)).arg(int(view->selectionFilter())));
    check(claimed && x->text() == "120" && view->selectionFilter() == filter, "digits typed in the view land in X; the filter shortcut never sees them");
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(x, &tab);
    check(area->field(1)->window()->focusWidget() == area->field(1), "Tab goes on to Y");
    emit x->returnPressed();
    const opad::CanvasPlace p = opad::canvas_place(doc->scene, st->canvas);
    check(doc->doc.ops.size() == st->ops + 1 && std::fabs(p.x - 120) < 1e-9, "Enter moves its centre to X = 120 mm, one op");
    area->panel()->grab().save(prefix + ".panel.png");
    view->setFocus();
    next();
  });
  // 4. Calibrate: two clicks on the picture, then the real distance typed.
  steps.push_back([=](std::function<void()> next) {
    view->benchDesignShot(prefix + ".fit2.png");
    area->calibrate();
    const opad::CanvasPlace p = editor->place();
    const opad::Mat4 world = canvasWorld();
    st->world = world;
    for (const double dx : {-p.width / 4, p.width / 5}) {
      const QPoint at = view->widgetPoint(p.plane.to_world(p.x + dx, p.y));
      mouse(view, QEvent::MouseButtonPress, at, Qt::LeftButton);
      mouse(view, QEvent::MouseButtonRelease, at, Qt::NoButton);
    }
    st->picks = area->flowPoints();
    if (!check(st->picks.size() == 2 && area->field(5)->isVisible(), "two clicks on the picture, then the distance is asked")) return next();
    const opad::Mat4 inverse = opad::affine_inverse(world);
    const opad::Vec3 a = inverse.apply(st->picks[0]), b = inverse.apply(st->picks[1]);
    area->field(5)->setText("100");
    emit area->field(5)->returnPressed();
    const opad::Mat4 now = canvasWorld();
    check(std::fabs(gap(now.apply(a), now.apply(b)) - 100) < 1e-6 && gap(now.apply(a), st->picks[0]) < 1e-6 && area->flow() == CanvasArea::Flow::None,
          QString("calibrated: the two points are 100 mm apart, the first stays (was %1 mm)").arg(gap(st->picks[0], st->picks[1])));
    next();
  });
  // 5. Align to model: a point of the picture onto a vertex of the box, another onto another.
  steps.push_back([=](std::function<void()> next) {
    view->benchDesignShot(prefix + ".fit3.png");
    st->world = canvasWorld();
    area->align();
    waitFor(area, [view] { return view->selectionFilter() == Viewport::SelFilter::Vertex; }, 5000, [=](bool) {
      QTimer::singleShot(600, area, [=] {  // the vertex filter reaches every body
        view->benchDesignShot(prefix + ".fit4.png");
        const opad::CanvasPlace p = editor->place();
        const opad::Vec3 to1{130, 0, 0}, to2{170, 0, 40};
        auto click = [&](const opad::Vec3& at) {
          const QPoint pt = view->widgetPoint(at);
          mouse(view, QEvent::MouseButtonPress, pt, Qt::LeftButton);
          mouse(view, QEvent::MouseButtonRelease, pt, Qt::NoButton);
        };
        click(p.plane.to_world(p.x - p.width / 4, p.y - p.height / 4));
        const auto first = area->flowPoints();
        click(to1);
        click(p.plane.to_world(p.x + p.width / 4, p.y + p.height / 4));
        const auto second = area->flowPoints();
        click(to2);
        trace::log(QString("bench: canvas: align picks %1, %2, flow %3").arg(first.size()).arg(second.size()).arg(int(area->flow())));
        if (!check(first.size() == 1 && second.size() == 3 && area->flow() == CanvasArea::Flow::None, "picture, vertex, picture, vertex: aligned")) return next();
        const opad::Mat4 was = opad::affine_inverse(st->world), now = canvasWorld();
        check(gap(second[1], to1) < 1e-6 && gap(now.apply(was.apply(second[0])), to1) < 1e-6 && gap(now.apply(was.apply(second[2])), to2) < 1e-6,
              "the picture's two points lie on the box's two vertices");
        next();
      });
    });
  });
  // 6. Lock: no handles, a drag moves nothing; unlocked again.
  steps.push_back([=](std::function<void()> next) {
    QCheckBox* lock = area->panel()->findChild<QCheckBox*>("canvasLock");
    lock->click();
    st->ops = doc->doc.ops.size();
    const opad::Mat4 before = canvasWorld();
    QPointF at;
    editor->gripPoint(CanvasEditor::Grip::Move, at);
    mouse(view, QEvent::MouseButtonPress, at, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, at + QPointF(30, 0), Qt::LeftButton);
    const bool dragging = editor->dragging();
    mouse(view, QEvent::MouseButtonRelease, at + QPointF(30, 0), Qt::NoButton);
    check(doc->node(st->canvas)->locked && editor->locked() && !dragging && same(canvasWorld(), before) && !area->field(0)->isEnabled() &&
              !doc->doc.ops.empty() && doc->doc.ops.back().type == "appearance",
          "locked: no handles, the drag moved nothing, X is off");
    lock->click();
    check(!doc->node(st->canvas)->locked, "unlocked");
    next();
  });
  // 6b. Moving snaps to the model: a corner let go a few pixels from a vertex of the box lands on it.
  steps.push_back([=](std::function<void()> next) {
    waitFor(area, [editor] { return editor->modelPoints() > 0; }, 10000, [=](bool found) {
      if (!check(found, QString("the model's snap points found on a worker (%1)").arg(editor->modelPoints()))) return next();
      view->benchDesignShot(prefix + ".fit5.png");
      const opad::CanvasPlace p = editor->place();
      const opad::Vec3 vertex{170, 0, 40}, corner = opad::canvas_points(canvasWorld(), p.body_w, p.body_h)[0];
      QPointF grip;
      editor->gripPoint(CanvasEditor::Grip::Move, grip);
      const QPointF to = grip + QPointF(view->widgetPoint(vertex) - view->widgetPoint(corner)) + QPointF(3, -2);
      mouse(view, QEvent::MouseButtonPress, grip, Qt::LeftButton);
      mouse(view, QEvent::MouseMove, (grip + to) / 2, Qt::LeftButton);
      mouse(view, QEvent::MouseMove, to, Qt::LeftButton);
      mouse(view, QEvent::MouseButtonRelease, to, Qt::NoButton);
      const opad::Vec3 landed = opad::canvas_points(canvasWorld(), p.body_w, p.body_h)[0];
      check(gap(landed, vertex) < 1e-6, QString("a corner let go near a vertex of the box lands on it (%1, %2, %3)").arg(landed[0]).arg(landed[1]).arg(landed[2]));
      next();
    });
  });
  // 7. Flags: flipped left-right (drawn mirrored), shown through the model, not selectable in the view.
  steps.push_back([=](std::function<void()> next) {
    area->run({{"action", "place"}, {"set", {{"angle", 0.0}}}});  // upright in its plane, the plane's x to the right on screen
    view->lookAt(editor->place().plane, true, false);
    QTimer::singleShot(500, area, [=] {
      int dark = 0;
      const QImage straight = view->grabImage().convertToFormat(QImage::Format_RGB32);
      straight.save(prefix + ".upright.png");
      const QPoint centre = view->widgetPoint(editor->place().plane.to_world(editor->place().x, editor->place().y)) * view->displayScale();
      const QPointF disc = blueCentre(straight, dark);
      const int decoded = view->rastersDecoded();
      area->panel()->findChild<QCheckBox*>("canvasFlipH")->click();
      waitFor(area, [=] { return view->rastersDecoded() > decoded && view->showsPicture(st->canvas); }, 10000, [=](bool ok) {
        QTimer::singleShot(500, area, [=] {
          int darkFlipped = 0;
          const QImage flipped = view->grabImage().convertToFormat(QImage::Format_RGB32);
          flipped.save(prefix + ".flipped.png");
          const QPointF mirrored = blueCentre(flipped, darkFlipped);
          trace::log(QString("bench: canvas: blue block at %1,%2 then %3,%4 (centre %5,%6, %7/%8 pixels)").arg(disc.x()).arg(disc.y()).arg(mirrored.x()).arg(mirrored.y())
                         .arg(centre.x()).arg(centre.y()).arg(dark).arg(darkFlipped));
          check(dark > 20 && disc.x() > centre.x() && disc.y() > centre.y(), "the picture is drawn upright: its blue block bottom right");
          check(ok && darkFlipped > 20 && mirrored.x() < centre.x() && mirrored.y() > centre.y() && doc->doc.ops.back().type == "edit",
                "Flip left-right: decoded again on a worker, the block drawn bottom left; an edit of the import");
          // Show through: centred over the box and seen from behind it, the box's middle is the box, then the picture.
          area->run({{"action", "place"}, {"set", {{"x", 150.0}, {"y", 20.0}, {"width", 120.0}}}});
          view->clearSelection();  // a selected canvas is in Topmost, the X-ray, whatever its flags
          view->standardView("back");
          view->benchDesignShot(prefix + ".behind.png");
          auto boxMiddle = [view] {
            const QImage frame = view->grabImage().convertToFormat(QImage::Format_RGB32);
            const QPoint at = view->widgetPoint({140, 20, 12}) * view->displayScale();  // off the handles' centre cross
            return std::make_pair(frame, frame.rect().contains(at) ? QColor(frame.pixel(at)) : QColor());
          };
          const auto hidden = boxMiddle();
          area->panel()->findChild<QCheckBox*>("canvasThrough")->click();
          area->panel()->findChild<QCheckBox*>("canvasSelectable")->click();
          waitFor(area, [=] {
            const opad::json s = view->benchLookState(st->canvas);
            return s.is_object() && s.value("layer", 0) == int(view->throughLayer()) && s.value("activated", 1) == 0;
          }, 5000, [=](bool shown) {
            const opad::json s = view->benchLookState(st->canvas);
            check(shown && view->throughLayer() != Graphic3d_ZLayerId_Topmost && s.value("depth_test", true) == false && s.value("depth_write", true) == false,
                  "shown through the model (its own layer: no depth test, no depth written) and not selectable in the view (no selection modes)");
            const auto through = boxMiddle();
            through.first.save(prefix + ".through.png");
            trace::log(QString("bench: canvas: the box's middle from behind %1, shown through %2").arg(hidden.second.name(), through.second.name()));
            check(hidden.second.isValid() && hidden.second.lightness() < 200 && through.second.lightness() > 235,
                  "from behind the box, the box hides the canvas until it is shown through: then the picture is drawn over it");
            // A selected body's X-ray (Topmost) still shows over a canvas shown through (it covered it in Topmost).
            std::string box;
            for (const auto& id : doc->scene.all_bodies())
              if (!opad::is_canvas(*doc->node(id))) box = id;
            view->selectNodes({box});
            waitFor(area, [=] { return view->benchLookState(box).value("selected", false); }, 5000, [=](bool selected) {
              const auto xray = boxMiddle();
              trace::log(QString("bench: canvas: the box selected behind it %1").arg(xray.second.name()));
              check(selected && xray.second.lightness() < 235, "a selected body behind it shows over it (its X-ray)");
              view->clearSelection();
              area->panel()->findChild<QCheckBox*>("canvasSelectable")->click();
              area->panel()->findChild<QCheckBox*>("canvasThrough")->click();
              next();
            });
          });
        });
      });
    });
  });
  // 8. Trace to sketch: a sketch on the canvas's plane with the dark shapes as curves.
  steps.push_back([=](std::function<void()> next) {
    const size_t sketches = doc->scene.sketches.size();
    QObject::connect(area, &CanvasArea::traced, area, [=](bool ok, const QString& error) {
      const bool made = ok && doc->scene.sketches.size() == sketches + 1 && doc->scene.sketches.back().geometry.value("entities", opad::json::array()).size() >= 2;
      check(made, "Trace to sketch: a new sketch with the picture's shapes " + error);
      next();
    }, Qt::SingleShotConnection);
    area->trace();
  });
  // 9. Replace: the same node shows a square picture, as wide as before.
  steps.push_back([=](std::function<void()> next) {
    const opad::CanvasPlace was = opad::canvas_place(doc->scene, st->canvas);
    QObject::connect(area, &CanvasArea::planDone, area, [=](bool ok, const QString& error) {
      const opad::CanvasPlace p = opad::canvas_place(doc->scene, st->canvas);
      check(ok && std::fabs(p.width - was.width) < 1e-6 && std::fabs(p.height - p.width) < 1e-6 && doc->node(st->canvas)->raster.value("px", opad::json())[0] == 300,
            "Replace: the same canvas shows the square picture, as wide as before " + error);
      next();
    }, Qt::SingleShotConnection);
    area->replace(square);
  });
  // 10. A sketch's backdrop becomes a canvas where it lay; the sketch keeps its lines.
  steps.push_back([=, &w](std::function<void()> next) {
    QFile file(photo);
    if (!file.open(QIODevice::ReadOnly)) return (void)check(false, "the photo"), next();
    opad::design::Sketch sk;
    sk.add_line(sk.add_point(0, 0), sk.add_point(30, 0));
    sk.images.push_back({{"id", sk.next_id()}, {"name", photo.toStdString()}, {"data", file.readAll().toBase64().toStdString()}, {"position", {10, 20}},
                         {"width", 80.0}, {"height", 40.0}, {"angle", 0.0}, {"opacity", 0.5}});
    w.m_design->applyOps({opad::design::make_sketch_op("Backdrop", {{"base", "xy"}}, sk.to_json())}, MainWindow::tr("sketch"), [=](bool ok, const QString& error) {
      if (!check(ok, "a sketch with a backdrop " + error)) return next();
      const std::string sketch = doc->scene.sketches.back().id;
      const size_t canvases = [doc] {
        size_t n = 0;
        for (const auto& id : doc->scene.all_bodies()) n += opad::is_canvas(*doc->node(id));
        return n;
      }();
      QObject::connect(area, &CanvasArea::planDone, area, [=](bool done, const QString& why) {
        size_t now = 0;
        for (const auto& id : doc->scene.all_bodies()) now += opad::is_canvas(*doc->node(id));
        const opad::SketchItem* s = doc->scene.sketch(sketch);
        check(done && now == canvases + 1 && s && s->geometry.value("images", opad::json::array()).empty() && s->geometry["entities"].size() == 1,
              "the backdrop is a canvas now, the sketch keeps its line " + why);
        next();
      }, Qt::SingleShotConnection);
      area->fromBackdrop(sketch);
    });
  });
  // 11. A picture dropped onto the window with a face selected: Insert canvas, the placer on its plane, centred on the face.
  steps.push_back([=, &w](std::function<void()> next) {
    area->finish();
    opad::Ref top;
    for (const auto& id : doc->scene.all_bodies())
      if (!opad::is_canvas(*doc->node(id)))
        for (int i = 0; i < 6 && top.body.empty(); ++i) {
          const opad::json box = opad::inspect_ref(doc->doc, doc->scene, {id, opad::Ref::Kind::Face, i}).value("bbox", opad::json());
          if (box.is_object() && std::fabs(box["size"][2].get<double>()) < 1e-9 && box["center"][2].get<double>() > 39) top = {id, opad::Ref::Kind::Face, i};
        }
    if (!check(!top.body.empty(), "the box's top face")) return next();
    view->setSelectionFilter(Viewport::SelFilter::Face);
    QTimer::singleShot(600, area, [=, &w] {
      view->selectRefs({top});
      if (!check(view->selection().size() == 1 && view->selection().front().kind == opad::Ref::Kind::Face, "the top face selected")) return next();
      DrawingPlacer* placer = area->placer();
      QObject::connect(placer, &DrawingPlacer::ready, area, [=] {
        const opad::Vec3 origin = placer->placement().apply({0, 0, 0}), normal = placer->placement().apply_dir({0, 0, 1});
        check(placer->picture() && gap(origin, {150, 20, 40}) < 1e-6 && gap(normal, {0, 0, 1}) < 1e-9,
              QString("Insert canvas on the selected face: the placer on its plane, centred on it (%1, %2, %3)").arg(origin[0]).arg(origin[1]).arg(origin[2]));
        placer->cancel();
        view->setSelectionFilter(Viewport::SelFilter::Body);
        next();
      }, Qt::SingleShotConnection);
      QMimeData mime;  // dropped onto the window, as from the file manager
      mime.setUrls({QUrl::fromLocalFile(photo)});
      QDropEvent drop(QPointF(w.width() / 2, w.height() / 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
      w.dropEvent(&drop);  // a hidden window takes no drop events through the application
    });
  });
  // 12. Selected in the browser: the canvas commands, its context menu and its Properties section.
  steps.push_back([=, &w](std::function<void()> next) {
    w.m_browser->selectIds({st->canvas});
    QTimer::singleShot(300, area, [=, &w] {
      check(w.action("canvas.calibrate")->isEnabled() && w.action("canvas.trace")->isEnabled() && !w.action("canvas.fromBackdrop")->isEnabled(),
            "the canvas commands follow the selection");
      QMenu menu;
      area->contextMenu(w.selectionContext(), menu);
      QStringList entries;
      for (QAction* a : menu.actions()) entries << (a->menu() ? a->menu()->title() + ": " + QStringList([&] { QStringList s; for (QAction* b : a->menu()->actions()) s << b->text(); return s; }()).join("/") : a->text());
      check(entries.contains(w.action("canvas.calibrate")->text()) && entries.join('|').contains("Flip left-right"), "its context menu: " + entries.join(", "));
      w.action("inspect.properties")->trigger();
      QTreeWidget* table = w.m_props->table();
      int header = -1, size = -1;
      for (int i = 0; i < table->topLevelItemCount(); ++i) {
        if (table->topLevelItem(i)->text(0) == "CANVAS") header = i;
        if (header >= 0 && size < 0 && table->topLevelItem(i)->text(0) == "Size") size = i;
      }
      check(header > 0 && size > header && table->topLevelItem(size)->text(1).contains("×"), QString("Properties: the Canvas section (%1)").arg(size > 0 ? table->topLevelItem(size)->text(1) : QString()));
      w.m_propsPanel->grab().save(prefix + ".properties.png");
      w.m_propsPanel->hide();
      next();
    });
  });
  auto runner = std::make_shared<std::function<void(size_t)>>();
  *runner = [steps, runner, st, area](size_t i) {
    if (i >= steps.size()) {
      area->finish();
      trace::log(st->ok ? "bench: canvas all PASS" : "bench: canvas FAIL");
      return QCoreApplication::exit(st->ok ? 0 : 2);
    }
    steps[i]([runner, i, area] { QTimer::singleShot(0, area, [runner, i] { (*runner)(i + 1); }); });
  };
  (*runner)(0);
  return true;
}
