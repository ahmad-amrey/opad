// OPAD_BENCH_AGENT_CONNECT=<prefix> (tools/bench_cases/agent.py, case agent-connect): Settings > AI integration opens on
// Connect an AI client while no client is connected, with each client's state at a glance and the four steps. The case
// points APPDATA (Claude Desktop's folder on Windows) and CLAUDE_CONFIG_DIR (Claude Code's settings) at folders of its own,
// where this bench writes: Claude Desktop with another server and an "opad" for an old data folder (registered for another
// OPAD or an old folder), Claude Code with an "opad" whose program is gone (registered, but its program is missing). Point
// it at this OPAD on Claude Desktop rewrites only mcpServers.opad (the other server and key kept, a .bak copy), Remove takes
// it out again; step 3's switch turns agent access on (the Access page shows it too); step 4's prompt names the document.
// <prefix>.png is the page.
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <memory>
#include <nlohmann/json.hpp>

#include "AgentBridge.hpp"
#include "AgentClients.hpp"
#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"

OPAD_BENCH(OPAD_BENCH_AGENT_CONNECT, agentConnect) {
  using ojson = nlohmann::ordered_json;
  bool ok = true;
  auto check = [&ok](bool pass, const QString& what) {
    trace::log(QString("bench: agent connect: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
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

  AgentBridge* agent = w.m_agent;
  w.m_agent->settings();
  auto* dialog = w.findChild<QDialog*>("agentSettings");
  auto* tabs = dialog ? dialog->findChild<QTabWidget*>() : nullptr;
  check(tabs && tabs->currentWidget() && tabs->currentWidget()->objectName() == "agentConnectPage" && !agent->clientConnected(),
        "with no client connected it opens on Connect an AI client");
  // The rows check their clients asynchronously; a real Codex found as a Store app can take seconds to answer
  // `codex mcp list`, so wait until its row is checked (at most 20 s) instead of a fixed 400 ms.
  auto run = [=, &w]() mutable {
    bool ok2 = ok;
    auto check2 = [&ok2](bool pass, const QString& what) {
      trace::log(QString("bench: agent connect: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
      ok2 = ok2 && pass;
    };
    auto* desk = w.findChild<AgentClientRow*>("agentClientClaudeDesktop");
    auto* claude = w.findChild<AgentClientRow*>("agentClientClaudeCode");
    auto* codex = w.findChild<AgentClientRow*>("agentClientCodex");
    check2(desk && claude && codex, "a row for Claude Code, Claude Desktop and Codex");
    if (desk && claude && codex) {
      using S = agentclients::State;
      check2(desk->state() == S::Stale && desk->report().contains(QDir::toNativeSeparators("old/opad-data")), "Claude Desktop: registered for an old data folder: " + desk->report());
      check2(claude->state() == S::Missing || claude->state() == S::Stale, "Claude Code: registered, but its program is missing: " + claude->report());
      check2(codex->state() != S::Unchecked, "Codex: checked (" + codex->report().section('\n', 0, 0) + ")");
      auto* point = desk->findChild<QPushButton*>("agentClientPoint");
      check2(point && point->isVisibleTo(desk) && point->isEnabled(), "Claude Desktop offers Point it at this OPAD");
      if (point) point->click();
      const ojson after = readJson(desktop);
      check2(desk->state() == S::Registered && after["mcpServers"]["opad"]["command"] == QDir::toNativeSeparators(cli).toStdString() &&
                 after["mcpServers"].contains("other") && after["globalShortcut"] == "Ctrl+Space" && QFile::exists(desktop + ".bak") &&
                 desk->report().contains("Claude Desktop"),
             "pointed at this OPAD: only mcpServers.opad rewritten, the rest kept, a .bak copy; restart said: " + desk->report().replace('\n', " / "));
      if (auto* remove = desk->findChild<QPushButton*>("agentClientRemove")) remove->click();
      const ojson removed = readJson(desktop);
      check2(desk->state() == S::NotRegistered && !removed["mcpServers"].contains("opad") && removed["mcpServers"].contains("other"), "Remove takes only opad out");
    }
    auto* allow = dialog->findChild<QCheckBox*>("agentConnectEnable");
    if (allow) allow->setChecked(true);
    check2(allow && agent->statusSummary() != QObject::tr("Disabled"), "step 3's switch turns agent access on: " + agent->statusSummary());
    auto* prompt = dialog->findChild<QLineEdit*>("agentConnectPrompt");
    const QString file = QString::fromStdU16String(w.m_doc->doc.path.filename().u16string());
    check2(prompt && !file.isEmpty() && prompt->text().contains("\"" + file + "\""), "step 4's prompt names this document: " + (prompt ? prompt->text() : QString()));
    QCoreApplication::processEvents();  // the rows laid out again after their last change
    dialog->grab().save(prefix + ".png");
    agent->setAccess(false, false);
    dialog->close();
    trace::log(QString("bench: agent connect: the Connect an AI client page %1").arg(ok2 ? "PASS" : "FAIL"));
    QCoreApplication::exit(ok2 ? 0 : 2);
  };
  auto* poll = new QTimer(&w);
  auto started = std::make_shared<QElapsedTimer>();
  started->start();
  QObject::connect(poll, &QTimer::timeout, &w, [=, &w]() mutable {
    auto* codex = w.findChild<AgentClientRow*>("agentClientCodex");
    if (started->elapsed() < 400 || (codex && codex->state() == agentclients::State::Unchecked && started->elapsed() < 20000)) return;
    poll->stop();
    poll->deleteLater();
    run();
  });
  poll->start(100);
  return true;
}
