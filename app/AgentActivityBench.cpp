// OPAD_BENCH_AGENT_ACTIVITY=<prefix> (with OPAD_BENCH_AGENT, so agent access is on) on an empty document: an AI assistant
// ("claude-code") through the live bridge's own socket, as opad-cli mcp --live talks to it, and what the user sees of it
// (AgentActivity.hpp, AgentFeed.cpp). Step by step: no panel opens on connect, one toast says who connected; the chip's
// words per state; live_state adds no row; lookups fold into one row with their count; a look keeps its picture; a task
// (transaction) is one group row, "not applied yet", its steps under it, and Discard drops it (the chip leaves "preview");
// errors the assistant retries make no row; look-only access gives one "needs you" row (and a question toast) whose Allow
// changes grants edits; a change reads as what was done ("Added a 40 × 20 × 10 mm box"), Undo only on the newest step,
// the undo step "Claude: …", the step marked by the AI in the timeline with its name saved (ai_client); Stop's divider and
// toast. Pictures at <prefix>.chip.png, .panel.png and .settings.png. Text checks run in English; OPAD_LANG=ar checks the
// same structure (kinds, states, counts) and that no row shows English sentences.
#include <QCoreApplication>
#include <QDialog>
#include <QLocalSocket>
#include <QTimer>
#include <memory>

#include "AgentBridge.hpp"
#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "MainWindow.hpp"
#include "TimelineWidget.hpp"
#include "Toast.hpp"
#include "ToolPanel.hpp"
#include "opad/live.hpp"

