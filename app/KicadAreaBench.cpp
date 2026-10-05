// The KiCad area in the running app (KicadArea.hpp): Insert KiCad PCB, repeated models meshed once, the sync preview from the
// changed board's toast and an incremental sync from its footer; a board projected into a sketch that follows a sync
// (UI-134); small parts hidden while the view moves. Cases in tools/bench_cases/kicad.py.
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <cmath>
#include <set>
#include <utility>

#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BenchRegistry.hpp"
#include "CheckPanel.hpp"
#include "DesignController.hpp"
#include "KicadArea.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "PlanePicker.hpp"
#include "Theme.hpp"
#include "Toast.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"

namespace {
struct Checks {
  QString name;
  bool all = true;
  explicit Checks(const QString& n) : name(n) {}
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: %1: %2 %3").arg(name, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
  void finish() { QCoreApplication::exit(all ? 0 : 2); }
};

// Polls `ready` every 100 ms until it holds or `ms` have passed, then calls `then` with the outcome.
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
  timer->start(100);
}

bool copyOver(const QString& from, const QString& to) {  // written anew: its time is now
  QFile in(from), out(to);
  if (!in.open(QIODevice::ReadOnly)) return false;
  const QByteArray bytes = in.readAll();
  return out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(bytes) == bytes.size();
}

template <class Area>
Area* areaOf(MainWindow& w) {
  return w.window()->findChild<Area*>();
}

std::string kicadImport(const AppDocument* doc) {
  for (const auto& o : doc->doc.ops)
    if (o.type == "import" && o.data.contains("kicad")) return o.id;
  return {};
}

// The component of a footprint by its reference ("R2" -> "R2 chip"), and its first body.
std::string part(const AppDocument* doc, const QString& ref) {
  for (const auto& [id, n] : doc->scene.nodes)
    if (n.kind == opad::Node::Kind::Component && QString::fromStdString(n.name).section(' ', 0, 0) == ref) return id;
  return {};
}
std::string bodyOf(const AppDocument* doc, const std::string& component) {
  const opad::Node* n = doc->node(component);
  return n && !n->children.empty() ? n->children.front() : std::string();
}

bool sameColor(const std::array<double, 3>& a, const QColor& c) { return std::abs(a[0] - c.redF()) < 0.02 && std::abs(a[1] - c.greenF()) < 0.02 && std::abs(a[2] - c.blueF()) < 0.02; }
}  // namespace

