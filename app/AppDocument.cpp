#include "AppDocument.hpp"

#include "opad/geometry.hpp"
#include "opad/drawing_io.hpp"
#include "Jobs.hpp"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSettings>
#include <Standard_Failure.hxx>
#include <algorithm>
#include <QMetaObject>
#include <QPointer>
#include <thread>

namespace {
bool isExternalPath(const QString& path) {
  QString ext = QFileInfo(path).suffix().toLower();
  return ext != "opad";
}
// A file name as the core's std::filesystem wants it: from UTF-16, since a narrow string is read in the ANSI code page on
// Windows and a file named in Arabic or Chinese did not open.
std::filesystem::path fsPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }
QString phaseLabel(const std::string& what, const QString& file) {
  if (what == "reading") return AppDocument::tr("Reading %1").arg(file);
  if (what == "building") return AppDocument::tr("Building document");
  if (what == "preparing") return AppDocument::tr("Preparing bodies");
  if (what.rfind("translating", 0) == 0) {  // "translating" or "translating <scope> <i>/<n>" from the STEP reader
    const QString detail = QString::fromStdString(what.substr(11)).trimmed();
    return detail.isEmpty() ? AppDocument::tr("Translating geometry") : AppDocument::tr("Translating %1").arg(detail);
  }
  return QString::fromStdString(what);
}
}  // namespace

AppDocument::AppDocument(QObject* parent) : QObject(parent), m_storage(std::make_shared<opad::Document>()), doc(*m_storage), m_alive(std::make_shared<std::atomic<bool>>(true)) {}

AppDocument::~AppDocument() { *m_alive = false; }

