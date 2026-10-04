// Bench of the status bar for CAD work (UI-112): Ortho and Polar beside the other drafting toggles, their right-click
// menus, the cursor's coordinate readout (CoordinateReadout.cpp), Ortho in the sketch's Line tool, and its channels (UI-109):
// the prompt, status-bar messages that leave the path alone, the hover, all beside the progress strip.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "CoordinateReadout.hpp"
#include "Preferences.hpp"
#include "ProgressStrip.hpp"
#include "StatusRow.hpp"
#include "Units.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>
#include <QToolButton>

#include <cmath>

namespace {
QMenu* shownMenu(const QString& name) {
  for (QWidget* top : QApplication::topLevelWidgets())
    if (auto* m = qobject_cast<QMenu*>(top); m && m->isVisible() && m->objectName() == name) return m;
  return nullptr;
}
QAction* entry(QMenu* menu, const QString& text) {
  for (QAction* a : menu ? menu->actions() : QList<QAction*>())
    if (QString(a->text()).remove('&') == text) return a;
  return nullptr;
}
// The sketch's points as (x, y).
std::vector<std::pair<double, double>> points(const opad::json& geometry) {
  std::vector<std::pair<double, double>> out;
  for (const auto& p : geometry.value("points", opad::json::array())) out.emplace_back(p.value("x", 0.0), p.value("y", 0.0));
  return out;
}
bool hasPoint(const opad::json& geometry, double x, double y) {
  for (const auto& [px, py] : points(geometry))
    if (std::abs(px - x) < 1e-6 && std::abs(py - y) < 1e-6) return true;
  return false;
}
}  // namespace

