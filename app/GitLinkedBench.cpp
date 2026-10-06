// OPAD_BENCH_GIT_LINKED=<prefix> (with OPAD_BENCH_AGENT, so agent access is on) on tools/bench_cases/vcs.py's
// git-linked/project/model.opad: a document in its own repository linking KiCad boards from folders outside its project,
// checked out on feature/boards. An agent drives git through the live bridge's own socket (as opad-cli mcp --live talks to
// it): git_switch to develop (a reload: the boards go), git_merge feature/boards (a fast-forward: the boards come in by a
// merge of the file), git_switch feature/third (a third board comes in), back to feature/boards (a reload keeping the boards),
// then feature/alt (another history linking base again: a reload that reads it). After each one every linked file's parts are
// read again, as an open reads them; on every tick, no linked file's row shows the in-sync check while parts of it are not
// loaded. Pictures at <prefix>.<step>.png.
#include <QCoreApplication>
#include <QLocalSocket>
#include <QTimer>
#include <memory>

#include "AgentBridge.hpp"
#include "AppDocument.hpp"
#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BenchRegistry.hpp"
#include "BrowserDelegate.hpp"
#include "BrowserPanel.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "opad/live.hpp"

OPAD_BENCH(OPAD_BENCH_GIT_LINKED, gitLinked) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  using opad::json;
  const QString prefix = value;
  AppDocument* doc = w.m_doc;
  MainWindow* win = &w;
  auto* agent = w.findChild<AgentBridge*>();
  AssetsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (auto* found = qobject_cast<AssetsArea*>(a)) area = found;
  struct State {
    size_t step = 0;
    int wait = 0, requests = 0;
    QLocalSocket* socket = nullptr;
    QByteArray input;
    json reply;
    bool waiting = false;
  };
  auto st = std::make_shared<State>();
  auto require = [](bool ok, const QString& why) {
    if (!ok) throw std::runtime_error(why.toStdString());
  };
  auto pass = [](const QString& what) { trace::log("bench: git-linked: " + what + " PASS"); };
  auto send = [st](const json& request) {
    st->reply = nullptr;
    st->waiting = true;
    st->socket->write(QByteArray::fromStdString(request.dump()) + '\n');
  };
  auto tool = [send, st](const std::string& name, json args = json::object(), bool write = false) {
    if (write) args["request_id"] = "linked-" + std::to_string(++st->requests);
    send({{"name", name}, {"arguments", args}});
  };
  auto bind = [send, agent] {
    const json d = agent->descriptor();
    send({{"name", "live_bind"}, {"arguments", {{"instance", d["instance"]}, {"target", d["target"]}}}, {"client", {{"name", "git-linked bench"}}}, {"version", opad::version_string()}});
  };
  auto answered = [st] { return !st->waiting && !st->reply.is_null(); };
  auto failed = [st] { return st->reply.value("isError", false); };
  auto dump = [st] { return QString::fromStdString(st->reply.dump()).left(600); };
  auto settled = [doc] { return !doc->loading && !doc->designBusy && !doc->snapshotBusy(); };
  // The linked imports by file name ("base.kicad_pcb"): parts and those not loaded, from the scene.
  struct Linked {
    std::string import, root;
    int bodies = 0, missing = 0;
  };
  auto linked = [doc, area] {
    std::map<std::string, Linked> out;
    if (!area || !area->monitor()) return out;
    for (const auto& [import, a] : area->monitor()->assets()) {
      Linked& l = out[a.name];
      l.import = import;
      l.root = a.root;
      l.bodies = a.bodies;
      l.missing = a.missing;
    }
    (void)doc;
    return out;
  };
  auto names = [](const std::map<std::string, Linked>& l) {
    QStringList out;
    for (const auto& [name, info] : l) out << QStringLiteral("%1 (%2 parts, %3 not loaded)").arg(QString::fromStdString(name)).arg(info.bodies).arg(info.missing);
    return out.join(", ");
  };
  // Every board named is linked with all its parts loaded, and nothing else is linked.
  auto loaded = [linked](std::initializer_list<const char*> boards) {
    const auto l = linked();
    if (l.size() != boards.size()) return false;
    for (const char* b : boards) {
      const auto it = l.find(b);
      if (it == l.end() || it->second.bodies == 0 || it->second.missing > 0) return false;
    }
    return true;
  };
  auto badge = [doc, area](const std::string& root) {
    browser::Decoration d;
    if (const opad::Node* n = doc->node(root)) area->decorate({root, "component", {}, n}, d);
    return d.badges.isEmpty() ? browser::Badge{} : d.badges.front();
  };
  // On every tick: a linked file whose parts are not all loaded never shows the in-sync check.
  auto invariant = [linked, badge, names] {
    for (const auto& [name, l] : linked())
      if (l.missing > 0 && badge(l.root).icon == "check")
        throw std::runtime_error(QStringLiteral("%1 shows the in-sync check with %2 of %3 parts not loaded (%4)")
                                     .arg(QString::fromStdString(name)).arg(l.missing).arg(l.bodies).arg(names(linked())).toStdString());
  };
  // One agent git command, then the document as the branch has it: `boards` linked and loaded.
  auto gitStep = [=](const std::string& name, json args, std::initializer_list<const char*> boards, const QString& what) {
    std::vector<const char*> want(boards);
    return std::vector<std::function<bool()>>{
        [=] {
          if (!settled()) return false;
          tool(name, args, true);
          return true;
        },
        [=] {
          if (!answered()) return false;
          require(!failed(), QString::fromStdString(name) + ": " + dump());
          return true;
        },
        [=] {
          const auto l = linked();
          if (!settled() || l.size() != want.size()) return false;
          for (const char* b : want)
            if (!l.count(b) || l.at(b).missing > 0) return false;
          for (const char* b : want) require(badge(l.at(b).root).icon == "check", QString("%1: in sync after %2").arg(b, what));
          win->grab().save(prefix + "." + QString::fromStdString(name) + ".png");
          pass(what + ": " + names(l));
          bind();  // a reload of the document unbinds: bind its new generation
          return true;
        },
        [=] {
          if (!answered()) return false;
          require(!failed(), "bound again: " + dump());
          return true;
        },
    };
  };
  require(agent && area && area->monitor(), "the agent bridge and the linked files area");
  std::vector<std::function<bool()>> steps{
      [=] {  // opened: both boards read (their folder is trusted), each in sync
        if (!settled() || !loaded({"base.kicad_pcb", "screen.kicad_pcb"})) return false;
        for (const auto& [name, l] : linked()) require(badge(l.root).icon == "check", QString::fromStdString(name) + ": in sync when opened");
        pass("opened: " + names(linked()));
        st->socket = new QLocalSocket(win);
        QObject::connect(st->socket, &QLocalSocket::readyRead, win, [st] {
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
        require(!failed(), "bound: " + dump());
        return true;
      },
  };
  auto add = [&steps](std::vector<std::function<bool()>> more) { steps.insert(steps.end(), more.begin(), more.end()); };
  add(gitStep("git_switch", {{"branch", "develop"}}, {}, "git_switch develop (a reload): no board"));
  add(gitStep("git_merge", {{"source", "feature/boards"}}, {"base.kicad_pcb", "screen.kicad_pcb"}, "git_merge feature/boards (a merge of the file): both boards read"));
  add(gitStep("git_switch", {{"branch", "feature/third"}}, {"base.kicad_pcb", "screen.kicad_pcb", "third.kicad_pcb"},
              "git_switch feature/third (a merge of the file): the third board read"));
  add(gitStep("git_switch", {{"branch", "feature/boards"}}, {"base.kicad_pcb", "screen.kicad_pcb"}, "git_switch feature/boards (a reload): the boards loaded"));
  add(gitStep("git_switch", {{"branch", "feature/alt"}}, {"base.kicad_pcb"}, "git_switch feature/alt (a reload, another history): base read again"));
  auto* timer = new QTimer(&w);
  timer->setInterval(150);
  QObject::connect(timer, &QTimer::timeout, &w, [st, steps, timer, invariant, linked, names] {
    try {
      if (st->step >= steps.size()) {
        timer->stop();
        if (st->socket) st->socket->abort();
        QCoreApplication::exit(0);
        return;
      }
      if (st->step > 0) invariant();
      if (steps[st->step]()) {
        ++st->step;
        st->wait = 0;
      } else if (++st->wait > 400) {  // 1 min
        throw std::runtime_error(QStringLiteral("timed out in step %1 (linked: %2; last reply %3)").arg(st->step).arg(names(linked()), QString::fromStdString(st->reply.dump()).left(600)).toStdString());
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QStringLiteral("bench: git-linked: FAIL %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