opad::ImportOptions AppDocument::loadOptions(const std::shared_ptr<std::atomic<bool>>& cancel, const QString& file) {
  opad::ImportOptions o;
  o.author = QSettings().value("user/name").toString().trimmed().toStdString();
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

void AppDocument::startOpen(const QString& path) {
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
  auto alive = m_alive;
  emit loadProgress(external ? tr("Reading %1").arg(file) : tr("Opening %1").arg(file), -1);
  std::thread([this, alive, cancel, path, external, viewer, o, token, current]() {
    auto result = std::make_shared<opad::Document>();
    QString error;
    QStringList warnings;
    bool slowRead = false;  // worth remembering (viewer cache): the next open skips the translation
    try {
      if (external) {
        *result = opad::Document::create();
        QElapsedTimer clock;
        clock.start();
        if (!viewer || !opad::viewer_cache_load(*result, fsPath(path), o)) {
          const auto imported=opad::import_file(*result, fsPath(path), o);
          for(const auto& warning:imported.warnings) warnings.append(QString::fromStdString(warning));
          slowRead = viewer && clock.elapsed() > 1500;
        }
      } else *result = opad::Document::load(fsPath(path));
      // Parse the bodies here rather than on the UI thread when they are first displayed.
      if (!*cancel) opad::warm_shape_cache(*result, [&](size_t i, size_t n) { return o.progress(n ? double(i) / double(n) : 1.0, "preparing"); });
      if (*cancel) error = QStringLiteral("cancelled");
    } catch (const Standard_Failure& e) {
      error = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    if (!*alive || current->load() != token) return;  // dropped: freed here, off the UI thread
    QMetaObject::invokeMethod(this, [this, result, error, path, external, viewer, warnings, token, current, slowRead, o] {
      if (current->load() != token) return;
      loading = false;
      if (!error.isEmpty()) {
        emit loadFinished(false, error);
        return;
      }
      emit aboutToReplace();
      ++generation;
      m_rollback.clear();
      doc = std::move(*result);
      browse = viewer;
      viewing = viewer ? QFileInfo(path).absoluteFilePath() : QString();
      m_cacheSource = slowRead ? viewing : QString();
      m_cacheCenter = o.center_drawing;
      hasDocument = true;
      clearHistory();
      markSaved();  // a viewed file is never "unsaved": closing it asks nothing
      if (external && !viewer) m_savedIds.clear();  // imported for editing: not saved as an OPAD document yet
      refresh();
      emit pathChanged();
      if (viewer) emit message(tr("Viewing %1 (read-only)").arg(QFileInfo(path).fileName()));
      else if (external) emit message(tr("Imported %1 into a new document").arg(QFileInfo(path).fileName()));
      else emit message(tr("Opened %1").arg(path));
      if(!warnings.isEmpty()) emit message(warnings.join("; "));
      emit loadFinished(true, {});
    }, Qt::QueuedConnection);
  }).detach();
}

void AppDocument::startImport(const QString& path, const QString& parent, const opad::Mat4& placement, const opad::json& plane) {
  if (loading || designBusy) return;
  if (!hasDocument || browse) {
    doc = opad::Document::create();
    browse = false;
    hasDocument = true;
    clearHistory();
    markSaved();  // an empty, unsaved document: anything imported makes it dirty
    emit pathChanged();
  }
  loading = true;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_cancel = cancel;
  const QString file = QFileInfo(path).fileName();
  opad::ImportOptions o = loadOptions(cancel, file);
  o.parent = parent.toStdString();
  o.placement = placement;
  // Import into a snapshot: selection/render callbacks retain a valid live document.
  auto work = std::make_shared<opad::Document>(doc);
  const size_t opsBefore = work->ops.size();
  const bool dirtyBefore = work->dirty;
  auto alive = m_alive;
  const unsigned token = ++*m_loadToken;
  auto current = m_loadToken;
  emit loadProgress(tr("Reading %1").arg(file), -1);
  std::thread([this, alive, cancel, path, o, work, opsBefore, dirtyBefore, plane, token, current]() mutable {
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
      r = opad::import_file(*work, fsPath(path), o).to_json();
      if (!*cancel) opad::warm_shape_cache(*work, [&](size_t i, size_t n) { return o.progress(n ? double(i) / double(n) : 1.0, "preparing"); });
      if (*cancel) error = QStringLiteral("cancelled");
    } catch (const Standard_Failure& e) {
      error = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    if (!*alive || current->load() != token) return;  // cancelled: the document never saw it
    QMetaObject::invokeMethod(this, [this, work, error, r, path, opsBefore, dirtyBefore, token, current] {
      if (current->load() != token) return;
      if (!error.isEmpty() && work->ops.size() > opsBefore) {
        // Cancelled after the op was appended: roll it back and drop the orphaned body entries.
        work->ops.resize(opsBefore);
        work->gc();
        work->dirty = dirtyBefore;
      }
      doc = std::move(*work);
      loading = false;
      if (error.isEmpty()) recordStep(tr("import"), opsBefore);
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
  }).detach();
}

void AppDocument::newDocument() {
  if (loading || designBusy) return;
  emit aboutToReplace();
  ++generation;
  m_rollback.clear();
  doc = opad::Document::create();
  browse = false;
  hasDocument = true;
  clearHistory();
  markSaved();
  refresh();
  emit pathChanged();
  emit newDocumentCreated();
}

void AppDocument::closeDocument() {
  if (loading || designBusy) return;
  emit aboutToReplace();
  ++generation;
  m_rollback.clear();
  doc = opad::Document();
  browse = false;
  hasDocument = false;
  clearHistory();
  markSaved();
  refresh();
  emit pathChanged();
}

void AppDocument::open(const QString& path) {
  if (loading || designBusy) throw opad::Error("Document is busy; try again when the operation finishes.");
  QString ext = QFileInfo(path).suffix().toLower();
  opad::Document next;
  if (ext != "opad") {
    next = opad::Document::create();
    opad::ImportOptions options;
    options.author = QSettings().value("user/name").toString().trimmed().toStdString();
    opad::import_file(next, fsPath(path), options);
    emit message(tr("Imported %1 into a new document").arg(QFileInfo(path).fileName()));
  } else {
    next = opad::Document::load(fsPath(path));
    emit message(tr("Opened %1").arg(path));
  }
  emit aboutToReplace();
  ++generation;
  m_rollback.clear();
  doc = std::move(next);
  browse = false;
  hasDocument = true;
  clearHistory();
  markSaved();
  if (ext != "opad") m_savedIds.clear();
  refresh();
  emit pathChanged();
}

void AppDocument::importStep(const QString& path, const QString& parent) {
  if (loading || designBusy) throw opad::Error("Document is busy; try again when the operation finishes.");
  if (!hasDocument || browse) {
    doc = opad::Document::create();
    browse = false;
    hasDocument = true;
    clearHistory();
    markSaved();
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

void AppDocument::save() {
  if (loading || m_capturing) throw opad::Error("Document snapshot is in progress; try saving again shortly.");
  if (browse) throw opad::Error("viewer mode: export to an OPAD document first");
  doc.save();
  markSaved();
  emit pathChanged();
  emit saved();
  emit message(tr("Saved %1").arg(path()));
}

void AppDocument::saveAs(const QString& path) {
  if (loading || m_capturing) throw opad::Error("Document snapshot is in progress; try saving again shortly.");
  if (browse) throw opad::Error("viewer mode: export to an OPAD document first");
  doc.save_as(fsPath(path));
  markSaved();
  emit pathChanged();
  emit saved();
  emit message(tr("Saved %1").arg(path));
}

opad::json AppDocument::run(const std::string& command, opad::json args) {
  if (m_converting) throw opad::Error("The document is being prepared for editing; try again in a moment.");
  if (designBusy) throw opad::Error("The design is being recomputed; try again in a moment.");
  // Viewer mode changes how things look (shown, colour, opacity), never the model.
  if (browse && command != "appearance") throw opad::Error("Viewer mode: save the file as an OPAD document to edit it.");
  const size_t before = doc.ops.size();
  if (!args.contains("by")) args["by"] = QSettings().value("user/name").toString().trimmed().toStdString();
  opad::json out = opad::commands::run(command, args, &doc);
  recordStep(labelFor(command, args), before);
  refresh();
  return out;
}

opad::json AppDocument::commitPlan(opad::design::Plan&& plan, const QString& label) {
  if (m_capturing) throw opad::Error("Document snapshot is in progress; try again shortly.");
  if (browse) throw opad::Error("Viewer mode: save the file as an OPAD document to edit it.");
  const size_t before = doc.ops.size();
  opad::json report = opad::design::commit(doc, std::move(plan), QSettings().value("user/name").toString().trimmed().toStdString());
  recordStep(label, before);
  refresh();
  return report;
}

void AppDocument::setRollback(const std::string& opId) {
  if (m_rollback == opId) return;
  m_rollback = opId;
  refresh();
}

void AppDocument::refresh() {
  if (!m_rollback.empty() && !doc.find_op(m_rollback)) m_rollback.clear();  // undone or closed
  scene = hasDocument ? opad::resolve(doc, m_rollback) : opad::Scene{};
  updateDirty();
  ++revision;
  emit changed();
}

void AppDocument::recover(opad::Document&& document,opad::Scene&& resolved) {
  if(loading || designBusy)throw opad::Error("Document is busy; try recovery again shortly.");
  emit aboutToReplace();++generation;++revision;m_rollback.clear();
  doc=std::move(document);doc.path.clear();doc.dirty=true;scene=std::move(resolved);
  browse=false;hasDocument=true;clearHistory();m_savedIds.clear();m_savedBodies=0;
  emit changed();emit pathChanged();
}

// ---------------------------------------------------------------- undo / redo
void AppDocument::commitSnapshot(opad::Document& document,opad::Scene& resolved,
                                 unsigned long long expectedRevision,const QString& label) {
  if(browse) throw opad::Error("viewer_mode: the open file is shown read-only; the user has to save it as an OPAD document before it can be edited");
  if(loading || designBusy || revision!=expectedRevision || document.header.uuid!=doc.header.uuid)
    throw opad::Error("stale_revision: the document changed while the agent was working");
  const auto before=doc.ops.size();
  document.path=doc.path; // Save As may have changed the path without changing geometry.
  std::swap(doc,document);std::swap(scene,resolved);m_rollback.clear();
  recordStep(label,before);updateDirty();++revision;emit changed();emit undoChanged();
}

void AppDocument::recordStep(const QString& label, size_t opsBefore) {
  if (doc.ops.size() <= opsBefore) return;  // the command appended nothing
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
  if (command == "append") return tr("pin measurement");
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
  if (isDirty()) name += "*";
  return name + " - OPAD";
}

QString AppDocument::path() const { return QString::fromStdU16String(doc.path.u16string()); }

QString AppDocument::nodeName(const std::string& id) const {
  const opad::Node* n = scene.node(id);
  return n ? QString::fromStdString(n->name) : QString::fromStdString(id.substr(0, 8));
}
