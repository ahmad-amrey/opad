#include "MainWindow.hpp"

#include <QApplication>
#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <set>

#include "Jobs.hpp"
#include "KicadBoards.hpp"
#include "Viewport.hpp"
#include "opad/kicad_pcb.hpp"
#include "opad/mesh.hpp"

// OPAD_BENCH_COLORS=<png>: an OBJ cube (tools/gui_benches.py writes it) whose top is in a gold material of its own and the
// rest in Kd 0.439 grey, opened as a viewer (UI-74). The body must keep the grey as the file shows it (0.439: taken linear
// it was 0.162 and drew the model nearly black), carry the top as a face colour and draw it in a group of its own; the
// frame (<png>) must show gold and grey. Recoloured red, the body keeps its gold top: groups and frame (<png>.red.png).
bool MainWindow::benchColors() {
  const QString shot = qEnvironmentVariable("OPAD_BENCH_COLORS");
  if (shot.isEmpty()) return false;
  static bool ran = false;
  if (std::exchange(ran, true)) return true;
  auto fail = [](const QString& why) {
    trace::log("bench: colors FAIL: " + why);
    QCoreApplication::exit(2);
    return true;
  };
  const auto bodies = m_doc->scene.all_bodies();
  if (!m_doc->browse || bodies.size() != 1) return fail("one body in viewer mode expected");
  const std::string id = bodies.front();
  const opad::Node* n = m_doc->scene.node(id);
  const opad::FaceColors faces = opad::face_colors(m_doc->doc, n->body_key);
  const int painted = int(std::count_if(faces.face.begin(), faces.face.end(), [](int c) { return c >= 0; }));
  auto alike = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::abs(a[0] - b[0]) < 0.02 && std::abs(a[1] - b[1]) < 0.02 && std::abs(a[2] - b[2]) < 0.02;
  };
  const std::array<double, 3> grey{0.439, 0.439, 0.439}, gold{1.0, 0.766, 0.336}, red{1.0, 0.0, 0.0};
  auto drawn = [this, id, alike](const std::array<double, 3>& c) {
    const auto colors = m_viewport->drawnColors(id);
    return std::any_of(colors.begin(), colors.end(), [&](const auto& d) { return alike(d, c); });
  };
  // Pixels of a hue: gold (warm, blue well below red) and red (green and blue well below red), whatever the shading.
  auto pixels = [](const QImage& image, bool goldish) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x) {
        const QColor c = image.pixelColor(x, y);
        const double r = c.redF(), g = c.greenF(), b = c.blueF();
        count += goldish ? (r > 0.35 && r - b > 0.2 && g - b > 0.1 && r >= g) : (r > 0.3 && r - g > 0.25 && r - b > 0.25 && g < 0.25);
      }
    return count;
  };
  trace::log(QString("bench: colors: body colour %1 %2 %3, %4 face colours on %5 faces, drawn in %6 groups")
                 .arg(n->color[0]).arg(n->color[1]).arg(n->color[2]).arg(faces.colors.size()).arg(painted).arg(m_viewport->drawnColors(id).size()));
  if (!n->has_color || !alike(n->color, grey)) return fail("the grey material was not kept as the file shows it");
  if (faces.colors.size() != 1 || painted != 1 || !alike(faces.colors[0], gold)) return fail("the gold top is not a face colour");
  if (m_viewport->drawnColors(id).size() != 2 || !drawn(grey) || !drawn(gold)) return fail("not drawn as a grey and a gold group");
  m_viewport->standardView("iso");
  m_viewport->fitAll();
  QTimer::singleShot(800, this, [this, shot, fail, pixels, drawn, id, gold, red, grey] {
    const QImage before = m_viewport->grabImage();
    const int goldBefore = pixels(before, true);
    trace::log(QString("bench: colors: %1 gold pixels").arg(goldBefore));
    if (!before.save(shot) || goldBefore < 500) return (void)fail("the gold top does not show");
    m_doc->run("appearance", opad::json{{"target", id}, {"color", {1.0, 0.0, 0.0}}});  // a view change, allowed in viewer mode
    QTimer::singleShot(800, this, [this, shot, fail, pixels, drawn, gold, red, grey, goldBefore] {
      const QImage after = m_viewport->grabImage();
      const int goldAfter = pixels(after, true), redAfter = pixels(after, false);
      trace::log(QString("bench: colors: recoloured red: %1 gold pixels, %2 red pixels").arg(goldAfter).arg(redAfter));
      if (!after.save(QString(shot).replace(".png", ".red.png"))) return (void)fail("frame not saved");
      if (!drawn(red) || !drawn(gold) || drawn(grey)) return (void)fail("the groups did not follow the body's new colour");
      if (redAfter < 500 || goldAfter < goldBefore / 2) return (void)fail("the red body or its gold top does not show");
      trace::log("bench: colors OBJ material kept as shown, own face colour drawn as its own group, body recoloured keeps it PASS");
      QCoreApplication::exit(0);
    });
  });
  return true;
}

