// Where OPAD keeps the user's settings and data (app/AppData.hpp): the user's folders by default, beside the exe with an
// opad.portable marker, and what a single-file build before 0.9.0 left in <exe dir>/opad-data copied over once.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include "AppData.hpp"
#include "check.hpp"

namespace {
void write(const QString& path, const QByteArray& text) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  f.open(QIODevice::WriteOnly);
  f.write(text);
}
QByteArray read(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
// An old single-file layout: opad-data/opad/OPAD.ini, data folders beside it, the cache at opad-data/cache.
void oldLayout(const QString& program) {
  {
    QSettings old(program + "/opad-data/opad/OPAD.ini", QSettings::IniFormat);
    old.setValue("ui/language", "ar");
    old.setValue("ui/recent", QStringList{"a.opad", "b.step"});
  }
  write(program + "/opad-data/opad/templates/plate.opad", "template");
  write(program + "/opad-data/opad/recovery/doc/1.opad", "snapshot");
  write(program + "/opad-data/opad/agent/instance-1.json", "run");
  write(program + "/opad-data/cache/viewer/x.bin", "cache");
}
}  // namespace

TEST(the_users_folders_unless_a_marker_sits_beside_the_program) {
  qunsetenv("OPAD_BENCH_SETTINGS");
  QTemporaryDir program;
  CHECK(appdata::portableDir(program.path()).isEmpty());
  write(program.filePath("opad.portable"), "");
  CHECK(appdata::portableDir(program.path()) == program.path() + "/data");
  qputenv("OPAD_BENCH_SETTINGS", "C:/bench");
  CHECK(appdata::portableDir(program.path()) == "C:/bench");
  qunsetenv("OPAD_BENCH_SETTINGS");
}

TEST(old_single_file_data_is_copied_once_and_left_as_it_was) {
  QTemporaryDir program, home;
  oldLayout(program.path());
  QSettings settings(home.filePath("settings/OPAD.ini"), QSettings::IniFormat);
  const QString data = home.filePath("data");
  const QString adopted = appdata::adoptOldPortable(program.path(), settings, data, home.filePath("roaming/templates"));
  CHECK(adopted == program.path() + "/opad-data/opad/OPAD.ini");
  CHECK(settings.value("ui/language").toString() == "ar");
  CHECK(settings.value("ui/recent").toStringList() == QStringList({"a.opad", "b.step"}));
  CHECK(!settings.value("app/adoptedFrom").toString().isEmpty());
  CHECK(read(home.filePath("roaming/templates/plate.opad")) == "template");  // templates in their own (roaming) place
  CHECK(!QFileInfo::exists(data + "/templates"));
  CHECK(!QFileInfo::exists(data + "/agent"));  // per-run discovery files are not carried over
  CHECK(read(data + "/recovery/doc/1.opad") == "snapshot");
  CHECK(!QFileInfo::exists(data + "/cache"));  // the cache is not carried over
  CHECK(read(program.filePath("opad-data/opad/templates/plate.opad")) == "template");  // the old folder is untouched
  CHECK(read(program.filePath("opad-data/cache/viewer/x.bin")) == "cache");
  // Once only: the settings are no longer empty.
  CHECK(appdata::adoptOldPortable(program.path(), settings, data, home.filePath("roaming/templates")).isEmpty());
}

TEST(settings_already_there_are_never_replaced) {
  QTemporaryDir program, home;
  oldLayout(program.path());
  QSettings settings(home.filePath("settings/OPAD.ini"), QSettings::IniFormat);
  settings.setValue("ui/language", "en");
  write(home.filePath("roaming/templates/mine.opad"), "mine");
  CHECK(appdata::adoptOldPortable(program.path(), settings, home.filePath("data"), home.filePath("roaming/templates")).isEmpty());
  CHECK(settings.value("ui/language").toString() == "en");
  CHECK(!QFileInfo::exists(home.filePath("roaming/templates/plate.opad")));
}

TEST(a_data_folder_already_there_is_kept) {
  QTemporaryDir program, home;
  oldLayout(program.path());
  QSettings settings(home.filePath("settings/OPAD.ini"), QSettings::IniFormat);
  write(home.filePath("roaming/templates/mine.opad"), "mine");
  CHECK(!appdata::adoptOldPortable(program.path(), settings, home.filePath("data"), home.filePath("roaming/templates")).isEmpty());
  CHECK(read(home.filePath("roaming/templates/mine.opad")) == "mine");
  CHECK(!QFileInfo::exists(home.filePath("roaming/templates/plate.opad")));  // a folder of that name was there: kept whole
  CHECK(read(home.filePath("data/recovery/doc/1.opad")) == "snapshot");
}

TEST(nothing_to_adopt_without_an_old_folder) {
  QTemporaryDir program, home;
  QSettings settings(home.filePath("settings/OPAD.ini"), QSettings::IniFormat);
  CHECK(appdata::adoptOldPortable(program.path(), settings, home.filePath("data"), home.filePath("roaming/templates")).isEmpty());
  CHECK(settings.allKeys().isEmpty());
}

CHECK_MAIN()
