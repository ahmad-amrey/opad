#include "AgentClients.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

#include "Theme.hpp"
#include "opad/core.hpp"

using namespace agentclients;

AgentClientRow::AgentClientRow(Client client, QString executable, QString discovery, QWidget* parent)
    : QWidget(parent), m_client(client), m_executable(std::move(executable)), m_discovery(std::move(discovery)) {
  setObjectName(client == Client::ClaudeCode ? "agentClientClaudeCode" : client == Client::ClaudeDesktop ? "agentClientClaudeDesktop" : "agentClientCodex");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 4, 0, 4);
  layout->setSpacing(4);
  auto* head = new QHBoxLayout;
  layout->addLayout(head);
  m_title = new QLabel(QStringLiteral("<b>%1</b>").arg(name().toHtmlEscaped()));
  head->addWidget(m_title);
  m_status = new QLabel;
  m_status->setObjectName("agentClientState");
  m_status->setWordWrap(true);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  head->addWidget(m_status, 1);
  if (usesProgram()) {
    auto* row = new QHBoxLayout;
    layout->addLayout(row);
    const char* key = client == Client::ClaudeCode ? "agent/claudeExecutable" : "agent/codexExecutable";
    m_path = new QLineEdit(QSettings().value(key).toString());
    m_path->setObjectName(client == Client::ClaudeCode ? "claudeExecutable" : "codexExecutable");
    m_path->setPlaceholderText(client == Client::ClaudeCode ? tr("Found by itself; or the path of claude") : tr("Found by itself; or the path of codex"));
    row->addWidget(m_path, 1);
    m_browse = new QPushButton(tr("Browse..."));
    row->addWidget(m_browse);
    connect(m_browse, &QPushButton::clicked, this, [this] {
      if (m_running) return;
      const QString path = QFileDialog::getOpenFileName(this, tr("Select the %1 program").arg(name()));
      if (!path.isEmpty()) setProgram(path);
    });
    connect(m_path, &QLineEdit::editingFinished, this, &AgentClientRow::refresh);
  }
  auto* buttons = new QHBoxLayout;
  layout->addLayout(buttons);
  m_connect = new QPushButton(tr("Connect"));
  m_connect->setObjectName("agentClientConnect");
  m_point = new QPushButton(tr("Point it at this OPAD"));
  m_point->setObjectName("agentClientPoint");
  m_remove = new QPushButton(tr("Remove"));
  m_remove->setObjectName("agentClientRemove");
  m_check = new QPushButton(tr("Check again"));
  for (auto* b : {m_connect, m_point, m_remove, m_check}) buttons->addWidget(b);
  buttons->addStretch();
  connect(m_connect, &QPushButton::clicked, this, &AgentClientRow::connectClient);
  connect(m_point, &QPushButton::clicked, this, &AgentClientRow::pointHere);
  connect(m_remove, &QPushButton::clicked, this, &AgentClientRow::removeClient);
  connect(m_check, &QPushButton::clicked, this, &AgentClientRow::refresh);
  m_note = new QLabel;
  m_note->setObjectName("secondary");
  m_note->setWordWrap(true);
  m_note->hide();
  layout->addWidget(m_note);
  show(State::Unchecked);
}

QString AgentClientRow::name() const {
  switch (m_client) {
    case Client::ClaudeCode: return tr("Claude Code");
    case Client::ClaudeDesktop: return tr("Claude Desktop");
    case Client::Codex: return tr("Codex");
  }
  return {};
}

QString AgentClientRow::report() const { return m_status->text() + (m_note->text().isEmpty() ? QString() : "\n" + m_note->text()); }
void AgentClientRow::message(const QString& text) {
  m_note->setText(text);
  m_note->setVisible(!text.isEmpty());
  if (layout()) layout()->activate();  // a wrapped note's height changes the row's
}

void AgentClientRow::setProgram(const QString& path) {
  if (m_path) m_path->setText(path);
  refresh();
}

QString AgentClientRow::program() const {
  if (!m_path) return {};
  QString path = m_path->text().trimmed();
  if (path.isEmpty()) {
    const QStringList found = m_client == Client::ClaudeCode ? claudeCandidates() : codexCandidates();
    return found.value(0);
  }
  if (!QFileInfo(path).isAbsolute()) path = QStandardPaths::findExecutable(path);
  return QFileInfo(path).isFile() ? path : QString();
}

QString AgentClientRow::restartHint() const {
  switch (m_client) {
    case Client::ClaudeCode: return tr("Start a new Claude Code session (or run /mcp in it) so it starts OPAD's server.");
    case Client::ClaudeDesktop: return tr("Quit Claude Desktop (also from the system tray or menu bar) and open it again.");
    case Client::Codex: return tr("Restart Codex (or its MCP server) so it starts OPAD's server.");
  }
  return {};
}

