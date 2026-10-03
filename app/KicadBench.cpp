#include "MainWindow.hpp"

#include <QApplication>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <set>

#include "Jobs.hpp"
#include "Viewport.hpp"
#include "opad/kicad_pcb.hpp"

// OPAD_BENCH_KICAD=<png>: a KiCad board opened as a viewer (tools/gui_benches.py writes it: two footprints sharing a model
// that only the "KiCad 3D model folders" setting finds, one whose model is missing, one whose model is KiCad's library's,
// served from a local copy through OPAD_KICAD_MODELS_URL, a mounting hole). It must show the board, the found model once
// per footprint from one body entry and boxes for the missing ones, keep the 2D layers hidden; download the library model
// (setting kicad/download=always) and read the board again with it; save an iso frame; opened again without the folder
// setting, show boxes for those two and the downloaded model.
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
    default:
      if (models != 1 || placeholders != 3) return fail("without the folder setting only the downloaded model should be found");
      trace::log("bench: kicad board, shared model from the settings folder, boxes for missing models, library model downloaded and shown, hidden layers, "
                 "setting off -> boxes PASS");
      QCoreApplication::exit(0);
      return true;
  }
}