// OPAD_BENCH_KICAD_AREA=<prefix> on an empty document beside board.kicad_pcb (R1..R3 sharing one chip model, J1 a connector,
// two mounting holes) and its next version next.kicad_pcb. Insert KiCad PCB (Design > Assemble and the File menu) asks the
// board's options (answered) and links it; the three chips show one shape meshed once. The board written anew: its toast
// offers Show changes, which opens the sync preview (R2 and H1 moved, J1's model and footprint changed, R4 added; R2 tinted the moved
// colour, J1 the changed one); Sync in its footer syncs it as one step: only the board's and the new connector's shapes are
// meshed, R2 is relocated (same node), R4 shown with the chips' shape. Frames: <prefix>.png, .preview.png, .tinted.png.
OPAD_BENCH(OPAD_BENCH_KICAD_AREA, kicad_area) {
  static bool ran = false;  // each load that ends comes back here (the insert's too)
  if (std::exchange(ran, true)) return true;
  auto require = std::make_shared<Checks>("kicad-area");
  auto settled = [&w] {  // displayed and settled: no load, display or look job, every visible body shown
    int visible = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) visible += w.m_doc->scene.effectively_visible(id) && !w.m_doc->node(id)->body_missing;
    return !w.m_doc->loading && !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && !w.m_viewport->looksPending() && w.m_viewport->displayedCount() == visible;
  };
  const QString prefix = value;
  KicadArea* kicad = areaOf<KicadArea>(w);
  AssetsArea* assets = areaOf<AssetsArea>(w);
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  const QString dir = QFileInfo(doc->path()).absolutePath();
  const CommandInfo* insert = w.m_commands.find("kicad.insert");
  (*require)(kicad && assets && insert && w.action("kicad.insert")->isEnabled() && w.m_commands.inWorkspace("design").contains("kicad.insert") && insert->menuPath == "file",
             "Insert KiCad PCB on Design > Assemble and in the File menu: " + (insert ? insert->menuPath : QString("none")));
  if (!kicad || !assets) return require->finish(), true;
  auto answered = std::make_shared<bool>(false);
  auto* answer = new QTimer(&w);  // the board's options, asked as Import asks them: OK
  QObject::connect(answer, &QTimer::timeout, &w, [answer, answered] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (!dialog || dialog->objectName() != "kicadDialog") return;
    for (auto* b : dialog->findChild<QDialogButtonBox*>()->buttons())
      if (dialog->findChild<QDialogButtonBox*>()->buttonRole(b) == QDialogButtonBox::AcceptRole) b->click();
    *answered = true;
    answer->stop();
    answer->deleteLater();
  });
  answer->start(100);
  kicad->insert(dir + "/board.kicad_pcb");
  AssetMonitor* monitor = assets->monitor();
  auto state = [monitor](const std::string& import) {
    const opad::json* s = monitor->state(import);
    return s ? s->value("state", std::string()) : std::string();
  };
  waitFor(&w, [=, &w] { return !kicadImport(doc).empty() && settled() && state(kicadImport(doc)) == "ok" && !monitor->checking(); }, 30000, [=, &w](bool loaded) {
    const std::string import = kicadImport(doc);
    const opad::json* asset = monitor->asset(import) ? &monitor->asset(import)->asset : nullptr;
    (*require)(loaded && *answered && asset && asset->value("kind", "") == "kicad_pcb" && asset->value("storage", "") == "linked",
               "inserted: the board's options answered, linked as a KiCad board");
    if (!loaded || import.empty()) return require->finish();
    // Repeated models: one shape for the three chips, meshed once.
    std::set<std::string> keys, shown;
    for (const char* ref : {"R1", "R2", "R3"}) keys.insert(doc->node(bodyOf(doc, part(doc, ref)))->body_key);
    for (const auto& id : doc->scene.all_bodies())
      if (doc->scene.effectively_visible(id)) shown.insert(doc->node(id)->body_key);
    const auto stats = v->displayStats();
    (*require)(keys.size() == 1 && doc->scene.instance_count.at(*keys.begin()) == 3 && stats.meshed == int(shown.size()) && stats.displayed == v->displayedCount(),
               QString("repeated models instanced: three chips share one shape; %1 meshes for %2 bodies shown").arg(stats.meshed).arg(stats.displayed));
    v->standardView("iso");
    v->fitAll();
    QTimer::singleShot(600, &w, [=, &w] {
      (*require)(v->grabImage().save(prefix + ".png"), "frame");
      const std::string r2 = part(doc, "R2"), r2body = bodyOf(doc, r2), j1body = bodyOf(doc, part(doc, "J1"));
      const opad::Mat4 r2was = doc->scene.world(r2body);
      (*require)(copyOver(dir + "/next.kicad_pcb", dir + "/board.kicad_pcb"), "the board written anew");
      auto toastWith = [&w](const QString& action) -> Toast* {
        const QList<Toast*> toasts = w.m_toasts->toasts();
        Toast* t = toasts.isEmpty() ? nullptr : toasts.back();
        return t && t->text().contains("board.kicad_pcb changed since the last sync") && t->actionButton() && t->actionButton()->text() == action ? t : nullptr;
      };
      waitFor(&w, [=] { return state(import) == "changed" && toastWith("Show changes"); }, 15000, [=, &w](bool offered) {
        (*require)(offered, "the changed board's toast offers Show changes");
        if (!offered) return require->finish();
        toastWith("Show changes")->actionButton()->click();
        waitFor(&w, [=] { return kicad->previewPanel()->isVisible() && kicad->previewImport() == import && !v->looksPending(); }, 15000, [=, &w](bool shown) {
          QStringList groups, moved;
          QTreeWidget* list = kicad->previewList();
          for (int i = 0; i < list->topLevelItemCount(); ++i) {
            groups << list->topLevelItem(i)->text(0);
            if (list->topLevelItem(i)->text(0).startsWith("Moved"))
              for (int k = 0; k < list->topLevelItem(i)->childCount(); ++k) moved << list->topLevelItem(i)->child(k)->text(0);
          }
          (*require)(shown && groups == QStringList({"Moved (2)", "3D model changed (1)", "Footprint changed (1)", "Added (1)"}) && moved == QStringList({"R2", "H1"}),
                     "the sync preview lists " + groups.join(", ") + " (moved: " + moved.join(", ") + ")");
          PanelFooter* footer = kicad->previewFooter();
          (*require)(footer->primary()->isEnabled() && footer->primaryText() == "Sync" && footer->hint()->text() == "5 changes · 2 unchanged",
                     "its footer: " + footer->primaryText() + ", " + footer->hint()->text());
          const Tokens& t = theme::current();
          (*require)(sameColor(v->bodyLook(r2body).color, t.diffMoved) && sameColor(v->bodyLook(j1body).color, t.diffModified) &&
                         sameColor(v->shownLook(r2body).color, t.diffMoved),
                     "the moved part tinted the moved colour, the changed one the changed colour");
          (*require)(kicad->previewPanel()->grab().save(prefix + ".preview.png") && v->grabImage().save(prefix + ".tinted.png"), "preview frames");
          const auto before = v->displayStats();
          const size_t ops = doc->doc.ops.size();
          footer->primary()->click();  // Sync
          (*require)(!kicad->previewPanel()->isVisible() && !sameColor(v->bodyLook(r2body).color, t.diffMoved), "Sync closes the preview and its tints");
          waitFor(&w, [=, &w] { return doc->doc.ops.size() > ops && !assets->busy() && state(import) == "ok" && !monitor->checking() && settled(); }, 30000, [=, &w](bool synced) {
            const auto after = v->displayStats();
            const opad::Mat4 r2now = doc->scene.world(r2body);
            const bool kept = doc->node(r2) && doc->node(r2body) && doc->node(r2body)->body_key == doc->node(bodyOf(doc, part(doc, "R1")))->body_key;
            const std::string r4 = bodyOf(doc, part(doc, "R4"));
            (*require)(synced && kept && !r4.empty() && doc->node(r4)->body_key == doc->node(r2body)->body_key && std::abs(r2now.at(0, 3) - r2was.at(0, 3) - 2) < 1e-6 &&
                           std::abs(r2now.at(1, 3) - r2was.at(1, 3) + 2) < 1e-6,
                       "synced: R2 kept its node and shape and moved 2, -2; R4 shows the chips' shape");
            (*require)(after.meshed - before.meshed == 2 && after.displayed - before.displayed == 3 && after.relocated - before.relocated >= 1,
                       QString("incremental sync: %1 shapes meshed (the board, the new connector), %2 bodies shown anew (them and R4), %3 relocated")
                           .arg(after.meshed - before.meshed).arg(after.displayed - before.displayed).arg(after.relocated - before.relocated));
            require->finish();
          });
        });
      });
    });
  });
  return true;
}