namespace {
void require(bool ok, const QString& why) {
  if (!ok) throw std::runtime_error(why.toStdString());
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_AGENT_ACTIVITY, agentActivity) {
  auto* agent = w.findChild<AgentBridge*>();
  return agent && agent->benchActivity(value);
}

bool AgentBridge::benchActivity(const QString& prefix) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  struct State {
    size_t step = 0;
    int wait = 0, requests = 0;
    QLocalSocket* socket = nullptr;
    QByteArray input;
    json reply;
    bool waiting = false;
    std::string transaction;
    unsigned long long base = 0;
    quint64 group = 0, firstBox = 0;
    size_t rows = 0;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  const bool english = i18n::current() == QLatin1String("en") || i18n::current().isEmpty();
  auto pass = [](const QString& what) { trace::log("bench: agent activity: " + what + " PASS"); };
  auto send = [st](const json& request) {
    st->reply = nullptr;
    st->waiting = true;
    st->socket->write(QByteArray::fromStdString(request.dump()) + '\n');
  };
  auto tool = [send, st](const std::string& name, json args = json::object(), bool write = false) {
    if (write) args["request_id"] = "activity-" + std::to_string(++st->requests);
    send({{"name", name}, {"arguments", args}});
  };
  auto answered = [st] { return !st->waiting && !st->reply.is_null(); };
  auto failedWith = [st](const std::string& code) { return st->reply.value("isError", false) && st->reply["structuredContent"]["error"].value("code", "") == code; };
  auto idle = [this] { return !m_busy && !m_doc->loading && !m_doc->snapshotBusy() && !m_doc->designBusy; };
  auto events = [this]() -> const std::vector<ActivityEvent>& {
    static const std::vector<ActivityEvent> none;
    return m_feed ? m_feed->events() : none;
  };
  auto last = [events]() -> const ActivityEvent& {
    require(!events().empty(), "a row");
    return events().back();
  };
  auto toastWith = [this](const QString& text) {
    if (!m_services || !m_services->toasts()) return false;
    for (Toast* t : m_services->toasts()->toasts())
      if (t->text().contains(text)) return true;
    return false;
  };
  auto anyToast = [this] { return m_services && m_services->toasts() && !m_services->toasts()->toasts().isEmpty(); };
  auto chipText = [this] { return m_chip ? agentsay::plain(m_chip->text()) : QString(); };
  auto box = [st](json extra = json::object()) {
    json args = {{"kind", "box"}, {"inputs", {{"length", "40 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}, {"expected_revision", st->base}};
    for (auto& [k, v] : extra.items()) args[k] = v;
    return args;
  };
  require(m_enabled && m_edit, "agent access on (OPAD_BENCH_AGENT)");
  std::vector<std::function<bool()>> steps{
      [=, this] {  // before anyone connects: the chip says AI is ready, nothing is open
        if (!idle()) return false;
        require(m_chip, "the AI chip");
        if (english) require(chipText() == "AI ready", "the chip before a connection: " + chipText());
        require(!m_panel || !m_panel->isVisible(), "no panel before a connection");
        st->socket = new QLocalSocket(this);
        connect(st->socket, &QLocalSocket::readyRead, this, [st] {
          st->input += st->socket->readAll();
          if (const auto end = st->input.indexOf('\n'); end >= 0) {
            st->reply = json::parse(st->input.left(end).toStdString());
            st->input.remove(0, end + 1);
            st->waiting = false;
          }
        });
        st->socket->connectToServer(m_endpoint);
        require(st->socket->waitForConnected(3000), "the bridge's socket: " + st->socket->errorString());
        const json d = descriptor();
        send({{"name", "live_bind"}, {"arguments", {{"instance", d["instance"]}, {"target", d["target"]}}}, {"client", {{"name", "claude-code"}}}, {"version", opad::version_string()}});
        return true;
      },
      [=, this] {  // connected: one toast, no panel, the chip names the assistant
        if (!answered()) return false;
        require(!st->reply.value("isError", false), "bound: " + QString::fromStdString(st->reply.dump()));
        require(!m_panel || !m_panel->isVisible(), "the panel stays closed on connect");
        require(anyToast(), "a toast on the first connection");
        if (english) {
          require(toastWith("Claude connected to"), "the toast names Claude");
          require(chipText() == "Claude", "the chip while connected: " + chipText());
        }
        require(events().size() == 1 && events()[0].kind == ActivityEvent::Divider, "one divider row");
        st->base = m_doc->revision;
        st->rows = events().size();
        pass("connect: a toast and the chip, no panel");
        tool("live_state");
        return true;
      },
      [=, this] {  // polling makes no row
        if (!answered()) return false;
        require(events().size() == st->rows, "live_state adds no row");
        tool("context", {{"section", "nodes"}});
        return true;
      },
      [=, this] {
        if (!answered()) return false;
        tool("info");
        return true;
      },
      [=, this] {  // two lookups: one folded row
        if (!answered()) return false;
        const auto& e = last();
        require(e.kind == ActivityEvent::Reads && e.reads == 2 && e.children.size() == 2, QStringLiteral("one fold with 2 lookups (%1, %2)").arg(e.kind).arg(e.reads));
        if (english) require(feedLines().join(QStringLiteral(" | ")).contains("Looked over the model · 2 lookups"), "the fold's line: " + feedLines().join(" | "));
        pass("lookups fold into one row");
        tool("viewport_image", {{"view", "front"}, {"width", 240}, {"height", 160}});
        return true;
      },
      [=, this] {  // a look keeps its picture
        if (!answered()) return false;
        const auto& e = last();
        require(e.kind == ActivityEvent::Look && !e.thumb.isNull() && e.thumb.width() <= 96 && e.thumb.height() <= 64, "a look row with a 96 x 64 thumbnail");
        if (english) require(agentsay::plain(e.text) == "Looked at the model from the front", "the look's sentence: " + agentsay::plain(e.text));
        pass("a look row with its picture");
        tool("transaction_begin", {{"label", "Mounting block"}, {"expected_revision", st->base}}, true);
        return true;
      },
      [=, this] {  // a task: one group row, not applied yet
        if (!answered()) return false;
        require(!st->reply.value("isError", false), "transaction_begin: " + QString::fromStdString(st->reply.dump()));
        st->transaction = st->reply["structuredContent"].value("transaction", "");
        const auto& e = last();
        require(e.kind == ActivityEvent::Group && e.state == ActivityEvent::Staged, "a staged group row");
        if (english) require(agentsay::plain(e.text).contains("Mounting block") && agentsay::plain(e.second).startsWith("Preview, not applied yet"), "the group's words: " + agentsay::plain(e.text) + " / " + e.second);
        st->group = e.id;
        tool("feature", box({{"transaction", st->transaction}}), true);
        return true;
      },
      [=, this] {  // its step under it; the chip says preview
        if (!answered() || !idle()) return false;
        require(!st->reply.value("isError", false), "a staged box: " + QString::fromStdString(st->reply.dump()));
        const auto* g = row(st->group);
        require(g && g->children.size() == 1, "the step under its task");
        if (english) {
          require(agentsay::plain(g->children[0]) == "Added a 40 × 20 × 10 mm box", "the step's sentence: " + agentsay::plain(g->children[0]));
          require(chipText() == "Claude · preview", "the chip while a preview waits: " + chipText());
        }
        pass("a task is one group row, its steps under it, not applied yet");
        emit m_feed->activated(st->group, QStringLiteral("discard"));  // the user's Discard
        return true;
      },
      [=, this] {  // Discard drops it
        const auto* g = row(st->group);
        require(g && g->state == ActivityEvent::Dropped && !m_prepared, "Discard drops the task");
        if (english) {
          require(g->second == "Discarded by you. Nothing changed.", "the dropped line: " + g->second);
          require(chipText() == "Claude", "the chip after Discard: " + chipText());
        }
        st->rows = events().size();
        tool("transaction_commit", {{"id", st->transaction}, {"expected_revision", st->base}}, true);
        return true;
      },
      [=, this] {  // the assistant's commit of a discarded task fails, silently for the user
        if (!answered()) return false;
        require(st->reply.value("isError", false), "the discarded task cannot commit");
        tool("feature", box({{"expected_revision", st->base + 99}}), true);
        return true;
      },
      [=, this] {
        if (!answered()) return false;
        require(failedWith("stale_revision"), "a stale revision: " + QString::fromStdString(st->reply.dump()));
        require(events().size() == st->rows, "errors the assistant retries make no row");
        pass("Discard drops a task; retried errors make no row");
        setAccess(true, false);  // look only
        tool("feature", box(), true);
        return true;
      },
      [=, this] {  // look only: it needs the user
        if (!answered()) return false;
        require(failedWith("read_only"), "read_only: " + QString::fromStdString(st->reply.dump()));
        const auto& e = last();
        require(e.kind == ActivityEvent::NeedsYou && e.action == "allowEdit" && m_needs == e.id, "a needs-you row with Allow changes");
        require(m_needsToast, "a question toast while the panel is closed");
        if (english) {
          require(agentsay::plain(e.text) == "Claude can look but not change anything", "the headline: " + agentsay::plain(e.text));
          require(chipText() == "Claude needs you", "the chip: " + chipText());
        }
        emit m_feed->activated(e.id, QStringLiteral("allowEdit"));
        require(m_edit && !m_needs, "Allow changes grants edits");
        if (english) require(toastWith("Claude can now change your designs"), "the toast after Allow changes");
        pass("look only: one needs-you row, Allow changes");
        tool("feature", box(), true);
        return true;
      },
      [=, this] {  // a change: what was done, Undo on it, the undo step and the timeline
        if (!answered() || !idle()) return false;
        require(!st->reply.value("isError", false), "a box: " + QString::fromStdString(st->reply.dump()));
        const auto& e = last();
        require(e.kind == ActivityEvent::Change && e.state == ActivityEvent::Applied && e.canUndo && !e.subjects.empty() && !e.op.empty(), "a change row with Undo");
        const auto* op = m_doc->doc.find_op(e.op);
        require(op && op->data.value("by", "") == "Agent" && op->data.value("ai_client", "") == "Claude", "the step saved with by Agent and ai_client Claude");
        if (english) {
          require(agentsay::plain(e.text) == "Added a 40 × 20 × 10 mm box", "the change's sentence: " + agentsay::plain(e.text));
          require(m_doc->undoLabel() == "Claude: Added a 40 × 20 × 10 mm box", "the undo step: " + m_doc->undoLabel());
          require(chipText() == "Claude" && m_chip->state().count == 1, "the chip counts one change: " + chipText());
          if (auto* tl = m_services ? m_services->timeline() : nullptr) require(tl->tooltip(e.op).contains("Claude (AI)"), "the timeline names Claude");
        }
        st->firstBox = e.id;
        st->base = m_doc->revision;
        tool("feature", box({{"inputs", {{"x", "60 mm"}, {"length", "40 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}}), true);
        return true;
      },
      [=, this] {  // Undo only on the newest step
        if (!answered() || !idle()) return false;
        require(!st->reply.value("isError", false), "a second box");
        const auto* first = row(st->firstBox);
        require(first && !first->canUndo && last().canUndo, "Undo only on the newest step");
        pass("a change reads as what was done; Undo only on the newest step; the step names its AI");
        showActivity();
        require(m_panel->isVisible() && m_chip->state().count == 0, "Show clears the count");
        return true;
      },
      [=, this] {  // pictures
        if (++st->wait < 5) return false;
        m_chip->grab().save(prefix + ".chip.png");
        m_panel->grab().save(prefix + ".panel.png");
        m_benchDelay = 1500;  // a long tail, so Stop finds it running
        st->base = m_doc->revision;
        tool("feature", box({{"inputs", {{"x", "120 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}}}), true);
        st->clock.start();
        return true;
      },
      [=, this] {  // Stop while it works
        if (st->clock.elapsed() < 500) return false;
        require(m_running && m_busy, "a call running");
        if (english) require(chipText() == "Claude · working", "the chip while it works: " + chipText());
        userStop();
        require(last().kind == ActivityEvent::Divider, "Stop's divider");
        if (english) {
          require(agentsay::plain(last().text).startsWith("You stopped Claude"), "the divider: " + last().text);
          require(toastWith("Stopped Claude. Changes it already made stay"), "Stop's toast");
        }
        m_benchDelay = 0;
        return true;
      },
      [=, this] {
        if (!answered()) return false;
        require(st->reply.value("isError", false), "the stopped call did not commit");
        pass("Stop: its divider and toast; the call did not commit");
        settings(0);
        return true;
      },
      [=, this] {
        if (++st->wait < 6) return false;
        for (auto* d : m_window->findChildren<QDialog*>("agentSettings")) {
          d->grab().save(prefix + ".settings.png");
          d->close();
        }
        if (!english)
          for (const auto& e : events())
            require(!agentsay::plain(e.text).startsWith("Looked") && !agentsay::plain(e.text).startsWith("Added"), "rows in Arabic: " + e.text);
        pass("the whole session");
        return true;
      },
  };
  auto* timer = new QTimer(this);
  timer->setInterval(100);
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
      } else if (++st->wait > 1200) {  // 2 min
        throw std::runtime_error(QStringLiteral("timed out in step %1 (last reply %2)").arg(st->step).arg(QString::fromStdString(st->reply.dump()).left(400)).toStdString());
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QStringLiteral("bench: agent activity: FAIL step %1: %2").arg(st->step).arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
