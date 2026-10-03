// The linked-file UI (UI-68) in the running app: badges, read-only parts, Properties, the context menu, the monitor's toast,
// Sync all, a badge click, a missing file located, pack (LFS) and embed. Case "asset-sync" in tools/bench_cases/assets.py.
#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>

#include <Bnd_Box.hxx>

#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "opad/geometry.hpp"

namespace {
struct Checks {
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: asset-sync: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
// Frames: <prefix>.browser.png, .changed.png, .properties.png, .final.png.
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
      waitFor(&w, [=] { return doc->doc.ops.size() == ops + 2 && !area->busy() && state(part) == "ok" && state(second) == "ok" && !monitor->checking(); }, 30000,
              [=, &w](bool synced) {
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