// OPAD_BENCH_KICAD=<png>: a KiCad board opened as a viewer (tools/gui_benches.py writes it: two footprints sharing a model
// that only the "KiCad 3D model folders" setting finds, one whose model is missing, one whose model is KiCad's library's,
// served from a local copy through OPAD_KICAD_MODELS_URL, a mounting hole). It must show the board, the found model once
// per footprint from one body entry and boxes for the missing ones, keep the 2D layers hidden; download the library model
// (setting kicad/download=always) and read the board again with it; save an iso frame; opened again without the folder
// setting, show boxes for those two and the downloaded model; the KiCad boards dialog (Settings) then turns the components
// off and puts the origin on KiCad's page, and the board read again must follow (its pictures: <png>.import.png, .dialog.png).
bool MainWindow::benchKicad() {
  const QString shot = qEnvironmentVariable("OPAD_BENCH_KICAD");
  if (shot.isEmpty()) return false;
  static int phase = 0;
  auto fail = [](const QString& why) {
    trace::log("bench: kicad FAIL: " + why);
    QCoreApplication::exit(2);
    return true;
  };
  const opad::Scene& s = m_doc->scene;
  const opad::Op* import = nullptr;
  for (const auto& o : m_doc->doc.ops)
    if (o.type == "import" && o.data.contains("kicad")) import = &o;
  if (!m_doc->browse || !import) return fail("not a KiCad board in viewer mode");
  int placeholders = 0, models = 0;
  std::set<std::string> keys;
  std::function<void(const opad::json&)> walk = [&](const opad::json& nodes) {
    for (const auto& n : nodes) {
      if (n.value("placeholder", false)) ++placeholders;
      else if (n.value("type", "") == "body" && n.value("representation", "solid") == "solid" && n.value("name", "") != "Board") ++models, keys.insert(n.value("key", ""));
      if (n.contains("children")) walk(n["children"]);
    }
  };
  walk(import->data["nodes"]);
  int shown = 0, layers = 0;
  bool board = false;
  for (const auto& id : s.all_bodies()) {
    const bool visible = s.effectively_visible(id);
    shown += visible;
    layers += !visible && s.node(id)->representation == "drawing2d";
    board = board || (visible && s.node(id)->name == "Board");
  }
  const QStringList dirs = m_settings.value("kicad/modelDirs").toStringList();
  const int downloadable = m_doc->lastLoad.contains("info") ? m_doc->lastLoad["info"].value("downloadable", 0) : -1;
  const bool downloaded = std::filesystem::exists(opad::kicad_download_dir() / "Bench.3dshapes" / "library.step");
  trace::log(QString("bench: kicad: phase %1: board %2, %3 model bodies from %4 entries, %5 boxes, %6 hidden layers, %7 of %8 bodies displayed, "
                     "%9 downloadable, downloaded %10, model folders %11")
                 .arg(phase).arg(board).arg(models).arg(keys.size()).arg(placeholders).arg(layers).arg(m_viewport->displayedCount()).arg(shown)
                 .arg(downloadable).arg(downloaded).arg(dirs.join(';')));
  if (!board || layers != 3 || m_viewport->displayedCount() != shown) return fail("board, layers or display");
  switch (phase++) {
    case 0:  // the settings folder's model on both footprints, boxes for the other two; the library one is fetched
      if (models != 2 || keys.size() != 1 || placeholders != 2 || dirs.isEmpty()) return fail("the model in the settings folder was not shared by both footprints");
      if (downloadable != 1 || downloaded || m_settings.value("kicad/download").toString() != "always") return fail("one library model to download");
      offerKicadModels();  // a job on a worker; when done, the board is read again and runBench comes back for phase 1
      if (!m_jobs->busy()) return fail("no download job");
      QTimer::singleShot(30000, this, [fail] {
        if (phase == 1) fail("the board was not read again after the download");
      });
      return true;
    case 1:
      if (!downloaded || models != 3 || keys.size() != 2 || placeholders != 1 || downloadable != 0) return fail("the downloaded model is not shown");
      m_viewport->standardView("iso");
      m_viewport->fitAll();
      QTimer::singleShot(800, this, [this, shot, fail] {
        if (!m_viewport->grabImage().save(shot)) return (void)fail("frame not saved");
        m_settings.setValue("kicad/modelDirs", QStringList());
        m_settings.setValue("kicad/download", "never");
        openPath(m_doc->viewing);  // the same board again: runBench comes back here for phase 2
      });
      return true;
    case 2: {
      if (models != 1 || placeholders != 3) return fail("without the folder setting only the downloaded model should be found");
      trace::log("bench: kicad board, shared model from the settings folder, boxes for missing models, library model downloaded and shown, hidden layers, "
                 "setting off -> boxes PASS");
      auto* asked = new KicadDialog(this, true);  // as File > Import asks
      asked->show();
      const QPushButton* go = nullptr;
      for (auto* b : asked->findChild<QDialogButtonBox*>()->buttons())
        if (asked->findChild<QDialogButtonBox*>()->buttonRole(b) == QDialogButtonBox::AcceptRole) go = qobject_cast<QPushButton*>(b);
      if (!go || go->text() != tr("Import") || !asked->grab().save(QString(shot).replace(".png", ".import.png"))) return fail("import dialog");
      asked->reject();
      asked->deleteLater();
      auto* dialog = new KicadDialog(this, false);  // Settings > KiCad boards
      dialog->show();
      auto* components = dialog->findChild<QCheckBox*>("components");
      auto* origin = dialog->findChild<QComboBox*>("origin");
      auto* height = dialog->findChild<QDoubleSpinBox*>("placeholderHeight");
      if (!components || !origin || !height || !components->isChecked() || origin->currentData() != "auto" || height->value() != 1.0) return fail("dialog defaults");
      components->setChecked(false);
      origin->setCurrentIndex(origin->findData("page"));
      height->setValue(2.5);
      QTimer::singleShot(300, this, [this, dialog, shot, fail] {
        if (!dialog->grab().save(QString(shot).replace(".png", ".dialog.png"))) return (void)fail("dialog picture");
        for (auto* b : dialog->findChild<QDialogButtonBox*>()->buttons())
          if (dialog->findChild<QDialogButtonBox*>()->buttonRole(b) == QDialogButtonBox::AcceptRole) b->click();
        dialog->deleteLater();
        if (m_settings.value("kicad/components").toBool() || m_settings.value("kicad/origin").toString() != "page" || m_settings.value("kicad/placeholderHeight").toDouble() != 2.5)
          return (void)fail("the dialog did not save");
        openPath(m_doc->viewing);  // runBench comes back for phase 3
      });
      return true;
    }
    default: {
      const opad::json origin = import->data["kicad"].value("origin", opad::json());
      if (models != 0 || placeholders != 0 || origin != opad::json::array({0.0, 0.0})) return fail("the dialog's choices were not used: " + QString::fromStdString(origin.dump()));
      trace::log("bench: kicad import and settings dialog: components off and page origin read back PASS");
      QCoreApplication::exit(0);
      return true;
    }
  }
}
