// OPAD_BENCH_VERSION_PROTECT=<prefix> (with OPAD_BENCH_AGENT, so agent access is on) on tools/bench_cases/vcs.py's
// protect/model.opad, committed on main with OPAD's attributes. Branch protection (GitAgent.hpp): the Preferences page's
// rows; the Version control panel asking before a commit to protected main (New branch…, Cancel, Commit to main) and
// not asking once the preference is off, and before a push when that preference is on; then an agent through the live
// bridge's own socket (as opad-cli mcp --live talks to it): git_status sees main protected, git_commit is refused there,
// git_branch_create makes and switches to a branch of its own (the chip and the panel follow), git_commit refuses the
// unsaved document and commits it once saved (the panel's history shows it), git_switch back to main (the document
// follows the file, the connection still bound), git_merge into main refused, previewed; every action a line in Agent
// activity. Pictures at <prefix>.<step>.png.
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QSettings>
#include <QTimer>
#include <memory>

#include "AgentBridge.hpp"
#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "GitAgent.hpp"
#include "GitWatch.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Preferences.hpp"
#include "ToolPanel.hpp"
#include "VersionControl.hpp"
#include "VersionPanel.hpp"
#include "opad/live.hpp"

OPAD_BENCH(OPAD_BENCH_VERSION_PROTECT, versionProtect) {
  auto* vc = w.findChild<VersionControl*>();
  return vc && vc->benchProtect(value);
}

