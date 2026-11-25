#include "AgentClients.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>

#include "Theme.hpp"
#include "opad/core.hpp"

using namespace agentclients;

namespace {
// A step after Connect: its number in a disc, then the words.
QLabel* stepNumber(int n, bool current) {
  const Tokens& t = theme::current();
  auto* l = new QLabel(QString::number(n));
  l->setFixedSize(22, 22);
  l->setAlignment(Qt::AlignCenter);
  l->setStyleSheet(QStringLiteral("border-radius:11px;font-size:12px;font-weight:600;background:%1;color:%2")
                       .arg(theme::css(current ? t.sel : t.bg4), theme::css(current ? t.onsel : t.fg)));
  return l;
}
}  // namespace

AgentClientRow::AgentClientRow(Client client, QString executable, QString discovery, QWidget* parent)
    : QWidget(parent), m_client(client), m_executable(std::move(executable)), m_discovery(std::move(discovery)) {
  setObjectName(client == Client::ClaudeCode ? "agentClientClaudeCode" : client == Client::ClaudeDesktop ? "agentClientClaudeDesktop" : "agentClientCodex");
  // A card: the app's initials, its name, its state in plain words, the one button that fits.
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  m_card = new QFrame;
  m_card->setObjectName("agentClientCard");
  outer->addWidget(m_card);
  auto* layout = new QVBoxLayout(m_card);
  layout->setContentsMargins(14, 12, 14, 12);
  layout->setSpacing(10);
  auto* head = new QHBoxLayout;
  head->setSpacing(14);
  layout->addLayout(head);
  m_badge = new QLabel(client == Client::ClaudeCode ? "CC" : client == Client::ClaudeDesktop ? "CD" : "CX");
  m_badge->setFixedSize(36, 36);
  m_badge->setAlignment(Qt::AlignCenter);
  head->addWidget(m_badge);
  auto* words = new QVBoxLayout;
  words->setSpacing(2);
  head->addLayout(words, 1);
  m_title = new QLabel(name());
  words->addWidget(m_title);
  m_headline = new QLabel;
  m_headline->setObjectName("agentClientPill");
  m_headline->setWordWrap(true);
  words->addWidget(m_headline);
  m_get = new QToolButton;
  m_get->setObjectName("toastAction");
  m_get->setAutoRaise(true);
  m_get->setCursor(Qt::PointingHandCursor);
  m_get->setText(tr("Get %1").arg(name()));
  connect(m_get, &QToolButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(getUrl())); });
  head->addWidget(m_get);
  m_action = new QPushButton;
  m_action->setObjectName("agentClientAction");
  m_action->setMinimumHeight(30);
  connect(m_action, &QPushButton::clicked, this, &AgentClientRow::act);
  head->addWidget(m_action);
  m_note = new QLabel;
  m_note->setObjectName("agentClientNote");
  m_note->setWordWrap(true);
  m_note->hide();
  layout->addWidget(m_note);

  // After Connect: only what is left. 1 restart the app, 2 ask it something about this design.
  m_next = new QWidget;
  m_next->setObjectName("agentClientNext");
  auto* next = new QVBoxLayout(m_next);
  next->setContentsMargins(50, 0, 0, 0);
  next->setSpacing(10);
  auto* one = new QHBoxLayout;
  one->setSpacing(12);
  one->addWidget(stepNumber(1, true), 0, Qt::AlignTop);
  m_restart = new QLabel;
  m_restart->setObjectName("agentClientRestart");
  m_restart->setWordWrap(true);
  m_restart->setTextFormat(Qt::RichText);
  one->addWidget(m_restart, 1);
  next->addLayout(one);
  auto* two = new QHBoxLayout;
  two->setSpacing(12);
  two->addWidget(stepNumber(2, false), 0, Qt::AlignTop);
  auto* ask = new QVBoxLayout;
  ask->setSpacing(8);
  ask->addWidget(new QLabel(tr("Ask it something about this design, for example:")));
  auto* askRow = new QHBoxLayout;
  m_prompt = new QLineEdit;
  m_prompt->setObjectName("agentConnectPrompt");
  m_prompt->setReadOnly(true);
  askRow->addWidget(m_prompt, 1);
  auto* copy = new QPushButton(tr("Copy"));
  connect(copy, &QPushButton::clicked, this, [this, copy] {
    QApplication::clipboard()->setText(m_prompt->text());
    copy->setText(tr("Copied"));
    QTimer::singleShot(1500, copy, [copy] { copy->setText(tr("Copy")); });
  });
  askRow->addWidget(copy);
  ask->addLayout(askRow);
  two->addLayout(ask, 1);
  next->addLayout(two);
  m_next->hide();
  layout->addWidget(m_next);

  // Details (laid out by the dialog's Details page): the technical line, the program, Browse, Check again.
  m_status = new QLabel(this);
  m_status->setObjectName("agentClientState");
  m_status->setWordWrap(true);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_status->hide();
  if (usesProgram()) {
    const char* key = client == Client::ClaudeCode ? "agent/claudeExecutable" : "agent/codexExecutable";
    QString saved = QSettings().value(key).toString();
    if (isOpadProgram(saved)) {  // OPAD's own opad-cli chosen by mistake (the box once read like it asked for it)
      QSettings().remove(key);
      saved.clear();
    }
    m_path = new QLineEdit(saved, this);
    m_path->setObjectName(client == Client::ClaudeCode ? "claudeExecutable" : "codexExecutable");
    m_path->setPlaceholderText(tr("Found by OPAD; or the path to %1").arg(programName()));
    m_path->hide();
    m_browse = new QPushButton(tr("Browse..."), this);
    m_browse->hide();
    connect(m_browse, &QPushButton::clicked, this, [this] {
      if (m_running) return;
      const QString path = QFileDialog::getOpenFileName(this, tr("Select the %1 program (%2)").arg(name(), programName()));
      if (path.isEmpty()) return;
      if (isOpadProgram(path)) return message(tr("That is OPAD's own program. Choose %1's program (%2).").arg(name(), programName()), true);
      message({});
      setProgram(path);
    });
    connect(m_path, &QLineEdit::editingFinished, this, &AgentClientRow::refresh);
  }
  m_check = new QPushButton(tr("Check again"), this);
  m_check->hide();
  connect(m_check, &QPushButton::clicked, this, &AgentClientRow::refresh);
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { show(m_state); });
  show(State::Unchecked);
}

