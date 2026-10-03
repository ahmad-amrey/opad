// The linked-file UI (UI-68) in the running app: badges, read-only parts, Properties, the context menu, the monitor's toast,
// Sync all, a badge click, a missing file located, pack (LFS) and embed; the asset look in the view. Cases "asset-sync" and
// "asset-look" in tools/bench_cases/assets.py.
#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>

#include <Bnd_Box.hxx>

#include <algorithm>
#include <cmath>
#include <set>

#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DrawingPlacer.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "Viewport.hpp"
#include "opad/geometry.hpp"

namespace {
struct Checks {
  QString name = "asset-sync";
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: %1: %2 %3").arg(name, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
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

// A file written anew (now as its time, so its hash is not one remembered for its path, size and time).
bool rewrite(const QString& from, const QString& to) {
  QFile in(from), out(to);
  if (!in.open(QIODevice::ReadOnly)) return false;
  const QByteArray bytes = in.readAll();
  return out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(bytes) == bytes.size();
}

QTreeWidgetItem* rowOf(QTreeWidget* tree, const std::string& id) {
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if ((*it)->data(0, browser::kIdRole).toString().toStdString() == id) return *it;
  return nullptr;
}

double width(AppDocument* doc, const std::string& node) {
  const Bnd_Box b = opad::node_world_bbox(doc->doc, doc->scene, node);
  if (b.IsVoid()) return -1;
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return x1 - x0;
}
}  // namespace

// OPAD_BENCH_ASSET_SYNC=<prefix> on a document in a git work tree whose .gitattributes stores assets/** with LFS, linking
// parts/part.step and parts/second.step (in sync), with next versions of both in next/. Opened: each file's top node has the
// link icon, an italic name and the in-sync badge, its parts are read-only (no rename editor, rename and reparent refused,
// the top node renamed), Properties shows the Linked file section, the context menu its entries, both files are watched.
// Both files changed on disk: the monitor marks them changed (Sync badge) and one toast offers Sync all, which syncs both
// as two undo steps keeping their node ids and taking the new geometry (wider part); undo and redo. The part changed again:
// its badge's click syncs it. The second file moved away: missing badge, located (synced from its new place). The part
// packed into the project (assets/, LFS badge), the second embedded (editable, stored). Import choice and reveal command.
// Frames: <prefix>.browser.png, .changed.png, .syncing.png, .properties.png, .final.png.
OPAD_BENCH(OPAD_BENCH_ASSET_SYNC, asset_sync) {
  auto require = std::make_shared<Checks>();
  const QString prefix = value;
  AssetsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
  AssetMonitor* monitor = area ? area->monitor() : nullptr;
  AppDocument* doc = w.m_doc;
  auto importOf = [doc](const std::string& source) {
    for (const auto& o : doc->doc.ops)
      if (o.type == "import" && o.data.value("source", "") == source) return o.id;
    return std::string();
  };
  const std::string part = importOf("part.step"), second = importOf("second.step");
  (*require)(area && monitor && !part.empty() && !second.empty(), "the assets area and two linked imports");
  if (!area || part.empty() || second.empty()) {
    QCoreApplication::exit(2);
    return true;
  }
  auto state = [monitor](const std::string& import) {
    const opad::json* s = monitor->state(import);
    return s ? s->value("state", std::string()) : std::string("none");
  };
  auto bodyOf = [doc](const std::string& import) {
    for (const auto& id : doc->scene.all_bodies())
      if (doc->node(id)->source_op == import) return id;
    return std::string();
  };
  const QString dir = QFileInfo(doc->path()).absolutePath();
  const std::string partBody = bodyOf(part), secondBody = bodyOf(second);
  const std::string partRoot = monitor->asset(part)->root, secondRoot = monitor->asset(second)->root;
  (*require)(state(part) == "ok" && state(second) == "ok" && !partBody.empty() && partRoot != partBody && monitor->isRoot(partRoot) && !monitor->isRoot(partBody) &&
                 monitor->importOf(partBody) == part,
             QString("opened in sync, a top node and a part each (%1, %2)").arg(QString::fromStdString(state(part)), QString::fromStdString(state(second))));
  const QStringList watched = monitor->watched();
  auto isWatched = [&watched](const QString& path) {
    for (const QString& p : watched)
      if (QDir::cleanPath(p).compare(QDir::cleanPath(path), Qt::CaseInsensitive) == 0) return true;
    return false;
  };
  (*require)(isWatched(dir + "/parts/part.step") && isWatched(dir + "/parts/second.step") && isWatched(dir + "/parts") && watched.size() == 3,
             "watched: both files and their folder (" + watched.join(", ") + ")");

  // Read-only parts: no editor, rename and reparent refused; the top node is the user's.
  const size_t ops0 = doc->doc.ops.size();
  bool renameRefused = false, reparentRefused = false;
  try { doc->run("rename", opad::json{{"target", partBody}, {"name", "Renamed part"}}); } catch (const std::exception&) { renameRefused = true; }
  try { doc->run("reparent", opad::json{{"targets", {secondBody}}, {"parent", nullptr}}); } catch (const std::exception&) { reparentRefused = true; }
  bool rootRenamed = true;
  try { doc->run("rename", opad::json{{"target", partRoot}, {"name", "Part asset"}}); } catch (const std::exception&) { rootRenamed = false; }
  rootRenamed = rootRenamed && doc->nodeName(partRoot) == "Part asset";
  doc->undo();
  (*require)(renameRefused && reparentRefused && rootRenamed && doc->doc.ops.size() == ops0, "parts: rename and reparent refused, the top node renamed (undone)");

  w.m_browserOverlay->setAutoHide(false);
  w.m_browserOverlay->reveal();
  QTimer::singleShot(600, &w, [&w, area, monitor, doc, require, prefix, part, second, partBody, secondBody, partRoot, secondRoot, dir, state, ops0] {
    BrowserTree* tree = w.m_browser->tree();
    auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
    auto decoration = [tree, delegate](const std::string& id) {
      QTreeWidgetItem* item = rowOf(tree, id);
      return item ? delegate->decoration(tree->indexFromItem(item)) : browser::Decoration();
    };
    const browser::Decoration root = decoration(partRoot), child = decoration(partBody);
    (*require)(root.italic && root.typeIcon == "link" && root.badges.size() == 1 && root.badges[0].icon == "check" && !root.badges[0].clicked && !root.readOnly &&
                   root.tooltip.contains("part.step") && child.readOnly && !child.italic && child.badges.isEmpty(),
               "browser: link icon, italic name, in-sync badge on the top node; its part read-only");
    QTreeWidgetItem* bodyRow = rowOf(tree, partBody);
    w.m_browser->startRename(partBody);
    (*require)(bodyRow && tree->viewport()->findChildren<QLineEdit*>().isEmpty() && !delegate->createEditor(tree->viewport(), QStyleOptionViewItem(), tree->indexFromItem(bodyRow)),
               "a part opens no rename editor");
    w.m_browser->grab().save(prefix + ".browser.png");

    // Properties: the Linked file section under the body's rows.
    w.m_browser->selectIds({partBody});
    (*require)(w.action("assets.sync")->isEnabled() && !w.action("assets.syncAll")->isEnabled() && w.action("assets.embed")->isEnabled() && w.action("assets.pack")->isEnabled(),
               "commands: sync, embed and pack for the selection, sync all with nothing changed off");
    w.action("inspect.properties")->trigger();
    QTreeWidget* table = w.m_props->table();
    auto row = [table](const QString& text, int column) {
      for (int i = 0; i < table->topLevelItemCount(); ++i)
        if (table->topLevelItem(i)->text(column).contains(text)) return i;
      return -1;
    };
    const int header = row("LINKED FILE", 0), status = row("Status", 0), storage = row("Storage", 0);
    (*require)(w.m_propsPanel->isVisible() && header > 0 && status > header && table->topLevelItem(status)->text(1).contains("In sync") && storage > status &&
                   table->topLevelItem(storage)->text(1).contains("Linked") && row("Modified", 0) > storage && row("Found at", 0) < 0 && row("Show in folder", 1) > storage &&
                   row("Embed as editable", 1) > storage,
               QString("properties: the Linked file section, the file's time from the first look (header at %1)").arg(header));
    if (header > 0) table->scrollToItem(table->topLevelItem(header), QAbstractItemView::PositionAtTop);
    w.m_propsPanel->grab().save(prefix + ".properties.png");
    w.m_propsPanel->hide();
    {  // The context menu's entries for a part of a linked file.
      QMenu menu;
      SelectionContext selection;
      selection.ids = {partBody};
      area->contextMenu(selection, menu);
      QStringList texts;
      for (QAction* a : menu.actions())
        if (!a->isSeparator()) texts << a->text();
      (*require)(texts == QStringList({"Sync linked file", "Show in folder", "Copy path", "Replace linked file…", "Embed as editable", "Pack into project"}),
                 "context menu: " + texts.join(", "));
    }
    const auto [program, arguments] = assets::revealCommand(dir + "/parts/part.step");
    (*require)(program == "explorer.exe" && arguments.size() == 2 && arguments[1] == QDir::toNativeSeparators(dir + "/parts/part.step"), "reveal: explorer /select");

    // Both files change on disk.
    const std::string partKey = doc->node(partBody)->body_key, secondKey = doc->node(secondBody)->body_key;
    const double partWidth = width(doc, partBody);
    const bool written = rewrite(dir + "/next/part.step", dir + "/parts/part.step") && rewrite(dir + "/next/second.step", dir + "/parts/second.step");
    waitFor(&w, [=] { return state(part) == "changed" && state(second) == "changed" && !monitor->checking(); }, 15000, [=, &w](bool changed) {
      (*require)(written && changed, QString("both files changed on disk: marked changed (%1, %2)").arg(QString::fromStdString(state(part)), QString::fromStdString(state(second))));
      QList<Toast*> toasts = w.m_toasts->toasts();
      Toast* toast = toasts.isEmpty() ? nullptr : toasts.back();
      const browser::Decoration changedRoot = decoration(partRoot);
      (*require)(toast && toast->text().contains("2 linked files changed") && toast->actionButton() && toast->actionButton()->text() == "Sync all" && toasts.size() == 1,
                 "one toast: " + (toast ? toast->text() : QString("none")));
      (*require)(changedRoot.badges.size() == 1 && changedRoot.badges[0].text == "Sync" && changedRoot.badges[0].clicked && changedRoot.badges[0].color == &Tokens::assetStale &&
                     w.action("assets.syncAll")->isEnabled(),
                 "badge: Sync, clickable, stale colour; Sync all enabled");
      w.m_browser->grab().save(prefix + ".changed.png");
      if (!toast || !toast->actionButton()) return QCoreApplication::exit(2);
      const size_t ops = doc->doc.ops.size();
      toast->actionButton()->click();  // Sync all
      {  // The file being read: a turning badge, painted again while it shows.
        const browser::Decoration d = decoration(partRoot);
        w.m_browser->grab().save(prefix + ".syncing.png");
        (*require)(monitor->syncing(part) && d.badges.size() == 1 && d.badges[0].spin && d.badges[0].text == "syncing…" && delegate->spinning(),
                   "syncing: the badge turns (" + (d.badges.isEmpty() ? QString("none") : d.badges[0].text) + ")");
      }
      // Painted while the sync runs: the badge keeps turning until it is over, then stops (the frame after the last turn).
      waitFor(&w, [=, &w] {
        w.m_browser->grab();
        return doc->doc.ops.size() == ops + 2 && !area->busy() && state(part) == "ok" && state(second) == "ok" && !monitor->checking() && !delegate->spinning();
      }, 30000, [=, &w](bool synced) {
        (*require)(synced && !decoration(partRoot).badges.value(0).spin, "synced: nothing turns");
        const opad::Node* p = doc->node(partBody);
        const opad::Node* s = doc->node(secondBody);
        const QStringList undo = doc->undoLabels();
        (*require)(synced && p && s && p->body_key != partKey && s->body_key != secondKey && !p->body_missing && doc->doc.ops.back().type == "edit" &&
                       undo.size() >= 2 && undo[0] == "sync second.step" && undo[1] == "sync part.step",
                   QString("Sync all: two edits, two undo steps (%1), node ids kept, new bodies").arg(undo.mid(0, 2).join(", ")));
        const double now = width(doc, partBody);
        (*require)(now > partWidth + 1.5, QString("geometry updated: the part is %1 mm wide (was %2)").arg(now, 0, 'f', 2).arg(partWidth, 0, 'f', 2));
        QList<Toast*> after = w.m_toasts->toasts();
        (*require)(!after.isEmpty() && after.back()->text() == "2 linked files synced", "toast: " + (after.isEmpty() ? QString("none") : after.back()->text()));
        doc->undo();
        const bool undone = doc->node(secondBody)->body_key == secondKey && doc->node(partBody)->body_key != partKey;
        doc->redo();
        (*require)(undone && doc->node(secondBody)->body_key != secondKey, "undo and redo of the second sync");
        const std::string syncedKey = doc->node(partBody)->body_key;

        // The part changes again: its badge's click syncs it.
        const bool again = rewrite(dir + "/next/part-3.step", dir + "/parts/part.step");
        waitFor(&w, [=] { return state(part) == "changed" && state(second) == "ok" && !monitor->checking(); }, 15000, [=, &w](bool changed) {
          QList<Toast*> toasts = w.m_toasts->toasts();
          (*require)(again && changed && !toasts.isEmpty() && toasts.back()->text() == "part.step changed since the last sync" && toasts.back()->actionButton() &&
                         toasts.back()->actionButton()->text() == "Sync",
                     "changed again: one file, its toast: " + (toasts.isEmpty() ? QString("none") : toasts.back()->text()));
          QTreeWidgetItem* item = rowOf(tree, partRoot);
          const QModelIndex index = tree->indexFromItem(item);
          tree->scrollTo(index);
          const QRect r = tree->visualRect(index);
          const browser::Decoration d = delegate->decoration(index);
          QRect badge;
          for (int x = r.right(); x > r.left() && badge.isEmpty(); --x) delegate->badgeAt(d, index, r, QPoint(x, r.center().y()), &badge);
          const size_t ops = doc->doc.ops.size();
          for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent e(type, QPointF(badge.center()), QPointF(tree->viewport()->mapToGlobal(badge.center())), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(tree->viewport(), &e);
          }
          waitFor(&w, [=] { return doc->doc.ops.size() == ops + 1 && !area->busy() && state(part) == "ok" && !monitor->checking(); }, 30000, [=, &w](bool synced) {
            QList<Toast*> toasts = w.m_toasts->toasts();
            const QString text = toasts.isEmpty() ? QString() : toasts.back()->text();
            (*require)(!badge.isEmpty() && synced && doc->node(partBody) && doc->node(partBody)->body_key != syncedKey &&
                           text == "Synced part.step: 1 parts changed, 0 added, 0 removed" && toasts.back()->actionButton() && toasts.back()->actionButton()->text() == "Undo",
                       "the badge's click synced it, node kept: " + text);

            // The second file moves away: missing; located at its new place.
            const QString moved = dir + "/parts/second-moved.step";
            const bool away = QFile::rename(dir + "/parts/second.step", moved);
            waitFor(&w, [=] { return state(second) == "missing" && !monitor->checking(); }, 15000, [=, &w](bool missing) {
              const browser::Decoration gone = decoration(secondRoot);
              (*require)(away && missing && gone.badges.size() == 1 && gone.badges[0].text == "missing" && gone.badges[0].clicked && gone.badges[0].color == &Tokens::assetMissing,
                         "moved away: missing badge, clickable (locate)");
              w.m_browser->selectIds({secondBody});
              w.action("inspect.properties")->trigger();
              QTreeWidget* table = w.m_props->table();
              bool notFound = false, locate = false;
              for (int i = 0; i < table->topLevelItemCount(); ++i) {
                notFound = notFound || (table->topLevelItem(i)->text(0) == "Status" && table->topLevelItem(i)->text(1).contains("Not found"));
                locate = locate || table->topLevelItem(i)->text(1) == "Locate…";
              }
              (*require)(notFound && locate, "properties: not found, Locate…");
              w.m_propsPanel->hide();
              const std::string key = doc->node(secondBody)->body_key;
              area->sync({second}, moved);  // what Locate… does with the file chosen
              waitFor(&w, [=] { return !area->busy() && state(second) == "ok" && !monitor->checking(); }, 30000, [=, &w](bool found) {
                const opad::json asset = monitor->asset(second)->asset;
                (*require)(found && asset.value("path", "") == "parts/second-moved.step" && doc->node(secondBody)->body_key == key,
                           "located: the asset points at its new place, same node and body (" + QString::fromStdString(asset.value("path", "")) + ")");

                // Pack the part into the project (git LFS by .gitattributes), embed the second.
                area->pack(part);
                waitFor(&w, [=] { return !area->busy() && monitor->state(part) && monitor->state(part)->value("lfs", false) && !monitor->checking(); }, 30000, [=, &w](bool packed) {
                  const opad::json asset = monitor->asset(part)->asset;
                  const browser::Decoration d = decoration(partRoot);
                  (*require)(packed && asset.value("storage", "") == "project" && QFileInfo::exists(dir + "/assets/part.step") && d.badges.size() == 2 && d.badges[1].text == "LFS",
                             "packed: assets/part.step, storage project, LFS badge (" + QString::fromStdString(asset.value("path", "")) + ")");
                  area->embed(second);
                  waitFor(&w, [=] { return !area->busy() && monitor->asset(second) && monitor->asset(second)->asset.value("storage", "") == "embedded"; }, 30000, [=, &w](bool embedded) {
                    const opad::Node* n = doc->node(secondBody);
                    const opad::BodyEntry* entry = n ? doc->doc.body(n->body_key) : nullptr;
                    const bool unlinked = n && !n->linked && monitor->importOf(secondBody).empty() && !decoration(secondBody).readOnly, stored = entry && !entry->external;
                    QString refused;  // the rename replaces the scene (n is gone)
                    try { doc->run("rename", opad::json{{"target", secondBody}, {"name", "Now mine"}}); } catch (const std::exception& e) { refused = e.what(); }
                    (*require)(embedded && unlinked && stored && refused.isEmpty(), "embedded: stored, no longer linked, editable " + refused);
                    // Import… asks only when linking suits the file (STEP, IGES, KiCad, over 20 MB).
                    QFile big(dir + "/big.stl");
                    const bool made = big.open(QIODevice::WriteOnly) && big.resize(assets::kSuggestBytes + 1);
                    big.close();
                    QFile small(dir + "/small.stl");
                    const bool tiny = small.open(QIODevice::WriteOnly) && small.write("solid x\nendsolid x\n") > 0;
                    small.close();
                    (*require)(made && tiny && assets::suggestLink(dir + "/big.stl") && !assets::suggestLink(dir + "/small.stl") && assets::suggestLink(dir + "/parts/part.step") &&
                                   !assets::suggestLink(dir + "/plan.dxf") && assets::askImport(&w, dir + "/small.stl") == assets::Mode::Copy,
                               "import choice: suggested for STEP and big files only");
                    QFile::remove(dir + "/big.stl");
                    w.m_browser->grab().save(prefix + ".final.png");
                    trace::log(QString("bench: asset-sync: %1 ops").arg(doc->doc.ops.size() - ops0));
                    QCoreApplication::exit(require->all ? 0 : 2);
                  });
                });
              });
            });
          });
        });
      });
    });
  });
  return true;
}

// OPAD_BENCH_ASSET_LOOK=<prefix> on a document linking parts/part.step, which changed since its sync, opened with a cache that
// never saw the version synced: its part is read from the file as it is and shown stale, tinted the stale colour in the view
// (LookSource::Asset, as composed and as applied) with the Sync badge. Synced: the part fades while it is read again, then
// shows in its own colour, with the new geometry, the same node. Then the file moves away while the project has a copy of it
// (assets/part.step): the copy is read, Properties and the context menu say so and offer Use project copy, which links it.
// Frames: <prefix>.stale.png, .synced.png.
OPAD_BENCH(OPAD_BENCH_ASSET_LOOK, asset_look) {
  auto require = std::make_shared<Checks>();
  require->name = "asset-look";
  const QString prefix = value;
  AssetsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
  AssetMonitor* monitor = area ? area->monitor() : nullptr;
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  std::string part, body;
  for (const auto& o : doc->doc.ops)
    if (o.type == "import" && o.data.value("source", "") == "part.step") part = o.id;
  for (const auto& id : doc->scene.all_bodies())
    if (doc->node(id)->source_op == part) body = id;
  (*require)(monitor && !part.empty() && !body.empty(), "a linked file and its part");
  if (!monitor || body.empty()) {
    QCoreApplication::exit(2);
    return true;
  }
  auto state = [monitor, part] {
    const opad::json* s = monitor->state(part);
    return s ? s->value("state", std::string()) : std::string("none");
  };
  auto settled = [&w, v, doc] {
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() == int(doc->scene.all_bodies().size()) && !v->looksPending();
  };
  auto same = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 1e-6;
  };
  waitFor(&w, [=] { return settled() && state() == "changed" && !monitor->checking(); }, 20000, [=, &w](bool opened) {
    const opad::Node* n = doc->node(body);
    const QColor c = v->tokens().assetStale;
    const std::array<double, 3> tint = looks::mix(n->color, {c.redF(), c.greenF(), c.blueF()}, 0.6);
    const BodyLook look = v->bodyLook(body);
    (*require)(opened && monitor->stale() == std::set<std::string>{body} && monitor->asset(part)->stale == 1,
               QString("opened: the changed file read as it is, its part stale (%1, %2 stale)").arg(QString::fromStdString(state())).arg(monitor->stale().size()));
    (*require)(same(look.color, tint) && look.opacity == 1 && v->shownLook(body) == look && !same(look.color, n->color),
               "the stale part is tinted the stale colour, as composed and as drawn");
    const opad::json* s = monitor->state(part);
    (*require)(s && s->value("reason", "") == "1 parts differ from the version synced", "its state says so: " + QString::fromStdString(s ? s->value("reason", "") : ""));
    {  // The other theme's stale colour, then back.
      const bool dark = v->tokens().dark;
      w.applyTheme(!dark);
      const QColor o = v->tokens().assetStale;
      const auto shown = v->bodyLook(body).color;
      const bool other = same(shown, looks::mix(doc->node(body)->color, {o.redF(), o.greenF(), o.blueF()}, 0.6)) && o != c;
      w.applyTheme(dark);
      (*require)(other && same(v->bodyLook(body).color, tint), QString("a theme switch takes that theme's stale colour (%1, then %2)").arg(c.name(), o.name()));
    }
    int x = 0, y = 0;
    const bool found = v->benchBodyPoint(body, x, y);
    const QImage stale = v->grabImage();
    stale.save(prefix + ".stale.png");
    const QColor before = found ? stale.pixelColor(x, y) : QColor();
    const std::string key = doc->node(body)->body_key;  // the theme switch may have rebuilt the scene (n)
    area->sync({part});
    const BodyLook reading = v->bodyLook(body);
    (*require)(monitor->syncing(part) && std::abs(reading.opacity - 0.45) < 1e-9 && same(reading.color, tint), QString("syncing: the file's part fades (%1)").arg(reading.opacity));
    waitFor(&w, [=] { return !area->busy() && state() == "ok" && !monitor->checking() && settled() && doc->node(body) && doc->node(body)->body_key != key; }, 30000,
            [=, &w](bool synced) {
      const opad::Node* now = doc->node(body);
      const BodyLook look = v->bodyLook(body);
      (*require)(synced && now && monitor->stale().empty() && same(look.color, now->color) && look.opacity == 1 && v->shownLook(body) == look,
                 "synced: the same node, in its own colour, nothing faded");
      int x2 = 0, y2 = 0;
      const bool again = v->benchBodyPoint(body, x2, y2);
      const QImage image = v->grabImage();
      image.save(prefix + ".synced.png");
      const QColor after = again ? image.pixelColor(x2, y2) : QColor();
      const int apart = std::abs(before.red() - after.red()) + std::abs(before.green() - after.green()) + std::abs(before.blue() - after.blue());
      (*require)(found && again && apart > 40, QString("drawn so: %1 -> %2").arg(before.name(), after.name()));

      // Gone from where it was linked while the project has a copy: the copy is read, and Use project copy links it.
      const QString dir = QFileInfo(doc->path()).absolutePath();
      QDir().mkpath(dir + "/assets");
      const bool moved = QFile::copy(dir + "/parts/part.step", dir + "/assets/part.step") && QFile::rename(dir + "/parts/part.step", dir + "/parts/part.old");
      auto onCopy = [=] {
        const opad::json* s = monitor->state(part);
        return s && s->contains("file") && QString::fromStdString((*s)["file"].get<std::string>()).endsWith("assets/part.step", Qt::CaseInsensitive);
      };
      waitFor(&w, [=] { return onCopy() && state() == "ok" && !monitor->checking(); }, 15000, [=, &w](bool copied) {
        QMenu menu;
        SelectionContext selection;
        selection.ids = {body};
        area->contextMenu(selection, menu);
        QStringList texts;
        for (QAction* a : menu.actions())
          if (!a->isSeparator()) texts << a->text();
        w.m_browser->selectIds({body});
        w.action("inspect.properties")->trigger();
        QTreeWidget* table = w.m_props->table();
        bool note = false, use = false;
        QStringList rows;
        for (int i = 0; i < table->topLevelItemCount(); ++i) {
          note = note || table->topLevelItem(i)->text(1).contains("Not found where it was linked: the project's copy is read");  // values come wrapped LRE..PDF
          use = use || table->topLevelItem(i)->text(1) == "Use project copy";
          rows << table->topLevelItem(i)->text(0) + "=" + table->topLevelItem(i)->text(1);
        }
        w.m_propsPanel->hide();
        (*require)(moved && copied && texts.contains("Use project copy") && note && use,
                   "read from the project's copy: said so, Use project copy offered (" + texts.join(", ") + (note && use ? QString() : "; " + rows.join("; ")) + ")");
        area->pack(part);
        waitFor(&w, [=] { return !area->busy() && monitor->asset(part)->asset.value("storage", "") == "project" && !monitor->checking(); }, 20000, [=, &w](bool linked) {
          const opad::json asset = monitor->asset(part)->asset;
          QList<Toast*> toasts = w.m_toasts->toasts();
          const QString text = toasts.isEmpty() ? QString() : toasts.back()->text();
          (*require)(linked && asset.value("path", "") == "assets/part.step" && state() == "ok" && text.startsWith("part.step now links the project's copy"),
                     "the link points at the project's copy: " + QString::fromStdString(asset.value("path", "")) + ", " + text);
          QCoreApplication::exit(require->all ? 0 : 2);
        });
      });
    });
  });
  return true;
}