void AgentClientRow::show(State state, const QString& detail) {
  m_state = state;
  const Tokens& t = theme::current();
  QString text;
  QColor colour = t.fg2;
  switch (state) {
    case State::Unchecked: text = tr("Not checked yet"); break;
    case State::NotFound: text = usesProgram() ? tr("%1 was not found on this computer: install it, or Browse to its program").arg(name()) : tr("%1 was not found on this computer").arg(name()); break;
    case State::NotRegistered: text = tr("Not connected"); break;
    case State::Registered: text = tr("Connected to this OPAD"); colour = t.green; break;
    case State::Disabled: text = tr("Connected to this OPAD, but switched off in %1").arg(name()); colour = t.amber; break;
    case State::Missing: text = tr("Registered, but its program is missing: %1").arg(detail); colour = t.amber; break;
    case State::Stale: text = tr("Registered for another OPAD or an old data folder: %1").arg(detail); colour = t.amber; break;
    case State::Failed: text = tr("Could not read its settings: %1").arg(detail); colour = t.red; break;
  }
  m_status->setText(text);
  m_status->setStyleSheet(QString("color: %1;").arg(theme::css(colour)));
  const bool program = !usesProgram() || !this->program().isEmpty();
  m_connect->setVisible(state == State::NotRegistered);
  m_connect->setEnabled(program && !m_running);
  m_point->setVisible(state == State::Missing || state == State::Stale);
  m_point->setEnabled(program && !m_running);
  m_remove->setVisible(state == State::Registered || state == State::Disabled || state == State::Missing);
  m_remove->setEnabled(program && !m_running);
  m_check->setEnabled(!m_running);
  if (m_path) m_path->setEnabled(!m_running);
  if (m_browse) m_browse->setEnabled(!m_running);
  // Registered, but this client's own program is not here: the buttons wait for it, and say so.
  if (!program && state != State::NotFound && state != State::Unchecked && m_note->text().isEmpty())
    message(tr("%1's program was not found on this computer: Browse to it to change this registration.").arg(name()));
  if (layout()) layout()->activate();
  emit stateChanged();
}

// The client's command, asynchronous and without a shell. The process belongs to the application, so closing Settings
// never destroys a running QProcess; callbacks go through a guarded pointer; only this child may be timed out.
void AgentClientRow::run(const QStringList& args, std::function<void(bool, QString)> done) {
  if (m_running) return;
  const QString path = program();
  if (path.isEmpty()) return done(false, tr("%1 was not found on this computer: install it, or Browse to its program").arg(name()));
  QSettings().setValue(m_client == Client::ClaudeCode ? "agent/claudeExecutable" : "agent/codexExecutable", m_path->text().trimmed());
  m_running = true;
  show(m_state);
  auto* process = new QProcess(QCoreApplication::instance());
  process->setWorkingDirectory(QDir::homePath());
  auto* timer = new QTimer(process);
  timer->setSingleShot(true);
  QPointer<AgentClientRow> self = this;
  auto completed = std::make_shared<bool>(false), timedOut = std::make_shared<bool>(false);
  auto finish = [self, process, timer, done, completed](bool ok, QString text) {
    if (*completed) return;
    *completed = true;
    timer->stop();
    process->deleteLater();
    if (!self) return;
    self->m_running = false;
    done(ok, text);
  };
  const QString who = name();
  connect(process, &QProcess::finished, process, [process, finish, timedOut, who](int code, QProcess::ExitStatus status) {
    const bool ok = !*timedOut && code == 0 && status == QProcess::NormalExit;
    QString text = QString::fromUtf8(ok ? process->readAllStandardOutput() : process->readAllStandardError() + process->readAllStandardOutput()).trimmed().left(65536);
    if (*timedOut) text = tr("The %1 command did not answer within 15 seconds.").arg(who);
    finish(ok, text);
  });
  connect(process, &QProcess::errorOccurred, process, [process, finish](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) finish(false, process->errorString());
  });
  connect(timer, &QTimer::timeout, process, [process, timedOut] {
    *timedOut = true;
    process->kill();
  });
  process->setProgram(path);
  process->setArguments(args);
  process->start();
  timer->start(15000);
}

void AgentClientRow::inspect(std::function<void(bool, std::optional<Entry>)> done) {
  if (m_client == Client::Codex) {
    run({"mcp", "list", "--json"}, [this, done](bool ok, QString output) {
      if (!ok) {
        show(program().isEmpty() ? State::NotFound : State::Failed, output);
        return done(false, std::nullopt);
      }
      const auto entries = opad::json::parse(output.toStdString(), nullptr, false);
      if (!entries.is_array()) {
        show(State::Failed, tr("Codex answered in a form OPAD does not read."));
        return done(false, std::nullopt);
      }
      for (const auto& entry : entries) {
        if (entry.value("name", "") != "opad") continue;
        const auto transport = entry.value("transport", opad::json::object());
        Entry e;
        e.command = QString::fromStdString(transport.value("command", ""));
        for (const auto& a : transport.value("args", opad::json::array()))
          if (a.is_string()) e.args << QString::fromStdString(a.get<std::string>());
        e.enabled = entry.value("enabled", true);
        return done(true, e);
      }
      done(true, std::nullopt);
    });
    return;
  }
  const QString file = m_client == Client::ClaudeCode ? claudeCodeConfigPath() : desktopConfigPath();
  QString error;
  const auto entry = readServer(file, &error);
  if (!error.isEmpty()) {
    show(State::Failed, error);
    return done(false, std::nullopt);
  }
  done(true, entry);
}

