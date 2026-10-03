// Linked files in the window (UI-68, AssetsArea.hpp): browser badges, the Properties section, the commands and context
// actions, the import choice and the trust question.
#include "AssetsArea.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <memory>

#include "AppDocument.hpp"
#include "AssetMonitor.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"
#include "Viewport.hpp"
#include "opad/drawing_io.hpp"

OPAD_ICON_TABLE(assets,
                {"link", R"(<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1"/><path d="M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1"/>)"},
                {"embed", R"(<path d="M12 2v8M9 7l3 3 3-3"/><path d="M4 13l8 4 8-4M4 13v5l8 4 8-4v-5"/>)"},
                {"pack", R"(<rect x="3" y="4" width="18" height="5"/><path d="M5 9v11h14V9M10 13h4"/>)"});

namespace {
namespace fs = std::filesystem;

fs::path fsPath(const QString& path) { return fs::path(path.toStdU16String()); }
QString native(const QString& path) { return QDir::toNativeSeparators(path); }

QString kindText(const std::string& kind) {
  if (kind == "step") return "STEP";
  if (kind == "iges") return "IGES";
  if (kind == "brep") return "BREP";
  if (kind == "kicad_pcb") return AssetsArea::tr("KiCad board");
  if (kind == "image") return AssetsArea::tr("Picture");
  if (kind == "drawing") return AssetsArea::tr("2D drawing");
  return AssetsArea::tr("Mesh");
}

QString isoText(const std::string& iso) {
  const QDateTime t = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODate);
  return t.isValid() ? t.toLocalTime().toString("yyyy-MM-dd HH:mm") : QString();
}

// How the file is read: OPAD's readers, or KiCad's own STEP export of a board (asset.derived).
QString builderText(const opad::json& asset) {
  const opad::json derived = asset.value("derived", opad::json::object()).value("builder", opad::json::object());
  if (derived.value("name", "") == "kicad-cli") return AssetsArea::tr("KiCad's STEP export (kicad-cli %1)").arg(QString::fromStdString(derived.value("kicad", std::string())));
  return "OPAD";
}

// A cancelled job reports back at once while its worker still reads the copy, whose shapes the document shares.
void afterWorker(QObject* ctx, std::shared_ptr<std::atomic<bool>> reading, std::function<void()> fn) {
  if (!*reading) return fn();
  QTimer::singleShot(20, ctx, [ctx, reading, fn] { afterWorker(ctx, reading, fn); });
}
}  // namespace

// ---------------------------------------------------------------- import choice, trust, reveal
bool assets::suggestLink(const QString& path) {
  const QFileInfo info(path);
  const QString suffix = info.suffix().toLower();
  return suffix == "step" || suffix == "stp" || suffix == "iges" || suffix == "igs" || suffix == "kicad_pcb" || info.size() > kSuggestBytes;  // a drawing: when big
}

assets::Mode assets::askImport(QWidget* parent, const QString& path) {
  if (!suggestLink(path)) return Mode::Copy;
  QSettings settings;
  const QString remembered = settings.value("assets/import").toString();
  if (remembered == "link" || remembered == "copy") return remembered == "link" ? Mode::Link : Mode::Copy;
  const QFileInfo info(path);
  QMessageBox box(QMessageBox::Question, AssetsArea::tr("Import"), AssetsArea::tr("How should “%1” come in?").arg(info.fileName()), QMessageBox::NoButton, parent);
  const QString why = info.size() > kSuggestBytes ? AssetsArea::tr("Linking is suggested: the file is %1.").arg(QLocale().formattedDataSize(info.size(), 1, QLocale::DataSizeTraditionalFormat))
                                                 : AssetsArea::tr("Linking is suggested for models one designs around (STEP, IGES, KiCad boards).");
  box.setInformativeText(AssetsArea::tr("Linked: the document keeps a reference and reads the file whenever it opens, so it stays small; the file is watched and "
                                        "its changes are offered for syncing. Its parts are read-only until it is embedded.\n\nEditable copy: the shapes are "
                                        "stored in the document and can be edited; later changes to the file do not reach it.") +
                         "\n\n" + why);
  QPushButton* link = box.addButton(AssetsArea::tr("Link as asset"), QMessageBox::AcceptRole);
  QPushButton* copy = box.addButton(AssetsArea::tr("Import editable copy"), QMessageBox::AcceptRole);
  box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(link);
  auto* remember = new QCheckBox(AssetsArea::tr("Do not ask again"));
  box.setCheckBox(remember);
  box.exec();
  const Mode mode = box.clickedButton() == link ? Mode::Link : box.clickedButton() == copy ? Mode::Copy : Mode::Cancel;
  if (mode != Mode::Cancel && remember->isChecked()) settings.setValue("assets/import", mode == Mode::Link ? "link" : "copy");
  return mode;
}

