// OPAD_BENCH_AGENT_CONNECT=<prefix> (tools/bench_cases/agent.py, case agent-connect): Settings > AI assistants is one
// screen. The case points APPDATA (Claude Desktop's folder on Windows) and CLAUDE_CONFIG_DIR (Claude Code's settings) at
// folders of its own, where this bench writes: Claude Desktop with another server and an "opad" for an old data folder,
// Claude Code with an "opad" whose program is gone. Along the way: no program path on the screen; each app's card with its
// state in plain words and one button; Connect on Claude Desktop rewrites only mcpServers.opad (the other server and key
// kept, a .bak copy) and the card then shows only what is left (restart, the first question naming the document); the
// status line waits for it, says the access choice is needed while it is Nothing, and turns green by itself when a client
// binds through the bridge's socket; the access choice is one setting with the chip's (Nothing / Look only / Look and
// edit); Copy setup message copies one message naming the server, this opad-cli, the discovery folder and the document,
// and its JSON link the same server as settings; Details has the program paths, the raw settings, Check OPAD is listening
// and who is connected now; Disconnect takes only opad out. The clipboard is put back as it was.
// <prefix>.png is the screen after Connect, <prefix>.copy.png the message page, <prefix>.details.png Details.
#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

#include "AgentBridge.hpp"
#include "AgentClients.hpp"
#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "opad/util.hpp"