void AgentClientRow::refresh() {
  if (m_running) return;
  if (m_client == Client::Codex && program().isEmpty()) return show(State::NotFound);
  if (m_client == Client::ClaudeDesktop && !QFileInfo(QFileInfo(desktopConfigPath()).absolutePath()).isDir()) return show(State::NotFound);
  inspect([this](bool ok, std::optional<Entry> entry) {
    if (!ok) return;
    m_entry = entry;
    const State state = classify(entry, m_executable, m_discovery);
    // Claude Code: its settings file is read without the program; with nothing registered and no program, it is not here.
    if (m_client == Client::ClaudeCode && state == State::NotRegistered && program().isEmpty()) return show(State::NotFound);
    QString detail;
    if (entry) detail = QDir::toNativeSeparators(entry->command) + (entry->args.size() == 4 ? " · " + QDir::toNativeSeparators(entry->args[3]) : QString());
    show(state, detail);
  });
}

void AgentClientRow::add(std::function<void(bool, QString)> done) {
  const QStringList args = serverArgs(m_discovery);
  const QString exe = QDir::toNativeSeparators(m_executable);
  if (m_client == Client::ClaudeDesktop) {
    QString error;
    const bool ok = writeServer(desktopConfigPath(), Entry{exe, args, true}, &error);
    return done(ok, error);
  }
  QStringList command = m_client == Client::ClaudeCode ? QStringList{"mcp", "add", "opad", "--scope", "user", "--", exe} : QStringList{"mcp", "add", "opad", "--", exe};
  command << args;
  run(command, done);
}

void AgentClientRow::drop(std::function<void(bool, QString)> done) {
  if (m_client == Client::ClaudeDesktop) {
    QString error;
    const bool ok = writeServer(desktopConfigPath(), std::nullopt, &error);
    return done(ok, error);
  }
  run(m_client == Client::ClaudeCode ? QStringList{"mcp", "remove", "opad", "--scope", "user"} : QStringList{"mcp", "remove", "opad"}, done);
}

void AgentClientRow::connectClient() {
  if (!QFileInfo::exists(m_executable)) return message(tr("opad-cli was not found beside OPAD: install the matching opad-cli before connecting a client."));
  inspect([this](bool ok, std::optional<Entry> entry) {
    if (!ok) return;
    if (entry) {
      if (classify(entry, m_executable, m_discovery) == State::Registered) {
        refresh();
        return message(tr("Already connected. %1").arg(restartHint()));
      }
      return pointHere();  // something else is registered as "opad": replaced by this copy
    }
    add([this](bool success, QString error) {
      refresh();
      message(success ? tr("Connected. %1").arg(restartHint()) : tr("%1 refused: %2").arg(name(), error));
    });
  });
}

void AgentClientRow::pointHere() {
  if (!QFileInfo::exists(m_executable)) return message(tr("opad-cli was not found beside OPAD: install the matching opad-cli before connecting a client."));
  inspect([this](bool ok, std::optional<Entry> entry) {
    if (!ok) return;
    auto then = [this] {
      add([this](bool success, QString error) {
        refresh();
        message(success ? tr("Now pointed at this OPAD. %1").arg(restartHint()) : tr("%1 refused: %2").arg(name(), error));
      });
    };
    if (!entry) return then();
    drop([this, then](bool success, QString error) {
      if (!success) {
        refresh();
        return message(tr("%1 refused: %2").arg(name(), error));
      }
      then();
    });
  });
}

void AgentClientRow::removeClient() {
  inspect([this](bool ok, std::optional<Entry> entry) {
    if (!ok) return;
    const State state = classify(entry, m_executable, m_discovery);
    if (state == State::NotRegistered) {
      refresh();
      return message(tr("Not connected: nothing to remove."));
    }
    if (state == State::Stale) {
      refresh();
      return message(tr("This registration belongs to another OPAD; it was not removed. Point it at this OPAD, or manage it in %1.").arg(name()));
    }
    drop([this](bool success, QString error) {
      refresh();
      message(success ? tr("Removed. %1 keeps its other servers. Disconnect clients (Access and activity) ends a running connection at once.").arg(name())
                      : tr("%1 refused: %2").arg(name(), error));
    });
  });
}