// A document from elsewhere must not make OPAD open files of its choosing (a network share hands over the user's
// credentials): the linked files outside its project are read only once the user says so, here or for good (settings).
bool assets::askTrust(QWidget* parent, AppDocument* doc, JobRunner* jobs, std::function<void(const QString&)> failed, const std::vector<std::string>& imports) {
  QStringList files, folders;
  std::vector<std::string> asked;
  for (const auto& s : doc->assetStates) {
    if (s.value("state", "") != "untrusted") continue;
    const std::string import = s.value("import", "");
    if (!imports.empty() && std::find(imports.begin(), imports.end(), import) == imports.end()) continue;
    const QString file = QString::fromStdString(s.value("file", s.value("path", std::string())));
    files << QDir::toNativeSeparators(file);
    asked.push_back(import);
    if (const QString folder = QFileInfo(file).absolutePath(); !folders.contains(folder)) folders << folder;
  }
  if (files.isEmpty()) return false;
  QMessageBox box(QMessageBox::Question, AssetsArea::tr("Linked files"),
                  AssetsArea::tr("This document links files outside its project folder:\n\n%1\n\nRead them?").arg(files.mid(0, 6).join('\n') + (files.size() > 6 ? "\n…" : "")),
                  QMessageBox::NoButton, parent);
  auto* once = box.addButton(AssetsArea::tr("Read them"), QMessageBox::AcceptRole);
  auto* always = box.addButton(folders.size() == 1 ? AssetsArea::tr("Always trust this folder") : AssetsArea::tr("Always trust these folders"), QMessageBox::AcceptRole);
  box.addButton(AssetsArea::tr("Not now"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == always) {
    QSettings settings;
    QStringList trusted = settings.value("assets/trusted").toStringList();
    for (const QString& f : folders)
      if (!trusted.contains(f)) trusted << f;
    settings.setValue("assets/trusted", trusted);
  } else if (box.clickedButton() != once) {
    return true;
  }
  doc->loadAssets(jobs, box.clickedButton() == once, [failed](bool ok, const QString& error) {
    if (!ok && failed) failed(error);
  }, asked);
  return true;
}

std::pair<QString, QStringList> assets::revealCommand(const QString& file) {
#if defined(_WIN32)
  return {"explorer.exe", {"/select,", QDir::toNativeSeparators(file)}};
#elif defined(__APPLE__)
  return {"open", {"-R", file}};
#else
  return {"xdg-open", {QFileInfo(file).absolutePath()}};
#endif
}

// ---------------------------------------------------------------- the area
AssetsArea::AssetsArea(AreaServices& services) : AreaController(services) {}

void AssetsArea::buildActions() {
  auto command = [this](const char* id, const QString& label, const char* icon, const QStringList& keywords, std::function<bool(const CommandContext&)> when,
                        std::function<void()> fn) {
    CommandInfo info;
    info.id = id;
    info.label = label;
    info.icon = icon;
    info.group = tr("Linked files");
    info.keywords = keywords;
    info.enabledWhen = std::move(when);
    services().addCommand(info, std::move(fn));
  };
  auto one = [this](const CommandContext& c) { return c.document && !c.viewer && imports(c.selection).size() == 1; };
  auto selected = [this] { return imports(services().selection()); };
  command("assets.link", tr("Link as asset…"), "link", {"insert", "reference", "external", "xref"}, {}, [this] { link(); });
  command("assets.sync", tr("Sync linked file"), "regen", {"reload", "update", "refresh"},
          [this](const CommandContext& c) { return c.document && !c.viewer && !imports(c.selection).empty(); }, [this, selected] { sync(selected()); });
  command("assets.syncAll", tr("Sync all linked files"), "regen", {"reload", "update", "refresh"},
          [this](const CommandContext& c) { return c.document && m_monitor && !m_monitor->changed().empty(); }, [this] { syncAll(); });
  command("assets.replace", tr("Replace linked file…"), "import", {"relink", "swap"}, one, [this, selected] { if (const auto s = selected(); s.size() == 1) replace(s.front()); });
  command("assets.reveal", tr("Show linked file in folder"), "open", {"reveal", "explorer", "finder"}, one, [this, selected] { if (const auto s = selected(); s.size() == 1) reveal(s.front()); });
  command("assets.copyPath", tr("Copy linked file path"), "copy", {"clipboard"}, one, [this, selected] { if (const auto s = selected(); s.size() == 1) copyPath(s.front()); });
  command("assets.embed", tr("Embed as editable"), "embed", {"bind", "unlink", "break link"}, one, [this, selected] { if (const auto s = selected(); s.size() == 1) embed(s.front()); });
  command("assets.pack", tr("Pack into project"), "pack", {"copy", "collect", "lfs"},
          [this, one](const CommandContext& c) {
            if (!one(c)) return false;
            const AssetMonitor::Asset* a = m_monitor ? m_monitor->asset(imports(c.selection).front()) : nullptr;
            return a && a->asset.value("storage", "linked") == "linked" && !services().document()->doc.path.empty();
          },
          [this, selected] { if (const auto s = selected(); s.size() == 1) pack(s.front()); });
}

void AssetsArea::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  if (QMenu* file = menus.value("file")) {  // after Import…
    const QList<QAction*> entries = file->actions();
    QAction* before = nullptr;
    for (int i = 0; i < entries.size(); ++i)
      if (entries[i]->objectName() == "file.import") before = entries.value(i + 1);
    file->insertAction(before, services().action("assets.link"));
  }
  if (QMenu* design = menus.value("design")) {
    QMenu* sub = design->addMenu(tr("Linked files"));
    sub->setObjectName("assets");
    for (const char* id : {"assets.link", "assets.sync", "assets.syncAll", "assets.replace", "assets.reveal", "assets.copyPath", "assets.embed", "assets.pack"})
      sub->addAction(services().action(id));
  }
}