// OPAD_BENCH_STATUSBAR=<prefix> (a box, a cylinder and the sketch "Plate"): the status bar reads, in reading order, Ortho
// (F8), Polar (F10), Extensions, Tracking, Grid snapping, the coordinate readout, the selection and the units chip; Ortho
// and Polar switch their settings; right-clicking Grid snapping offers its spacing (a step sets the grid and shows it),
// Polar its angle step, and both their page of Preferences at the row. The readout: on the XY plane where nothing is under
// the mouse (top view), on the model where the box is, live from mouse moves over the view, in the sketch's own X and Y
// while sketching, only X and Y in 2D mode. In the sketch, Ortho levels a line's second point; without it the point stays
// where it was clicked. <prefix>.status.png (the status bar), <prefix>.message.png and .busy.png (a message, a job), <prefix>.grid-menu.png.
OPAD_BENCH(OPAD_BENCH_STATUSBAR, statusbar) {
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: status bar: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; std::function<bool()> until; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn, std::function<bool()> until = {}) { steps->push_back({delay, std::move(fn), std::move(until)}); };
  auto idle = [&w] { return !w.m_jobs->busy(); };
  auto toggle = [&w](const char* id) { return w.statusBar()->findChild<QToolButton*>(QString("toggle.") + id); };
  CoordinateReadout* readout = w.m_readout;
  // UI-109: one channel each. A status-bar message no longer hides the path: it shows in the prompt while it lasts, then
  // the tool's prompt is back; what is under the mouse has its own label; both keep their room while a job shows.
  auto job = std::make_shared<QPointer<Job>>();
  add(800, [=, &w] {
    emit w.m_design->status("Extrude: pick a profile");
    w.statusBar()->showMessage("Bench note", 5000);
    check(w.m_statusPath->isVisible() && w.m_statusPrompt->text() == "Bench note" && w.statusBar()->currentMessage() == "Bench note",
          "a message leaves the path where it is and shows in the prompt");
    w.statusBar()->grab().save(prefix + ".message.png");
    w.statusBar()->clearMessage();
    check(w.m_statusPrompt->text() == "Extrude: pick a profile" && w.m_statusPath->isVisible(), "then the tool's prompt is back");
    emit w.m_viewport->hoverChanged("Box · Face 3");
    check(w.m_statusHover->text() == "Box · Face 3" && w.m_statusPrompt->text() == "Extrude: pick a profile", "what is under the mouse has its own label beside the prompt");
    w.m_jobs->backgroundNext();
    *job = w.m_jobs->begin("Bench: a long job");
  });
  add(900, [=, &w] {
    check(w.m_progress->isVisible() && w.m_statusHover->isVisible() && w.m_statusPrompt->isVisible() && w.m_statusPath->isVisible() &&
              w.m_statusHover->width() >= 50 && w.m_statusPrompt->width() >= 120,
          QString("with the progress strip showing, the prompt (%1 px) and the hover (%2 px) keep their room").arg(w.m_statusPrompt->width()).arg(w.m_statusHover->width()));
    w.statusBar()->grab().save(prefix + ".busy.png");
    if (*job) (*job)->finish(true);
    emit w.m_design->status(QString());
    emit w.m_viewport->hoverChanged(QString());
  });
  add(300, [=, &w] {
    const bool rtl = w.layoutDirection() == Qt::RightToLeft;
    QList<QWidget*> order{toggle("view.orthoSnap"), toggle("view.polarSnap"), toggle("view.extensions"), toggle("view.tracking"), toggle("view.gridSnap"), readout, w.m_statusSel, w.m_statusUnits};
    bool inOrder = !order.contains(nullptr);
    for (int i = 1; inOrder && i < order.size(); ++i) {
      const int a = order[i - 1]->mapTo(&w, QPoint()).x(), b = order[i]->mapTo(&w, QPoint()).x();
      inOrder = rtl ? b < a : b > a;
    }
    check(inOrder, "Ortho, Polar, Extensions, Tracking, Grid snapping, the readout, the selection, the units, in reading order");
    check(w.action("view.orthoSnap")->shortcut() == QKeySequence("F8") && w.action("view.polarSnap")->shortcut() == QKeySequence("F10"), "Ortho on F8, Polar on F10");
    const bool ortho = QSettings().value("view/orthoSnap", false).toBool();
    w.action("view.orthoSnap")->trigger();
    const bool polar = QSettings().value("sketch/snap/angle", true).toBool();
    w.action("view.polarSnap")->trigger();
    check(QSettings().value("view/orthoSnap").toBool() != ortho && QSettings().value("sketch/snap/angle").toBool() != polar, "they switch their settings");
    w.action("view.orthoSnap")->trigger();
    w.action("view.polarSnap")->trigger();
    // Right-click: Grid snapping.
    emit toggle("view.gridSnap")->customContextMenuRequested(QPoint(4, 4));
    QMenu* menu = shownMenu("toggleMenu");
    const QString one = units::compact(units::Kind::Length, units::fromDisplay(units::Kind::Length, 1));
    check(menu && menu->actions().first() == w.action("view.gridSnap") && menu->actions().contains(w.action("view.grid")) && entry(menu, one) &&
              entry(menu, QCoreApplication::translate("MainWindow", "Automatic")),
          "right-click on Grid snapping: the switch, the grid, the spacing (" + one + ")");
    if (menu) menu->grab().save(prefix + ".grid-menu.png");
    if (QAction* a = entry(menu, one)) a->trigger();
    check(std::abs(QSettings().value("view/gridSpacing").toDouble() - units::fromDisplay(units::Kind::Length, 1)) < 1e-9 && w.action("view.grid")->isChecked(),
          "a spacing sets the grid and shows it");
    if (menu) menu->close();
    w.m_viewport->configureGrid(0, QSettings().value("view/gridExtent", 100).toDouble());
    // Right-click: Polar.
    emit toggle("view.polarSnap")->customContextMenuRequested(QPoint(4, 4));
    menu = shownMenu("toggleMenu");
    const QString fortyFive = units::compact(units::Kind::Angle, 45);
    if (QAction* a = entry(menu, fortyFive)) a->trigger();
    check(QSettings().value("sketch/angleStep").toDouble() == 45 && w.action("view.polarSnap")->isChecked(), "Polar's menu sets the angle step (" + fortyFive + ")");
    QAction* settings = menu ? menu->actions().last() : nullptr;
    if (settings) settings->trigger();
    if (menu) menu->close();
    auto* dialog = w.findChild<PreferencesDialog*>("preferences");
    QWidget* row = dialog ? dialog->pageWidget("sketch")->findChild<QWidget*>("sketch/angleStep") : nullptr;
    check(dialog && dialog->isVisible() && dialog->page() == "sketch" && row && row->property("prefMatch").toBool(), "and leads to its row in Preferences");
    if (dialog) dialog->close();
    QSettings().setValue("sketch/angleStep", 15);
  }, idle);
  add(200, [=, &w] {
    // The readout: the top view, a corner of the view meets the XY plane only.
    w.m_viewport->standardView("top");
  });
  add(700, [=, &w] {
    readout->updateAt(QPointF(12, 12));
    check(readout->source() == CoordinateReadout::Source::Plane && std::abs(readout->point()[2]) < 1e-9 && readout->text().contains("XY") && readout->text().contains("Z "),
          "nothing under the cursor: the XY plane (" + readout->text() + ")");
    const std::string box = w.m_doc->scene.all_bodies().front();
    int x = 0, y = 0;
    const bool found = w.m_viewport->benchBodyPoint(box, x, y) && w.m_viewport->benchDetect(x, y);
    const double scale = w.m_viewport->displayScale();
    readout->updateAt(QPointF(x / scale, y / scale));
    check(found && readout->source() == CoordinateReadout::Source::Model && std::abs(readout->point()[2] - 10) < 1e-3 && readout->text().startsWith(QChar(0x202A) + QString("3D")),
          "on the box: the model point on its top face (" + readout->text() + ")");
    readout->clear();  // the move below must fill it again
    QMouseEvent move(QEvent::MouseMove, QPointF(40, 30), w.m_viewport->mapToGlobal(QPointF(40, 30)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w.m_viewport, &move);
  });
  add(150, [=, &w] {
    check(readout->text() != QString() && readout->source() != CoordinateReadout::Source::None, "live from mouse moves over the view (" + readout->text() + ")");
    w.statusBar()->grab().save(prefix + ".status.png");
    const auto& sketches = w.m_doc->scene.sketches;
    if (!sketches.empty()) w.m_design->editOp(sketches.front().id);
  });
  add(300, [=, &w] {
    const QPointF centre = QRectF(w.m_viewport->rect()).center();
    readout->updateAt(centre);
    double u = 0, v = 0;
    w.m_viewport->planePoint(centre, w.m_design->sketch()->frame(), u, v);
    check(readout->source() == CoordinateReadout::Source::Sketch && std::abs(readout->point()[0] - u) < 1e-9 && std::abs(readout->point()[1] - v) < 1e-9 && !readout->text().contains("Z "),
          "sketching: the sketch's own X and Y (" + readout->text() + ")");
    // Ortho in the Line tool: the second point levels with the first; without Ortho (and inference and Polar off) it stays.
    SketchEditor* sketch = w.m_design->sketch();
    QSettings().setValue("sketch/snap/inference", false);
    if (w.action("view.polarSnap")->isChecked()) w.action("view.polarSnap")->trigger();
    if (!w.action("view.orthoSnap")->isChecked()) w.action("view.orthoSnap")->trigger();
    auto line = [sketch](double x0, double y0, double x1, double y1) {
      sketch->setTool("line");
      for (const auto& [x, y] : {std::pair{x0, y0}, std::pair{x1, y1}}) {
        sketch->sketchMove(x, y, Qt::NoModifier, false);
        sketch->sketchPress(x, y, Qt::NoModifier);
        sketch->sketchRelease(x, y, Qt::NoModifier);
      }
      QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      sketch->sketchKey(&esc);
    };
    line(100, 100, 115, 103);
    check(hasPoint(sketch->geometry(), 115, 100), "Ortho: the line from (100, 100) towards (115, 103) ends level, at (115, 100)");
    w.action("view.orthoSnap")->trigger();
    line(100, 130, 115, 133);
    check(hasPoint(sketch->geometry(), 115, 133) && !hasPoint(sketch->geometry(), 115, 130), "without Ortho it ends where it was clicked, (115, 133)");
    QSettings().remove("sketch/snap/inference");
    w.action("view.polarSnap")->trigger();
  }, [&w] { return w.m_design->sketchActive() && !w.m_design->sketch()->busy(); });
  add(300, [=, &w] {
    if (w.m_design->sketchActive()) w.m_design->sketch()->end();  // the bench answers no question: leave the sketch
    w.m_doc->setRollback({});
    w.action("view.2d")->setChecked(true);
  });
  add(600, [=, &w] {
    readout->updateAt(QPointF(12, 12));
    check(readout->source() == CoordinateReadout::Source::View && !readout->text().contains("Z ") && readout->text().contains("X ") && readout->text().contains("Y "),
          "2D mode: X and Y of the view plane (" + readout->text() + ")");
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(w.m_viewport, &leave);
    check(readout->text().isEmpty(), "empty once the mouse leaves the view");
    w.action("view.2d")->setChecked(false);
    trace::log(QString("bench: status bar: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t, int)>>();
  *next = [&w, steps, next, check](size_t i, int waited) {
    if (i >= steps->size()) return;
    QTimer::singleShot(waited ? 50 : (*steps)[i].delay, &w, [steps, next, check, i, waited] {
      const Step& s = (*steps)[i];
      if (s.until && !s.until() && waited < 300) return (*next)(i, waited + 1);
      try { s.fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1, 0);
    });
  };
  (*next)(0, 0);
  return true;
}