// OPAD_BENCH_ASSET_DRAWING=<prefix> on a document holding a box, with plan.dxf (a 40 mm line) beside it and its next version
// (60 mm) in next/: plan.dxf linked as Import… does with "Link as asset" chosen (MainWindow::importDrawing with link): the
// XY plane picked, the drawing moved there by an offset in the placer, Place. The import is a linked drawing whose asset
// records that placement, its top node marked linked in the browser, in sync; the file changes: marked changed, synced,
// the same node now 60 mm long where it was placed. Sync changed files automatically turned on, the file changes back: synced
// with nothing clicked, 40 mm again, its toast offering Undo. Frame: <prefix>.browser.png.
OPAD_BENCH(OPAD_BENCH_ASSET_DRAWING, asset_drawing) {
  static bool started = false;
  if (std::exchange(started, true)) return true;  // the import's load comes back here: the timers below go on
  auto require = std::make_shared<Checks>();
  require->name = "asset-drawing";
  const QString prefix = value;
  AssetsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
  AssetMonitor* monitor = area ? area->monitor() : nullptr;
  AppDocument* doc = w.m_doc;
  const QString dir = QFileInfo(doc->path()).absolutePath();
  auto importOf = [doc] {
    for (const auto& o : doc->doc.ops)
      if (o.type == "import" && o.data.value("source", "") == "plan.dxf") return o.id;
    return std::string();
  };
  auto state = [monitor, importOf] {
    const opad::json* s = monitor->state(importOf());
    return s ? s->value("state", std::string()) : std::string("none");
  };
  (*require)(monitor && !doc->scene.all_bodies().empty() && importOf().empty(), "a document with a box, nothing linked yet");
  if (!monitor) {
    QCoreApplication::exit(2);
    return true;
  }
  w.importDrawing(dir + "/plan.dxf", {}, true);
  waitFor(&w, [&w] { return w.m_design->pickingPlane(); }, 10000, [=, &w](bool picking) {
    (*require)(picking, "no face selected: the plane is picked first");
    w.m_design->planePicker()->choose({{"base", "xy"}});
    waitFor(&w, [&w] { return w.m_drawingPlacer->active() && w.m_drawingPlacer->panel()->findChild<QPushButton*>("primary")->isEnabled(); }, 15000, [=, &w](bool placing) {
      (*require)(placing, "the placer shows the drawing on the plane");
      w.m_drawingPlacer->setOffset(15, 25);
      const opad::Mat4 placement = w.m_drawingPlacer->placement();
      w.m_drawingPlacer->panel()->findChild<QPushButton*>("primary")->click();  // Place
      waitFor(&w, [=, &w] { return !importOf().empty() && !doc->loading && !w.m_loadJob && state() == "ok" && !monitor->checking(); }, 20000, [=, &w](bool linked) {
        const std::string import = importOf();
        const opad::json asset = monitor->asset(import) ? monitor->asset(import)->asset : opad::json();
        const opad::json recorded = asset.value("builder", opad::json::object()).value("options", opad::json::object()).value("placement", opad::json());
        const bool same = !recorded.is_null() && opad::Mat4::from_json(recorded).apply({0, 0, 0}) == placement.apply({0, 0, 0});
        (*require)(linked && asset.value("kind", "") == "drawing" && asset.value("storage", "") == "linked" && same,
                   "linked where it was placed: a drawing asset recording the placement (" + QString::fromStdString(recorded.dump()) + ")");
        std::string body;
        for (const auto& id : doc->scene.all_bodies())
          if (doc->node(id)->source_op == import) body = id;
        const std::string root = monitor->asset(import) ? monitor->asset(import)->root : std::string();
        const opad::Node* n = body.empty() ? nullptr : doc->node(body);
        auto span = [doc, body] {
          const Bnd_Box b = opad::node_world_bbox(doc->doc, doc->scene, body);
          double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
          if (!b.IsVoid()) b.Get(x0, y0, z0, x1, y1, z1);
          return std::array<double, 3>{x0, x1, (y0 + y1) / 2};
        };
        const auto at = span();
        (*require)(n && n->linked && n->representation == "drawing2d" && monitor->isRoot(root) && std::abs(at[0] - 15) < 0.5 && std::abs(at[1] - 55) < 0.5 &&
                       std::abs(at[2] - 25) < 0.5,
                   QString("its part is linked, from x %1 to %2 at y %3").arg(at[0]).arg(at[1]).arg(at[2]));
        browser::Decoration d;
        area->decorate({root, "component", {}, doc->node(root)}, d);
        (*require)(d.italic && d.typeIcon == "link" && d.badges.size() == 1 && d.badges[0].icon == "check", "the browser marks it linked and in sync");
        w.m_browserOverlay->setAutoHide(false);
        w.m_browserOverlay->reveal();
        const bool written = rewrite(dir + "/next/plan.dxf", dir + "/plan.dxf");
        waitFor(&w, [=] { return state() == "changed" && !monitor->checking(); }, 15000, [=, &w](bool changed) {
          (*require)(written && changed, "the drawing changed on disk: marked changed");
          area->sync({import});
          waitFor(&w, [=] { return !area->busy() && state() == "ok" && !monitor->checking(); }, 30000, [=, &w](bool synced) {
            const auto now = span();
            const opad::json after = monitor->asset(import)->asset.value("builder", opad::json::object()).value("options", opad::json::object()).value("placement", opad::json());
            (*require)(synced && doc->node(body) && after == recorded && std::abs(now[0] - 15) < 0.5 && std::abs(now[1] - 75) < 0.5 && std::abs(now[2] - 25) < 0.5,
                       QString("synced where it was placed: the same node from x %1 to %2 at y %3").arg(now[0]).arg(now[1]).arg(now[2]));
            w.m_browser->grab().save(prefix + ".browser.png");
            QAction* autoSync = w.action("assets.autoSync");
            (*require)(autoSync && autoSync->isCheckable() && !autoSync->isChecked(), "Sync changed files automatically: off by default");
            if (!autoSync) return QCoreApplication::exit(2);
            autoSync->trigger();
            const size_t ops = doc->doc.ops.size();
            const bool back = rewrite(dir + "/next/plan-40.dxf", dir + "/plan.dxf");
            waitFor(&w, [=] { return doc->doc.ops.size() == ops + 1 && !area->busy() && state() == "ok" && !monitor->checking(); }, 30000, [=, &w](bool synced) {
              const auto again = span();
              QList<Toast*> toasts = w.m_toasts->toasts();
              Toast* toast = toasts.isEmpty() ? nullptr : toasts.back();
              (*require)(back && synced && QSettings().value("assets/autoSync").toBool() && std::abs(again[1] - 55) < 0.5 && toast && toast->text().startsWith("Synced plan.dxf") &&
                             toast->actionButton() && toast->actionButton()->text() == "Undo",
                         QString("auto sync: changed back, synced with nothing clicked (to x %1): %2").arg(again[1]).arg(toast ? toast->text() : QString("no toast")));
              autoSync->trigger();
              QCoreApplication::exit(require->all ? 0 : 2);
            });
          });
        });
      });
    });
  });
  return true;
}

