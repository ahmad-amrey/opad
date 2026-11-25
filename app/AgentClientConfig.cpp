#include "AgentClientConfig.hpp"

#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVersionNumber>
#include <nlohmann/json.hpp>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agentclients {
using ojson = nlohmann::ordered_json;

QStringList serverArgs(const QString& discovery) { return {"mcp", "--live", "--discovery", QDir::toNativeSeparators(QDir::cleanPath(discovery))}; }

bool samePath(const QString& a, const QString& b) {
  if (a.isEmpty() || b.isEmpty()) return false;
  const QString x = QDir::cleanPath(QFileInfo(a).absoluteFilePath()), y = QDir::cleanPath(QFileInfo(b).absoluteFilePath());
#ifdef Q_OS_WIN
  return x.compare(y, Qt::CaseInsensitive) == 0;
#else
  return x == y;
#endif
}

State classify(const std::optional<Entry>& found, const QString& executable, const QString& discovery) {
  if (!found) return State::NotRegistered;
  if (!QFileInfo::exists(found->command)) return State::Missing;
  const QStringList& a = found->args;
  const bool ours = samePath(found->command, executable) && a.size() == 4 && a[0] == "mcp" && a[1] == "--live" && a[2] == "--discovery" && samePath(a[3], discovery);
  if (!ours) return State::Stale;
  return found->enabled ? State::Registered : State::Disabled;
}

namespace {
// The file as JSON: an empty object when it is not there; none when it cannot be read or is not a JSON object.
std::optional<ojson> load(const QString& file, QString* error) {
  QFile f(file);
  if (!f.exists()) return ojson::object();
  if (!f.open(QIODevice::ReadOnly)) {
    if (error) *error = f.errorString();
    return std::nullopt;
  }
  const QByteArray data = f.readAll();
  if (data.trimmed().isEmpty()) return ojson::object();
  ojson j = ojson::parse(data.toStdString(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    if (error) *error = QStringLiteral("%1 is not a JSON object").arg(QDir::toNativeSeparators(file));
    return std::nullopt;
  }
  return j;
}
}  // namespace

std::optional<Entry> readServer(const QString& file, QString* error) {
  const auto j = load(file, error);
  if (!j) return std::nullopt;
  const auto servers = j->find("mcpServers");
  if (servers == j->end() || !servers->is_object()) return std::nullopt;
  const auto opad = servers->find("opad");
  if (opad == servers->end() || !opad->is_object()) return std::nullopt;
  Entry e;
  e.command = QString::fromStdString(opad->value("command", std::string()));
  if (const auto args = opad->find("args"); args != opad->end() && args->is_array())
    for (const auto& a : *args)
      if (a.is_string()) e.args << QString::fromStdString(a.get<std::string>());
  if (opad->contains("disabled") && (*opad)["disabled"].is_boolean()) e.enabled = !(*opad)["disabled"].get<bool>();
  return e;
}

bool writeServer(const QString& file, const std::optional<Entry>& entry, QString* error, bool* changed) {
  if (changed) *changed = false;
  auto j = load(file, error);
  if (!j) return false;
  const ojson before = *j;
  if (entry) {
    ojson args = ojson::array();
    for (const QString& a : entry->args) args.push_back(a.toStdString());
    if (!j->contains("mcpServers") || !(*j)["mcpServers"].is_object()) (*j)["mcpServers"] = ojson::object();
    ojson& server = (*j)["mcpServers"]["opad"];
    if (!server.is_object()) server = ojson::object();
    server["command"] = entry->command.toStdString();
    server["args"] = args;
    server.erase("disabled");
  } else if (j->contains("mcpServers") && (*j)["mcpServers"].is_object()) {
    (*j)["mcpServers"].erase("opad");
  }
  if (*j == before) return true;  // as it is: nothing written, no copy
  if (!QDir().mkpath(QFileInfo(file).absolutePath())) {
    if (error) *error = QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(QFileInfo(file).absolutePath()));
    return false;
  }
  if (QFile::exists(file)) {
    const QString backup = file + ".bak";
    QFile::remove(backup);
    if (!QFile::copy(file, backup)) {
      if (error) *error = QStringLiteral("cannot copy %1 to %2").arg(QDir::toNativeSeparators(file), QDir::toNativeSeparators(backup));
      return false;
    }
  }
  QSaveFile out(file);
  if (!out.open(QIODevice::WriteOnly) || out.write(QByteArray::fromStdString(j->dump(2) + "\n")) < 0 || !out.commit()) {
    if (error) *error = out.errorString();
    return false;
  }
  if (changed) *changed = true;
  return true;
}

