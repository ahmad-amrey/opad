#pragma once
// One row per AI client in Settings > AI integration (Connect an AI client): Claude Code, Claude Desktop and Codex. Each
// shows at a glance whether the client has the MCP server "opad" for this copy of OPAD (AgentClientConfig.hpp: registered,
// not registered, not found, registered but its program is missing, registered for another OPAD or an old data folder) and
// offers the one action that fits: Connect, Point it at this OPAD (the "opad" entry replaced, nothing else), Remove.
// Claude Code and Codex are changed through their own CLI (asynchronous QProcess, no shell, 15 s timeout); Claude Code's
// state is read from its user settings file, Codex's from `codex mcp list --json`. Claude Desktop's is its JSON file,
// changed in place (only mcpServers.opad; a .bak copy first). Registration never enables OPAD access or picks a document.
#include <QWidget>
#include <functional>

#include "AgentClientConfig.hpp"

class QLabel;
class QLineEdit;
class QPushButton;

class AgentClientRow : public QWidget {
  Q_OBJECT
 public:
  enum class Client { ClaudeCode, ClaudeDesktop, Codex };
  AgentClientRow(Client client, QString executable, QString discovery, QWidget* parent = nullptr);
  Client client() const { return m_client; }
  QString name() const;
  agentclients::State state() const { return m_state; }
  QString report() const;  // the row's state line (benches)
  void refresh();
  void connectClient();   // add "opad" (not registered)
  void pointHere();       // replace "opad" with this copy (missing program, another OPAD, an old folder)
  void removeClient();    // remove "opad" (this copy's, or one whose program is missing)
  void setProgram(const QString& path);  // the client's program (benches, Browse)
 signals:
  void stateChanged();

 private:
  bool usesProgram() const { return m_client != Client::ClaudeDesktop; }
  QString program() const;  // the client's program, found or as typed (empty: none)
  void run(const QStringList& args, std::function<void(bool, QString)> done);
  void inspect(std::function<void(bool, std::optional<agentclients::Entry>)> done);
  void add(std::function<void(bool, QString)> done);
  void drop(std::function<void(bool, QString)> done);
  void show(agentclients::State state, const QString& detail = {});
  void message(const QString& text);
  QString restartHint() const;
  Client m_client;
  QString m_executable, m_discovery;
  agentclients::State m_state = agentclients::State::Unchecked;
  std::optional<agentclients::Entry> m_entry;
  QLabel *m_title, *m_status, *m_note;
  QLineEdit* m_path = nullptr;
  QPushButton *m_browse = nullptr, *m_connect, *m_point, *m_remove, *m_check;
  bool m_running = false;
};
