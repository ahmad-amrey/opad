#include "SettingsBackup.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

namespace settingsbackup {

QString defaultDir(const QSettings& settings) {
  if (settings.format() == QSettings::IniFormat) return QFileInfo(settings.fileName()).absolutePath() + "/settings-backups";
  return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/settings-backups";
}

QString keepBeforeChange(QSettings& settings, const QString& version, const QString& dir, int keep) {
  const QString last = settings.value(kVersionKey).toString();
  if (last == version) return {};
  const QStringList keys = settings.allKeys();
  QString written;
  if (std::any_of(keys.begin(), keys.end(), [](const QString& k) { return k != kVersionKey; })) {  // a first start has none
    if (!QDir().mkpath(dir)) return {};
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    written = QDir(dir).filePath(QString("OPAD-%1-before-%2-%3.ini").arg(last.isEmpty() ? QString("earlier") : last, version, stamp));
    {
      QSettings copy(written, QSettings::IniFormat);
      for (const QString& k : keys) copy.setValue(k, settings.value(k));
      copy.sync();
      if (copy.status() != QSettings::NoError) {
        QFile::remove(written);
        return {};
      }
    }
    // The newest `keep` copies stay (by when they were written); the one just written always does.
    QFileInfoList copies = QDir(dir).entryInfoList({"OPAD-*.ini"}, QDir::Files);
    std::sort(copies.begin(), copies.end(), [](const QFileInfo& a, const QFileInfo& b) { return a.lastModified() > b.lastModified(); });
    for (qsizetype i = std::max(1, keep); i < copies.size(); ++i)
      if (copies[i].absoluteFilePath() != QFileInfo(written).absoluteFilePath()) QFile::remove(copies[i].absoluteFilePath());
  }
  settings.setValue(kVersionKey, version);
  settings.sync();
  return written;
}

}  // namespace settingsbackup