// OPAD_BENCH_ASSET_KICAD=<prefix> on a document beside a board it links, whose one model (D1's) comes from KiCad's library,
// missing here and downloadable from a local copy of the library: the first look counts it and a toast offers the download;
// Properties shows the board's 3D models (none found, one missing, from KiCad's library) with Download models… and Model
// folders…, the context menu Download KiCad models…. Downloaded (setting always: not asked): the board's models changed, a
// toast offers Sync, which shows the model (4 mm tall where nothing stood). Frame: <prefix>.properties.png.
OPAD_BENCH(OPAD_BENCH_ASSET_KICAD, asset_kicad) {
  auto require = std::make_shared<Checks>();
  require->name = "asset-kicad";
  const QString prefix = value;
  AssetsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
  AssetMonitor* monitor = area ? area->monitor() : nullptr;
  AppDocument* doc = w.m_doc;
  std::string import;
  for (const auto& o : doc->doc.ops)
    if (o.type == "import" && o.data.value("source", "") == "board.kicad_pcb") import = o.id;
  (*require)(monitor && !import.empty(), "a linked board");
  if (!monitor || import.empty()) {
    QCoreApplication::exit(2);
    return true;
  }
  auto state = [monitor, import] {
    const opad::json* s = monitor->state(import);
    return s ? *s : opad::json::object();
  };
  auto toastWith = [&w](const QString& text, const QString& action) -> Toast* {
    const QList<Toast*> toasts = w.m_toasts->toasts();
    Toast* t = toasts.isEmpty() ? nullptr : toasts.back();
    return t && t->text().contains(text) && t->actionButton() && t->actionButton()->text() == action ? t : nullptr;
  };
  auto tallest = [doc, import] {  // the highest body of the board's: the board itself (1.6 mm) until D1's model shows
    double most = 0;
    for (const auto& id : doc->scene.all_bodies())
      if (const opad::Node* n = doc->node(id); n->source_op == import && !n->body_missing) {
        const Bnd_Box b = opad::node_world_bbox(doc->doc, doc->scene, id);
        double x0, y0, z0, x1, y1, z1;
        if (!b.IsVoid()) b.Get(x0, y0, z0, x1, y1, z1), most = std::max(most, z1 - z0);
      }
    return most;
  };
  waitFor(&w, [=] { return state().value("models_downloadable", 0) == 1 && !monitor->checking() && toastWith("come from KiCad's library", "Download…"); }, 15000,
          [=, &w](bool offered) {
    const opad::json s = state();
    (*require)(offered && s.value("state", "") == "ok" && s.value("models_missing", 0) == 1 && s.value("models_found", 0) == 0,
               "opened: its library model counted missing, the download offered: " + QString::fromStdString(s.dump()));
    std::string body;
    for (const auto& id : doc->scene.all_bodies())
      if (doc->node(id)->source_op == import) body = id;
    w.m_browser->selectIds({body});
    w.action("inspect.properties")->trigger();
    QTreeWidget* table = w.m_props->table();
    bool models = false, download = false, folders = false;
    for (int i = 0; i < table->topLevelItemCount(); ++i) {
      const QTreeWidgetItem* row = table->topLevelItem(i);
      models = models || (row->text(0) == "3D models" && row->text(1).contains("0 found, 1 missing (1 from KiCad's library)"));
      download = download || row->text(1) == "Download models…";
      folders = folders || row->text(1) == "Model folders…";
      if (row->text(0) == "LINKED FILE") table->scrollToItem(row, QAbstractItemView::PositionAtTop);
    }
    w.m_propsPanel->grab().save(prefix + ".properties.png");
    w.m_propsPanel->hide();
    QMenu menu;
    SelectionContext selection;
    selection.ids = {body};
    area->contextMenu(selection, menu);
    bool entry = false;
    for (QAction* a : menu.actions()) entry = entry || a->text() == "Download KiCad models…";
    (*require)(models && download && folders && entry, "Properties: the board's 3D models, Download models… and Model folders…; the context menu's entry");
    const double before = tallest();
    QSettings().setValue("kicad/download", "always");  // the question is not asked then (a bench answers none)
    toastWith("come from KiCad's library", "Download…")->actionButton()->click();
    waitFor(&w, [=] { return state().value("state", "") == "changed" && !monitor->checking() && toastWith("1 KiCad 3D models downloaded", "Sync"); }, 30000,
            [=, &w](bool downloaded) {
      (*require)(downloaded && state().value("models_found", 0) == 1 && state().value("models_downloadable", 0) == 0,
                 "downloaded: the board's models changed, Sync offered: " + QString::fromStdString(state().dump()));
      if (Toast* t = toastWith("1 KiCad 3D models downloaded", "Sync")) t->actionButton()->click();
      waitFor(&w, [=] { return !area->busy() && state().value("state", "") == "ok" && !monitor->checking(); }, 30000, [=, &w](bool synced) {
        const double after = tallest();
        (*require)(synced && before < 2 && after > 3.9, QString("synced: D1's model shows (tallest part %1 mm, was %2)").arg(after, 0, 'f', 2).arg(before, 0, 'f', 2));
        QCoreApplication::exit(require->all ? 0 : 2);
      });
    });
  });
  return true;
}

