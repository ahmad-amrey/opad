// OPAD_BENCH_AGENT_ACTIVITY=<prefix> (with OPAD_BENCH_AGENT, so agent access is on) on an empty document: an AI assistant
// ("claude-code") through the live bridge's own socket, as opad-cli mcp --live talks to it, and what the user sees of it
// (AgentActivity.hpp, AgentFeed.cpp). Step by step: no panel opens on connect, one toast says who connected; the chip's
// words per state; live_state adds no row; lookups fold into one row with their count; a look keeps its picture; a task
// (transaction) is one group row, "not applied yet", its steps under it, with the viewport's preview chip ("Claude's
// preview · not applied", the steps in its tooltip) whose Discard drops it (the chip leaves "preview", the preview chip
// goes); errors the assistant retries make no row; look-only access gives one "needs you" row (and a question toast) whose
// Allow changes grants edits; a change reads as what was done ("Added a 40 × 20 × 10 mm box"), Undo only on the newest
// step, the undo step "Claude: …", the step marked by the AI in the timeline with its name saved (ai_client); Properties
// says "Claude (AI) · hh:mm" for it; the marker's Show in AI activity opens the panel on its row; Show keeps the camera and
// Back restores it until the camera moves; the assistant's note under "AI" in the Annotations author filter; Stop's divider
// and toast, no row left running; undo / redo replies with their labels as before; a failure names its own call; Look and
// edit chosen elsewhere ends the needs-you row; "Retrying…" goes with the next call; a failed commit's row says so.
// Pictures at <prefix>.chip.png, .panel.png and .settings.png. Text checks run in English; OPAD_LANG=ar checks the same
// structure (kinds, states, counts) and that no row shows English sentences.
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QRegularExpression>
#include <QToolButton>
#include <QLocalSocket>
#include <QTimer>
#include <algorithm>
#include <memory>

#include "AgentBridge.hpp"
#include "AnnotationsPanel.hpp"
#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "MainWindow.hpp"
#include "PropertiesPanel.hpp"
#include "Viewport.hpp"
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
  MainWindow* win = &w;
  // Properties pinned open, as a click on a timeline marker shows the step there: its author line.
  auto propertiesLine = [win](const std::string& op) {
    const auto pin = [win](bool on) {
      for (QToolButton* b : win->m_propsPanel->findChildren<QToolButton*>())
        if (b->isCheckable()) b->setChecked(on);  // pinned: it stays open for the op (a new selection closes it otherwise)
    };
    pin(true);
    win->m_propsPanel->show();
    win->selectOpTargets(op);
    const QString line = win->m_props->subtitle();
    pin(false);
    win->m_propsPanel->hide();
    return line;
  };
  return agent && agent->benchActivity(value, propertiesLine);
}