void AssetsArea::ribbon(RibbonLayout& layout) {
  QAction* link = services().action("assets.link");
  for (const char* id : {"design.assemble.components", "design.export.file", "review.export.file"})  // beside Import
    if (RibbonLayout::Group* g = layout.group(id)) {
      int at = 0;
      while (at < g->items.size() && g->items[at].action != services().action("file.import")) ++at;
      RibbonLayout::Item item;
      item.action = link;
      g->items.insert(std::min(at + 1, int(g->items.size())), item);
    }
  // Sync and the rest are where the files are: their badges, the context menu, Properties, Design > Linked files, the toast.
}

void AssetsArea::ready() {
  m_monitor = new AssetMonitor(services().document(), services().jobs(), this);
  connect(m_monitor, &AssetMonitor::statesChanged, this, [this] {
    updateLooks();
    services().browser()->refreshDecorations();
    services().properties()->refresh();
    services().updateCommands();
  });
  connect(m_monitor, &AssetMonitor::filesChanged, this, &AssetsArea::filesChanged);
  services().browser()->addDecorator([this](const browser::Row& row, browser::Decoration& d) { decorate(row, d); });
  services().properties()->addSectionProvider([this](const PropertySubject& s, const opad::json& props, QList<PropertySection>& out) { section(s, props, out); });
  m_monitor->documentChanged(true);
}

void AssetsArea::documentChanged(bool replaced) {
  if (replaced) m_queue.clear();
  if (m_monitor) m_monitor->documentChanged(replaced);
}

std::vector<std::string> AssetsArea::imports(const SelectionContext& selection) const {
  std::vector<std::string> out;
  if (!m_monitor) return out;
  for (const auto& id : selection.ids)
    if (const std::string import = m_monitor->importOf(id); !import.empty() && std::find(out.begin(), out.end(), import) == out.end()) out.push_back(import);
  return out;
}

QString AssetsArea::name(const std::string& import) const {
  const AssetMonitor::Asset* a = m_monitor ? m_monitor->asset(import) : nullptr;
  return a ? QString::fromStdString(a->name) : QString::fromStdString(import.substr(0, 8));
}

QString AssetsArea::stateText(const std::string& import) const {
  const opad::json* s = m_monitor->state(import);
  const std::string state = m_monitor->syncing(import) ? "syncing" : s ? s->value("state", "") : "";
  if (state == "ok") return tr("In sync");
  if (state == "changed") return tr("Changed since the last sync");
  if (state == "missing") return tr("Not found");
  if (state == "untrusted") return tr("Outside the document's project: not read");
  if (state == "error") return tr("Could not be read");
  if (state == "embedded") return tr("Embedded in the document");
  if (state == "syncing") return tr("Syncing…");
  return tr("Not checked yet");
}