QWidget* AgentClientRow::details() {
  if (m_details) return m_details;
  m_details = new QWidget;
  m_details->setObjectName(objectName() + "Details");
  auto* box = new QVBoxLayout(m_details);
  box->setContentsMargins(0, 0, 0, 0);
  box->setSpacing(4);
  auto* head = new QHBoxLayout;
  auto* title = new QLabel(name());
  title->setStyleSheet(QStringLiteral("font-weight:600"));
  head->addWidget(title, 1);
  head->addWidget(m_check);
  box->addLayout(head);
  box->addWidget(m_status);
  if (m_path) {
    auto* row = new QHBoxLayout;
    auto* label = new QLabel(tr("Program:"));
    label->setObjectName("secondary");
    row->addWidget(label);
    row->addWidget(m_path, 1);
    row->addWidget(m_browse);
    box->addLayout(row);
    m_path->setVisible(true);
    m_browse->setVisible(true);
  }
  m_status->setVisible(true);
  m_check->setVisible(true);
  return m_details;
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
QString AgentClientRow::headline() const { return m_headline->text(); }
void AgentClientRow::setPrompt(const QString& text) { m_prompt->setText(text); }

void AgentClientRow::message(const QString& text, bool problem) {
  const Tokens& t = theme::current();
  m_note->setText(text);
  m_note->setStyleSheet(QStringLiteral("font-size:12px;color:%1").arg(theme::css(problem ? t.amber : t.fg2)));
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
  if (path.isEmpty() || isOpadProgram(path)) {
    const QStringList found = m_client == Client::ClaudeCode ? claudeCandidates() : codexCandidates();
    return found.value(0);
  }
  if (!QFileInfo(path).isAbsolute()) path = QStandardPaths::findExecutable(path);
  return QFileInfo(path).isFile() ? path : QString();
}

QString AgentClientRow::programName() const {
#ifdef Q_OS_WIN
  return m_client == Client::ClaudeCode ? QStringLiteral("claude.exe") : QStringLiteral("codex.exe");
#else
  return m_client == Client::ClaudeCode ? QStringLiteral("claude") : QStringLiteral("codex");
#endif
}

QString AgentClientRow::restartStep() const {
  switch (m_client) {
    case Client::ClaudeCode: return tr("Start a new Claude Code session.");
    case Client::ClaudeDesktop: return tr("Quit Claude Desktop and open it again.");
    case Client::Codex: return tr("Restart Codex.");
  }
  return {};
}

QString AgentClientRow::restartWhy() const {
  switch (m_client) {
    case Client::ClaudeDesktop: return tr("Quit it from the tray too, or it keeps the old settings.");
    default: return tr("A session that is already running does not see OPAD yet.");
  }
}

QString AgentClientRow::getUrl() const {
  switch (m_client) {
    case Client::ClaudeCode: return QStringLiteral("https://claude.com/product/claude-code");
    case Client::ClaudeDesktop: return QStringLiteral("https://claude.ai/download");
    case Client::Codex: return QStringLiteral("https://openai.com/codex/");
  }
  return {};
}

void AgentClientRow::show(State state, const QString& detail) {
  m_state = state;
  const bool justAdded = m_added && state == State::Registered;
  const Tokens& t = theme::current();
  // The technical line (Details): what was found, and the files OPAD uses.
  QString text;
  switch (state) {
    case State::Unchecked: text = tr("Not checked yet"); break;
    case State::NotFound: text = usesProgram() ? tr("%1 was not found on this computer: install it, or Browse to its program").arg(name()) : tr("%1 was not found on this computer").arg(name()); break;
    case State::NotRegistered: text = tr("Not connected"); break;
    case State::Registered: text = tr("Connected to this OPAD"); break;
    case State::Disabled: text = tr("Connected to this OPAD, but switched off in %1").arg(name()); break;
    case State::Missing: text = tr("Registered, but its program is missing: %1").arg(detail); break;
    case State::Stale: text = tr("Registered for another OPAD or an old data folder: %1").arg(detail); break;
    case State::Failed: text = tr("Could not read its settings: %1").arg(detail); break;
  }
  if (usesProgram()) {
    if (const QString p = program(); !p.isEmpty()) text += "\n" + tr("Program: %1").arg(QDir::toNativeSeparators(p));
  } else if (state != State::NotFound) {
    text += "\n" + tr("Settings file: %1").arg(QDir::toNativeSeparators(desktopConfigPath()));
  }
  m_status->setText(text);
  m_status->setStyleSheet(QString("color: %1; font-size: 12px;").arg(theme::css(t.fg2)));

  // The card: the state in plain words (with a glyph where colour says something: never by colour alone), one button.
  QString line;
  QColor colour = t.fg2;
  switch (state) {
    case State::Unchecked: line = tr("Checking…"); colour = t.fg3; break;
    case State::NotFound: line = tr("Not on this computer"); colour = t.fg3; break;
    case State::NotRegistered: line = tr("Not connected yet"); break;
    case State::Registered: line = justAdded ? QStringLiteral("✓ ") + tr("Added to %1").arg(name()) : tr("Connected"); colour = t.green; break;
    case State::Disabled: line = QStringLiteral("⚠ ") + tr("Connected, but turned off in %1").arg(name()); colour = t.amber; break;
    case State::Missing: line = QStringLiteral("⚠ ") + tr("Set up for an OPAD that has moved: Connect sets it up again"); colour = t.amber; break;
    case State::Stale: line = QStringLiteral("⚠ ") + tr("Set up for another copy of OPAD: Connect uses this one"); colour = t.amber; break;
    case State::Failed: line = QStringLiteral("⚠ ") + tr("Couldn't read its settings"); colour = t.red; break;
  }
  m_headline->setText(line);
  m_headline->setToolTip(text);
  m_headline->setStyleSheet(QString("color: %1; font-size: 12px;").arg(theme::css(colour)));
  const bool absent = state == State::NotFound;
  m_title->setStyleSheet(QStringLiteral("font-weight:600;font-size:14px;color:%1").arg(theme::css(absent ? t.fg2 : t.fg)));
  m_badge->setStyleSheet(QStringLiteral("border-radius:8px;font-weight:700;background:%1;color:%2").arg(theme::css(absent ? t.bg3 : t.bg4), theme::css(absent ? t.fg2 : t.fg)));
  const QColor tint(t.green.red(), t.green.green(), t.green.blue(), 26), rim(t.green.red(), t.green.green(), t.green.blue(), 128);
  m_card->setStyleSheet(justAdded  ? QStringLiteral("QFrame#agentClientCard{background:%1;border:1px solid %2;border-radius:10px}").arg(theme::css(tint), theme::css(rim))
                        : absent   ? QStringLiteral("QFrame#agentClientCard{background:transparent;border:1px dashed %1;border-radius:10px}").arg(theme::css(t.line))
                                   : QStringLiteral("QFrame#agentClientCard{background:%1;border:1px solid %2;border-radius:10px}").arg(theme::css(t.bg3), theme::css(t.line)));
  const bool program = !usesProgram() || !this->program().isEmpty();
  const bool connects = state == State::NotRegistered || state == State::Missing || state == State::Stale;
  const bool disconnects = state == State::Registered || state == State::Disabled;
  m_get->setVisible(absent);
  m_action->setVisible(connects || disconnects || state == State::Failed);
  m_action->setText(connects ? tr("Connect") : disconnects ? tr("Disconnect") : tr("Try again"));
  m_action->setToolTip(state == State::Stale ? tr("%1 has an \"opad\" entry for another copy of OPAD: Connect replaces it with this one.").arg(name()) : QString());
  m_action->setEnabled((program || state == State::Failed) && !m_running);
  m_action->setStyleSheet(connects ? QStringLiteral("QPushButton{background:%1;border:1px solid %1;color:%2;font-weight:600;padding:0 18px;border-radius:6px}"
                                                    "QPushButton:hover{background:%3}QPushButton:disabled{background:%4;border-color:%4}")
                                         .arg(theme::css(t.sel), theme::css(t.onsel), theme::css(t.sel.lighter(115)), theme::css(t.bg4))
                                   : QStringLiteral("QPushButton{padding:0 14px;border-radius:6px}"));
  m_next->setVisible(justAdded);
  m_restart->setText(QStringLiteral("<b>%1</b> %2").arg(restartStep().toHtmlEscaped(), restartWhy().toHtmlEscaped()));
  m_check->setEnabled(!m_running);
  if (m_path) m_path->setEnabled(!m_running);
  if (m_browse) m_browse->setEnabled(!m_running);
  // Registered, but this client's own program is not here: the button waits for it, and says where to choose it.
  if (!program && state != State::NotFound && state != State::Unchecked && state != State::Failed && m_note->text().isEmpty())
    message(tr("%1's program was not found on this computer: choose it under Details.").arg(name()), true);
  if (layout()) layout()->activate();
  emit stateChanged();
}

void AgentClientRow::act() {
  switch (m_state) {
    case State::NotRegistered:
    case State::Missing:
    case State::Stale: return connectClient();
    case State::Registered:
    case State::Disabled: return removeClient();
    case State::Failed: message({}); return refresh();
    default: return;
  }
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

// Connect worked: the card shows the next steps (restart, a first question); it failed: why, on the card.
void AgentClientRow::added(bool success, const QString& error) {
  if (success) {
    message({});
    m_added = true;  // kept by refresh while the state stays Registered
  }
  refresh();
  if (!success) message(tr("%1 couldn't be connected: %2").arg(name(), error), true);
  else emit connected();
}

void AgentClientRow::connectClient() {
  if (!QFileInfo::exists(m_executable)) return message(tr("opad-cli was not found beside OPAD: install the matching opad-cli before connecting an AI app."), true);
  message({});
  inspect([this](bool ok, std::optional<Entry> entry) {
    if (!ok) return;
    if (entry) {
      if (classify(entry, m_executable, m_discovery) == State::Registered) return added(true, {});  // already here: the next steps
      return pointHere();  // something else is registered as "opad": replaced by this copy
    }
    add([this](bool success, QString error) { added(success, error); });
  });
}

void AgentClientRow::pointHere() {
  if (!QFileInfo::exists(m_executable)) return message(tr("opad-cli was not found beside OPAD: install the matching opad-cli before connecting an AI app."), true);
  inspect([this](bool ok, std::optional<Entry> entry) {
    if (!ok) return;
    auto then = [this] { add([this](bool success, QString error) { added(success, error); }); };
    if (!entry) return then();
    drop([this, then](bool success, QString error) {
      if (!success) {
        refresh();
        return message(tr("%1 refused: %2").arg(name(), error), true);
      }
      then();
    });
  });
}

void AgentClientRow::removeClient() {
  m_added = false;
  inspect([this](bool ok, std::optional<Entry> entry) {
    if (!ok) return;
    const State state = classify(entry, m_executable, m_discovery);
    if (state == State::NotRegistered) {
      refresh();
      return message(tr("Not connected: nothing to remove."));
    }
    if (state == State::Stale) {  // another OPAD's entry is never removed
      refresh();
      return message(tr("This registration belongs to another OPAD; it was not removed. Connect points it at this OPAD, or manage it in %1.").arg(name()), true);
    }
    drop([this](bool success, QString error) {
      refresh();
      message(success ? tr("Disconnected. %1 keeps its other tools; a session already running stays connected until it ends.").arg(name())
                      : tr("%1 refused: %2").arg(name(), error), !success);
    });
  });
}
