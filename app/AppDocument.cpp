#include "AppDocument.hpp"

#include "opad/assets.hpp"
#include "opad/kicad_pcb.hpp"
#include "opad/geometry.hpp"
#include "opad/drawing_io.hpp"
#include "Jobs.hpp"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSettings>
#include <QThread>
#include <Standard_Failure.hxx>
#include <algorithm>
#include <set>
#include <QMetaObject>
#include <QPointer>

namespace {
bool isExternalPath(const QString& path) {
  QString ext = QFileInfo(path).suffix().toLower();
  return ext != "opad";
}
// A file name as the core's std::filesystem wants it: from UTF-16, since a narrow string is read in the ANSI code page on
// Windows and a file named in Arabic or Chinese did not open.
std::filesystem::path fsPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }
// A replaced document (its body text, its shapes) is let go on a thread of its own: freeing it scales with the model. Reset
// there: QThread::create's callable is destroyed with the thread object, on the UI thread.
void dispose(std::shared_ptr<opad::Document> old) {
  auto* thread = QThread::create([old = std::move(old)]() mutable { old.reset(); });
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start(QThread::LowPriority);
}
// A load's worker: a QThread, never a std::thread (Qt adopts a foreign thread that posts to it, and on MinGW/Qt 6.10 its
// TLS cleanup can fault at exit). Not parented: a load still running when the window goes is left to finish. Its callable
// is destroyed with the thread object, on the UI thread: anything big is freed inside the work.
template <class F>
void start(F work) {
  auto* thread = QThread::create(std::move(work));
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start();
}
QString phaseLabel(const std::string& what, const QString& file) {
  if (what == "reading") return AppDocument::tr("Reading %1").arg(file);
  if (what == "building") return AppDocument::tr("Building document");
  if (what == "preparing") return AppDocument::tr("Preparing bodies");
  if (what == "linked") return AppDocument::tr("Reading linked files");
  if (what.rfind("exporting with KiCad", 0) == 0) return AppDocument::tr("KiCad is exporting %1").arg(file);  // kicad-cli at work
  if (what.rfind("translating", 0) == 0) {  // "translating" or "translating <scope> <i>/<n>" from the STEP reader
    const QString detail = QString::fromStdString(what.substr(11)).trimmed();
    return detail.isEmpty() ? AppDocument::tr("Translating geometry") : AppDocument::tr("Translating %1").arg(detail);
  }
  return QString::fromStdString(what);
}
}  // namespace

AppDocument::AppDocument(QObject* parent) : QObject(parent), m_storage(std::make_shared<opad::Document>()), doc(*m_storage), m_alive(std::make_shared<std::atomic<bool>>(true)) {
  connect(this, &AppDocument::aboutToReplace, this, [this] { setActiveComponent({}); });  // another document: its root
}

AppDocument::~AppDocument() { *m_alive = false; }

opad::KicadOptions AppDocument::kicadOptions() {
  opad::KicadOptions o;
  QSettings s;  // Settings > KiCad boards (KicadDialog)
  for (const QString& dir : s.value("kicad/modelDirs").toStringList())
    if (!dir.trimmed().isEmpty()) o.model_dirs.push_back(fsPath(dir.trimmed()));
  o.components = s.value("kicad/components", true).toBool();
  o.dnp = s.value("kicad/dnp", true).toBool();
  o.vias = s.value("kicad/vias", false).toBool();
  o.placeholder_height = std::clamp(s.value("kicad/placeholderHeight", 1.0).toDouble(), 0.01, 200.0);
  const QString origin = s.value("kicad/origin", "auto").toString();
  o.origin = origin == "center" || origin == "page" ? origin.toStdString() : "auto";
  o.kicad_cli = s.value("kicad/reader", "opad").toString() == "kicad-cli" && !opad::kicad_cli().program.empty();  // else OPAD's reader
  o.tracks = s.value("kicad/tracks", false).toBool();
  o.pads = s.value("kicad/pads", false).toBool();
  o.silkscreen = s.value("kicad/silkscreen", false).toBool();
  return o;
}

opad::AssetOptions AppDocument::assetOptions() {
  opad::AssetOptions o;
  for (const QString& dir : QSettings().value("assets/trusted").toStringList())  // folders the user said to trust
    if (!dir.trimmed().isEmpty()) o.trusted.push_back(fsPath(dir.trimmed()));
  o.kicad = kicadOptions();
  o.derive = opad::derive_asset;  // a board read through kicad-cli: its STEP made again when missing here or synced
  return o;
}

QString AppDocument::assetSummary(const opad::json& states) {
  int changed = 0, missing = 0, untrusted = 0, failed = 0;
  for (const auto& s : states) {
    const std::string state = s.value("state", "");
    changed += state == "changed";
    missing += state == "missing";
    untrusted += state == "untrusted";
    failed += state == "error";
  }
  QStringList parts;
  if (changed) parts << tr("%1 changed since the last sync").arg(changed);
  if (missing) parts << tr("%1 not found").arg(missing);
  if (untrusted) parts << tr("%1 outside the document's project, not read").arg(untrusted);
  if (failed) parts << tr("%1 could not be read").arg(failed);
  return parts.isEmpty() ? QString() : tr("Linked files: %1").arg(parts.join(tr(", ")));
}

