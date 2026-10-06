#pragma once
// AI agents' git tools (git_status ... git_push; their schemas: opad::agent::git_tools in core/src/live.cpp) for both MCP
// servers: opad-cli's headless `mcp` runs them in its own process, the live bridge on a worker of the app
// (AgentGit.cpp). git runs through git:: (Git.hpp) only, as the Version control panel runs it. Everything here blocks:
// workers only in the app.
//
// Branch protection is the user's (Preferences > Version control > Branch protection, QSettings git/...): a protected
// branch refuses agents' commits and merges into it, and when set, pushes of it; the panel asks the user instead
// (VersionControl). A fast-forward pull of a protected branch's own upstream is no merge into it and is allowed.
// No force or destructive operation exists here: no force push, reset, rebase, branch deletion, clean, checkout of
// files, stash drop or history rewrite. git_merge_abort only undoes a merge's own changes; git_switch refuses
// uncommitted changes instead of carrying or discarding them; git_commit stages exactly the paths it is given.
#include <QString>
#include <QStringList>
#include <functional>
#include <stdexcept>
#include <string>

#include "opad/json.hpp"

namespace gitagent {

// The settings (Preferences > Version control).
inline constexpr const char* kBranchesKey = "git/protectedBranches";  // "main, master": names, * matches any run of characters
inline constexpr const char* kCommitsKey = "git/protectCommits";      // true
inline constexpr const char* kMergesKey = "git/protectMerges";        // true
inline constexpr const char* kPushesKey = "git/protectPushes";        // false
inline constexpr const char* kInitialBranchKey = "git/initialBranch";  // "main": a new repository's (Set up repository, git_init)
QString defaultBranches();
QString initialBranch();

struct Policy {
  QStringList branches{"main", "master"};
  bool commits = true, merges = true, pushes = false;
  static Policy read();  // QSettings as they are now: a change in Preferences counts from the next call on
  static QStringList parse(const QString& text);  // "main, master release/*" -> its names
  bool protects(const QString& branch) const;
  opad::json to_json() const;
};

// QSettings for a process that is not the app (opad-cli): OPAD's organisation and application names and its portable
// places, as app/main.cpp sets them: <exe dir>/data beside an opad.portable marker, <exe dir>/opad-data in a single-file
// build, OPAD_BENCH_SETTINGS (tests and benches) first.
void useAppSettings(const QString& programDir, bool singleFile);

// A refusal or failure an agent can act on: code (protected_branch, uncommitted_changes, nothing_to_commit, ...), the
// sentence, and what to call next.
struct Refused : std::runtime_error {
  std::string code, next;
  Refused(std::string code, const QString& message, std::string next = {});
};

struct Call {
  QString repo;      // a folder or file inside the repository (absolute); git_init: the folder
  QString document;  // live: the open document; empty headless
  bool documentDirty = false;  // its unsaved changes refuse what rewrites the file
  Policy policy;
  QString program;  // git; empty: git::findProgram()
  std::function<bool()> cancelled;
  std::function<void(const QString&)> progress;
};

// Changes the repository (live: edit permission). A merge preview and a git_resolve that only lists change nothing: reads.
bool writes(const std::string& name, const opad::json& args = opad::json::object());
// A write that needs a request_id (live; request_status finds its receipt): every write but a pull preview, which only
// fetches.
bool receipted(const std::string& name, const opad::json& args = opad::json::object());
bool changesFiles(const std::string& name);  // may rewrite files of the work tree (live: the document follows)
// Runs one tool; throws Refused (or std::exception for anything else).
opad::json run(const std::string& name, const opad::json& args, const Call& call);

}  // namespace gitagent