// ---------------------------------------------------------------- browser and Properties
void AssetsArea::decorate(const browser::Row& row, browser::Decoration& d) {
  if (!row.node || !m_monitor) return;
  const std::string import = m_monitor->importOf(row.id);
  if (import.empty()) return;
  if (!m_monitor->isRoot(row.id)) {  // its parts: the file's names and places
    d.readOnly = true;
    d.tooltip = tr("Part of the linked file %1: read-only").arg(name(import));
    if (m_monitor->stale().count(row.id)) d.tooltip += '\n' + tr("Shown as the file is now, not as last synced (tinted in the view): sync to take it");
    return;
  }
  const AssetMonitor::Asset* a = m_monitor->asset(import);
  const opad::json* s = m_monitor->state(import);
  const std::string state = m_monitor->syncing(import) ? "syncing" : s ? s->value("state", "") : "";
  const QString file = native(m_monitor->file(import));
  d.typeIcon = "link";
  d.italic = true;
  browser::Badge badge;
  badge.fill = nullptr;
  // Each state with its own icon and word, not only its colour (theme::cue).
  auto later = [this](std::function<void()> fn) { return [this, fn] { QTimer::singleShot(0, this, fn); }; };  // the press may rebuild the row
  if (state == "changed") {
    badge.icon = "regen";
    badge.text = tr("Sync");
    badge.color = &Tokens::assetStale;
    badge.fill = &Tokens::bg4;
    badge.tooltip = tr("%1 changed since the last sync: click to sync").arg(file);
    badge.clicked = later([this, import] { sync({import}); });
  } else if (state == "missing") {
    badge.icon = "warning";
    badge.text = tr("missing");
    badge.color = &Tokens::assetMissing;
    badge.fill = &Tokens::bg4;
    badge.tooltip = tr("Not found: %1. Click to locate it.").arg(file);
    badge.clicked = later([this, import] { locate(import); });
  } else if (state == "untrusted") {
    badge.icon = "warning";
    badge.text = tr("not read");
    badge.color = &Tokens::warning;
    badge.fill = &Tokens::bg4;
    badge.tooltip = tr("%1 is outside the document's project: click to read it").arg(file);
    badge.clicked = later([this, import] { trust(import); });
  } else if (state == "error") {
    badge.icon = "warning";
    badge.text = tr("error");
    badge.color = &Tokens::error;
    badge.tooltip = i18n::t(QString::fromStdString(s->value("reason", std::string())));
  } else if (state == "syncing") {
    badge.icon = "regen";
    badge.text = tr("syncing…");
    badge.color = &Tokens::fg3;
    badge.tooltip = tr("Reading %1 again").arg(file);
    badge.spin = true;
  } else if (state != "embedded") {
    badge.icon = "check";
    badge.color = &Tokens::assetLinked;
    badge.tooltip = fromProjectCopy(import) ? tr("In sync with the project's copy %1 (not found where it was linked)").arg(file) : tr("In sync with %1").arg(file);
  }
  if (!badge.icon.isEmpty()) d.badges << badge;
  if (s && s->value("lfs", false)) {
    browser::Badge lfs;
    lfs.text = "LFS";
    lfs.tooltip = tr("Stored with Git LFS");
    d.badges << lfs;
  }
  QStringList tip{tr("Linked file: %1").arg(file)};
  if (a) {
    QStringList facts;
    if (const std::string sha = a->asset.value("sha256", ""); !sha.empty()) facts << "SHA-256 " + QString::fromStdString(sha.substr(0, 12));
    if (const QString synced = isoText(a->asset.value("synced", "")); !synced.isEmpty()) facts << tr("synced %1").arg(synced);
    facts << tr("read by %1").arg(builderText(a->asset));
    tip << facts.join(" · ");
  }
  d.tooltip = tip.join('\n');
}