opad::ImportOptions AppDocument::loadOptions(const std::shared_ptr<std::atomic<bool>>& cancel, const QString& file) {
  opad::ImportOptions o;
  o.author = QSettings().value("user/name").toString().trimmed().toStdString();
  o.kicad = kicadOptions();
  auto last = std::make_shared<std::pair<std::string, int>>("", -2);
  auto lastEmit = std::make_shared<QElapsedTimer>();
  lastEmit->start();
  auto alive = m_alive;
  o.progress = [this, cancel, alive, last, lastEmit, file](double frac, const std::string& what) {
    const int pct = (what != "reading" && frac >= 0) ? static_cast<int>(frac * 100.0) : -1;  // reading has no progress source
    if (trace::enabled()) trace::log(QStringLiteral("import progress: %1 %2").arg(QString::fromStdString(what)).arg(frac));
    // The STEP reader reports thousands of sub-steps per second; the strip only needs ~20 updates/s, so
    // intermediate ones are dropped unless the phase itself changes.
    const bool newPhase = what.substr(0, what.find(' ')) != last->first.substr(0, last->first.find(' '));
    if (newPhase || ((what != last->first || pct != last->second) && lastEmit->elapsed() >= 50)) {
      *last = {what, pct};
      lastEmit->restart();
      const QString label = phaseLabel(what, file);
      if (*alive) QMetaObject::invokeMethod(this, [this, label, pct] { emit loadProgress(label, pct); }, Qt::QueuedConnection);
    }
    return !*cancel;
  };
  return o;
}

void AppDocument::cancelLoad() {
  if (m_cancel) *m_cancel = true;
  if (!loading) return;
  // The worker may sit in a step that does not poll (OCCT parsing a big STEP): stop waiting for it. Whatever it
  // produces later is dropped, so the next file can open at once.
  ++*m_loadToken;
  loading = false;
  emit loadFinished(false, QStringLiteral("cancelled"));
}