OPAD_BENCH(OPAD_BENCH_AGENT_CONNECT, agentConnect) {
  using ojson = nlohmann::ordered_json;
  struct State {
    bool ok = true;
    int step = 0, wait = 0;
    QString clipboard;  // the user's clipboard, put back at the end
    QLocalSocket* socket = nullptr;
    QByteArray input;
    bool bound = false;
    QElapsedTimer started;
  };
  auto st = std::make_shared<State>();
  auto check = [st](bool pass, const QString& what) {
    trace::log(QString("bench: agent connect: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    st->ok = st->ok && pass;
  };
  const QString prefix = value;
  QString cli = QCoreApplication::applicationDirPath() + "/opad-cli";
#ifdef Q_OS_WIN
  cli += ".exe";
#endif
  const QString desktop = agentclients::desktopConfigPath(), code = agentclients::claudeCodeConfigPath();
#if defined(Q_OS_WIN)
  const char* base = "APPDATA";
#elif defined(Q_OS_MACOS)
  const char* base = "HOME";
#else
  const char* base = "XDG_CONFIG_HOME";
#endif
  const QString own = QDir::cleanPath(QDir::fromNativeSeparators(qEnvironmentVariable(base)));
  const bool isolated = !qEnvironmentVariable("CLAUDE_CONFIG_DIR").isEmpty() && !own.isEmpty() && desktop.startsWith(own);
  check(isolated, "the clients' settings are the case's own folders: " + desktop + ", " + code);
  if (!isolated) {  // never a real Claude configuration
    QCoreApplication::exit(2);
    return true;
  }
  auto writeJson = [](const QString& file, const ojson& j) {
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::fromStdString(j.dump(2)));
  };
  auto readJson = [](const QString& file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? ojson::parse(f.readAll().toStdString(), nullptr, false) : ojson();
  };
  writeJson(desktop, ojson{{"globalShortcut", "Ctrl+Space"},
                           {"mcpServers", {{"other", {{"command", "npx"}, {"args", {"-y", "other-server"}}}},
                                           {"opad", {{"command", cli.toStdString()}, {"args", {"mcp", "--live", "--discovery", "C:/old/opad-data/opad/agent"}}}}}}});
  writeJson(code, ojson{{"mcpServers", {{"opad", {{"command", (QDir::cleanPath(prefix) + "/gone/opad-cli.exe").toStdString()}, {"args", {"mcp", "--live", "--discovery", "x"}}}}}}});
  st->clipboard = QApplication::clipboard()->text();

  AgentBridge* agent = w.m_agent;
  agent->setAccess(false, false);  // the access choice starts at Nothing
  agent->settings();
  st->started.start();
  auto dialog = [&w] { return w.findChild<QDialog*>("agentSettings"); };
  auto pages = [dialog] { return dialog() ? dialog()->findChild<QStackedWidget*>("agentSettingsPages") : nullptr; };
  auto page = [pages] { return pages() && pages()->currentWidget() ? pages()->currentWidget()->objectName() : QString(); };
  auto row = [&w](const char* name) { return w.findChild<AgentClientRow*>(name); };
  auto action = [](AgentClientRow* r) { return r ? r->findChild<QPushButton*>("agentClientAction") : nullptr; };
  auto label = [dialog](const char* name) {
    auto* l = dialog() ? dialog()->findChild<QLabel*>(name) : nullptr;
    return l ? l->text() : QString();
  };
  auto stateShown = [dialog] {
    auto* bar = dialog() ? dialog()->findChild<QWidget*>("agentConnectStateBar") : nullptr;
    return bar && bar->isVisibleTo(dialog());
  };
  auto access = [dialog](int i) -> QAbstractButton* {
    auto* box = dialog() ? dialog()->findChild<QWidget*>("agentAccess") : nullptr;
    if (!box) return nullptr;
    for (auto* b : box->findChildren<QAbstractButton*>())
      if (b->property("access").toInt() == i) return b;
    return nullptr;
  };
  auto byRole = [dialog](const char* role) -> QToolButton* {
    if (!dialog()) return nullptr;
    for (auto* b : dialog()->findChildren<QToolButton*>())
      if (b->property("role").toString() == QLatin1String(role) && b->isVisibleTo(dialog())) return b;
    return nullptr;
  };
  const QString file = QString::fromStdU16String(w.m_doc->doc.path.filename().u16string());
  using S = agentclients::State;

  std::vector<std::function<bool()>> steps{
      [=, &w] {  // the one screen, each app checked (a real Codex found as a Store app can take seconds to answer)
        auto* codex = row("agentClientCodex");
        if (st->started.elapsed() < 400 || (codex && codex->state() == S::Unchecked && st->started.elapsed() < 20000)) return false;
        check(dialog() && page() == "agentConnectPage" && !agent->clientConnected(), "it opens on the one screen");
        check(!dialog()->findChild<QWidget*>("agentConnectEnable") && !dialog()->findChild<QWidget*>("agentConnectEdit"), "no access checkboxes");
        bool noPaths = true;
        for (auto* e : dialog()->findChildren<QLineEdit*>())
          if ((e->objectName() == "claudeExecutable" || e->objectName() == "codexExecutable") && e->isVisibleTo(dialog())) noPaths = false;
        check(noPaths, "no program path or Browse on the screen");
        auto *desk = row("agentClientClaudeDesktop"), *claude = row("agentClientClaudeCode");
        check(desk && claude && codex, "a card for Claude Desktop, Claude Code and Codex");
        if (!desk || !claude || !codex) return true;
        check(desk->state() == S::Stale && desk->report().contains(QDir::toNativeSeparators("old/opad-data")), "Claude Desktop: registered for an old data folder: " + desk->report());
        check(claude->state() == S::Missing || claude->state() == S::Stale, "Claude Code: registered, but its program is missing: " + claude->report());
        check(codex->state() != S::Unchecked, "Codex: checked: " + codex->headline());
        if (codex->state() == S::NotFound) {
          auto* get = codex->findChild<QToolButton*>("toastAction");
          check(codex->headline() == QObject::tr("Not on this computer") && get && get->isVisibleTo(codex) && !action(codex)->isVisibleTo(codex),
                "Codex not here: \"Not on this computer\" with a get-it link, no button");
        }
        check(action(desk) && action(desk)->isVisibleTo(desk) && action(desk)->isEnabled() && action(desk)->text() == QObject::tr("Connect"),
              "Claude Desktop's one button: Connect (" + desk->headline() + ")");
        check(access(0) && access(0)->isChecked() && !access(1)->isChecked() && !access(2)->isChecked(), "the access choice shows Nothing");
        check(!stateShown(), "no status line before anything is connected");
        action(desk)->click();
        return true;
      },
      [=] {  // connected: only what is left, the status line waits and asks for the access choice
        auto* desk = row("agentClientClaudeDesktop");
        if (desk->state() != S::Registered) return st->wait > 50 ? (check(false, "Claude Desktop connected: " + desk->report()), true) : false;
        const ojson after = readJson(desktop);
        check(after["mcpServers"]["opad"]["command"] == QDir::toNativeSeparators(cli).toStdString() && after["mcpServers"].contains("other") &&
                  after["globalShortcut"] == "Ctrl+Space" && QFile::exists(desktop + ".bak"),
              "Connect rewrote only mcpServers.opad: the rest kept, a .bak copy");
        auto* next = desk->findChild<QWidget*>("agentClientNext");
        auto* restart = desk->findChild<QLabel*>("agentClientRestart");
        auto* prompt = desk->findChild<QLineEdit*>("agentConnectPrompt");
        check(desk->justConnected() && next && next->isVisibleTo(desk) && restart && restart->text().contains(QObject::tr("Quit it from the tray too, or it keeps the old settings.")),
              "the card shows what is left: restart (from the tray too): " + (restart ? restart->text() : QString()));
        check(prompt && !file.isEmpty() && prompt->text().contains("\"" + file + "\""), "then a first question naming this document: " + (prompt ? prompt->text() : QString()));
        check(action(desk)->text() == QObject::tr("Disconnect"), "the button is now Disconnect");
        check(stateShown() && label("agentConnectState").contains("Claude Desktop") && label("agentConnectState").contains(QObject::tr("Look only")),
              "the status line waits for Claude Desktop and asks for the access choice: " + label("agentConnectState"));
        access(2)->click();
        check(agent->descriptor().value("enabled", false) && agent->descriptor().value("edit", false), "Look and edit turns access on with changes");
        check(label("agentConnectState") == QObject::tr("Waiting for %1 to connect…").arg("Claude Desktop"), "the status line: " + label("agentConnectState"));
        access(1)->click();
        check(agent->descriptor().value("enabled", false) && !agent->descriptor().value("edit", true) && access(1)->isChecked(), "Look only: access on, no changes");
        agent->setAccess(true, true);  // as the chip's menu does
        check(access(2)->isChecked(), "a change from the chip shows in the choice");
        QCoreApplication::processEvents();
        dialog()->grab().save(prefix + ".png");
        return true;
      },
      [=] {  // Copy setup message: one message for any AI app's chat
        auto* copy = dialog()->findChild<QPushButton*>("agentCopySetup");
        check(copy && copy->isVisibleTo(dialog()), "Using another AI app? has Copy setup message");
        if (!copy) return true;
        copy->click();
        const QString message = QApplication::clipboard()->text();
        check(page() == "agentCopyPage" && message == label("agentSetupMessage"), "it copies the message and shows it: " + page());
        check(message.contains("\"opad\"") && message.contains("stdio") && message.contains(QDir::toNativeSeparators(cli)) &&
                  message.contains(QDir::toNativeSeparators(QDir::cleanPath(agent->discoveryPath()))) && message.contains("--live") && message.contains("\"" + file + "\""),
              "the message names the server, this opad-cli, the discovery folder and the document: " + QString(message).replace('\n', " / "));
        if (auto* note = dialog()->findChild<QLabel*>("agentCopyJson")) emit note->linkActivated("json");
        const auto j = nlohmann::json::parse(QApplication::clipboard()->text().toStdString(), nullptr, false);
        check(j.is_object() && j["mcpServers"]["opad"]["command"] == QDir::toNativeSeparators(cli).toStdString() && j["mcpServers"]["opad"]["args"].size() == 4,
              "its link copies the same server as settings (JSON)");
        QCoreApplication::processEvents();
        dialog()->grab().save(prefix + ".copy.png");
        if (auto* back = byRole("back")) back->click();
        check(page() == "agentConnectPage", "← All AI apps goes back");
        // A client binds through the bridge's socket, as opad-cli mcp --live does.
        const auto d = agent->descriptor();
        st->socket = new QLocalSocket(dialog());
        QObject::connect(st->socket, &QLocalSocket::readyRead, st->socket, [st] {
          st->input += st->socket->readAll();
          if (st->input.contains('\n')) st->bound = true;
        });
        st->socket->connectToServer(QString::fromStdString(d.value("endpoint", "")));
        st->socket->waitForConnected(3000);
        const nlohmann::json bind = {{"name", "live_bind"}, {"arguments", {{"instance", d["instance"]}, {"target", d["target"]}}}, {"client", {{"name", "claude-desktop"}}}, {"version", opad::version_string()}};
        st->socket->write(QByteArray::fromStdString(bind.dump()) + '\n');
        return true;
      },
      [=] {  // green by itself
        if (!agent->clientConnected()) return st->wait > 100 ? (check(false, "a client binds: " + QString::fromUtf8(st->input)), true) : false;
        QCoreApplication::processEvents();
        check(label("agentConnectState").contains(file) && label("agentConnectState") != QObject::tr("Waiting for %1 to connect…").arg("Claude Desktop"),
              "the status line turned green by itself: " + label("agentConnectState"));
        auto* activity = byRole("activity");
        check(activity != nullptr, "with Show activity beside it");
        return true;
      },
      [=] {  // Details: what experts need
        if (auto* details = byRole("details")) details->click();
        check(page() == "agentDetailsPage", "Details opens its page");
        auto* path = dialog()->findChild<QLineEdit*>("claudeExecutable");
        check(path && path->isVisibleTo(dialog()), "the program paths with Browse are there");
        check(dialog()->findChild<QPushButton*>("agentCheckListening") && dialog()->findChild<QWidget*>("agentSetupFormat") &&
                  dialog()->findChild<QWidget*>("agentFollow") && dialog()->findChild<QWidget*>("agentAnnounce"),
              "raw settings in every format, Check OPAD is listening, zoom and notify");
        check(label("agentConnectedNow").contains(file), "Connected now lists the client: " + label("agentConnectedNow"));
        QCoreApplication::processEvents();
        dialog()->grab().save(prefix + ".details.png");
        if (auto* back = byRole("back")) back->click();
        st->socket->abort();
        // Disconnect takes only opad out.
        auto* desk = row("agentClientClaudeDesktop");
        action(desk)->click();
        const ojson removed = readJson(desktop);
        check(desk->state() == S::NotRegistered && !removed["mcpServers"].contains("opad") && removed["mcpServers"].contains("other"), "Disconnect takes only opad out");
        check(action(desk)->text() == QObject::tr("Connect") && desk->headline() == QObject::tr("Not connected yet"), "and the card says Not connected yet");
        return true;
      },
  };
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [=] {
    if (st->step >= int(steps.size()) || !dialog()) {
      timer->stop();
      check(st->step >= int(steps.size()), "the dialog stayed open to the end");
      QApplication::clipboard()->setText(st->clipboard);
      agent->setAccess(false, false);
      if (dialog()) dialog()->close();
      trace::log(QString("bench: agent connect: the AI assistants screen %1").arg(st->ok ? "PASS" : "FAIL"));
      QCoreApplication::exit(st->ok ? 0 : 2);
      return;
    }
    if (steps[st->step]()) {
      ++st->step;
      st->wait = 0;
    } else ++st->wait;
  });
  timer->start();
  return true;
}
