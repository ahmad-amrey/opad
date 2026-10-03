#include "MainWindow.hpp"

#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>

#include <functional>
#include <map>

#include "AppDocument.hpp"
#include "Jobs.hpp"
#include "Viewport.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"

// OPAD_BENCH_EXPORT=<prefix>: a 60 x 40 x 10 plate with a 10 mm hole through it. The Export dialog offers DXF, SVG and
// DWG for the solid (they were refused before), shows its 2D view row only while one of those is chosen, and exports the
// front view with hidden lines as DXF on a worker (the call returns while the job runs); the file read back has the
// plate's outline on Visible (4 lines) and the hole's sides on Hidden (2). Then SVG of the current camera (iso): the
// hole's rims come out as arcs, no hidden layer. The dialog opens again with those choices. <prefix>.dialog.png is the
// dialog, <prefix>.front.dxf and <prefix>.iso.svg the files.
bool MainWindow::benchExport() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_EXPORT");
  if (prefix.isEmpty()) return false;
  bool ok = true;
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: export: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto settle = [&](const std::function<bool()>& done, int ms) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  // Runs `fn` on the Export dialog from inside its own event loop (it is modal), then answers it.
  const auto withDialog = [&](const std::function<bool(QDialog*)>& fn) {
    QTimer::singleShot(100, this, [this, fn] {
      auto* d = findChild<QDialog*>("exportDialog");
      if (!d) return;
      if (fn(d)) d->accept();
      else d->reject();
    });
    exportDialog({});
  };
  const auto radio = [](QDialog* d, const char* format) {
    for (auto* r : d->findChildren<QRadioButton*>())
      if (r->property("format").toString() == format) return r;
    return static_cast<QRadioButton*>(nullptr);
  };
  // Edges per layer of a 2D file read back.
  const auto layers = [](const QString& file) {
    std::map<std::string, int> edges;
    opad::Document doc = opad::Document::create();
    opad::import_file(doc, std::filesystem::path(file.toStdU16String()));
    const opad::Scene scene = opad::resolve(doc);
    for (const auto& id : scene.all_bodies())
      for (TopExp_Explorer e(opad::node_world_shape(doc, scene, id), TopAbs_EDGE); e.More(); e.Next()) ++edges[scene.node(id)->name];
    return edges;
  };
  try {
    m_doc->newDocument();
    const std::string plate = m_doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}})["body_ids"][0];
    double x0, y0, z0, x1, y1, z1;
    opad::node_world_bbox(m_doc->doc, m_doc->scene, plate).Get(x0, y0, z0, x1, y1, z1);
    const auto mm = [](double v) { return QString::number(v, 'f', 3).toStdString() + " mm"; };
    m_doc->run("feature", {{"kind", "cylinder"},
                           {"inputs", {{"plane", {{"origin", {0, 0, z0 - 5}}, {"normal", {0, 0, 1}}}}, {"x", mm((x0 + x1) / 2)}, {"y", mm((y0 + y1) / 2)},
                                       {"diameter", "10 mm"}, {"height", "30 mm"}, {"operation", "cut"}}}});
    settle([&] { return !m_jobs->busy(); }, 20000);
    check(m_doc->scene.all_bodies().size() == 1, "a plate with a hole");

    // DXF of the front view with hidden lines.
    const QString dxf = prefix + ".front.dxf";
    QFile::remove(dxf);
    qputenv("OPAD_BENCH_EXPORT_OUT", dxf.toUtf8());
    m_lastExport = opad::json();
    withDialog([&](QDialog* d) {
      auto* view = d->findChild<QComboBox*>("export.view");
      auto* hidden = d->findChild<QCheckBox*>("export.hidden");
      check(view && hidden && radio(d, "dxf") && radio(d, "dxf")->isEnabled() && radio(d, "svg")->isEnabled() && radio(d, "dwg")->isEnabled(),
            "DXF, SVG and DWG are offered for a solid");
      if (!view || !hidden) return false;
      radio(d, "step")->click();
      check(!view->isVisibleTo(d), "STEP shows no 2D view row");
      radio(d, "dxf")->click();
      check(view->isVisibleTo(d) && view->count() == 8 && hidden->isVisibleTo(d), "DXF shows the view row (8 views, hidden lines)");
      view->setCurrentIndex(view->findData("front"));
      hidden->setChecked(true);
      QString summary;
      for (auto* l : d->findChildren<QLabel*>())
        if (l->objectName() == "secondary" && l->text().contains("2D view")) summary = l->text();
      check(!summary.isEmpty(), "the summary names the view: " + summary);
      d->grab().save(prefix + ".dialog.png");
      return true;
    });
    check(m_jobs->busy() && m_lastExport.is_null(), "the export runs as a job, the dialog's call has returned");
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    check(m_lastExport.value("layers", opad::json::object()).value("Visible", 0) == 4 && m_lastExport["layers"].value("Hidden", 0) == 2,
          "front view: 4 visible and 2 hidden lines " + QString::fromStdString(m_lastExport.dump()));
    const auto front = layers(dxf);
    check(front.count("Visible") && front.at("Visible") == 4 && front.count("Hidden") && front.at("Hidden") == 2, "the DXF reads back with them on their layers");

    // SVG of the current camera, no hidden lines.
    m_viewport->setCameraJson(opad::Camera::preset("iso").to_json());
    const QString svg = prefix + ".iso.svg";
    QFile::remove(svg);
    qputenv("OPAD_BENCH_EXPORT_OUT", svg.toUtf8());
    m_lastExport = opad::json();
    withDialog([&](QDialog* d) {
      radio(d, "svg")->click();
      auto* view = d->findChild<QComboBox*>("export.view");
      view->setCurrentIndex(view->findData("camera"));
      d->findChild<QCheckBox*>("export.hidden")->setChecked(false);
      return true;
    });
    settle([&] { return !m_lastExport.is_null(); }, 60000);
    const auto dir = m_lastExport.value("view", opad::json::object()).value("dir", opad::json::array());
    const bool iso = dir.size() == 3 && std::abs(dir[0].get<double>() + dir[1].get<double>()) < 1e-6 * std::abs(dir[0].get<double>()) &&
                     std::abs(dir[0].get<double>() - dir[2].get<double>()) < 1e-6 * std::abs(dir[0].get<double>());
    check(iso && m_lastExport.value("entities", opad::json::object()).value("ELLIPSE", 0) >= 2 && !m_lastExport["layers"].contains("Hidden"),
          "the current (iso) view as SVG: the hole's rims as arcs, no hidden layer");
    const auto round = layers(svg);
    check(round.count("Visible") && !round.count("Hidden"), "the SVG reads back");

    // The choices are kept.
    withDialog([&](QDialog* d) {
      radio(d, "dxf")->click();
      check(d->findChild<QComboBox*>("export.view")->currentData().toString() == "camera" && !d->findChild<QCheckBox*>("export.hidden")->isChecked(),
            "the dialog opens with the last view and hidden-line choice");
      return false;
    });
  } catch (const std::exception& e) {
    check(false, QString("error: ") + e.what());
  }
  qunsetenv("OPAD_BENCH_EXPORT_OUT");
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
