// The KiCad area in the running app (KicadArea.hpp): Insert KiCad PCB, repeated models meshed once, the sync preview from the
// changed board's toast and an incremental sync from its footer. Cases in tools/bench_cases/kicad.py.
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
#include "KicadArea.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"
#include "Toast.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"

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