bool VersionControl::benchProtect(const QString& prefix) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  m_benching = true;
  using opad::json;
  QWidget* window = m_services.window();
  AppDocument* doc = m_services.document();
  auto* agent = window->findChild<AgentBridge*>();
  struct State {
    size_t step = 0;
    int wait = 0, asked = 0;
    size_t ops = 0;
    QLocalSocket* socket = nullptr;
    QByteArray input;
    json reply;
    bool waiting = false, saved = false;
    int requests = 0;
  };
  auto st = std::make_shared<State>();
  auto require = [](bool ok, const QString& why) {
    if (!ok) throw std::runtime_error(why.toStdString());
  };
  auto pass = [](const QString& what) { trace::log("bench: version-protect: " + what + " PASS"); };
  auto dialog = [window](const char* name) -> QDialog* {
    for (QDialog* d : window->findChildren<QDialog*>(QString::fromLatin1(name)))
      if (d->isVisible()) return d;
    return nullptr;
  };
  auto idle = [this, doc] { return settled() && m_git->idle() && !doc->loading && !doc->designBusy && !doc->snapshotBusy(); };
  auto asked = [this] { return int(m_asked.count("vcsProtected")); };
  auto activity = [agent](const QString& text) {
    for (const QString& line : agent->activityLines())
      if (line.contains(text)) return true;
    return false;
  };
  // The live bridge's own protocol, one request at a time: {"name", "arguments"} and a line back.
  auto send = [st](const json& request) {
    st->reply = nullptr;
    st->waiting = true;
    st->socket->write(QByteArray::fromStdString(request.dump()) + '\n');
  };
  auto tool = [send, st](const std::string& name, json args = json::object(), bool write = false) {
    if (write) args["request_id"] = "protect-" + std::to_string(++st->requests);
    send({{"name", name}, {"arguments", args}});
  };
  auto bind = [send, agent] {
    const json d = agent->descriptor();
    send({{"name", "live_bind"}, {"arguments", {{"instance", d["instance"]}, {"target", d["target"]}}}, {"client", {{"name", "protect bench"}}}, {"version", opad::version_string()}});
  };
  auto answered = [st] { return !st->waiting && !st->reply.is_null(); };
  auto result = [st]() -> const json& { return st->reply["structuredContent"]; };
  auto refusedWith = [st](const std::string& code) { return st->reply.value("isError", false) && st->reply["structuredContent"]["error"].value("code", "") == code; };
  auto box = [doc](int x) {
    doc->run("feature", {{"kind", "box"}, {"inputs", {{"x", std::to_string(x) + " mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}}}});
  };
  require(agent, "the agent bridge");
  std::vector<std::function<bool()>> steps{
      [=, this] {  // the Preferences page: the rows and their defaults
        if (!idle()) return false;
        PreferencesDialog* d = PreferencesDialog::open(window, "vcs");
        QWidget* page = d->pageWidget("vcs");
        require(page, "the Version control page");
        auto* branches = page->findChild<QLineEdit*>(gitagent::kBranchesKey);
        auto* initial = page->findChild<QLineEdit*>(gitagent::kInitialBranchKey);
        auto* commits = page->findChild<QCheckBox*>(gitagent::kCommitsKey);
        auto* merges = page->findChild<QCheckBox*>(gitagent::kMergesKey);
        auto* pushes = page->findChild<QCheckBox*>(gitagent::kPushesKey);
        require(branches && initial && commits && merges && pushes, "the branch protection rows");
        require(branches->text() == gitagent::defaultBranches() && initial->text() == "main" && commits->isChecked() && merges->isChecked() && !pushes->isChecked(),
                "their defaults: main, master; commits and merges refused, pushes not");
        d->setSearch(QStringLiteral("protected"));
        require(d->visiblePages().contains("vcs"), "found by searching");
        d->setSearch({});
        d->grab().save(prefix + ".preferences.png");
        d->close();
        pass("the Preferences rows (Protected branches, Refuse commits / merges / pushes, Initial branch) with their defaults");
        m_answers["vcsProtected"] = "branch";
        m_services.action("vcs.commit")->trigger();
        return true;
      },
      [=, this] {
        QDialog* d = dialog("vcsNewBranch");
        if (!d) return false;
        require(asked() == 1 && !dialog("vcsCommit"), "Commit… on main asked first");
        d->reject();
        pass("Commit… on protected main asks; New branch… offers a branch");
        m_answers["vcsProtected"] = "cancel";
        m_services.action("vcs.commit")->trigger();
        return true;
      },
      [=, this] {
        if (asked() != 2) return false;
        require(!dialog("vcsCommit") && !dialog("vcsNewBranch"), "Cancel: nothing opens");
        pass("Cancel: no commit");
        m_answers["vcsProtected"] = "anyway";
        m_services.action("vcs.commit")->trigger();
        return true;
      },
      [=, this] {
        QDialog* d = dialog("vcsCommit");
        if (!d) return false;
        require(asked() == 3, "asked again");
        d->reject();
        pass("Commit to main: the commit dialog");
        QSettings().setValue(gitagent::kCommitsKey, false);
        m_services.action("vcs.commit")->trigger();
        return true;
      },
      [=, this] {
        QDialog* d = dialog("vcsCommit");
        if (!d) return false;
        require(asked() == 3, "the preference off: not asked");
        d->reject();
        QSettings().setValue(gitagent::kCommitsKey, true);
        pass("the preference off: the commit dialog at once");
        QSettings().setValue(gitagent::kPushesKey, true);
        m_answers["vcsProtected"] = "cancel";
        m_lastDone.clear();
        push();
        return true;
      },
      [=, this] {
        if (asked() != 4) return false;
        require(m_lastDone.isEmpty() && m_running == 0, "nothing pushed");
        QSettings().setValue(gitagent::kPushesKey, false);
        pass("Push of protected main asks while that preference is on");
        st->socket = new QLocalSocket(this);
        connect(st->socket, &QLocalSocket::readyRead, this, [st] {
          st->input += st->socket->readAll();
          if (const auto end = st->input.indexOf('\n'); end >= 0) {
            st->reply = json::parse(st->input.left(end).toStdString());
            st->input.remove(0, end + 1);
            st->waiting = false;
          }
        });
        st->socket->connectToServer(QString::fromStdString(agent->descriptor()["endpoint"].get<std::string>()));
        require(st->socket->waitForConnected(3000), "the bridge's socket: " + st->socket->errorString());
        bind();
        return true;
      },
      [=] {
        if (!answered()) return false;
        require(!st->reply.value("isError", false), "bound: " + QString::fromStdString(st->reply.dump()));
        tool("git_status");
        return true;
      },
      [=] {
        if (!answered()) return false;
        const json& r = result()["result"];
        require(r["branch"] == "main" && r["protected"] == true && r["clean"] == true, "the agent's status: " + QString::fromStdString(r.dump()));
        pass("git_status: main, protected");
        tool("git_commit", {{"message", "Agent on main"}, {"all", true}}, true);
        return true;
      },
      [=] {
        if (!answered()) return false;
        require(refusedWith("protected_branch"), "refused on main: " + QString::fromStdString(st->reply.dump()));
        require(result()["error"]["next"] == "git_branch_create", "the refusal says what to do");
        require(activity(QStringLiteral("Refused: main is a protected branch")), "the refusal in Agent activity");
        pass("git_commit on main refused, said in Agent activity");
        tool("git_branch_create", {{"name", "agent/work"}}, true);
        return true;
      },
      [=, this] {
        if (!answered() || !idle() || m_git->repo().status.branch != "agent/work") return false;
        require(!st->reply.value("isError", false) && result()["result"]["branch"] == "agent/work", "created: " + QString::fromStdString(st->reply.dump()));
        require(activity(tr("Agent created the branch %1 and switched to it").arg("agent/work")), "in Agent activity");
        openPanel(Branches);
        return true;
      },
      [=, this] {
        if (!idle() || m_panel->branchLabel()->text() != "agent/work") return false;
        pass("git_branch_create: the chip and the panel show agent/work");
        st->ops = doc->doc.ops.size();
        box(30);
        tool("git_commit", {{"message", "A box by the agent"}, {"paths", {"model.opad"}}}, true);
        return true;
      },
      [=, this] {
        if (!answered()) return false;
        require(refusedWith("unsaved_document"), "the unsaved document refused: " + QString::fromStdString(st->reply.dump()));
        pass("git_commit refuses the unsaved document");
        doc->saveAsync(m_services.jobs(), QString(), false, [st](bool saved, const QString&) { st->saved = saved; });
        return true;
      },
      [=] {
        if (!st->saved || !idle()) return false;
        tool("git_commit", {{"message", "A box by the agent"}, {"paths", {"model.opad"}}}, true);
        return true;
      },
      [=, this] {
        if (!answered() || !idle() || m_history.empty() || m_history.front().subject != "A box by the agent") return false;
        require(!st->reply.value("isError", false) && result()["result"]["branch"] == "agent/work", "committed: " + QString::fromStdString(st->reply.dump()));
        require(activity(QStringLiteral("A box by the agent")), "in Agent activity");
        m_tool->grab().save(prefix + ".panel.png");
        pass("git_commit on agent/work: the panel's history shows it");
        tool("git_switch", {{"branch", "main"}}, true);
        return true;
      },
      [=, this] {
        if (!answered() || !idle() || m_git->repo().status.branch != "main" || doc->doc.ops.size() != st->ops) return false;
        require(!st->reply.value("isError", false), "switched: " + QString::fromStdString(st->reply.dump()));
        require(activity(tr("Agent switched to %1").arg("main")), "in Agent activity");
        pass("git_switch to main: the document followed the file");
        tool("live_state");  // the same document reloaded: still bound, no live_bind
        return true;
      },
      [=] {
        if (!answered()) return false;
        require(!st->reply.value("isError", false) && result().contains("notice"), "still bound after the reload, told once: " + QString::fromStdString(st->reply.dump()).left(400));
        pass("git_switch: the connection still bound to the reloaded document (a notice says so)");
        tool("git_merge", {{"source", "agent/work"}}, true);
        return true;
      },
      [=] {
        if (!answered()) return false;
        require(refusedWith("protected_branch"), "a merge into main refused: " + QString::fromStdString(st->reply.dump()));
        tool("git_merge", {{"source", "agent/work"}, {"preview", true}}, true);
        return true;
      },
      [=, this] {
        if (!answered() || !idle()) return false;
        const json& r = result()["result"];
        require(r["state"] == "preview" && r["fast_forward"] == true && r["commits"].size() == 1 && m_git->repo().status.branch == "main", "the preview: " + QString::fromStdString(r.dump()));
        require(activity(tr("Agent previewed %1").arg("agent/work")), "in Agent activity");
        for (ToolPanel* p : window->findChildren<ToolPanel*>())
          if (p->id() == "agent-activity") p->grab().save(prefix + ".activity.png");
        pass("git_merge into main refused, its preview allowed; Agent activity lists every action");
        return true;
      },
  };
  auto* timer = new QTimer(this);
  timer->setInterval(150);
  connect(timer, &QTimer::timeout, this, [this, st, steps, timer] {
    try {
      if (st->step >= steps.size()) {
        timer->stop();
        if (st->socket) st->socket->abort();
        QCoreApplication::exit(0);
        return;
      }
      if (steps[st->step]()) {
        ++st->step;
        st->wait = 0;
      } else if (++st->wait > 800) {  // 2 min
        throw std::runtime_error(QStringLiteral("timed out in step %1 (last reply %2; %3)").arg(st->step).arg(QString::fromStdString(st->reply.dump()), m_lastFailure).toStdString());
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QStringLiteral("bench: version-protect: FAIL %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
