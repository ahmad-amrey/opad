#include "AppDocument.hpp"

#include <QFileInfo>
#include <QMetaObject>
#include <thread>

namespace {
bool isStepPath(const QString& path) {
  QString ext = QFileInfo(path).suffix().toLower();
  return ext == "step" || ext == "stp";
}
QString phaseLabel(const std::string& what, const QString& file) {
  if (what == "reading") return AppDocument::tr("Reading %1").arg(file);
  if (what == "building") return AppDocument::tr("Building document");
  return AppDocument::tr("Translating geometry");
}
}  // namespace

AppDocument::AppDocument(QObject* parent) : QObject(parent), m_alive(std::make_shared<std::atomic<bool>>(true)) {}

AppDocument::~AppDocument() { *m_alive = false; }

opad::ImportOptions AppDocument::loadOptions(const std::shared_ptr<std::atomic<bool>>& cancel, const QString& file) {
  opad::ImportOptions o;
  auto last = std::make_shared<std::pair<std::string, int>>("", -2);
  auto alive = m_alive;
  o.progress = [this, cancel, alive, last, file](double frac, const std::string& what) {
    const bool determinate = (what == "building");  // the only phase with reliable fractions
    const int pct = (determinate && frac >= 0) ? static_cast<int>(frac * 100.0) : -1;
    if (what != last->first || pct != last->second) {
      *last = {what, pct};
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
  const bool step = isStepPath(path);
  const QString file = QFileInfo(path).fileName();
  opad::ImportOptions o = loadOptions(cancel, file);
  auto alive = m_alive;
  emit loadProgress(step ? tr("Reading %1").arg(file) : tr("Opening %1").arg(file), -1);
  std::thread([this, alive, cancel, path, step, o]() {
    auto result = std::make_shared<opad::Document>();
    QString error;
    try {
      *result = step ? opad::browse_step(path.toStdString(), o) : opad::Document::load(path.toStdString());
      if (*cancel) error = QStringLiteral("cancelled");
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    if (!*alive) return;
    QMetaObject::invokeMethod(this, [this, result, error, path, step] {
      loading = false;
      if (!error.isEmpty()) {
        emit loadFinished(false, error);
        return;
      }
      doc = std::move(*result);
      browse = step;
      hasDocument = true;
      refresh();
      emit pathChanged();
      if (step) emit message(tr("Browsing %1 (nothing is saved; use Import to create a document)").arg(QFileInfo(path).fileName()));
      else emit message(tr("Opened %1").arg(path));
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
    emit pathChanged();
  }
  loading = true;
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  m_cancel = cancel;
  const QString file = QFileInfo(path).fileName();
  opad::ImportOptions o = loadOptions(cancel, file);
  o.parent = parent.toStdString();
  // The worker owns the document while importing; the live one is empty until the result lands.
  auto work = std::make_shared<opad::Document>(std::move(doc));
  doc = opad::Document();
  const size_t opsBefore = work->ops.size();
  const bool dirtyBefore = work->dirty;
  auto alive = m_alive;
  emit loadProgress(tr("Reading %1").arg(file), -1);
  std::thread([this, alive, cancel, path, o, work, opsBefore, dirtyBefore]() {
    QString error;
    opad::json r;
    try {
      r = opad::import_step(*work, path.toStdString(), o).to_json();
      if (*cancel) error = QStringLiteral("cancelled");
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
      refresh();
      if (!error.isEmpty()) {
        emit loadFinished(false, error);
        return;
      }
      emit message(tr("Imported %1: %2 bodies, %3 new body entries").arg(QFileInfo(path).fileName()).arg(r.value("bodies", 0)).arg(r.value("new_entries", 0)));
      emit loadFinished(true, {});
    }, Qt::QueuedConnection);
  }).detach();
}

void AppDocument::newDocument() {
  doc = opad::Document::create();
  browse = false;
  hasDocument = true;
  refresh();
  emit pathChanged();
}

void AppDocument::open(const QString& path) {
  QString ext = QFileInfo(path).suffix().toLower();
  if (ext == "step" || ext == "stp") {
    doc = opad::browse_step(path.toStdString());
    browse = true;
    emit message(tr("Browsing %1 (nothing is saved; use Import to create a document)").arg(QFileInfo(path).fileName()));
  } else {
    doc = opad::Document::load(path.toStdString());
    browse = false;
    emit message(tr("Opened %1").arg(path));
  }
  hasDocument = true;
  refresh();
  emit pathChanged();
}

void AppDocument::importStep(const QString& path, const QString& parent) {
  if (!hasDocument || browse) {
    doc = opad::Document::create();
    browse = false;
    hasDocument = true;
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
  if (browse) throw opad::Error("browse mode: import the STEP file to create a document first");
  doc.save();
  emit pathChanged();
  emit message(tr("Saved %1").arg(path()));
}

void AppDocument::saveAs(const QString& path) {
  if (browse) throw opad::Error("browse mode: import the STEP file to create a document first");
  doc.save_as(path.toStdString());
  emit pathChanged();
  emit message(tr("Saved %1").arg(path));
}

opad::json AppDocument::run(const std::string& command, opad::json args) {
  opad::json out = opad::commands::run(command, args, &doc);
  refresh();
  return out;
}

void AppDocument::refresh() {
  scene = hasDocument ? opad::resolve(doc) : opad::Scene{};
  emit changed();
}

QString AppDocument::title() const {
  if (!hasDocument) return tr("OPAD");
  QString name = doc.path.empty() ? tr("Untitled") : QString::fromStdString(doc.path.filename().string());
  if (browse) name = tr("[browse] ") + name;
  if (isDirty()) name += "*";
  return name + " - OPAD";
}

QString AppDocument::path() const { return QString::fromStdString(doc.path.string()); }

QString AppDocument::nodeName(const std::string& id) const {
  const opad::Node* n = scene.node(id);
  return n ? QString::fromStdString(n->name) : QString::fromStdString(id.substr(0, 8));
}
