#include "OwnFolder.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ownfolder {

bool isSidecar(const QString& document, const QString& entry) {
  const QString name = QFileInfo(document).fileName() + ".";
  return entry.size() > name.size() && entry.startsWith(name, Qt::CaseInsensitive);
}

bool isSystemFile(const QString& entry) {
  for (const char* junk : {"desktop.ini", "thumbs.db", ".ds_store"})
    if (entry.compare(QLatin1String(junk), Qt::CaseInsensitive) == 0) return true;
  return false;
}

QString freeFolder(const QString& parent, const QString& base) {
  const QDir dir(parent);
  for (int n = 1;; ++n) {
    const QString name = n == 1 ? base : QStringLiteral("%1 %2").arg(base).arg(n);
    if (!QFileInfo::exists(dir.filePath(name))) return dir.filePath(name);  // the file system's own case rule
  }
}

Plan plan(const QString& document) {
  Plan p;
  const QFileInfo doc(document);
  const QDir dir = doc.absoluteDir();
  const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
  for (const QFileInfo& e : entries) {
    const QString name = e.fileName();
    if (name.compare(doc.fileName(), Qt::CaseInsensitive) == 0 || isSidecar(document, name) || isSystemFile(name)) continue;
    p.others << name;
  }
  p.needed = !p.others.isEmpty();
  p.folder = freeFolder(dir.absolutePath(), doc.completeBaseName());
  p.file = QDir(p.folder).filePath(doc.fileName());
  return p;
}

QStringList moveSidecars(const QString& from, const QString& to) {
  QStringList failed;
  const QFileInfo source(from);
  const QString prefix = source.fileName() + ".";
  for (const QFileInfo& e : source.absoluteDir().entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name)) {
    if (!isSidecar(from, e.fileName())) continue;
    const QString target = QFileInfo(to).absoluteFilePath() + e.fileName().mid(prefix.size() - 1);  // ".tmp", ".orig"...
    if (QFileInfo::exists(target) || !QFile::rename(e.absoluteFilePath(), target)) failed << e.fileName();
  }
  return failed;
}

}  // namespace ownfolder