// OPAD_BENCH_KICAD_PROJECT=<prefix> on a document linking board.kicad_pcb (J1, H1, H2) beside next.kicad_pcb (outline notched,
// H1 moved, J1 turned). A sketch on XY: Project KiCad board (Sketch > Reference) offers the outline, the mounting holes and the
// parts (filtered by typing); outline, holes and J1 projected as linked references (10 curves), the sketch finished and its
// outline extruded (Lid). The board changed: its sync preview lists the design it affects (the sketch: the outline projected
// again, the holes and J1 moving; Lid recomputed with another body), a row selects its sketch, and Sync commits the plan
// previewed at once. The sketch follows the board without an error (12 lines, H1's circle where it went, J1's outline turned),
// Lid with it. Frames: <prefix>.dialog.png, <prefix>.png, .preview.png, .synced.png.
OPAD_BENCH(OPAD_BENCH_KICAD_PROJECT, kicad_project) {
  static bool ran = false;  // each load that ends comes back here (the insert's too)
  if (std::exchange(ran, true)) return true;
  auto require = std::make_shared<Checks>("kicad-project");
  auto settled = [&w] {  // displayed and settled: no load, display or look job, every visible body shown
    int visible = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) visible += w.m_doc->scene.effectively_visible(id) && !w.m_doc->node(id)->body_missing;
    return !w.m_doc->loading && !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && !w.m_viewport->looksPending() && w.m_viewport->displayedCount() == visible;
  };
  const QString prefix = value;
  KicadArea* kicad = areaOf<KicadArea>(w);
  AssetsArea* assets = areaOf<AssetsArea>(w);
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  DesignController* design = w.m_design;
  const QString dir = QFileInfo(doc->path()).absolutePath();
  const std::string import = kicadImport(doc);
  (*require)(kicad && assets && !import.empty(), "a document linking a KiCad board");
  if (!kicad || !assets || import.empty()) return require->finish(), true;
  AssetMonitor* monitor = assets->monitor();
  auto ok = [monitor, import] {
    const opad::json* s = monitor->state(import);
    return s && s->value("state", "") == "ok" && !monitor->checking();
  };
  auto curves = [import](const opad::json& geometry, int& lines, int& circles, bool& linked) {
    lines = circles = 0;
    linked = true;
    for (const auto& e : opad::design::Sketch::from_json(geometry).entities) {
      lines += e.type == opad::design::SkEntity::Type::Line;
      circles += e.type == opad::design::SkEntity::Type::Circle;
      linked = linked && !e.source.is_null() && e.source.value("ref", opad::json()).value("asset", "") == import;
    }
  };
  auto done = std::make_shared<bool>(false);
  QObject::connect(assets, &AssetsArea::done, &w, [done](const QString& what, const std::string&, bool okay, const QString&, const opad::json&) {
    if (what == "sync") *done = okay;
  });

  // 4. Synced: the sketch follows the board, Lid with it.
  auto synced = [=, &w](bool okay) {
    const opad::SketchItem& item = doc->scene.sketches.front();
    int lines = 0, circles = 0;
    bool linked = false;
    curves(item.geometry, lines, circles, linked);
    const opad::design::Sketch g = opad::design::Sketch::from_json(item.geometry);
    bool h1 = false;  // H1 from page (104, 104) to (108, 106): board (-22, 14)
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (const auto& e : g.entities) {
      if (e.type == opad::design::SkEntity::Type::Circle) h1 = h1 || (std::abs(g.point(e.p[0])->x + 22) < 1e-6 && std::abs(g.point(e.p[0])->y - 14) < 1e-6);
      if (e.source.value("ref", opad::json()).value("kicad", "") == "part")
        for (int p : e.p) x0 = std::min(x0, g.point(p)->x), x1 = std::max(x1, g.point(p)->x), y0 = std::min(y0, g.point(p)->y), y1 = std::max(y1, g.point(p)->y);
    }
    (*require)(okay && item.error.empty() && lines == 12 && circles == 2 && linked && h1 && std::abs(x1 - x0 - 7) < 1e-6 && std::abs(y1 - y0 - 9) < 1e-6,
               QString("after the sync the sketch follows the board: %1 lines (the notch), H1 moved %2, J1 turned (%3 x %4)%5")
                   .arg(lines).arg(h1 ? "yes" : "no").arg(x1 - x0).arg(y1 - y0).arg(item.error.empty() ? QString() : ": " + QString::fromStdString(item.error)));
    const opad::Feature* lid = doc->scene.features.empty() ? nullptr : &doc->scene.features.front();
    (*require)(lid && lid->error.empty(), "Lid recomputed without an error");
    v->isolate({item.id});
    QTimer::singleShot(600, &w, [=] {
      (*require)(v->grabImage().save(prefix + ".synced.png"), "frame after the sync");
      v->isolate({});
      require->finish();
    });
  };

  // 3. The preview: the design the sync affects, a row selecting its sketch, Sync committing what was previewed.
  auto previewed = [=, &w](bool shown) {
    const std::string sketchId = doc->scene.sketches.front().id;
    const QString sketchName = QString::fromStdString(doc->scene.sketches.front().name);
    QTreeWidget* list = kicad->previewList();
    QTreeWidgetItem* affected = nullptr;
    for (int i = 0; i < list->topLevelItemCount(); ++i)
      if (list->topLevelItem(i)->text(0).startsWith("Design affected")) affected = list->topLevelItem(i);
    QStringList rows;  // "name: line; line" per sketch or feature (its first row named, one row per line)
    for (int k = 0; affected && k < affected->childCount(); ++k)
      if (!affected->child(k)->text(0).isEmpty()) rows << affected->child(k)->text(0) + ": " + affected->child(k)->text(1);
      else if (!rows.isEmpty()) rows.back() += "; " + affected->child(k)->text(1);
    const bool two = rows.size() == 2;
    const QString sketchRow = two ? rows[0] : QString(), featureRow = two ? rows[1] : QString();
    (*require)(shown && two && affected->text(0) == "Design affected (2)" && sketchRow.startsWith(sketchName + ": ") && sketchRow.contains("The board outline is projected again") &&
                   sketchRow.contains("The mounting holes move") && sketchRow.contains("J1 moves") && featureRow == "Lid: Recomputed: its body changes" && kicad->previewPlanned(),
               "the preview lists the design the sync affects: " + sketchRow + " | " + featureRow);
    (*require)(kicad->previewPanel()->grab().save(prefix + ".preview.png"), "preview frame");
    if (two) emit list->itemClicked(affected->child(0), 0);
    (*require)(w.m_browser->selectedIds() == std::vector<std::string>{sketchId}, "a click on the sketch's row selects the sketch");
    const size_t ops = doc->doc.ops.size();
    kicad->previewFooter()->primary()->click();  // Sync
    (*require)(doc->doc.ops.size() > ops && *done && !kicad->previewPanel()->isVisible(), "Sync commits the plan previewed at once (the board is not read again)");
    waitFor(&w, [=] { return *done && !doc->designBusy && settled(); }, 30000, synced);
  };

  // 2. Lid extruded from the projected outline, the board written anew, its preview.
  auto changed = [=, &w] {
    auto extruded = std::make_shared<int>(0);
    const opad::json profile = {{"sketch", doc->scene.sketches.front().id}, {"at", {10, 5}}};
    design->applyOps({opad::design::make_feature_op("extrude", "Lid", {{"profiles", opad::json::array({profile})}, {"distance", "2 mm"}, {"operation", "new"}})}, "extrude",
                     [extruded](bool okay, const QString&) { *extruded = okay ? 1 : -1; });
    waitFor(&w, [=] { return *extruded && !doc->designBusy && settled(); }, 20000, [=, &w](bool) {
      (*require)(*extruded == 1 && doc->scene.features.size() == 1, "the projected outline extruded (Lid)");
      (*require)(copyOver(dir + "/next.kicad_pcb", dir + "/board.kicad_pcb"), "the board written anew");
      waitFor(&w, [=] { return monitor->state(import) && monitor->state(import)->value("state", "") == "changed" && !monitor->checking(); }, 15000, [=, &w](bool found) {
        (*require)(found, "the board changed");
        kicad->preview(import);
        waitFor(&w, [=] { return kicad->previewPanel()->isVisible() && kicad->previewImport() == import && !v->looksPending(); }, 20000, previewed);
      });
    });
  };

  // 1. A sketch on XY, the board projected into it from the dialog, finished.
  waitFor(&w, [=, &w] { return settled() && ok(); }, 20000, [=, &w](bool shown) {
    (*require)(shown, "the board shown");
    design->startSketch();
    waitFor(&w, [=] { return design->pickingPlane(); }, 5000, [=, &w](bool) {
      design->planePicker()->choose({{"base", "xy"}});
      waitFor(&w, [=] { return design->planePicker()->positioning(); }, 10000, [=, &w](bool positioning) {
        if (positioning) design->planePicker()->apply();
        waitFor(&w, [=] { return design->sketchActive(); }, 10000, [=, &w](bool sketching) {
          w.updateCommands();
          bool onTab = false;  // the Sketch tab, first in Design while sketching: its Reference group
          if (RibbonPage* page = w.m_ribbon->page("design.sketch"))
            for (QToolButton* b : page->findChildren<QToolButton*>()) onTab = onTab || b->defaultAction() == w.action("kicad.project");
          (*require)(sketching && w.action("kicad.project")->isEnabled() && onTab && w.m_ribbon->tabIds().value(0) == "design.sketch",
                     "a sketch on XY: Project KiCad board offered in the Sketch tab's Reference group");
          QDialog* dialog = kicad->project();
          auto* outline = dialog ? dialog->findChild<QCheckBox*>("kicadOutline") : nullptr;
          auto* holes = dialog ? dialog->findChild<QCheckBox*>("kicadHoles") : nullptr;
          auto* parts = dialog ? dialog->findChild<QListWidget*>("kicadParts") : nullptr;
          auto* filter = dialog ? dialog->findChild<QLineEdit*>("kicadPartFilter") : nullptr;
          (*require)(outline && holes && parts && filter && outline->isChecked() && holes->isChecked() && holes->text().contains("(2)") && parts->count() == 1 &&
                         parts->item(0)->text().startsWith("J1"),
                     "the dialog: outline and both mounting holes checked, J1 listed");
          if (!dialog || !parts || parts->count() != 1) return require->finish();
          filter->setText("U9");
          const bool hidden = parts->item(0)->isHidden();
          filter->setText("j1");
          (*require)(hidden && !parts->item(0)->isHidden(), "typing filters the parts");
          parts->item(0)->setCheckState(Qt::Checked);
          (*require)(dialog->grab().save(prefix + ".dialog.png"), "dialog frame");
          dialog->findChild<QPushButton*>("kicadProjectOk")->click();
          SketchEditor* sketch = design->sketch();
          waitFor(&w, [=] { return !sketch->busy() && !doc->designBusy && opad::design::Sketch::from_json(sketch->geometry()).entities.size() >= 10; }, 15000, [=, &w](bool projected) {
            int lines = 0, circles = 0;
            bool linked = false;
            curves(sketch->geometry(), lines, circles, linked);
            (*require)(projected && lines == 8 && circles == 2 && linked, QString("projected: %1 lines (outline, J1), %2 circles (holes), all linked to the board").arg(lines).arg(circles));
            design->finishSketch();
            waitFor(&w, [=] { return !design->sketchActive() && !doc->designBusy && doc->scene.sketches.size() == 1; }, 20000, [=, &w](bool finished) {
              (*require)(finished, "the sketch finished");
              if (!finished) return require->finish();
              v->isolate({doc->scene.sketches.front().id});  // the sketch alone: on the board's bottom face the board hides it
              v->standardView("top");
              v->fitAll();
              QTimer::singleShot(600, &w, [=] {
                (*require)(v->grabImage().save(prefix + ".png"), "frame");
                v->isolate({});
                changed();
              });
            });
          });
        });
      });
    });
  });
  return true;
}

