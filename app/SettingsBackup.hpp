#pragma once
// A copy of the user's settings before a new version of OPAD changes them. Settings are name/value pairs (QSettings) and
// each area moves what changed meaning once, behind its own counter (shortcuts::migrate, view/defaultsVersion,
// ui/layoutVersion...). Before any of that runs, the first start of a version other than the one that last wrote the
// settings (newer or older: an update or a downgrade) writes every setting to an INI file in a settings-backups folder,
// then records this version under app/settingsVersion. The newest `keep` copies stay. Restoring is copying one back
// (docs/building.md, Windows installer).
#include <QSettings>
#include <QString>

namespace settingsbackup {
// app/settingsVersion: the version of OPAD that last wrote the settings.
inline constexpr const char* kVersionKey = "app/settingsVersion";
// Beside the INI file for the portable and single-file builds (<data>/settings-backups), else in the app's local data
// folder (the registry has no folder of its own).
QString defaultDir(const QSettings& settings);
// The copy written (empty: nothing to keep, this version wrote them last, or the copy failed: then the version is not
// recorded, so the next start tries again).
QString keepBeforeChange(QSettings& settings, const QString& version, const QString& dir, int keep = 5);
}  // namespace settingsbackup