bool AgentBridge::benchActivity(const QString& prefix, std::function<QString(const std::string&)> propertiesLine) {
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
    quint64 group = 0, firstBox = 0, shown = 0;
    json away;  // a camera far from the model: Show moves off it, Back returns to it
    quint64 retry = 0;  // a row left saying "Retrying…"
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
  auto noRunning = [events] {
    return std::none_of(events().begin(), events().end(), [](const ActivityEvent& e) { return e.kind == ActivityEvent::Running; });
  };
  auto anyToast = [this] { return m_services && m_services->toasts() && !m_services->toasts()->toasts().isEmpty(); };
  auto chipText = [this] { return m_chip ? agentsay::plain(m_chip->text()) : QString(); };
  // The viewport's preview chip (AgentArea.cpp): shown (not hidden: the bench window itself is), its words, its Discard.
  auto preview = [this]() -> QFrame* { return m_window->findChild<QFrame*>(QStringLiteral("aiPreviewChip")); };
  auto previewShown = [preview] { return preview() && !preview()->isHidden(); };
  auto previewText = [preview] {
    auto* label = preview() ? preview()->findChild<QLabel*>(QStringLiteral("aiPreviewText")) : nullptr;
    return label ? label->text() : QString();
  };
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
        require(preview() && !previewShown(), "no preview chip while nothing is staged");
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
        require(previewShown(), "the viewport's preview chip while a task is staged");
        if (english) {
          require(previewText() == "Claude's preview · not applied", "the preview chip's words: " + previewText());
          require(preview()->toolTip().contains("No steps yet."), "the preview chip's tooltip before a step: " + preview()->toolTip());
        }
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
        require(previewShown() && preview()->toolTip().contains(g->children[0]), "the preview chip's tooltip lists the step: " + preview()->toolTip());
        pass("a task is one group row, its steps under it, not applied yet");
        auto* discard = preview()->findChild<QToolButton*>(QStringLiteral("chipAction"));
        require(discard, "the preview chip's Discard");
        discard->click();  // the user's Discard, on the viewport's chip (the group row's discard)
        return true;
      },
      [=, this] {  // Discard drops it
        const auto* g = row(st->group);
        require(g && g->state == ActivityEvent::Dropped && !m_prepared, "Discard drops the task");
        require(!previewShown(), "the preview chip goes with the preview");
        pass("the viewport's preview chip: its words, the steps in its tooltip, Discard");
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
        // Properties: who made the step and when.
        const std::string firstOp = first->op;
        const QString line = agentsay::plain(propertiesLine(firstOp));
        if (english) {
          static const QRegularExpression aiLine(QStringLiteral("^Claude \\(AI\\) · \\d\\d:\\d\\d$"));
          require(aiLine.match(line).hasMatch(), "the Properties author line: " + line);
        } else {
          require(line.contains("Claude") && !line.contains("Agent"), "the Properties author line names Claude: " + line);
        }
        // The timeline marker's Show in AI activity: the panel opens on the step's row.
        require(rowForOp(firstOp) == st->firstBox, "the first box's step finds its row");
        QMenu menu;
        m_services->timelineMenu(menu, firstOp);
        QAction* reveal = nullptr;
        for (QAction* a : menu.actions())
          if (a->objectName() == QLatin1String("timelineAiActivity")) reveal = a;
        require(reveal && reveal->isEnabled(), "Show in AI activity on an AI step's marker, enabled");
        if (english) require(reveal->text() == "Show in AI activity", "the entry's words: " + reveal->text());
        reveal->trigger();
        require(m_panel->isVisible() && m_feed->revealed() == st->firstBox, "Show in AI activity opens the panel on the step's row");
        pass("Properties says Claude (AI) and when; the timeline marker's Show in AI activity");
        // Show on a change row: Back returns to the camera it left.
        const json camera = m_viewport->cameraJson();
        require(!camera.is_null(), "a camera");
        json away = camera;
        for (const char* key : {"eye", "target"}) away[key][0] = camera[key][0].get<double>() + 500.0;
        away["scale"] = camera.value("scale", 1.0) * 4.0;
        m_viewport->setCameraJson(away);
        st->away = m_viewport->cameraJson();
        st->shown = last().id;
        rowAction(st->shown, QStringLiteral("show"));
        require(m_feed->showing() == st->shown && !sameCamera(m_viewport->cameraJson(), st->away), "Show frames the change and offers Back");
        if (english) require(m_feed->actionsOf(st->shown).startsWith("Back|"), "the row's buttons after Show: " + m_feed->actionsOf(st->shown));
        return true;
      },
      [=, this] {  // Back: the camera Show left
        if (m_shownCamera.is_null()) return false;  // the watch took the camera Show rested at
        rowAction(st->shown, QStringLiteral("back"));
        require(sameCamera(m_viewport->cameraJson(), st->away) && m_feed->showing() == 0, "Back puts the camera back");
        if (english) require(m_feed->actionsOf(st->shown).startsWith("Show|"), "the row's buttons after Back: " + m_feed->actionsOf(st->shown));
        rowAction(st->shown, QStringLiteral("show"));
        require(m_feed->showing() == st->shown, "Show again offers Back again");
        return true;
      },
      [=, this] {  // the user moves the camera: Back goes
        if (m_shownCamera.is_null()) return false;
        m_viewport->setCameraJson(st->away);
        return true;
      },
      [=, this] {
        if (m_feed->showing() != 0) return false;
        if (english) require(m_feed->actionsOf(st->shown).startsWith("Show|"), "the row reads Show once the camera moved: " + m_feed->actionsOf(st->shown));
        pass("Show keeps the camera, Back restores it, a camera move turns Back into Show");
        // A note by the assistant: the Annotations panel's author filter has "AI".
        const auto* shown = row(st->shown);
        require(shown && !shown->subjects.empty(), "a body to pin a note to");
        tool("annotate", {{"anchor", shown->subjects.front()}, {"text", "Check this wall"}, {"expected_revision", m_doc->revision}}, true);
        return true;
      },
      [=, this] {
        if (!answered() || !idle()) return false;
        require(!st->reply.value("isError", false), "a note: " + QString::fromStdString(st->reply.dump()));
        auto* notes = m_window->findChild<AnnotationsPanel*>();
        require(notes, "the Annotations panel");
        notes->rebuild();
        QComboBox* authors = notes->authorFilter();
        const int ai = authors->findData(QStringLiteral("Agent"));
        require(ai > 0, "an AI entry in the author filter");
        if (english) require(authors->itemText(ai) == "AI", "the entry's words: " + authors->itemText(ai));
        authors->setCurrentIndex(ai);
        require(notes->shownCount() == 1, QStringLiteral("AI shows the assistant's note (%1)").arg(notes->shownCount()));
        authors->setCurrentIndex(0);
        pass("Annotations: an AI entry in the author filter");
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
        // Job::cancel runs the call's done callback at once: its row went with it, nothing says "working".
        require(!m_running && noRunning() && !m_busy, "no row left running after Stop");
        if (english) require(chipText() == "Claude", "the chip after Stop: " + chipText());
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
        require(!m_running && noRunning(), "still no row running once the stopped call replied");
        pass("Stop: its divider and toast; the call did not commit; no row left running");
        tool("undo", {{"expected_revision", m_doc->revision}}, true);
        return true;
      },
      [=, this] {  // undo / redo replies name an AI step as they always did
        if (!answered() || !idle()) return false;
        require(!st->reply.value("isError", false), "undo: " + QString::fromStdString(st->reply.dump()));
        const auto undone = QString::fromStdString(st->reply["structuredContent"]["result"].value("undone", ""));
        if (english) require(undone == "Agent: annotate", "the undo reply's label, as before: " + undone);
        else require(!undone.contains("Claude"), "the undo reply's label is not the window's: " + undone);
        tool("redo", {{"expected_revision", m_doc->revision}}, true);
        return true;
      },
      [=, this] {
        if (!answered() || !idle()) return false;
        require(!st->reply.value("isError", false), "redo: " + QString::fromStdString(st->reply.dump()));
        const auto redone = QString::fromStdString(st->reply["structuredContent"]["result"].value("redone", ""));
        if (english) require(redone == "Agent: annotate", "the redo reply's label, as before: " + redone);
        pass("undo / redo replies keep their labels");
        // A failure names the call that failed, not another connection's call refused meanwhile.
        ActivityEvent e;
        e.kind = ActivityEvent::Running;
        e.text = QStringLiteral("Filleting the test part…");
        m_running = post(e);
        m_call.running = QStringLiteral("Something else…");
        failed("invalid_operation", QStringLiteral("boom"));
        require(!m_running && last().kind == ActivityEvent::Quiet && last().text.contains("Filleting the test part") && !last().text.contains("Something else"),
                "the failure row names its own call: " + last().text);
        // Edits allowed from the menu: the needs-you row asking for them goes.
        setAccess(true, false);
        needsYou(QStringLiteral("Needs edits"), QString(), QStringLiteral("allowEdit"), QStringLiteral("Allow changes"), QString());
        const quint64 asking = m_needs;
        require(asking && row(asking), "a needs-you row");
        setAccess(true, true);
        require(!m_needs && !row(asking), "Look and edit chosen elsewhere ends the needs-you row");
        // A third retryable failure of a running call in 20 s leaves "Retrying…" until the next call.
        for (int i = 0; i < 3; ++i) {
          ActivityEvent r;
          r.kind = ActivityEvent::Running;
          r.text = QStringLiteral("Adding a box…");
          m_running = post(r);
          st->retry = m_running;
          failed("snapshot_failed", QStringLiteral("busy"));
        }
        require(st->retry && m_retrying == st->retry && row(st->retry), "a Retrying row");
        // A commit that fails (a read-only document): its row says so, none stays running.
        m_doc->readOnly = true;
        st->base = m_doc->revision;
        tool("feature", box({{"inputs", {{"x", "200 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}}}), true);
        return true;
      },
      [=, this] {
        if (!answered() || !idle()) return false;
        m_doc->readOnly = false;
        require(failedWith("commit_failed"), "a read-only document's commit fails: " + QString::fromStdString(st->reply.dump()));
        require(!row(st->retry) && !m_retrying, "the next call's row replaced the Retrying one");
        require(!m_running && noRunning() && last().kind == ActivityEvent::Quiet, "the failed commit's row says so, none left running");
        pass("failures name their own call; needs-you ends when edits are allowed; Retrying goes; a failed commit's row");
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