// OPAD_BENCH_ASSET_PERF=sync|embed on a document linking a big file (the Engine STEP; not a gui_benches case): once the file
// is looked at and every body is displayed, the first linked file is synced (read again, forced: from its own path) or
// embedded through the area as the user's click does; the trace shows the job's time and any stall of the UI thread
// ("bench: asset-perf: <action> N ms").
OPAD_BENCH(OPAD_BENCH_ASSET_PERF, asset_perf) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  auto require = std::make_shared<Checks>();
  require->name = "asset-perf";
  const QString action = value;
  AssetsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
  AssetMonitor* monitor = area ? area->monitor() : nullptr;
  AppDocument* doc = w.m_doc;
  const std::string import = monitor && !monitor->assets().empty() ? monitor->assets().begin()->first : std::string();
  (*require)(!import.empty() && (action == "sync" || action == "embed"), "a linked file, sync or embed");
  if (import.empty()) {
    QCoreApplication::exit(2);
    return true;
  }
  auto ready = [=, &w] {
    const opad::json* s = monitor->state(import);
    return s && s->value("state", "") == "ok" && !monitor->checking() && !w.m_displayJob && w.m_meshRemaining == 0 && !doc->loading;
  };
  waitFor(&w, ready, 600000, [=, &w](bool shown) {
    (*require)(shown, QString("looked at and displayed: %1 bodies").arg(w.m_viewport->displayedCount()));
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QObject::connect(area, &AssetsArea::done, &w, [=, &w](const QString& what, const std::string&, bool ok, const QString& error, const opad::json& report) {
      trace::log(QString("bench: asset-perf: %1 %2 ms (%3)").arg(what).arg(clock->elapsed()).arg(QString::fromStdString(report.dump()).left(300)));
      (*require)(ok, what + " committed " + error);
      QTimer::singleShot(3000, &w, [require] { QCoreApplication::exit(require->all ? 0 : 2); });  // the display settles
    });
    if (action == "embed") area->embed(import);
    else area->sync({import}, monitor->file(import));
  });
  return true;
}