void AppDocument::startOpen(const QString& path, bool asked) {
  if (designBusy || m_converting) return;
  if (loading) cancelLoad();
  loading = true;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_cancel = cancel;
  const unsigned token = ++*m_loadToken;
  auto current = m_loadToken;
  const bool external = isExternalPath(path);
  const bool viewer = external && viewerOpens;
  const QString file = QFileInfo(path).fileName() + QStringLiteral(" (%1 MB)").arg(QFileInfo(path).size() / (1024.0 * 1024.0), 0, 'f', 0);
  opad::ImportOptions o = loadOptions(cancel, file);
  o.viewer = viewer;  // viewer mode: nothing is prepared for saving (no healing, BREP text or hashing)
  const QString suffix = QFileInfo(path).suffix().toLower();
  o.center_drawing = suffix == "dxf" || suffix == "svg" || suffix == "dwg";  // opened on its own: centred on the grid
  // A board's 3D models change without the board: never remembered; drawings are not (a DWG keeps its conversion itself).
  const bool cacheable = viewer && suffix != "kicad_pcb" && opad::viewer_cache_applies(fsPath(path));
  opad::AssetOptions assets = assetOptions();
  assets.progress = [progress = o.progress](double f, const std::string& what) { return progress(f, what == "reading" ? "linked" : what); };
  auto alive = m_alive;
  emit loadProgress(external ? tr("Reading %1").arg(file) : tr("Opening %1").arg(file), -1);
  start([this, alive, cancel, path, external, viewer, cacheable, o, assets, token, current, asked]() {
    auto result = std::make_shared<opad::Document>();
    QString error;
    QStringList warnings;
    opad::json report;
    bool slowRead = false;  // worth remembering (viewer cache): the next open skips the translation
    const DiskStat stat = external ? DiskStat{} : statFile(path);  // before reading: a change meanwhile is noticed later
    const bool locked = !external && (asked || !QFileInfo(path).isWritable());  // read-only: asked for, or write-protected
    std::shared_ptr<const opad::Manifest> manifest;
    double readMs = 0;
    try {
      if (external) {
        *result = opad::Document::create();
        QElapsedTimer clock;
        clock.start();
        if (!cacheable || !opad::viewer_cache_load(*result, fsPath(path), o)) {
          const auto imported=opad::import_file(*result, fsPath(path), o);
          for(const auto& warning:imported.warnings) warnings.append(QString::fromStdString(warning));
          report = imported.to_json();
          readMs = double(clock.nsecsElapsed()) / 1e6;
          if (qEnvironmentVariableIsSet("OPAD_BENCH_CACHE")) readMs = 60000;  // the viewer cache bench: remembered whatever it took
          slowRead = cacheable && readMs > 1500;
        }
      } else {
        *result = opad::Document::load(fsPath(path));
        manifest = std::make_shared<opad::Manifest>(opad::Manifest::of(*result));
        // Linked files are read where they are now; one that is missing or untrusted leaves only its own bodies out.
        if (opad::has_assets(*result)) {
          report["assets"] = opad::json::array();
          for (const auto& s : opad::load_assets(*result, assets)) report["assets"].push_back(s.to_json());
        }
      }
      // Parse the bodies here rather than on the UI thread when they are first displayed.
      if (!*cancel) opad::warm_shape_cache(*result, [&](size_t i, size_t n) { return o.progress(n ? double(i) / double(n) : 1.0, "preparing"); });
      if (*cancel) error = QStringLiteral("cancelled");
    } catch (const Standard_Failure& e) {
      error = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    if (!*alive || current->load() != token) return;  // dropped: freed here, off the UI thread
    QMetaObject::invokeMethod(this, [this, result, error, path, external, viewer, warnings, token, current, slowRead, readMs, o, report, stat, manifest, locked] {
      if (current->load() != token) return;
      loading = false;
      lastLoad = report;
      lastLoad["file"] = path.toStdString();
      if (!error.isEmpty()) {
        emit loadFinished(false, error);
        return;
      }
      assetStates = report.is_object() ? report.value("assets", opad::json::array()) : opad::json::array();
      emit aboutToReplace();
      ++generation;
      dropRollback();
      doc = std::move(*result);
      browse = viewer;
      readOnly = locked;
      viewing = viewer ? QFileInfo(path).absoluteFilePath() : QString();
      m_cacheSource = slowRead ? viewing : QString();
      m_cacheCenter = o.center_drawing;
      m_cacheReadMs = readMs;
      hasDocument = true;
      clearHistory();
      markSaved();  // a viewed file is never "unsaved": closing it asks nothing
      if (external && !viewer) m_savedIds.clear();  // imported for editing: not saved as an OPAD document yet
      if (external) setDisk({}, {}, {});
      else setDisk(QFileInfo(path).absoluteFilePath(), stat, manifest);
      refresh();
      emit pathChanged();
      if (viewer) emit message(tr("Viewing %1 (read-only)").arg(QFileInfo(path).fileName()));
      else if (external) emit message(tr("Imported %1 into a new document").arg(QFileInfo(path).fileName()));
      else if (locked) emit message(tr("Opened %1 read-only").arg(path));
      else emit message(tr("Opened %1").arg(path));
      if(!warnings.isEmpty()) emit message(warnings.join("; "));
      if (const QString linked = assetSummary(assetStates); !linked.isEmpty()) emit message(linked);
      emit loadFinished(true, {});
    }, Qt::QueuedConnection);
  });
}

void AppDocument::startImport(const QString& path, const QString& parent, const opad::Mat4& placement, const opad::json& plane, bool link) {
  if (loading || designBusy) return;
  if (!hasDocument || browse) {
    doc = opad::Document::create();
    assetStates = opad::json::array();
    browse = readOnly = false;
    hasDocument = true;
    clearHistory();
    markSaved();  // an empty, unsaved document: anything imported makes it dirty
    setActiveComponent({});
    setDisk({}, {}, {});
    emit pathChanged();
  }
  loading = true;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_cancel = cancel;
  const QString file = QFileInfo(path).fileName();
  opad::ImportOptions o = loadOptions(cancel, file);
  o.parent = parent.isEmpty() ? m_active : parent.toStdString();  // into the active component unless told otherwise (UI-33)
  o.placement = placement;
  // A drawing placed in world coordinates (on a face, a picked plane) keeps its place under a moved component: its
  // placement is relative to the component it goes into.
  const opad::Mat4 into = !o.parent.empty() && scene.node(o.parent) && (!placement.is_identity() || plane.is_object()) ? scene.world(o.parent).inverse() : opad::Mat4();
  // Import into a snapshot: selection/render callbacks retain a valid live document.
  auto work = std::make_shared<opad::Document>(doc);
  const size_t opsBefore = work->ops.size();
  const bool dirtyBefore = work->dirty;
  auto alive = m_alive;
  const unsigned token = ++*m_loadToken;
  auto current = m_loadToken;
  emit loadProgress(tr("Reading %1").arg(file), -1);
  start([this, alive, cancel, path, o, work, opsBefore, dirtyBefore, plane, link, token, current, into]() mutable {
    QString error;
    opad::json r;
    try {
      if (plane.is_object()) {  // on a face: its frame, resolved here and stored only as the drawing's placement
        const opad::Frame f = opad::design::resolve_plane(*work, opad::resolve(*work), plane);
        const opad::Vec3 n = f.normal();
        opad::Mat4 m;
        for (int r = 0; r < 3; ++r) { m.at(r, 0) = f.x[r]; m.at(r, 1) = f.y[r]; m.at(r, 2) = n[r]; m.at(r, 3) = f.origin[r]; }
        o.placement = m * o.placement;
      }
      if (!into.is_identity()) o.placement = into * o.placement;
      r = (link ? opad::link_file(*work, fsPath(path), o) : opad::import_file(*work, fsPath(path), o)).to_json();
      if (!*cancel) opad::warm_shape_cache(*work, [&](size_t i, size_t n) { return o.progress(n ? double(i) / double(n) : 1.0, "preparing"); });
      if (*cancel) error = QStringLiteral("cancelled");
    } catch (const Standard_Failure& e) {
      error = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    if (!*alive || current->load() != token) return work.reset();  // cancelled: the document never saw it; freed here
    QMetaObject::invokeMethod(this, [this, work, error, r, path, opsBefore, dirtyBefore, token, current] {
      if (current->load() != token) return;
      lastLoad = r;
      lastLoad["file"] = path.toStdString();
      if (!error.isEmpty() && work->ops.size() > opsBefore) {
        // Cancelled after the op was appended: roll it back and drop the orphaned body entries.
        work->ops.resize(opsBefore);
        work->gc();
        work->dirty = dirtyBefore;
      }
      doc = std::move(*work);
      loading = false;
      if (error.isEmpty()) recordStep(tr("import"), opsBefore);
      if (error.isEmpty() && r.contains("info") && r["info"].value("linked", false))  // read just now: as it is
        assetStates.push_back({{"import", r.value("op", "")}, {"name", QFileInfo(path).fileName().toStdString()}, {"kind", r["info"].value("kind", "")},
                               {"storage", "linked"}, {"state", "ok"}, {"bodies", r.value("bodies", 0)}});
      refresh();
      emit undoChanged();
      if (!error.isEmpty()) {
        emit loadFinished(false, error);
        return;
      }
      emit message(tr("Imported %1: %2 bodies, %3 new body entries").arg(QFileInfo(path).fileName()).arg(r.value("bodies", 0)).arg(r.value("new_entries", 0)));
      if(r.contains("warnings") && !r["warnings"].empty()) {
        QStringList warnings; for(const auto& w:r["warnings"]) warnings.append(QString::fromStdString(w.get<std::string>()));
        emit message(warnings.join("; "));
      }
      emit loadFinished(true, {});
    }, Qt::QueuedConnection);
    work.reset();  // the posted copy is the last one: never the thread's callable, which is freed on the UI thread
  });
}

void AppDocument::newDocument() {
  if (loading || designBusy) return;
  emit aboutToReplace();
  ++generation;
  dropRollback();
  doc = opad::Document::create();
  assetStates = opad::json::array();
  browse = readOnly = false;
  hasDocument = true;
  clearHistory();
  markSaved();
  setDisk({}, {}, {});
  refresh();
  emit pathChanged();
  emit newDocumentCreated();
}

void AppDocument::detachCopy() {
  if (!hasDocument || browse) return;
  doc.path.clear();
  doc.header.uuid = opad::new_uuid();
  doc.header.created = opad::now_iso8601();
  emit pathChanged();
}

void AppDocument::closeDocument() {
  if (loading || designBusy) return;
  emit aboutToReplace();
  ++generation;
  dropRollback();
  doc = opad::Document();
  assetStates = opad::json::array();
  browse = readOnly = false;
  hasDocument = false;
  clearHistory();
  markSaved();
  setDisk({}, {}, {});
  refresh();
  emit pathChanged();
}

void AppDocument::open(const QString& path) {
  if (loading || designBusy) throw opad::Error("Document is busy; try again when the operation finishes.");
  QString ext = QFileInfo(path).suffix().toLower();
  opad::Document next;
  const DiskStat stat = ext == "opad" ? statFile(path) : DiskStat{};
  if (ext != "opad") {
    next = opad::Document::create();
    opad::ImportOptions options;
    options.author = QSettings().value("user/name").toString().trimmed().toStdString();
    opad::import_file(next, fsPath(path), options);
    emit message(tr("Imported %1 into a new document").arg(QFileInfo(path).fileName()));
  } else {
    next = opad::Document::load(fsPath(path));
    assetStates = opad::json::array();  // synchronous: linked files stay unread (loadAssets reads them on a worker)
    emit message(tr("Opened %1").arg(path));
  }
  emit aboutToReplace();
  ++generation;
  dropRollback();
  doc = std::move(next);
  browse = readOnly = false;
  hasDocument = true;
  clearHistory();
  markSaved();
  if (ext != "opad") m_savedIds.clear();
  if (ext != "opad") setDisk({}, {}, {});
  else setDisk(QFileInfo(path).absoluteFilePath(), stat, std::make_shared<opad::Manifest>(opad::Manifest::of(doc)));
  refresh();
  emit pathChanged();
}

void AppDocument::importStep(const QString& path, const QString& parent) {
  if (loading || designBusy) throw opad::Error("Document is busy; try again when the operation finishes.");
  if (!hasDocument || browse) {
    doc = opad::Document::create();
    browse = readOnly = false;
    hasDocument = true;
    clearHistory();
    markSaved();
    setDisk({}, {}, {});
    emit pathChanged();
  }
  opad::json args;
  args["file"] = path.toStdString();
  if (!parent.isEmpty()) args["parent"] = parent.toStdString();
  opad::json r = run("import", args);
  emit message(tr("Imported %1: %2 bodies, %3 new body entries")
                   .arg(QFileInfo(path).fileName())
                   .arg(r.value("bodies", 0))
                   .arg(r.value("new_entries", 0)));
}

bool AppDocument::save(bool overwriteDisk) {
  if (loading || m_capturing) throw opad::Error("Document snapshot is in progress; try saving again shortly.");
  if (browse) throw opad::Error("viewer mode: export to an OPAD document first");
  if (readOnly) throw opad::Error("This document is open read-only: save a copy to edit it.");
  if (!overwriteDisk && !doc.path.empty() && QFileInfo(path()) == QFileInfo(m_diskFile) && diskChanged()) {
    emit message(tr("Not saved: %1 changed on disk").arg(QFileInfo(m_diskFile).fileName()));
    emit saveBlocked(true);
    return false;
  }
  doc.save();
  markSaved();
  wroteDisk();
  emit pathChanged();
  emit saved();
  emit message(tr("Saved %1").arg(path()));
  return true;
}

bool AppDocument::saveAs(const QString& path) {
  if (loading || m_capturing) throw opad::Error("Document snapshot is in progress; try saving again shortly.");
  if (browse) throw opad::Error("viewer mode: export to an OPAD document first");
  if (readOnly && QFileInfo(path) == QFileInfo(this->path())) throw opad::Error("This document is open read-only: save a copy to edit it.");
  if (QFileInfo(path) == QFileInfo(m_diskFile) && diskChanged()) {  // the open file chosen again: the same as Save
    emit message(tr("Not saved: %1 changed on disk").arg(QFileInfo(m_diskFile).fileName()));
    emit saveBlocked(true);
    return false;
  }
  doc.save_as(fsPath(path));
  readOnly = false;  // the copy is this session's file now
  markSaved();
  wroteDisk();
  emit pathChanged();
  emit saved();
  emit message(tr("Saved %1").arg(path));
  return true;
}

opad::json AppDocument::run(const std::string& command, opad::json args, const QString& label) {
  if (m_converting) throw opad::Error("The document is being prepared for editing; try again in a moment.");
  if (designBusy) throw opad::Error("The design is being recomputed; try again in a moment.");
  // Viewer mode changes how things look (shown, colour, opacity), never the model.
  if (browse && command != "appearance") throw opad::Error("Viewer mode: save the file as an OPAD document to edit it.");
  if (readOnly && command != "appearance") throw opad::Error("This document is open read-only: save a copy to edit it.");
  const size_t before = doc.ops.size();
  if (!args.contains("by")) args["by"] = QSettings().value("user/name").toString().trimmed().toStdString();
  opad::json out;
  try {
    out = opad::commands::run(command, args, &doc);
  } catch (const opad::LockedError& e) {
    throw opad::Error(lockedMessage(e).toStdString());
  }
  if (m_batching) return out;
  recordStep(label.isEmpty() ? labelFor(command, args) : label, before);
  refresh();
  return out;
}

opad::json AppDocument::runAll(const std::vector<std::pair<std::string, opad::json>>& commands, const QString& label) {
  if (m_converting) throw opad::Error("The document is being prepared for editing; try again in a moment.");
  if (designBusy) throw opad::Error("The design is being recomputed; try again in a moment.");
  for (const auto& [command, args] : commands) {
    if (browse && command != "appearance") throw opad::Error("Viewer mode: save the file as an OPAD document to edit it.");
    if (readOnly && command != "appearance") throw opad::Error("This document is open read-only: save a copy to edit it.");
  }
  const size_t before = doc.ops.size();
  const std::string by = QSettings().value("user/name").toString().trimmed().toStdString();
  opad::json out = opad::json::array();
  try {
    for (auto [command, args] : commands) {
      if (!args.contains("by")) args["by"] = by;
      out.push_back(opad::commands::run(command, args, &doc));
    }
  } catch (const opad::LockedError& e) {
    doc.truncate_ops(before);  // all or nothing
    updateDirty();
    throw opad::Error(lockedMessage(e).toStdString());
  } catch (...) {
    doc.truncate_ops(before);
    updateDirty();
    throw;
  }
  if (m_batching) return out;
  recordStep(label, before);
  refresh();
  return out;
}

QString AppDocument::lockedMessage(const opad::LockedError& e) {
  const QString node = QString::fromStdString(e.node), holder = QString::fromStdString(e.holder);
  QString text;
  if (e.node == e.holder)
    text = e.change == "removing" ? tr("“%1” is locked: unlock it before removing it").arg(node)
         : e.change == "moving"   ? tr("“%1” is locked: unlock it before moving it").arg(node)
                                  : tr("“%1” is locked: unlock it before changing it").arg(node);
  else
    text = e.change == "removing" ? tr("“%1” is locked with “%2”: unlock “%2” before removing it").arg(node, holder)
         : e.change == "moving"   ? tr("“%1” is locked with “%2”: unlock “%2” before moving it").arg(node, holder)
                                  : tr("“%1” is locked with “%2”: unlock “%2” before changing it").arg(node, holder);
  return e.more > 0 ? tr("%1 (and %2 more locked)").arg(text).arg(e.more) : text;
}

void AppDocument::batch(const QString& label, const std::function<void()>& commands) {
  if (m_batching) return commands();
  const size_t before = doc.ops.size();
  m_batching = true;
  try {
    commands();
  } catch (...) {
    m_batching = false;
    if (doc.ops.size() > before) doc.truncate_ops(before);
    refresh();
    throw;
  }
  m_batching = false;
  recordStep(label, before);
  refresh();
}

opad::json AppDocument::commitPlan(opad::design::Plan&& plan, const QString& label) {
  if (m_capturing) throw opad::Error("Document snapshot is in progress; try again shortly.");
  if (browse) throw opad::Error("Viewer mode: save the file as an OPAD document to edit it.");
  if (readOnly) throw opad::Error("This document is open read-only: save a copy to edit it.");
  const size_t before = doc.ops.size();
  opad::json report = opad::design::commit(doc, std::move(plan), QSettings().value("user/name").toString().trimmed().toStdString());
  recordStep(label, before);
  refresh();
  return report;
}

void AppDocument::dropRollback() {  // another document: no roll-back of the user's or an editor's
  m_rollback.clear();
  m_userRollback = false;
  m_resume.clear();
}

void AppDocument::setRollback(const std::string& opId) {
  if (opId.empty() && rolledBack()) return;  // the user's roll-back: no editor's to end
  if (!opId.empty() && rolledBack()) m_resume = m_rollback;  // an editor takes it over: back there when it is done
  m_userRollback = false;
  if (opId.empty() && !m_resume.empty())
    if (const std::string resume = std::exchange(m_resume, {}); doc.find_op(resume)) return rollBackTo(resume);
  if (m_rollback == opId) return;
  m_rollback = opId;
  refresh();
}

void AppDocument::rollBackTo(const std::string& opId) {
  if (m_rollback == opId && m_userRollback == !opId.empty()) return;
  const bool moved = m_rollback != opId;
  m_rollback = opId;
  m_userRollback = !opId.empty();
  if (moved) refresh();
}

void AppDocument::refresh() {
  if (!m_rollback.empty() && !doc.find_op(m_rollback)) m_rollback.clear();  // undone or closed
  if (!m_resume.empty() && !doc.find_op(m_resume)) m_resume.clear();
  scene = hasDocument ? opad::resolve(doc, m_rollback) : opad::Scene{};
  if (!m_rollback.empty())  // an earlier op edited: values are still shown and typed in the document's unit, the last one
    for (const auto& e : opad::effective_ops(doc))
      if (e.op->type == "units" && e.data().contains("length")) scene.units = e.data()["length"].get<std::string>();
  updateDirty();
  ++revision;
  checkActive();
  emit changed();
}

void AppDocument::recover(opad::Document&& document,opad::Scene&& resolved) {recover(std::move(document),std::move(resolved),Recovered{});}

void AppDocument::recover(opad::Document&& document,opad::Scene&& resolved,const Recovered& into) {
  if(loading || designBusy)throw opad::Error("Document is busy; try recovery again shortly.");
  emit aboutToReplace();++generation;++revision;m_rollback.clear();m_userRollback=false;m_resume.clear();
  dispose(std::make_shared<opad::Document>(std::move(doc)));
  doc=std::move(document);scene=std::move(resolved);
  browse=readOnly=false;hasDocument=true;clearHistory();m_savedIds.clear();m_savedBodies=0;
  if(into.file.isEmpty()){doc.path.clear();doc.dirty=true;setDisk({},{},{});}
  else {
    const QString file=QFileInfo(into.file).absoluteFilePath();
    doc.path=fsPath(file);
    for(size_t i=0;i<std::min(into.saved,doc.ops.size());++i)m_savedIds.push_back(doc.ops[i].id);
    updateDirty();setDisk(file,into.stat,into.base);
  }
  emit changed();emit pathChanged();
}

// ---------------------------------------------------------------- undo / redo
void AppDocument::commitSnapshot(opad::Document& document,opad::Scene& resolved,
                                 unsigned long long expectedRevision,const QString& label) {
  if(browse) throw opad::Error("viewer_mode: the open file is shown read-only; the user has to save it as an OPAD document before it can be edited");
  if(readOnly) throw opad::Error("read_only: the document is open read-only; the user has to save a copy before it can be edited");
  if(loading || designBusy || revision!=expectedRevision || document.header.uuid!=doc.header.uuid)
    throw opad::Error("stale_revision: the document changed while the agent was working");
  const auto before=doc.ops.size();
  document.path=doc.path; // Save As may have changed the path without changing geometry.
  std::swap(doc,document);std::swap(scene,resolved);m_rollback.clear();m_userRollback=false;m_resume.clear();
  recordStep(label,before);updateDirty();++revision;checkActive();emit changed();emit undoChanged();
}

void AppDocument::detach() {
  if (!readOnly || loading || designBusy || m_capturing) return;
  readOnly = false;
  doc.path.clear();
  m_savedIds.clear();  // saved nowhere: closing asks
  m_savedBodies = 0;
  setDisk({}, {}, {});
  updateDirty();
  ++revision;
  emit changed();
  emit pathChanged();
  emit undoChanged();
}

// Ops appended from `from` on that change only ops before `point`: edits (and the regeneration they bring) and tombstones,
// which a model rolled back to the point shows.
bool AppDocument::changesBefore(const std::string& point, size_t from) const {
  auto index = [this](const std::string& id) {
    const opad::Op* op = doc.find_op(id);
    return op ? size_t(op - doc.ops.data()) : doc.ops.size();
  };
  const size_t at = std::min(index(point), from);
  for (size_t i = from; i < doc.ops.size(); ++i) {
    const opad::Op& op = doc.ops[i];
    if (op.type == "regen" || op.type == "appearance") continue;  // how things look shows on the earlier state too (resolve)
    if ((op.type != "edit" && op.type != "delete") || index(op.data.value("target", "")) >= at) return false;
  }
  return true;
}

void AppDocument::recordStep(const QString& label, size_t opsBefore) {
  if (doc.ops.size() <= opsBefore) return;  // the command appended nothing
  // Rolled back by the user (or an editor over it): an edit of an earlier step shows there and the model stays rolled
  // back; any other step goes at the end and is shown from there (the caller refreshes; an editor rolls forward as it ends).
  if (const std::string& point = rolledBack() ? m_rollback : m_resume; !point.empty() && !changesBefore(point, opsBefore)) {
    if (rolledBack()) m_rollback.clear();
    m_userRollback = false;
    m_resume.clear();
    emit message(tr("Rolled forward to the end of the timeline: new steps are added there."));
  }
  m_undo.push_back(Step{label, doc.ops.size() - opsBefore, {}});
  m_redo.clear();
  while (static_cast<int>(m_undo.size()) > m_undoLimit) m_undo.erase(m_undo.begin());
  emit undoChanged();
}

void AppDocument::undo(int steps) {
  if(m_capturing){const auto identity=generation;QTimer::singleShot(10,this,[this,identity,steps]{if(generation==identity)undo(steps);});return;}
  if (!canUndo()) return;
  for (; steps > 0 && !m_undo.empty(); --steps) {
    Step s = std::move(m_undo.back());
    m_undo.pop_back();
    s.ops = doc.truncate_ops(doc.ops.size() - std::min(s.count, doc.ops.size()));
    m_redo.push_back(std::move(s));
  }
  refresh();
  emit undoChanged();
}

void AppDocument::redo(int steps) {
  if(m_capturing){const auto identity=generation;QTimer::singleShot(10,this,[this,identity,steps]{if(generation==identity)redo(steps);});return;}
  if (!canRedo()) return;
  for (; steps > 0 && !m_redo.empty(); --steps) {
    Step s = std::move(m_redo.back());
    m_redo.pop_back();
    s.count = s.ops.size();
    doc.restore_ops(std::move(s.ops));
    s.ops.clear();
    m_undo.push_back(std::move(s));
  }
  refresh();
  emit undoChanged();
}

QStringList AppDocument::undoLabels() const {
  QStringList out;
  for (auto s = m_undo.rbegin(); s != m_undo.rend(); ++s) out << s->label;
  return out;
}

QStringList AppDocument::redoLabels() const {
  QStringList out;
  for (auto s = m_redo.rbegin(); s != m_redo.rend(); ++s) out << s->label;
  return out;
}

void AppDocument::setUndoLimit(int steps) {
  m_undoLimit = std::clamp(steps, 1, 1000);
  while (static_cast<int>(m_undo.size()) > m_undoLimit) m_undo.erase(m_undo.begin());
  emit undoChanged();
}

void AppDocument::clearHistory() {
  m_undo.clear();
  m_redo.clear();
  emit undoChanged();
}

void AppDocument::markSaved() {
  m_savedIds.clear();
  for (const auto& o : doc.ops) m_savedIds.push_back(o.id);
  m_savedBodies = doc.body_count();
  doc.dirty = false;
}

void AppDocument::updateDirty() {
  if (!hasDocument) return;
  // Entries left behind by an undone import or feature are not a change: the same log refers to the same bodies.
  bool same = doc.ops.size() == m_savedIds.size();
  for (size_t i = 0; same && i < m_savedIds.size(); ++i) same = doc.ops[i].id == m_savedIds[i];
  doc.dirty = !same;
}

QString AppDocument::labelFor(const std::string& command, const opad::json& args) {
  if (command == "appearance") {
    if (args.contains("visible")) return args["visible"].get<bool>() ? tr("show") : tr("hide");
    if (args.contains("color")) return tr("colour");
    if (args.contains("locked")) return args["locked"].get<bool>() ? tr("lock") : tr("unlock");
    return tr("appearance");
  }
  if (command == "rename") return tr("rename");
  if (command == "reparent") return tr("move");
  if (command == "delete") return tr("delete");
    if (command == "annotate") return args.contains("drawing") ? tr("hand drawing") : tr("note");
    if (command == "delete_annotation") return tr("delete annotation");
  if (command == "append") {
    // Several records in one step (the browser's drawing rows tombstone or rename them), or one: a pinned measurement, a
    // comment, or an edit of a note's text or tag.
    const opad::json ops = args.value("ops", opad::json::array());
    if (!ops.empty()) {
      const auto all = [&](const char* type) { return std::all_of(ops.begin(), ops.end(), [&](const opad::json& o) { return o.value("op", "") == type; }); };
      return all("delete") ? tr("delete") : all("edit") ? tr("rename") : tr("pin measurement");
    }
    const std::string type = args.contains("op") && args["op"].is_object() ? args["op"].value("op", "") : "";
    if (type == "measurement") return tr("pin measurement");
    if (type == "annotation") return args["op"].contains("reply_to") ? tr("comment") : tr("note");
    if (type == "edit") return tr("edit note");
    return tr("change");
  }
  if (command == "sheet_edit") {
    const opad::json set = args.value("set", opad::json::object());
    return set.size() == 1 && set.contains("name") ? tr("rename") : tr("edit drawing");
  }
  if (command == "part_properties") return args.value("document", false) ? tr("document properties") : tr("part properties");
  if (command == "section") return tr("named section");
  if (command == "view") return tr("named view");
  if (command == "import") return tr("import");
  if (command == "transform") return tr("transform");
  if (command == "component") return tr("new component");
  return QString::fromStdString(command);
}

QString AppDocument::title() const {
  if (!hasDocument) return tr("OPAD");
  QString name = doc.path.empty() ? tr("Untitled") : QString::fromStdU16String(doc.path.filename().u16string());
  if (browse) return tr("%1 · Viewer - OPAD").arg(QFileInfo(viewing).fileName());
  if (readOnly) return tr("%1 (read-only) - OPAD").arg(name);
  if (isDirty()) name += "*";
  return name + " - OPAD";
}

QString AppDocument::path() const { return QString::fromStdU16String(doc.path.u16string()); }

// ---------------------------------------------------------------- the file on disk (UI-56)
AppDocument::DiskStat AppDocument::statFile(const QString& file) {
  const QFileInfo info(file);
  if (file.isEmpty() || !info.exists()) return {};
  return {true, info.size(), info.lastModified().toMSecsSinceEpoch()};
}

AppDocument::DiskRead AppDocument::readDisk(const QString& file, std::shared_ptr<const opad::Manifest> base, std::shared_ptr<opad::ShapeCache> cache, bool skipKnown) {
  DiskRead r;
  r.file = file;
  r.base = base;
  r.stat = statFile(file);
  if (!r.stat.exists) return r;
  try {
    std::set<std::string> listed;
    auto doc = std::make_shared<opad::Document>(opad::Document::load(fsPath(file), [&](const std::string& key) {
      if (listed.insert(key).second) r.bodies.push_back(key);
      return skipKnown && base && base->bodies.count(key) > 0;
    }));
    auto manifest = std::make_shared<opad::Manifest>(opad::Manifest::of(*doc));
    manifest->bodies.insert(r.bodies.begin(), r.bodies.end());
    r.relation = base ? opad::relation(*base, *doc) : opad::Relation::rewritten;  // nothing to compare with: never "same"
    if (cache) {  // the bodies read, parsed here rather than on the UI thread when they are displayed
      doc->shape_cache = std::move(cache);
      opad::warm_shape_cache(*doc);
    }
    r.doc = std::move(doc);
    r.manifest = std::move(manifest);
  } catch (const Standard_Failure& e) {
    r.error = QString::fromUtf8(e.GetMessageString());
  } catch (const std::exception& e) {
    r.error = QString::fromUtf8(e.what());
  }
  return r;
}

bool AppDocument::diskChanged() const {
  if (m_diskFile.isEmpty()) return false;
  const DiskStat now = statFile(m_diskFile);
  return now.exists && now != m_diskStat;
}

opad::MergePlan AppDocument::planDisk(const DiskRead& read) const {
  opad::MergePlan plan;
  if (!read.doc || !read.base || read.base != m_diskBase || QFileInfo(read.file) != QFileInfo(m_diskFile)) plan.error = "the file was read for another state of the document";
  else plan = opad::plan_merge(*read.base, doc, *read.doc);
  return plan;
}

opad::MergePlan AppDocument::mergeDisk(DiskRead&& read, const QString& label) {
  if (loading || designBusy || m_capturing || m_converting) throw opad::Error("The document is busy; try again in a moment.");
  opad::MergePlan plan = planDisk(read);
  if (!plan.error.empty()) throw opad::Error(plan.error);
  // Undo: the steps of the unsaved ops stay on top. Below them the merge is one step when it only inserted there;
  // otherwise (the file's new ops sit between saved ones) what came before can no longer be undone.
  const size_t mine = plan.mine.size();
  size_t covered = 0, at = m_undo.size();
  while (at > 0 && covered < mine) covered += m_undo[--at].count;
  opad::apply_merge(doc, *read.doc, plan, read.bodies);
  if (plan.shared || covered > mine) m_undo.clear();
  else if (covered == mine && !plan.appended) m_undo.erase(m_undo.begin(), m_undo.begin() + static_cast<std::ptrdiff_t>(at));
  else if (covered == mine && plan.incoming) m_undo.insert(m_undo.begin() + static_cast<std::ptrdiff_t>(at), Step{label, plan.incoming, {}});
  while (static_cast<int>(m_undo.size()) > m_undoLimit) m_undo.erase(m_undo.begin());
  m_redo.clear();
  m_savedIds.clear();  // the file's state: everything but the unsaved ops, now last
  for (size_t i = 0; i + mine < doc.ops.size(); ++i) m_savedIds.push_back(doc.ops[i].id);
  m_savedBodies = read.bodies.size();
  setDisk(read.file, read.stat, read.manifest);
  refresh();
  emit undoChanged();
  return plan;
}

void AppDocument::reloadDisk(DiskRead&& read) {
  if (loading || designBusy || m_capturing || m_converting) throw opad::Error("The document is busy; try again in a moment.");
  if (!read.doc) throw opad::Error(read.error.isEmpty() ? std::string("the file could not be read") : read.error.toStdString());
  for (const auto& key : read.bodies)
    if (!doc.has_body(key) && !read.doc->has_body(key)) throw opad::Error("body entry missing: " + key);
  emit aboutToReplace();
  ++generation;
  m_rollback.clear();
  doc.arrange_bodies(read.bodies, *read.doc, false);
  doc.ops = std::move(read.doc->ops);
  doc.header = read.doc->header;
  doc.path = fsPath(read.file);
  clearHistory();
  markSaved();
  m_savedBodies = doc.body_count();
  setDisk(read.file, read.stat, read.manifest);
  refresh();
  emit pathChanged();
  emit message(tr("Reloaded %1 from disk").arg(QFileInfo(read.file).fileName()));
}

void AppDocument::acceptDisk(const DiskRead& read) {
  if (read.manifest && QFileInfo(read.file) == QFileInfo(m_diskFile)) setDisk(read.file, read.stat, read.manifest);
}

void AppDocument::setDisk(const QString& file, const DiskStat& stat, std::shared_ptr<const opad::Manifest> base) {
  m_diskFile = file;
  m_diskStat = stat;
  m_diskBase = std::move(base);
}

void AppDocument::wroteDisk() {
  const QString file = QFileInfo(path()).absoluteFilePath();
  setDisk(file, statFile(file), std::make_shared<opad::Manifest>(opad::Manifest::of(doc)));
}

QString AppDocument::nodeName(const std::string& id) const {
  const opad::Node* n = scene.node(id);
  return n ? QString::fromStdString(n->name) : QString::fromStdString(id.substr(0, 8));
}

static QString rememberKey(const opad::Document& doc) { return "view/active/" + QString::fromStdString(doc.header.uuid); }

void AppDocument::setActiveComponent(const std::string& id, bool remember) {
  const opad::Node* n = id.empty() ? nullptr : scene.node(id);
  if (!id.empty() && (!n || n->kind != opad::Node::Kind::Component)) throw opad::Error("Only a component can be activated.");
  if (remember && !browse && !doc.header.uuid.empty()) {
    QSettings settings;
    if (id.empty()) settings.remove(rememberKey(doc));
    else settings.setValue(rememberKey(doc), QString::fromStdString(id));
  }
  if (m_active == id) return;
  m_active = id;
  emit activeComponentChanged();
}

std::string AppDocument::rememberedComponent() const {
  if (browse || doc.header.uuid.empty()) return {};
  const std::string id = QSettings().value(rememberKey(doc)).toString().toStdString();
  const opad::Node* n = id.empty() ? nullptr : scene.node(id);
  return n && n->kind == opad::Node::Kind::Component ? id : std::string();
}

void AppDocument::checkActive() {
  if (m_active.empty() || !m_rollback.empty()) return;  // rolled back: the component may come later in the log
  const opad::Node* n = scene.node(m_active);
  if (n && n->kind == opad::Node::Kind::Component) return;
  m_active.clear();
  emit activeComponentChanged();
}