namespace {
#ifdef Q_OS_WIN
QString localAppData() { return QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA", QDir::homePath() + "/AppData/Local")); }

// The data folder of Claude Desktop installed from its MSIX package: %LOCALAPPDATA%\Claude-Data\claude_<publisher>,
// where it keeps claude_desktop_config.json and the Claude Code it ships (%APPDATA%\Claude then holds only a marker). The
// one with the configuration file, else the newest; empty when there is none.
QString claudeDataFolder() {
  const QDir root(localAppData() + "/Claude-Data");
  QString best;
  QDateTime newest;
  bool bestHasConfig = false;
  for (const QFileInfo& d : root.entryInfoList({"claude_*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
    const bool hasConfig = QFileInfo::exists(d.filePath() + "/claude_desktop_config.json");
    if (best.isEmpty() || (hasConfig && !bestHasConfig) || (hasConfig == bestHasConfig && d.lastModified() > newest)) {
      best = d.filePath();
      newest = d.lastModified();
      bestHasConfig = hasConfig;
    }
  }
  return best;
}

// The folder a packaged (MSIX) app is installed in, from its package family name; empty when it is not installed. The two
// calls are looked up at run time (Windows 8 on), so no SDK version is needed.
QString packageFolder(const wchar_t* family) {
  using ByFamily = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR*, UINT32*, WCHAR*);
  using PathOf = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR);
  const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  const auto byFamily = kernel ? reinterpret_cast<ByFamily>(reinterpret_cast<void*>(GetProcAddress(kernel, "GetPackagesByPackageFamily"))) : nullptr;
  const auto pathOf = kernel ? reinterpret_cast<PathOf>(reinterpret_cast<void*>(GetProcAddress(kernel, "GetPackagePathByFullName"))) : nullptr;
  if (!byFamily || !pathOf) return {};
  UINT32 count = 0, length = 0;
  if (byFamily(family, &count, nullptr, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || count == 0 || length == 0) return {};
  std::vector<PWSTR> names(count);
  std::vector<WCHAR> buffer(length);
  if (byFamily(family, &count, names.data(), &length, buffer.data()) != ERROR_SUCCESS || count == 0) return {};
  UINT32 size = 0;
  if (pathOf(names[0], &size, nullptr) != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
  std::vector<WCHAR> path(size);
  if (pathOf(names[0], &size, path.data()) != ERROR_SUCCESS) return {};
  return QDir::fromNativeSeparators(QString::fromWCharArray(path.data()));
}
#endif
}  // namespace

QString desktopConfigPath() {
#if defined(Q_OS_WIN)
  if (const QString data = claudeDataFolder(); !data.isEmpty()) return QDir::cleanPath(data + "/claude_desktop_config.json");
  const QString base = qEnvironmentVariable("APPDATA", QDir::homePath() + "/AppData/Roaming");
#elif defined(Q_OS_MACOS)
  const QString base = QDir::homePath() + "/Library/Application Support";
#else
  const QString base = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
#endif
  return QDir::cleanPath(base + "/Claude/claude_desktop_config.json");
}

QString claudeCodeConfigPath() {
  const QString dir = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
  return QDir::cleanPath((dir.isEmpty() ? QDir::homePath() : dir) + "/.claude.json");
}

namespace {
QStringList candidates(const QString& name, const QStringList& more) {
  QStringList out;
  if (const QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty()) out << onPath;
  for (const QString& p : more)
    if (QFileInfo(p).isFile() && !out.contains(QDir::cleanPath(p))) out << QDir::cleanPath(p);
  return out;
}
}  // namespace

QStringList claudeCandidates() {
  const QString home = QDir::homePath();
#if defined(Q_OS_WIN)
  const QString npm = qEnvironmentVariable("APPDATA", home + "/AppData/Roaming") + "/npm";
  QStringList more{home + "/.local/bin/claude.exe", npm + "/claude.cmd"};
  // The Claude Code that Claude Desktop ships: claude-code\<version>\<build>\claude.exe in its data folder, the newest.
  if (const QString data = claudeDataFolder(); !data.isEmpty()) {
    QList<QPair<QVersionNumber, QString>> shipped;
    for (const QFileInfo& v : QDir(data + "/claude-code").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
      for (const QFileInfo& b : QDir(v.filePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo(b.filePath() + "/claude.exe").isFile()) shipped.append({QVersionNumber::fromString(v.fileName()), b.filePath() + "/claude.exe"});
    std::stable_sort(shipped.begin(), shipped.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& s : shipped) more << s.second;
  }
  return candidates("claude", more);
#else
  return candidates("claude", {home + "/.local/bin/claude", "/opt/homebrew/bin/claude", "/usr/local/bin/claude",
                               home + "/.npm-global/bin/claude"});
#endif
}

QStringList codexCandidates() {
  const QString home = QDir::homePath();
#if defined(Q_OS_WIN)
  const QString npm = qEnvironmentVariable("APPDATA", home + "/AppData/Roaming") + "/npm";
  QStringList more{npm + "/codex.cmd", home + "/.local/bin/codex.exe"};
  // The Codex app (Microsoft Store / MSIX) carries the codex program: app/resources/codex.exe in its install folder.
  if (const QString app = packageFolder(L"OpenAI.Codex_2p2nqsd0c76g0"); !app.isEmpty()) more << app + "/app/resources/codex.exe";
  return candidates("codex", more);
#else
  return candidates("codex", {"/opt/homebrew/bin/codex", "/usr/local/bin/codex", home + "/.local/bin/codex", home + "/.npm-global/bin/codex"});
#endif
}

bool isOpadProgram(const QString& path) { return QFileInfo(path).baseName().startsWith("opad", Qt::CaseInsensitive); }

}  // namespace agentclients
