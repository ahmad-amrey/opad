// The copy of the settings a new version of OPAD keeps before it changes anything (app/SettingsBackup.hpp).
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include "SettingsBackup.hpp"
#include "check.hpp"

using settingsbackup::keepBeforeChange;

namespace {
QStringList copies(const QString& dir) { return QDir(dir).entryList({"OPAD-*.ini"}, QDir::Files, QDir::Name); }
}  // namespace

TEST(first_start_keeps_nothing_and_records_the_version) {
  QTemporaryDir root;
  QSettings s(root.filePath("OPAD.ini"), QSettings::IniFormat);
  const QString dir = root.filePath("backups");
  CHECK(keepBeforeChange(s, "0.1.0", dir).isEmpty());
  CHECK(copies(dir).isEmpty());
  CHECK(s.value(settingsbackup::kVersionKey).toString() == "0.1.0");
}

TEST(an_update_keeps_every_setting_once) {
  QTemporaryDir root;
  QSettings s(root.filePath("OPAD.ini"), QSettings::IniFormat);
  const QString dir = root.filePath("backups");
  s.setValue("ui/language", "ar");
  s.setValue("shortcuts/view.fit", "F");
  s.setValue("ui/state", QByteArray("\x01\x02\xff", 3));
  s.setValue("ui/recent", QStringList{"a.opad", "b.step"});
  // Settings written before the version was recorded (every build so far): kept as from an "earlier" version.
  const QString kept = keepBeforeChange(s, "0.2.0", dir);
  CHECK(!kept.isEmpty());
  CHECK(QFileInfo(kept).fileName().startsWith("OPAD-earlier-before-0.2.0-"));
  QSettings copy(kept, QSettings::IniFormat);
  CHECK(copy.value("ui/language").toString() == "ar");
  CHECK(copy.value("shortcuts/view.fit").toString() == "F");
  CHECK(copy.value("ui/state").toByteArray() == QByteArray("\x01\x02\xff", 3));
  CHECK(copy.value("ui/recent").toStringList() == (QStringList{"a.opad", "b.step"}));
  CHECK(s.value("ui/language").toString() == "ar");  // the settings themselves are untouched
  // The same version again: nothing more.
  CHECK(keepBeforeChange(s, "0.2.0", dir).isEmpty());
  CHECK_EQ(copies(dir).size(), qsizetype(1));
}

TEST(a_downgrade_is_kept_too_and_old_copies_go) {
  QTemporaryDir root;
  QSettings s(root.filePath("OPAD.ini"), QSettings::IniFormat);
  const QString dir = root.filePath("backups");
  s.setValue("view/background", 2);
  s.setValue(settingsbackup::kVersionKey, "1.0.0");
  const QString down = keepBeforeChange(s, "0.9.0", dir, 2);
  CHECK(QFileInfo(down).fileName().startsWith("OPAD-1.0.0-before-0.9.0-"));
  for (const char* v : {"1.1.0", "1.2.0", "1.3.0"}) CHECK(!keepBeforeChange(s, v, dir, 2).isEmpty());
  CHECK_EQ(copies(dir).size(), qsizetype(2));  // the newest two
  CHECK(copies(dir).filter("before-1.3.0").size() == 1);
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return check::run_all(argc, argv);
}
