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
#include <thread>

namespace {
bool isExternalPath(const QString& path) {
  QString ext = QFileInfo(path).suffix().toLower();
  return ext != "opad";
}
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

AppDocument::AppDocument(QObject* parent) : QObject(parent), m_alive(std::make_shared<std::atomic<bool>>(true)) {}

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
}

void AppDocument::startOpen(const QString& path) {
  if (loading) return;
  loading = true;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_cancel = cancel;
  const bool external = isExternalPath(path);
  const QString file = QFileInfo(path).fileName() + QStringLiteral(" (%1 MB)").arg(QFileInfo(path).size() / (1024.0 * 1024.0), 0, 'f', 0);
  opad::ImportOptions o = loadOptions(cancel, file);
  auto alive = m_alive;
  emit loadProgress(external ? tr("Reading %1").arg(file) : tr("Opening %1").arg(file), -1);
  std::thread([this, alive, cancel, path, external, o]() {
    auto result = std::make_shared<opad::Document>();
    QString error;
    QStringList warnings;
    try {
      if (external) {
        *result = opad::Document::create();
        const auto imported=opad::import_file(*result, path.toStdString(), o);
        for(const auto& warning:imported.warnings) warnings.append(QString::fromStdString(warning));
      } else *result = opad::Document::load(path.toStdString());
      // Parse the bodies here rather than on the UI thread when they are first displayed.
      if (!*cancel) opad::warm_shape_cache(*result, [&](size_t i, size_t n) { return o.progress(n ? double(i) / double(n) : 1.0, "preparing"); });
      if (*cancel) error = QStringLiteral("cancelled");
    } catch (const Standard_Failure& e) {
      error = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    if (!*alive) return;
    QMetaObject::invokeMethod(this, [this, result, error, path, external, warnings] {
      loading = false;
      if (!error.isEmpty()) {
        emit loadFinished(false, error);
        return;
      }
      emit aboutToReplace();
      ++generation;
      m_rollback.clear();
      doc = std::move(*result);
      browse = false;
      hasDocument = true;
      clearHistory();
      markSaved();
      if (external) m_savedIds.clear();  // imported content has not been saved as an OPAD document
      refresh();
      emit pathChanged();
      if (external) emit message(tr("Imported %1 into a new document").arg(QFileInfo(path).fileName()));
      else emit message(tr("Opened %1").arg(path));
      if(!warnings.isEmpty()) emit message(warnings.join("; "));
      emit loadFinished(true, {});
    }, Qt::QueuedConnection);
  }).detach();
}

void AppDocument::startImport(const QString& path, const QString& parent) {
  if (loading) return;
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
  // Import into a snapshot: selection/render callbacks retain a valid live document.
  auto work = std::make_shared<opad::Document>(doc);
  const size_t opsBefore = work->ops.size();
  const bool dirtyBefore = work->dirty;
  auto alive = m_alive;
  emit loadProgress(tr("Reading %1").arg(file), -1);
  std::thread([this, alive, cancel, path, o, work, opsBefore, dirtyBefore]() {
    QString error;
    opad::json r;
    try {
      r = opad::import_file(*work, path.toStdString(), o).to_json();
      if (!*cancel) opad::warm_shape_cache(*work, [&](size_t i, size_t n) { return o.progress(n ? double(i) / double(n) : 1.0, "preparing"); });
      if (*cancel) error = QStringLiteral("cancelled");
    } catch (const Standard_Failure& e) {
      error = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    if (!*alive) return;
    QMetaObject::invokeMethod(this, [this, work, error, r, path, opsBefore, dirtyBefore] {
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
  if (loading) return;
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
}

void AppDocument::closeDocument() {
  if (loading) return;
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
  QString ext = QFileInfo(path).suffix().toLower();
  opad::Document next;
  if (ext != "opad") {
    next = opad::Document::create();
    opad::ImportOptions options;
    options.author = QSettings().value("user/name").toString().trimmed().toStdString();
    opad::import_file(next, path.toStdString(), options);
    emit message(tr("Imported %1 into a new document").arg(QFileInfo(path).fileName()));
  } else {
    next = opad::Document::load(path.toStdString());
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
  if (browse) throw opad::Error("viewer mode: export to an OPAD document first");
  doc.save();
  markSaved();
  emit pathChanged();
  emit message(tr("Saved %1").arg(path()));
}

void AppDocument::saveAs(const QString& path) {
  if (browse) throw opad::Error("viewer mode: export to an OPAD document first");
  doc.save_as(path.toStdString());
  markSaved();
  emit pathChanged();
  emit message(tr("Saved %1").arg(path));
}

opad::json AppDocument::run(const std::string& command, opad::json args) {
  if (designBusy) throw opad::Error("The design is being recomputed; try again in a moment.");
  const size_t before = doc.ops.size();
  if (!args.contains("by")) args["by"] = QSettings().value("user/name").toString().trimmed().toStdString();
  opad::json out = opad::commands::run(command, args, &doc);
  recordStep(labelFor(command, args), before);
  refresh();
  return out;
}

opad::json AppDocument::commitPlan(opad::design::Plan&& plan, const QString& label) {
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
  emit changed();
}

// ---------------------------------------------------------------- undo / redo
void AppDocument::recordStep(const QString& label, size_t opsBefore) {
  if (doc.ops.size() <= opsBefore) return;  // the command appended nothing
  m_undo.push_back(Step{label, doc.ops.size() - opsBefore, {}});
  m_redo.clear();
  while (static_cast<int>(m_undo.size()) > m_undoLimit) m_undo.erase(m_undo.begin());
  emit undoChanged();
}

void AppDocument::undo() {
  if (!canUndo()) return;
  Step s = std::move(m_undo.back());
  m_undo.pop_back();
  s.ops = doc.truncate_ops(doc.ops.size() - std::min(s.count, doc.ops.size()));
  m_redo.push_back(std::move(s));
  refresh();
  emit undoChanged();
}

void AppDocument::redo() {
  if (!canRedo()) return;
  Step s = std::move(m_redo.back());
  m_redo.pop_back();
  s.count = s.ops.size();
  doc.restore_ops(std::move(s.ops));
  s.ops.clear();
  m_undo.push_back(std::move(s));
  refresh();
  emit undoChanged();
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
  if (command == "annotate") return tr("note");
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
  QString name = doc.path.empty() ? tr("Untitled") : QString::fromStdString(doc.path.filename().string());
  if (browse) name = tr("[viewer] ") + name;
  if (isDirty()) name += "*";
  return name + " - OPAD";
}

QString AppDocument::path() const { return QString::fromStdString(doc.path.string()); }

QString AppDocument::nodeName(const std::string& id) const {
  const opad::Node* n = scene.node(id);
  return n ? QString::fromStdString(n->name) : QString::fromStdString(id.substr(0, 8));
}
