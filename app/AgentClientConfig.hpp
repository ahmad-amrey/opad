#pragma once
// The AI clients OPAD registers itself with as the MCP server "opad" (Settings > AI integration, Connect an AI client), the
// parts without widgets: where each client keeps its configuration, how a registration found there compares with this copy
// of OPAD, and the one change OPAD makes to a client's JSON file (only mcpServers.opad: every other key kept, a .bak copy of
// the file first, written atomically, nothing written when nothing changes). tests/test_agent_clients.cpp covers it.
#include <QString>
#include <QStringList>
#include <optional>

namespace agentclients {

// A registration as a client keeps it: the program it starts and its arguments; `enabled` false when the client keeps it
// switched off (Codex).
struct Entry {
  QString command;
  QStringList args;
  bool enabled = true;
};

enum class State {
  Unchecked,      // not looked at yet
  NotFound,       // the client's program (or its configuration folder) is not on this computer
  NotRegistered,  // no "opad" server in the client
  Registered,     // "opad" starts this copy's opad-cli with this copy's discovery folder
  Disabled,       // registered for this copy but switched off in the client
  Missing,        // "opad" starts a program that is not there any more (a moved or deleted OPAD)
  Stale,          // "opad" starts another OPAD, or reads another (older) discovery folder
  Failed          // the client's command or file could not be read
};

QStringList serverArgs(const QString& discovery);  // mcp --live --discovery <dir>
// How `found` compares with this copy (`executable`: its opad-cli, `discovery`: AgentBridge's folder). Paths are compared as
// files (separators, case on Windows).
State classify(const std::optional<Entry>& found, const QString& executable, const QString& discovery);
bool samePath(const QString& a, const QString& b);

// A JSON file with an "mcpServers" object (Claude Desktop's claude_desktop_config.json, Claude Code's ~/.claude.json).
// readServer: the "opad" entry; none when the file or the entry is not there; `error` set (and none) when the file is not
// JSON. writeServer: `entry` set adds or replaces mcpServers.opad, none removes it; refuses a file that is not a JSON object
// (it is left alone); writes nothing when the result is the file as it is (`changed` false), else copies the file to
// <file>.bak first and replaces it atomically. Key order is kept.
std::optional<Entry> readServer(const QString& file, QString* error = nullptr);
bool writeServer(const QString& file, const std::optional<Entry>& entry, QString* error = nullptr, bool* changed = nullptr);

// Claude Desktop's configuration file: on Windows the MSIX app's data folder %LOCALAPPDATA%\Claude-Data\claude_<publisher>
// when there is one, else %APPDATA%\Claude; ~/Library/Application Support/Claude; ~/.config/Claude.
QString desktopConfigPath();
QString claudeCodeConfigPath();  // Claude Code's user settings: $CLAUDE_CONFIG_DIR/.claude.json, else ~/.claude.json
QStringList claudeCandidates();  // where the claude program is usually installed (PATH first)
QStringList codexCandidates();  // also the Codex app's own codex program (Windows: from its package's install folder)
bool isOpadProgram(const QString& path);  // opad-cli / opad itself: never a client's program (a Browse that picked it)

}  // namespace agentclients

namespace agentclients {

// "Using another AI app?": a message the user pastes into any AI app's chat, asking its assistant to add the local stdio
// MCP server "opad" (this copy's opad-cli, `mcp --live --discovery <dir>`) to its own user settings, keep its other servers,
// reload its tools and open `document` in OPAD (empty: connect to the open OPAD window). Written for coding agents (Claude
// Code, Codex, Cursor, VS Code Copilot), which can change their own settings.
QString setupMessage(const QString& executable, const QString& discovery, const QString& document);
// The same server as settings: {"mcpServers": {"opad": {"command", "args"}}} (apps that cannot edit their own settings).
QString setupJson(const QString& executable, const QString& discovery);

}  // namespace agentclients