void AssetsArea::section(const PropertySubject& subject, const opad::json&, QList<PropertySection>& out) {
  if (!m_monitor || subject.refs.empty()) return;
  const opad::Node* n = services().document()->node(subject.refs.front().body);
  const AssetMonitor::Asset* a = n ? m_monitor->asset(n->source_op) : nullptr;
  if (!a) return;
  const std::string import = n->source_op;
  const opad::json& asset = a->asset;
  const opad::json* s = m_monitor->state(import);
  const std::string storage = asset.value("storage", "linked");
  const bool embedded = storage == "embedded";
  const QString file = m_monitor->file(import);
  const bool found = s && s->contains("file");
  PropertySection sec;
  sec.title = tr("Linked file");
  sec.rows << qMakePair(tr("File"), name(import)) << qMakePair(tr("Status"), embedded ? tr("Embedded in the document") : stateText(import));
  const bool copy = !embedded && fromProjectCopy(import);
  if (s && s->contains("reason") && !embedded) sec.rows << qMakePair(tr("Note"), i18n::t(QString::fromStdString((*s)["reason"].get<std::string>())));
  else if (copy) sec.rows << qMakePair(tr("Note"), tr("Not found where it was linked: the project's copy is read"));
  const QString recorded = QString::fromStdString(asset.value("path", asset.value("abs", std::string())));
  sec.rows << qMakePair(embedded ? tr("Embedded from") : tr("Path"), native(recorded));
  const QString docDir = QFileInfo(services().document()->path()).absolutePath();
  const QString expected = QDir::isAbsolutePath(recorded) || docDir.isEmpty() ? recorded : docDir + "/" + recorded;  // where it was recorded
  if (found && !embedded && QDir::cleanPath(file).compare(QDir::cleanPath(expected), Qt::CaseInsensitive) != 0) sec.rows << qMakePair(tr("Found at"), native(file));
  QString store = embedded ? tr("Embedded (stored in the document)") : storage == "project" ? tr("Project copy (assets folder)") : tr("Linked (read from the file)");
  if (s && s->value("lfs", false)) store += " · Git LFS";
  sec.rows << qMakePair(tr("Kind"), kindText(asset.value("kind", ""))) << qMakePair(tr("Storage"), store);
  const qint64 bytes = s && s->contains("bytes") ? (*s)["bytes"].get<qint64>() : asset.value("size", qint64(-1));
  if (bytes >= 0) sec.rows << qMakePair(tr("Size"), QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat));
  if (const QString modified = s ? isoText(s->value("modified", "")) : QString(); !modified.isEmpty()) sec.rows << qMakePair(tr("Modified"), modified);
  if (const QString synced = isoText(asset.value("synced", "")); !synced.isEmpty()) sec.rows << qMakePair(tr("Last synced"), synced);
  sec.rows << qMakePair(tr("Read by"), builderText(asset));
  sec.rows << qMakePair(tr("Parts"), a->missing ? tr("%1, %2 missing").arg(a->bodies).arg(a->missing) : QString::number(a->bodies));
  if (const std::string sha = asset.value("sha256", ""); !sha.empty()) sec.rows << qMakePair(QString("SHA-256"), QString::fromStdString(sha.substr(0, 12)));
  if (!embedded) {
    const std::string state = s ? s->value("state", "") : "";
    // After the click: the panel is filled again by what they change.
    auto act = [this, &sec, import](const QString& label, std::function<void(AssetsArea*, const std::string&)> fn) {
      sec.actions << qMakePair(label, std::function<void()>([this, import, fn] { QTimer::singleShot(0, this, [this, import, fn] { fn(this, import); }); }));
    };
    if (state == "changed") act(tr("Sync"), [](AssetsArea* a, const std::string& i) { a->sync({i}); });
    if (state == "missing") act(tr("Locate…"), &AssetsArea::locate);
    if (state == "untrusted") act(tr("Read it…"), &AssetsArea::trust);
    if (found) {
      act(tr("Show in folder"), &AssetsArea::reveal);
      act(tr("Copy path"), &AssetsArea::copyPath);
    }
    act(tr("Replace…"), &AssetsArea::replace);
    act(tr("Embed as editable"), &AssetsArea::embed);
    if (storage == "linked" && !services().document()->doc.path.empty()) act(copy ? tr("Use project copy") : tr("Pack into project"), &AssetsArea::pack);
  }
  out << sec;
}

