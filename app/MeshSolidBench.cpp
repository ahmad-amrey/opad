#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"

// OPAD_BENCH_MESHSOLID=<prefix> (case mesh-to-solid in tools/bench_cases/meshsolid.py) on an STL of a faceted cylinder opened
// for editing: Mesh to solid opens its panel for the mesh, previews the result in place of the mesh (the mesh steps aside)
// with its match ("Match: 100.0%", a sketch of one curve extruded) without stalling the window; with Remove the mesh ticked,
// Create solid commits one undo step: a sketch, an Extrude and its body, the mesh gone; Undo brings the mesh back and takes
// the three away, Redo puts them back. Shots: <prefix>.panel.png (the panel) and <prefix>.png (the view with the preview).
OPAD_BENCH(OPAD_BENCH_MESHSOLID, meshsolid) {
  struct State {
    int step = 0, ticks = 0;
    std::string mesh;
  };
  auto st = std::make_shared<State>();
  for (const auto& id : w.m_doc->scene.all_bodies())
    if (w.m_doc->scene.node(id)->representation == "mesh") st->mesh = id;
  const QString prefix = value;
  MainWindow* win = &w;
  auto end = [](const QString& failure) {
    trace::log(failure.isEmpty() ? QStringLiteral("bench: mesh to solid PASS") : "bench: mesh to solid FAIL: " + failure);
    QCoreApplication::exit(failure.isEmpty() ? 0 : 2);
  };
  if (st->mesh.empty()) {
    end("the document has no mesh body");
    return true;
  }
  w.meshToSolid();
  auto* timer = new QTimer(&w);
  timer->setInterval(50);
  QObject::connect(timer, &QTimer::timeout, &w, [=] {
    auto fail = [&](const QString& why) {
      timer->stop();
      end(why);
    };
    if (++st->ticks > 1200) return fail(QString("timed out at step %1").arg(st->step));
    auto* panel = win->findChild<ToolPanel*>("meshSolidWizard");
    auto* report = panel ? panel->findChild<QLabel*>("meshSolidReport") : nullptr;
    auto* create = panel ? panel->findChild<QPushButton*>("primary") : nullptr;
    const auto& scene = win->m_doc->scene;
    switch (st->step) {
      case 0:  // the preview and its report
        if (!panel || !report || !create) return fail("the panel did not open");
        if (!create->isEnabled()) return;
        if (!report->text().contains("100.0%") || !report->text().contains(MainWindow::tr("A sketch of %L1 curves, extruded").arg(1)))
          return fail("the report does not say a one-curve extrusion matching the mesh: " + report->text());
        if (win->m_viewport->previewBodyCount() == 0) return fail("no preview is shown");
        trace::log("bench: mesh to solid: preview and report " + report->text().left(160));
        panel->grab().save(prefix + ".panel.png");
        win->m_viewport->grabImage().save(prefix + ".png");
        panel->findChildren<QCheckBox*>().front()->setChecked(true);  // Remove the mesh: the preview is made again
        st->step = 1;
        return;
      case 1:
        if (!create->isEnabled()) return;
        create->click();
        st->step = 2;
        return;
      case 2: {
        if (panel && panel->isVisible()) return;
        int solids = 0, meshes = 0;
        for (const auto& id : scene.all_bodies()) (scene.node(id)->representation == "mesh" ? meshes : solids)++;
        if (scene.sketches.size() != 1 || scene.features.size() != 2 || solids != 1 || meshes != 0)
          return fail(QString("after Create: %1 sketches, %2 features, %3 solids, %4 meshes").arg(scene.sketches.size()).arg(scene.features.size()).arg(solids).arg(meshes));
        win->m_doc->undo();
        if (!scene.sketches.empty() || !scene.features.empty() || !scene.node(st->mesh)) return fail("one Undo does not take the conversion back");
        win->m_doc->redo();
        if (scene.sketches.size() != 1 || scene.features.size() != 2) return fail("Redo does not put the conversion back");
        timer->stop();
        end({});
        return;
      }
    }
  });
  timer->start();
  return true;
}