// OPAD_BENCH_SMALL_PARTS=<prefix> on a document linking a board with twelve 2 mm chips and a 9 mm connector. Hide small parts
// while navigating (View menu, the View tabs) turned on (3 mm), R1 selected: a camera move hides the other eleven chips (the
// connector, the board and R1 stay), and they come back once the view is still; off, a move hides nothing. Frames:
// <prefix>.moving.png, .still.png.
OPAD_BENCH(OPAD_BENCH_SMALL_PARTS, small_parts) {
  static bool ran = false;  // each load that ends comes back here (the insert's too)
  if (std::exchange(ran, true)) return true;
  auto require = std::make_shared<Checks>("small-parts");
  auto settled = [&w] {  // displayed and settled: no load, display or look job, every visible body shown
    int visible = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) visible += w.m_doc->scene.effectively_visible(id) && !w.m_doc->node(id)->body_missing;
    return !w.m_doc->loading && !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && !w.m_viewport->looksPending() && w.m_viewport->displayedCount() == visible;
  };
  const QString prefix = value;
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  QAction* hide = w.action("view.hideSmallParts");
  const CommandInfo* info = w.m_commands.find("view.hideSmallParts");
  (*require)(hide && hide->isCheckable() && !hide->isChecked() && info && info->menuPath == "view" && w.m_commands.inWorkspace("review").contains("view.hideSmallParts"),
             "Hide small parts while navigating in the View menu and on the View tabs, off by default");
  if (!hide) return require->finish(), true;
  waitFor(&w, [=, &w] { return settled() && w.m_viewport->displayedCount() >= 14; }, 20000, [=, &w](bool shown) {
    (*require)(shown, QString("the board shown: %1 bodies").arg(v->displayedCount()));
    std::vector<std::string> chips;
    for (int i = 1; i <= 12; ++i) chips.push_back(bodyOf(doc, part(doc, QString("R%1").arg(i))));
    const std::string j1 = bodyOf(doc, part(doc, "J1"));
    hide->trigger();
    (*require)(hide->isChecked() && std::abs(v->smallPartFilter() - 3) < 1e-9 && w.m_settings.value("view/hideSmallParts").toBool(), "turned on: parts under 3 mm, remembered");
    v->selectNodes({chips[0]});
    v->standardView("iso");
    v->fitAll();
    QTimer::singleShot(400, &w, [=, &w] {
      v->standardView("front");  // a move, as a frame reports it
      v->smallPartsCameraMoved();
      waitFor(&w, [=] { return v->smallPartsHidden() && !v->looksPending(); }, 5000, [=, &w](bool hidden) {
        int gone = 0;
        for (size_t i = 1; i < chips.size(); ++i) gone += !v->shownLook(chips[i]).visible;
        (*require)(hidden && v->smallPartCount() == 11 && gone == 11 && v->shownLook(chips[0]).visible && v->shownLook(j1).visible,
                   QString("moving: %1 chips hidden, the selected one and the connector shown").arg(gone));
        (*require)(v->grabImage().save(prefix + ".moving.png"), "frame while moving");
        waitFor(&w, [=] { return !v->smallPartsHidden() && !v->looksPending(); }, 3000, [=, &w](bool back) {
          int seen = 0;
          for (const auto& c : chips) seen += v->shownLook(c).visible;
          (*require)(back && seen == 12, QString("still for 300 ms: all %1 chips shown again").arg(seen));
          (*require)(v->grabImage().save(prefix + ".still.png"), "frame when still");
          hide->trigger();
          v->standardView("top");
          v->smallPartsCameraMoved();
          (*require)(!hide->isChecked() && v->smallPartFilter() == 0 && !v->smallPartsHidden(), "off: a move hides nothing");
          require->finish();
        });
      });
    });
  });
  return true;
}