void AssetsArea::contextMenu(const SelectionContext& selection, QMenu& menu) {
  const std::vector<std::string> ids = imports(selection);
  if (ids.empty() || selection.sketching || services().document()->browse) return;
  const std::string import = ids.front();
  const opad::json* s = m_monitor->state(import);
  const std::string state = s ? s->value("state", "") : "";
  const AssetMonitor::Asset* a = m_monitor->asset(import);
  menu.addSeparator();
  if (ids.size() > 1) {
    menu.addAction(icons::themed("regen", 16), tr("Sync %1 linked files").arg(ids.size()), this, [this, ids] { sync(ids); });
    return;
  }
  if (state == "missing") menu.addAction(icons::themed("locate", 16), tr("Locate linked file…"), this, [this, import] { locate(import); });
  else if (state == "untrusted") menu.addAction(icons::themed("warning", 16), tr("Read linked file…"), this, [this, import] { trust(import); });
  else menu.addAction(icons::themed("regen", 16), tr("Sync linked file"), this, [this, import] { sync({import}); });
  const bool found = s && s->contains("file");
  menu.addAction(icons::themed("open", 16), tr("Show in folder"), this, [this, import] { reveal(import); })->setEnabled(found);
  menu.addAction(icons::themed("copy", 16), tr("Copy path"), this, [this, import] { copyPath(import); });
  menu.addAction(icons::themed("import", 16), tr("Replace linked file…"), this, [this, import] { replace(import); });
  menu.addAction(icons::themed("embed", 16), tr("Embed as editable"), this, [this, import] { embed(import); });
  menu.addAction(icons::themed("pack", 16), fromProjectCopy(import) ? tr("Use project copy") : tr("Pack into project"), this, [this, import] { pack(import); })
      ->setEnabled(a && a->asset.value("storage", "linked") == "linked" && !services().document()->doc.path.empty());
}

// The view's asset layer (LookSource::Asset, under every other look): the parts shown from a file that is not the version
// synced (a changed file read where the version synced is not remembered: what the features were computed from is not on
// screen) are tinted the stale colour, and a file being synced or embedded fades until that is committed.
void AssetsArea::updateLooks() {
  constexpr double kBusyFade = 0.45;
  std::map<std::string, LookDelta> layer;
  for (const std::string& root : m_monitor->roots())
    if (m_monitor->syncing(m_monitor->importOf(root))) layer[root].fade = kBusyFade;
  const QColor& c = theme::current().assetStale;
  for (const std::string& id : m_monitor->stale())
    if (const opad::Node* n = services().document()->node(id)) {
      LookDelta& d = layer[id];  // the nearest entry wins: the fade of its file too
      d.color = looks::mix(n->color, {c.redF(), c.greenF(), c.blueF()}, 0.6);
      if (m_monitor->syncing(n->source_op)) d.fade = kBusyFade;
    }
  services().viewport()->setLookLayer(LookSource::Asset, std::move(layer));
}

// ---------------------------------------------------------------- what the commands do
void AssetsArea::notify(const QString& text, bool undo, int ms) {
  if (m_toast) m_toast->dismiss();
  AppDocument* doc = services().document();
  m_toast = undo ? services().toast(text, tr("Undo"), [doc] { doc->undo(); }, ms) : services().toast(text, {}, {}, ms);
}

void AssetsArea::filesChanged(const std::vector<std::string>&) {
  const std::vector<std::string> all = m_monitor->changed();
  if (all.empty() || m_busy) return;
  if (m_toast) m_toast->dismiss();
  m_toast = services().toast(all.size() == 1 ? tr("%1 changed since the last sync").arg(name(all.front())) : tr("%1 linked files changed since the last sync").arg(all.size()),
                             all.size() == 1 ? tr("Sync") : tr("Sync all"), [this] { syncAll(); }, 15000);
}

