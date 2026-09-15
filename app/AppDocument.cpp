#include "AppDocument.hpp"

#include <QFileInfo>

AppDocument::AppDocument(QObject* parent) : QObject(parent) {}

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
