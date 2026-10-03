#include "MainWindow.hpp"

#include <QApplication>
#include <QTimer>
#include <functional>
#include <set>

#include "Jobs.hpp"
#include "Viewport.hpp"

// OPAD_BENCH_KICAD=<png>: a KiCad board opened as a viewer (tools/gui_benches.py writes it: two footprints sharing a model
// that only the "KiCad 3D model folders" setting finds, one whose model is missing, a mounting hole). It must show the
// board, the found model once per footprint from one body entry and a box for the missing one, keep the 2D layers
// hidden, save an iso frame, and, opened again without the setting, show boxes for all three.
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
  trace::log(QString("bench: kicad: phase %1: board %2, %3 model bodies from %4 entries, %5 boxes, %6 hidden layers, %7 of %8 bodies displayed, model folders %9")
                 .arg(phase).arg(board).arg(models).arg(keys.size()).arg(placeholders).arg(layers).arg(m_viewport->displayedCount()).arg(shown).arg(dirs.join(';')));
  if (!board || layers != 3 || m_viewport->displayedCount() != shown) return fail("board, layers or display");
  if (phase++ == 1) {
    if (models != 0 || placeholders != 3) return fail("without the setting every model should be a box");
    trace::log("bench: kicad board, shared model from the settings folder, box for a missing model, hidden layers, setting off -> boxes PASS");
    QCoreApplication::exit(0);
    return true;
  }
  if (models != 2 || keys.size() != 1 || placeholders != 1 || dirs.isEmpty()) return fail("the model in the settings folder was not shared by both footprints");
  m_viewport->standardView("iso");
  m_viewport->fitAll();
  QTimer::singleShot(800, this, [this, shot, fail] {
    if (!m_viewport->grabImage().save(shot)) return (void)fail("frame not saved");
    m_settings.setValue("kicad/modelDirs", QStringList());
    openPath(m_doc->viewing);  // the same board again: runBench comes back here for phase 1
  });
  return true;
}