void AssetsArea::planned(const QString& title, const QString& label, const std::string& import, std::function<opad::design::Plan(opad::Document&, const Progress&)> plan,
                         std::function<void(bool, const QString&, const opad::json&)> then, int waited) {
  AppDocument* doc = services().document();
  m_busy = true;
  if (!doc->hasDocument || doc->browse) {
    m_busy = false;
    return then(false, tr("There is no document to change."), {});
  }
  m_monitor->setSyncing(import, true);
  auto fail = [this, import, then](const QString& error) {
    m_monitor->setSyncing(import, false);
    m_busy = false;
    then(false, error, {});
  };
  // The copy is made on a worker (AppDocument::captureSnapshot: the document's size is no cost here), and the document
  // stays unchanged from then until the plan is committed. Busy (a recompute, a save, a recovery snapshot): after it.
  const auto generation = doc->generation;
  QPointer<AssetsArea> self(this);
  const bool started = !doc->loading && !doc->converting() &&
                       doc->captureSnapshot(services().jobs(), [self, doc, title, label, import, plan, then, fail, generation](std::shared_ptr<opad::Document> copy, const QString& error) {
    if (!self) return;
    if (!copy) return fail(error);
    if (generation != doc->generation) return fail(tr("The document changed meanwhile; try again."));
    doc->designBusy = true;
    emit doc->undoChanged();
    auto result = std::make_shared<opad::design::Plan>();
    auto reading = std::make_shared<std::atomic<bool>>(true);
    self->services().jobs()->async(title, [copy, result, plan, reading](Progress p) {
      struct Done {
        std::shared_ptr<std::atomic<bool>> flag;
        ~Done() { *flag = false; }
      } done{reading};
      *result = plan(*copy, p);
    }, [self, doc, result, reading, generation, label, import, then, fail](bool ok, const QString& error) {
      if (!self) return;
      afterWorker(self, reading, [self, doc, result, generation, label, import, then, fail, ok, error] {
        if (!self) return;
        doc->designBusy = false;
        emit doc->undoChanged();
        if (generation != doc->generation) return fail(tr("The document changed meanwhile; try again."));
        if (!ok) return fail(error);
        self->m_monitor->setSyncing(import, false);
        self->m_busy = false;
        try {
          const opad::json report = doc->commitPlan(std::move(*result), label);
          then(true, {}, report);
        } catch (const std::exception& e) {
          then(false, QString::fromUtf8(e.what()), {});
        }
      });
    });
  });
  if (started) return;
  if (waited >= 300) return fail(tr("The document is busy; try again in a moment."));
  QTimer::singleShot(100, this, [=, this] { planned(title, label, import, plan, then, waited + 1); });
}

void AssetsArea::sync(std::vector<std::string> imports, const QString& file) {
  for (const auto& import : imports)
    if (std::none_of(m_queue.begin(), m_queue.end(), [&](const auto& q) { return q.first == import; })) m_queue.push_back({import, imports.size() == 1 ? file : QString()});
  if (m_busy) return;
  m_synced = m_syncFailed = 0;
  nextSync();
}

void AssetsArea::syncAll() {
  if (m_monitor) sync(m_monitor->changed());
}

void AssetsArea::nextSync() {
  if (m_queue.empty()) return;
  const auto [import, file] = m_queue.front();
  m_queue.erase(m_queue.begin());
  const QString title = name(import);
  const fs::path from = file.isEmpty() ? fs::path() : fsPath(file);
  opad::AssetOptions options = AssetMonitor::options(services().document());
  planned(tr("Syncing %1").arg(title), tr("sync %1").arg(title), import,
          [import, from, options](opad::Document& doc, const Progress& p) mutable {
            options.progress = [p](double, const std::string&) { return !p.cancelled(); };
            return opad::plan_asset_sync(doc, import, options, from);
          },
          [this, import, title](bool ok, const QString& error, const opad::json& report) {
            if (ok) {
              ++m_synced;
              m_monitor->synced(import, report.value("sha256", ""));
            } else {
              ++m_syncFailed;
            }
            emit done("sync", import, ok, error, report);
            if (!m_queue.empty()) return nextSync();
            if (!ok) {
              notify(tr("%1 could not be synced: %2").arg(title, i18n::t(error)), false, 10000);
            } else if (m_synced + m_syncFailed > 1) {
              notify(m_syncFailed ? tr("%1 linked files synced, %2 could not be").arg(m_synced).arg(m_syncFailed) : tr("%1 linked files synced").arg(m_synced));
            } else if (report.value("up_to_date", false)) {
              notify(tr("%1 is in sync with its file").arg(title));
            } else {
              QString text = tr("Synced %1: %2 parts changed, %3 added, %4 removed")
                                 .arg(title)
                                 .arg(report.value("changed", opad::json::array()).size())
                                 .arg(report.value("added", opad::json::array()).size())
                                 .arg(report.value("removed", opad::json::array()).size());
              if (const size_t errors = report.value("errors", opad::json::array()).size()) text += "; " + tr("%1 later features could not be recomputed").arg(errors);
              notify(text, true, 8000);
            }
            // The parts a file left missing (it was not found before): read now that it is.
            bool missing = false;
            for (const auto& [id, a] : m_monitor->assets()) missing = missing || (a.missing > 0 && a.asset.value("storage", "linked") != "embedded");
            if (missing && !services().document()->designBusy) services().document()->loadAssets(services().jobs(), false);
          });
}

