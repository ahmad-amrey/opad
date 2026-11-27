#pragma once
// Where OPAD keeps the user's settings, cache, recovery snapshots and templates.
//
// Normally in the user's own folders, as installed programs do: Qt's native settings (Windows: the registry,
// HKCU\Software\opad\OPAD; Linux: ~/.config/opad/OPAD.conf; macOS: ~/Library/Preferences/com.opad.OPAD.plist), the data
// (recovery snapshots, settings backups, agent files) in QStandardPaths::AppLocalDataLocation (%LOCALAPPDATA%\opad\OPAD,
// ~/.local/share/opad/OPAD, ~/Library/Application Support/opad/OPAD), templates in AppDataLocation (on Windows the
// roaming %APPDATA%\opad\OPAD; elsewhere the same folder as the data) and the cache in opad::cache_dir(). Installing,
// updating or uninstalling the program never touches them.
//
// Portable when a file named `opad.portable` sits beside the program (Windows: the exe's folder): everything in
// <program dir>/data (INI settings, cache in data/cache, solver packs in data/engines), so the folder can move between
// machines. OPAD_BENCH_SETTINGS (tests, benches) wins over both.
#include <QString>

class QSettings;

namespace appdata {

// The portable data folder, or empty for the user's own folders.
QString portableDir(const QString& programDir);

// Points QSettings (INI) and the cache at the portable folder; leaves Qt's defaults otherwise. Needs the organisation
// and application names set first.
void use(const QString& programDir);

// use(), then, in the user's folders, adoptOldPortable() into Qt's default settings and folders. For the app and for
// opad-cli alike; returns what adoptOldPortable returned.
QString start(const QString& programDir);

// Single-file builds before 0.9.0 kept everything in <program dir>/opad-data. When that folder has settings and
// `settings` has none yet, its settings are copied into `settings`, its templates into `templatesDir` and its other
// folders (recovery snapshots, settings backups; not the cache, nor the agent folder of per-run files) into `dataDir`,
// each only where no folder of that name exists yet. The old folder is left as it was. Returns the settings file
// adopted, or empty when nothing was.
QString adoptOldPortable(const QString& programDir, QSettings& settings, const QString& dataDir, const QString& templatesDir);

// The installer moves an opad-cli.exe that an AI session still runs aside (opad-cli.exe.<n>.old, installer/opad.iss)
// rather than asking to close it. Each start removes those that nothing runs any more; one still running stays for the
// next start. Returns how many were removed.
int removeMovedAside(const QString& programDir);

}  // namespace appdata