// OPAD_BENCH_KICAD_CLEARANCE=<prefix> on a document linking a board (J1, a 3.5 mm tall connector) under a lid 0.5 mm over J1 and
// a wall against the lid's side. Check clearance to board (Review > Inspect, Design > KiCad, the board's context menu) with the
// lid selected: at 1 mm one pair, the lid and J1 0.5 mm apart (J1 on the board and the wall on the lid are no pairs), its row
// selecting both and measuring the gap; at 0.4 mm (remembered) nothing; closing the panel clears the measurement; with nothing
// selected every other visible solid counts, still that one pair. Frames: <prefix>.png, .panel.png.
OPAD_BENCH(OPAD_BENCH_KICAD_CLEARANCE, kicad_clearance) {
  static bool ran = false;  // each load that ends comes back here
  if (std::exchange(ran, true)) return true;
  auto require = std::make_shared<Checks>("kicad-clearance");
  auto settled = [&w] {  // displayed and settled: no load, display or look job, every visible body shown
    int visible = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) visible += w.m_doc->scene.effectively_visible(id) && !w.m_doc->node(id)->body_missing;
    return !w.m_doc->loading && !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && !w.m_viewport->looksPending() && w.m_viewport->displayedCount() == visible;
  };
  const QString prefix = value;
  KicadArea* kicad = areaOf<KicadArea>(w);
  AssetsArea* assets = areaOf<AssetsArea>(w);
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  QAction* check = w.action("kicad.clearance");
  const std::string import = kicadImport(doc);
  (*require)(kicad && assets && check && !import.empty() && w.m_commands.inWorkspace("review").contains("kicad.clearance"),
             "a board linked; Check clearance to board on Review > Inspect");
  if (!kicad || !assets || !check || import.empty()) return require->finish(), true;
  AssetMonitor* monitor = assets->monitor();
  auto body = [doc](const std::string& name) {
    for (const auto& [id, n] : doc->scene.nodes)
      if (n.kind == opad::Node::Kind::Body && n.name == name) return id;
    return std::string();
  };
  auto ready = std::make_shared<int>(0);  // the last check: 1 done, -1 failed
  QObject::connect(kicad, &KicadArea::clearanceReady, &w, [ready](bool okay) { *ready = okay ? 1 : -1; });
  CheckPanel* checks = kicad->clearanceChecks();
  auto status = [checks] { return checks->findChild<QLabel*>("secondary")->text(); };
  waitFor(&w, [=] { return settled() && monitor->state(import) && monitor->state(import)->value("state", "") == "ok" && !monitor->checking(); }, 20000, [=, &w](bool shown) {
    const std::string lid = body("Lid"), wall = body("Wall"), j1 = bodyOf(doc, part(doc, "J1"));
    (*require)(shown && !lid.empty() && !wall.empty() && !j1.empty(), "the board, the lid and the wall shown");
    w.m_browser->selectIds({lid});
    w.updateCommands();
    (*require)(check->isEnabled(), "the lid selected: Check clearance to board enabled");
    check->trigger();
    (*require)(kicad->clearancePanel()->isVisible() && status() == "Checking…" && checks->findingCount() == 0, "the panel opens and checks on a worker: " + status());
    waitFor(&w, [=] { return *ready != 0; }, 20000, [=, &w](bool) {
      const opad::json r = kicad->lastClearance();
      const opad::json first = r.value("items", opad::json::array()).empty() ? opad::json::object() : r["items"][0];
      (*require)(*ready == 1 && checks->findingCount() == 1 && r.value("too_close", 0) == 1 && r.value("interferences", 1) == 0 && first.value("a", "") == lid &&
                     first.value("b", "") == j1 && std::abs(first.value("distance_mm", 0.0) - 0.5) < 1e-3 && std::abs(r.value("clearance_mm", 0.0) - 1) < 1e-9 &&
                     checks->findChild<QListWidget*>()->item(0)->text() == "Lid and J1 conn: 0.500 mm apart",
                 "at 1 mm one pair, the lid and J1 0.5 mm apart (J1 on the board, the wall on the lid not counted): " + status() + " " +
                     (checks->findingCount() ? checks->findChild<QListWidget*>()->item(0)->text() : QString()));
      checks->activate(0);
      auto picked = [v] {  // selecting is sliced: one object per step
        std::set<std::string> out;
        for (const auto& ref : v->selection()) out.insert(ref.body);
        return out;
      };
      waitFor(&w, [=] { return picked() == std::set<std::string>{lid, j1}; }, 5000, [=, &w](bool both) {
      (*require)(both && !v->measurementCaptions().isEmpty(), "its row selects the lid and J1 and measures the gap: " + v->measurementCaptions().join(", "));
      v->standardView("front");
      v->fitAll();
      QTimer::singleShot(600, &w, [=, &w] {
        (*require)(v->grabImage().save(prefix + ".png") && kicad->clearancePanel()->grab().save(prefix + ".panel.png"), "frames");
        checks->findChild<QDoubleSpinBox*>()->setValue(units::toDisplay(units::Kind::Length, 0.4));
        *ready = 0;
        checks->findChild<QPushButton*>("primary")->click();  // Check
        waitFor(&w, [=] { return *ready != 0; }, 20000, [=, &w](bool) {
          (*require)(*ready == 1 && checks->findingCount() == 0 && kicad->lastClearance().value("status", "") == "clear" &&
                         std::abs(w.m_settings.value("kicad/clearance").toDouble() - 0.4) < 1e-9,
                     "at 0.4 mm nothing, the gap remembered: " + status());
          kicad->clearancePanel()->hide();
          (*require)(v->measurementCaptions().isEmpty(), "closing the panel clears the measurement");
          w.m_settings.setValue("kicad/clearance", 1.0);
          v->clearSelection();
          w.m_browser->selectIds({});
          *ready = 0;
          check->trigger();
          waitFor(&w, [=] { return *ready != 0; }, 20000, [=](bool) {
            const opad::json r = kicad->lastClearance();
            (*require)(*ready == 1 && r.value("too_close", 0) == 1 && r.value("bodies", 0) >= 4 && checks->findingCount() == 1,
                       QString("nothing selected: %1 solids checked, still one pair").arg(r.value("bodies", 0)));
            kicad->clearancePanel()->hide();
            require->finish();
          });
        });
      });
      });
    });
  });
  return true;
}

