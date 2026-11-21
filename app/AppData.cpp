#include "AppData.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace appdata {

QString portableDir(const QString& programDir) {
  if (!qEnvironmentVariableIsEmpty("OPAD_BENCH_SETTINGS")) return qEnvironmentVariable("OPAD_BENCH_SETTINGS");
  if (QFile::exists(programDir + "/opad.portable")) return programDir + "/data";
  return {};
}

void use(const QString& programDir) {
  const QString dir = portableDir(programDir);
  if (dir.isEmpty()) return;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
  if (qEnvironmentVariableIsEmpty("OPAD_CACHE_DIR")) qputenv("OPAD_CACHE_DIR", QDir::toNativeSeparators(dir + "/cache").toLocal8Bit());
}

namespace {
bool copyTree(const QString& from, const QString& to) {
  if (!QDir().mkpath(to)) return false;
  bool ok = true;
  QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString file = it.next();
    const QString target = to + "/" + QDir(from).relativeFilePath(file);
    ok = QDir().mkpath(QFileInfo(target).absolutePath()) && QFile::copy(file, target) && ok;
  }
  return ok;
}
}  // namespace

QString start(const QString& programDir) {
  use(programDir);
  if (!portableDir(programDir).isEmpty()) return {};
  QSettings settings;
  return adoptOldPortable(programDir, settings, QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),
                          QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/templates");
}

QString adoptOldPortable(const QString& programDir, QSettings& settings, const QString& dataDir, const QString& templatesDir) {
  // The old layout: QSettings' INI path was opad-data, so the file is opad-data/<organisation>/<application>.ini and the
  // data folders sit beside it; the cache was opad-data/cache.
  const QString old = programDir + "/opad-data/opad";
  const QString file = old + "/OPAD.ini";
  if (!QFile::exists(file) || !settings.allKeys().isEmpty()) return {};
  {
    QSettings from(file, QSettings::IniFormat);
    const QStringList keys = from.allKeys();
    if (keys.isEmpty()) return {};
    for (const QString& k : keys) settings.setValue(k, from.value(k));
  }
  settings.setValue("app/adoptedFrom", QDir::toNativeSeparators(file));
  settings.sync();
  for (const QFileInfo& sub : QDir(old).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (sub.fileName() == "agent") continue;  // the running app's discovery files (AgentBridge): written anew each run
    // Templates have their own place (templates::folder(): AppDataLocation, roaming on Windows).
    const QString target = sub.fileName() == "templates" ? templatesDir : dataDir + "/" + sub.fileName();
    if (!QFileInfo::exists(target)) copyTree(sub.absoluteFilePath(), target);
  }
  return file;
}

}  // namespace appdata
