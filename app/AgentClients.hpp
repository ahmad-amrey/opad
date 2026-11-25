#pragma once
// One card per AI app in Settings > AI assistants: Claude Desktop, Claude Code and Codex. The card says in plain words
// whether the app has the MCP server "opad" for this copy of OPAD (AgentClientConfig.hpp: registered, not registered, not
// found, registered but its program is missing, registered for another OPAD or an old data folder) and has one button:
// Connect (adds "opad", or points an old "opad" at this copy: the entry removed and added, nothing else), Disconnect
// (removes this copy's "opad"; another OPAD's entry is never removed), Try again. An app that is not here gets a link to
// get it. After Connect the card shows only what is left: restart the app, then a first question to copy.
// Claude Code and Codex are changed through their own CLI (asynchronous QProcess, no shell, 15 s timeout); Claude Code's
// state is read from its user settings file, Codex's from `codex mcp list --json`. Claude Desktop's is its JSON file,
// changed in place (only mcpServers.opad; a .bak copy first). Registration never enables OPAD access or picks a document.
// The technical part (the state line with the files used, the program path and Browse, Check again) is details(): the
// dialog lays it out on its Details page.
#include <QWidget>
#include <functional>

#include "AgentClientConfig.hpp"

class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

class AgentClientRow : public QWidget {
  Q_OBJECT
 public:
  enum class Client { ClaudeCode, ClaudeDesktop, Codex };
  AgentClientRow(Client client, QString executable, QString discovery, QWidget* parent = nullptr);
  Client client() const { return m_client; }
  QString name() const;
  agentclients::State state() const { return m_state; }
  QString report() const;    // the technical state line and the last message (benches, Details)
  QString headline() const;  // the card's plain state: "Not connected yet", "Connected", "Not on this computer"...
  bool justConnected() const { return m_added && m_state == agentclients::State::Registered; }  // Connect worked in this dialog: the card shows the next steps
  QWidget* details();        // the state line, the program path with Browse, Check again (built once; the caller lays it out)
  void setPrompt(const QString& text);  // the first question to ask, shown after Connect
  void refresh();
  void act();             // the card's one button
  void connectClient();   // add "opad" (not registered), or point an old one at this copy
  void pointHere();       // replace "opad" with this copy (missing program, another OPAD, an old folder)
  void removeClient();    // remove "opad" (this copy's, or one whose program is missing)
  void setProgram(const QString& path);  // the client's program (benches, Browse)
 signals:
  void stateChanged();
  void connected();  // Connect (or pointing it here) worked

 private:
  bool usesProgram() const { return m_client != Client::ClaudeDesktop; }
  QString program() const;  // the client's program, found or as typed (empty: none)
  void run(const QStringList& args, std::function<void(bool, QString)> done);
  void inspect(std::function<void(bool, std::optional<agentclients::Entry>)> done);
  void add(std::function<void(bool, QString)> done);
  void drop(std::function<void(bool, QString)> done);
  void show(agentclients::State state, const QString& detail = {});
  void message(const QString& text, bool problem = false);
  void added(bool success, const QString& error);
  QString restartStep() const;   // step 1 after Connect, in bold
  QString restartWhy() const;    // and the words after it
  QString getUrl() const;        // where to get the app
  QString programName() const;   // claude.exe / codex.exe (claude / codex elsewhere)
  Client m_client;
  QString m_executable, m_discovery;
  agentclients::State m_state = agentclients::State::Unchecked;
  std::optional<agentclients::Entry> m_entry;
  bool m_running = false, m_added = false;
  QFrame* m_card;
  QLabel *m_badge, *m_title, *m_headline, *m_note, *m_restart;
  QPushButton* m_action;
  QToolButton* m_get;
  QWidget* m_next;            // after Connect: restart, then the first question
  QLineEdit* m_prompt;
  // Details
  QWidget* m_details = nullptr;
  QLabel* m_status;           // the technical line, report()
  QLineEdit* m_path = nullptr;
  QPushButton *m_browse = nullptr, *m_check = nullptr;
};