// OPAD_BENCH_ASSET_PREVIEW=<prefix> on a document linking part.step (a 10 mm cube) beside next.step (12 mm long), with a box
// Cover beside it. An interference check of the part and the cover stored as a feature (Fit); the file written anew: the
// part's context menu offers Preview sync, which lists the part changed and Fit under Design affected, the part tinted the
// changed colour; Sync commits that plan at once and the part takes its new shape. Frame: <prefix>.preview.png.
OPAD_BENCH(OPAD_BENCH_ASSET_PREVIEW, asset_preview) {
  static bool ran = false;  // each load that ends comes back here
  if (std::exchange(ran, true)) return true;
  auto require = std::make_shared<Checks>("asset-preview");
  auto settled = [&w] {  // displayed and settled: no load, display or look job, every visible body shown
    int visible = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) visible += w.m_doc->scene.effectively_visible(id) && !w.m_doc->node(id)->body_missing;
    return !w.m_doc->loading && !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && !w.m_viewport->looksPending() && w.m_viewport->displayedCount() == visible;
  };
  const QString prefix = value;
  KicadArea* kicad = areaOf<KicadArea>(w);
  AssetsArea* assets = areaOf<AssetsArea>(w);
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  DesignController* design = w.m_design;
  const QString dir = QFileInfo(doc->path()).absolutePath();
  std::string import;
  for (const auto& o : doc->doc.ops)
    if (o.type == "import" && o.data.contains("asset")) import = o.id;
  (*require)(kicad && assets && !import.empty(), "a document linking a STEP file");
  if (!kicad || !assets || import.empty()) return require->finish(), true;
  AssetMonitor* monitor = assets->monitor();
  auto state = [monitor, import] { return monitor->state(import) && !monitor->checking() ? monitor->state(import)->value("state", "") : std::string(); };
  auto done = std::make_shared<bool>(false);
  QObject::connect(assets, &AssetsArea::done, &w, [done](const QString& what, const std::string&, bool okay, const QString&, const opad::json&) {
    if (what == "sync") *done = okay;
  });
  auto fit = [doc]() -> const opad::Feature* {
    for (const auto& f : doc->scene.features)
      if (f.name == "Fit") return &f;
    return nullptr;
  };
  waitFor(&w, [=] { return settled() && state() == "ok"; }, 20000, [=, &w](bool shown) {
    std::string part, cover;
    for (const auto& [id, n] : doc->scene.nodes)
      if (n.kind == opad::Node::Kind::Body && n.source_op == import) part = id;
      else if (n.kind == opad::Node::Kind::Body && n.name == "Cover") cover = id;
    (*require)(shown && !part.empty() && !cover.empty(), "the part and the cover shown");
    const std::string key = doc->node(part)->body_key;
    auto fitted = std::make_shared<int>(0);
    design->applyOps({opad::design::make_feature_op("interference", "Fit", {{"bodies", {part, cover}}, {"clearance", "1 mm"}})}, "interference",
                     [fitted](bool okay, const QString&) { *fitted = okay ? 1 : -1; });
    waitFor(&w, [=] { return *fitted && !doc->designBusy && settled(); }, 20000, [=, &w](bool) {
      (*require)(*fitted == 1 && fit() && fit()->error.empty(), "an interference check of the part and the cover stored (Fit)");
      (*require)(copyOver(dir + "/next.step", dir + "/part.step"), "the file written anew");
      waitFor(&w, [=] { return state() == "changed"; }, 15000, [=, &w](bool changed) {
        SelectionContext selection;
        selection.ids = {part};
        QMenu menu;
        assets->contextMenu(selection, menu);
        QAction* show = nullptr;
        for (QAction* a : menu.actions())
          if (a->text() == "Preview sync…") show = a;
        (*require)(changed && show, "the changed file's context menu offers Preview sync");
        if (!show) return require->finish();
        show->trigger();
        waitFor(&w, [=] { return kicad->previewPanel()->isVisible() && kicad->previewImport() == import && !v->looksPending(); }, 20000, [=, &w](bool open) {
          QTreeWidget* list = kicad->previewList();
          QStringList groups, rows;
          for (int i = 0; i < list->topLevelItemCount(); ++i) {
            groups << list->topLevelItem(i)->text(0);
            for (int k = 0; k < list->topLevelItem(i)->childCount(); ++k) rows << list->topLevelItem(i)->child(k)->text(0) + ": " + list->topLevelItem(i)->child(k)->text(1);
          }
          const bool partRow = list->topLevelItemCount() > 0 && list->topLevelItem(0)->childCount() == 1 && list->topLevelItem(0)->child(0)->data(0, Qt::UserRole).toString().toStdString() == part;
          (*require)(open && groups == QStringList({"Changed (1)", "Design affected (1)"}) && partRow && rows.contains("Fit: Recomputed") && kicad->previewPlanned() &&
                         kicad->previewFooter()->primary()->isEnabled(),
                     "the preview lists the part changed and the check it affects: " + groups.join(", ") + " | " + rows.join(", "));
          (*require)(sameColor(v->bodyLook(part).color, theme::current().diffModified), "the part tinted the changed colour");
          (*require)(kicad->previewPanel()->grab().save(prefix + ".preview.png"), "preview frame");
          const size_t ops = doc->doc.ops.size();
          kicad->previewFooter()->primary()->click();  // Sync
          (*require)(doc->doc.ops.size() > ops && *done, "Sync commits the plan previewed at once");
          waitFor(&w, [=] { return !doc->designBusy && settled() && state() == "ok"; }, 20000, [=](bool synced) {
            (*require)(synced && doc->node(part) && doc->node(part)->body_key != key && fit() && fit()->error.empty(), "synced: the part has its new shape, Fit recomputed");
            require->finish();
          });
        });
      });
    });
  });
  return true;
}