void AssetsArea::locate(const std::string& import) {
  const QString title = name(import);
  const QString start = QFileInfo(m_monitor->file(import)).absolutePath();
  const QString path = QFileDialog::getOpenFileName(services().window(), tr("Locate %1").arg(title), QFileInfo(start).isDir() ? start : QString(),
                                                    tr("%1 (%2);;All files (*)").arg(title, QFileInfo(title).fileName()));
  if (!path.isEmpty()) sync({import}, path);
}

void AssetsArea::replace(const std::string& import) {
  QStringList patterns;
  for (const auto& ext : opad::importable_extensions()) patterns << "*" + QString::fromStdString(ext);
  const QString path = QFileDialog::getOpenFileName(services().window(), tr("Replace %1").arg(name(import)), QFileInfo(m_monitor->file(import)).absolutePath(),
                                                    tr("Design files (%1)").arg(patterns.join(' ')));
  if (!path.isEmpty()) sync({import}, path);
}

void AssetsArea::embed(const std::string& import) {
  const QString title = name(import);
  planned(tr("Embedding %1").arg(title), tr("embed %1").arg(title), import,
          [import](opad::Document& doc, const Progress& p) { return opad::plan_asset_embed(doc, import, [p] { return p.cancelled(); }); },
          [this, import, title](bool ok, const QString& error, const opad::json& report) {
            if (ok) notify(tr("%1 is embedded: its parts are stored in the document and can be edited").arg(title), true, 8000);
            else notify(tr("%1 could not be embedded: %2").arg(title, i18n::t(error)), false, 10000);
            emit done("embed", import, ok, error, report);
          });
}

bool AssetsArea::fromProjectCopy(const std::string& import) const {
  const AssetMonitor::Asset* a = m_monitor ? m_monitor->asset(import) : nullptr;
  const opad::json* s = a ? m_monitor->state(import) : nullptr;
  if (!s || !s->contains("file") || a->asset.value("storage", "linked") != "linked" || services().document()->doc.path.empty()) return false;
  const QString folder = QDir::cleanPath(QFileInfo(services().document()->path()).absolutePath() + "/assets") + "/";
  return QDir::cleanPath(m_monitor->file(import)).startsWith(folder, Qt::CaseInsensitive);
}

void AssetsArea::pack(const std::string& import) {
  const QString title = name(import);
  const bool copy = fromProjectCopy(import);
  const std::string author = QSettings().value("user/name").toString().trimmed().toStdString();
  planned(tr("Packing %1").arg(title), tr("pack %1").arg(title), import,
          [import, author](opad::Document& doc, const Progress&) {
            opad::design::Plan plan;
            plan.report = opad::pack_asset(doc, import, author);  // copies the files; its edit goes on this copy first
            plan.ops.push_back(opad::design::make_edit_op(import, doc.ops.back().data["set"]));
            return plan;
          },
          [this, import, title, copy](bool ok, const QString& error, const opad::json& report) {
            const QString path = native(QString::fromStdString(report.value("path", std::string())));
            if (ok) notify(copy ? tr("%1 now links the project's copy: %2").arg(title, path) : tr("%1 is packed into the project: %2").arg(title, path), true, 8000);
            else notify(tr("%1 could not be packed: %2").arg(title, i18n::t(error)), false, 10000);
            emit done("pack", import, ok, error, report);
          });
}

void AssetsArea::reveal(const std::string& import) {
  const QString file = m_monitor->file(import);
  if (!QFileInfo::exists(file)) return notify(tr("Not found: %1").arg(native(file)));
  const auto [program, arguments] = assets::revealCommand(file);
  QProcess::startDetached(program, arguments);
}

void AssetsArea::copyPath(const std::string& import) {
  const QString file = native(m_monitor->file(import));
  QGuiApplication::clipboard()->setText(file);
  notify(tr("Copied %1").arg(file), false, 3000);
}

void AssetsArea::trust(const std::string& import) {
  assets::askTrust(services().window(), services().document(), services().jobs(), [this](const QString& error) { notify(i18n::t(error), false, 8000); }, {import});
}

void AssetsArea::link() {
  if (!services().requireEditable([this] { link(); })) return;
  QStringList patterns;  // a drawing is placed first (on the selected face, else on a plane picked), as Import… does
  for (const auto& ext : opad::importable_extensions()) patterns << "*" + QString::fromStdString(ext);
  const QString path = QFileDialog::getOpenFileName(services().window(), tr("Link as asset"), QSettings().value("ui/lastDir").toString(), tr("Design files (%1)").arg(patterns.join(' ')));
  if (!path.isEmpty()) services().importFile(path, true);
}

OPAD_AREA(AssetsArea)
