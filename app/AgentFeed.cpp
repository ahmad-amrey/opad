// The AI activity panel, the status bar's AI chip and the toasts an AI assistant may give (AgentActivity.hpp): what the
// bridge posts there and what the user's clicks do. Quiet by default: nothing opens by itself; only what the user can act
// on (an assistant that needs them, a first connection, Stop) is a toast.
#include <QActionGroup>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <unordered_set>

#include "AgentBridge.hpp"
#include "AreaController.hpp"
#include "DesignController.hpp"
#include "TimelineWidget.hpp"
#include "Toast.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"

using opad::json;

QStringList AgentBridge::activityLines() const { return m_raw; }
QStringList AgentBridge::feedLines() const { return m_feed ? m_feed->lines() : QStringList(); }

void AgentBridge::setServices(AreaServices* services) {
  m_services = services;
  if (m_panel && m_services) m_services->addPanel(m_panel);
}

AgentBridge::Names AgentBridge::who(const std::shared_ptr<Session>& s) const {
  const auto n = agentnames::display(s ? s->agent : QString());
  return {n.full, n.brief};
}

int AgentBridge::boundCount() const {
  int n = 0;
  for (const auto& s : m_sessions)
    if (s->bound && s->socket && s->socket->state() == QLocalSocket::ConnectedState) ++n;
  return n;
}

AgentBridge::Names AgentBridge::connectedName() const {
  std::shared_ptr<Session> pick;
  for (const auto& s : m_sessions) {
    if (!s->bound || !s->socket || s->socket->state() != QLocalSocket::ConnectedState) continue;
    if (!pick) pick = s;
    if (m_busy && s->socket == m_owner) pick = s;
  }
  return pick ? who(pick) : Names{};
}

QString AgentBridge::fileLabel() const {
  if (!m_doc->hasDocument) return tr("this window");
  if (m_doc->browse) return QFileInfo(m_doc->viewing).fileName();
  return m_doc->doc.path.empty() ? tr("Untitled") : QString::fromStdU16String(m_doc->doc.path.filename().u16string());
}

// ---------------------------------------------------------------- the chip
AgentChip* AgentBridge::chip() {
  if (!m_chip) {
    m_chip = new AgentChip(m_window);
    connect(m_chip, &AgentChip::clicked, this, [this](const QPoint& at) {
      if (!m_enabled && !m_everConnected && !m_listenFailed) settings(-1);  // never set up: straight to Connect
      else chipMenu(at);
    });
    refreshUi();
  }
  return m_chip;
}

AgentChip::State AgentBridge::chipState() const {
  AgentChip::State st;
  const QString file = fileLabel();
  if (!m_enabled) {
    st.label = &Tokens::fg2;
    if (m_listenFailed) {
      st.text = tr("AI off");
      st.mark = &Tokens::fg3;
      st.tip = tr("AI access couldn't start. Open AI assistants for details.");
    } else if (!m_everConnected) {
      st.text = tr("Connect AI");
      st.label = &Tokens::fg;
      st.tip = tr("Let an AI assistant such as Claude Code, Claude Desktop or Codex work on your designs.");
    } else {
      st.text = tr("AI off");
      st.mark = &Tokens::fg3;
      st.tip = tr("AI assistants can't see your designs. Click to turn access on.");
    }
    return st;
  }
  const int n = boundCount();
  if (n == 0) {
    if (m_leftAt.isValid() && m_leftAt.secsTo(QDateTime::currentDateTime()) < 60) {
      st.text = tr("%1 left").arg(m_leftName);
      st.mark = &Tokens::fg3;
      st.label = &Tokens::fg2;
      st.tip = tr("%1 disconnected at %2.").arg(m_leftName, m_leftAt.toString(QStringLiteral("HH:mm")));
    } else {
      st.text = tr("AI ready");
      st.tip = tr("AI access is on. In Claude Code, Claude Desktop or Codex, ask it to open \"%1\" in OPAD.").arg(file);
    }
    return st;
  }
  const auto name = connectedName();
  st.mark = &Tokens::hov;
  const bool human = m_doc->annotationEditing || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane();
  if (m_needs) {
    st.text = tr("%1 needs you").arg(name.brief);
    st.glyph = QStringLiteral("warning");
    st.glyphFirst = true;
    st.mark = &Tokens::amber;
    st.label = &Tokens::amber;
    if (m_feed)
      if (auto* e = m_feed->event(m_needs)) st.tip = agentsay::plain(e->text);
    return st;
  }
  if (human && m_waiting.isValid() && m_waiting.elapsed() < 60000) {
    st.text = tr("%1 · waiting for you").arg(name.brief);
    st.mark = &Tokens::amber;
    st.label = &Tokens::amber;
    st.tip = m_design->sketchActive() ? tr("%1 will continue when you finish the sketch.").arg(name.brief) : tr("%1 will continue when you finish editing.").arg(name.brief);
    return st;
  }
  if (m_prepared) {
    st.text = tr("%1 · preview").arg(name.brief);
    st.tip = tr("%1 is showing a preview. Your design hasn't changed yet.").arg(name.brief);
    return st;
  }
  if (m_running && m_working.isValid() && m_working.elapsed() >= 400) {
    st.text = tr("%1 · working").arg(name.brief);
    if (m_feed)
      if (auto* e = m_feed->event(m_running)) st.tip = agentsay::plain(e->text);
    return st;
  }
  if (m_paused) {
    st.text = tr("%1 · paused").arg(name.brief);
    st.glyph = QStringLiteral("eye");
    st.tip = tr("Changes are paused. %1 can still look. Resume from this menu.").arg(name.brief);
    return st;
  }
  st.text = n > 1 ? tr("%1 +%2").arg(name.brief).arg(n - 1) : name.brief;
  if (!m_edit) {
    st.glyph = QStringLiteral("eye");
    st.tip = tr("%1 is connected to %2 and can look but not change anything.").arg(name.full, file);
  } else {
    st.tip = tr("%1 is connected to %2 and can look and make changes.").arg(name.full, file);
  }
  if (m_unseen > 0 && (!m_panel || !m_panel->isVisible())) {
    st.count = m_unseen;
    st.tip = m_unseen == 1 ? tr("1 change by %1 since you last looked").arg(name.brief) : tr("%1 changes by %2 since you last looked").arg(m_unseen).arg(name.brief);
  }
  if (n > 1) {
    QStringList lines;
    for (const auto& s : m_sessions)
      if (s->bound && s->socket && s->socket->state() == QLocalSocket::ConnectedState)
        lines << (m_busy && s->socket == m_owner ? tr("%1 · working").arg(who(s).full) : tr("%1 · connected").arg(who(s).full));
    st.tip = lines.join(QLatin1Char('\n'));
  }
  return st;
}

void AgentBridge::accessMenu(QMenu& menu) {
  auto* group = new QActionGroup(&menu);
  const auto add = [&](const QString& text, bool on, bool edit, bool checked) {
    auto* a = menu.addAction(text);
    a->setCheckable(true);
    a->setChecked(checked);
    group->addAction(a);
    connect(a, &QAction::triggered, this, [this, on, edit] { setAccess(on, edit); });
  };
  add(tr("Off"), false, false, !m_enabled);
  add(tr("Look only"), true, false, m_enabled && !m_edit);
  add(tr("Look and edit"), true, true, m_enabled && m_edit);
}

void AgentBridge::chipMenu(const QPoint& global) {
  QMenu menu(m_window);
  const int n = boundCount();
  const auto name = connectedName();
  menu.addAction(n ? tr("%1 · %2").arg(name.full, fileLabel()) : tr("No AI assistant connected"))->setEnabled(false);
  menu.addAction(m_panel && m_panel->isVisible() ? tr("Hide AI activity") : tr("Show AI activity"), this, &AgentBridge::toggleActivity);
  if (m_running || m_prepared || m_busy) menu.addAction(n ? tr("Stop %1").arg(name.brief) : tr("Stop AI work"), this, &AgentBridge::userStop);
  menu.addSeparator();
  accessMenu(menu);
  if (n && m_edit && m_enabled) menu.addAction(m_paused ? tr("Resume changes") : tr("Pause changes"), this, [this] { setPaused(!m_paused); });
  auto* follow = menu.addAction(tr("Zoom to its changes"));
  follow->setCheckable(true);
  follow->setChecked(m_follow);
  connect(follow, &QAction::toggled, this, [this](bool on) {
    m_follow = on;
    QSettings().setValue("agent/follow", on);
    if (m_feed) m_feed->setFollow(on);
  });
  menu.addSeparator();
  if (n) menu.addAction(n > 1 ? tr("Disconnect all") : tr("Disconnect %1").arg(name.brief), this, &AgentBridge::disconnectClients);
  menu.addAction(tr("AI assistants…"), this, [this] { settings(-1); });
  const QSize size = menu.sizeHint();
  menu.exec(QPoint(global.x(), global.y() - size.height() - 2));
}

void AgentBridge::stripMenu(const QPoint& global) {
  QMenu menu(m_window);
  accessMenu(menu);
  const int n = boundCount();
  const auto name = connectedName();
  if (n && m_edit && m_enabled) menu.addAction(m_paused ? tr("Resume changes") : tr("Pause changes"), this, [this] { setPaused(!m_paused); });
  if (n) menu.addAction(n > 1 ? tr("Disconnect all") : tr("Disconnect %1").arg(name.brief), this, &AgentBridge::disconnectClients);
  menu.addSeparator();
  menu.addAction(tr("Clear activity"), this, [this] {
    if (!m_feed) return;
    m_feed->clear();
    m_fold = m_group = m_staged = m_needs = m_running = 0;
    refreshUi();
  });
  menu.addAction(tr("Copy activity as text"), this, [this] { QGuiApplication::clipboard()->setText(feedLines().join(QLatin1Char('\n'))); });
  menu.addAction(tr("Show technical log"), this, [this] {
    auto* dialog = new QDialog(m_window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("AI technical log"));
    dialog->resize(640, 420);
    auto* l = new QVBoxLayout(dialog);
    auto* text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setObjectName(QStringLiteral("mono"));
    text->setPlainText(m_raw.join(QLatin1Char('\n')));
    l->addWidget(text);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    l->addWidget(close);
    dialog->show();
  });
  menu.addAction(tr("AI assistants…"), this, [this] { settings(-1); });
  menu.exec(global);
}

// ---------------------------------------------------------------- the panel
void AgentBridge::ensurePanel() {
  if (m_panel) return;
  m_feed = new ActivityPanel;
  m_panel = new ToolPanel("agent-activity", "aiMark", &Tokens::hov, tr("AI activity"), m_feed, 360, m_window);
  m_panel->setPersistent(true);
  m_panel->setHelpId("tools.agentActivity");
  if (m_services) m_services->addPanel(m_panel);
  m_feed->setFollow(m_follow);
  connect(m_feed, &ActivityPanel::stopRequested, this, &AgentBridge::userStop);
  connect(m_feed, &ActivityPanel::followToggled, this, [this](bool on) {
    m_follow = on;
    QSettings().setValue("agent/follow", on);
  });
  connect(m_feed, &ActivityPanel::hovered, this, &AgentBridge::hoverRow);
  connect(m_feed, &ActivityPanel::activated, this, &AgentBridge::rowAction);
  connect(m_feed, &ActivityPanel::stripMenuRequested, this, &AgentBridge::stripMenu);
  connect(m_feed, &ActivityPanel::accessMenuRequested, this, [this](const QPoint& at) {
    QMenu menu(m_window);
    accessMenu(menu);
    menu.exec(at);
  });
  connect(m_feed, &ActivityPanel::stripLinkActivated, this, [this] {
    if (m_paused) setPaused(false);
    else if (m_doc->browse && m_services) m_services->requireEditable();
  });
  connect(m_feed, &ActivityPanel::emptyActionRequested, this, [this] { settings(0); });
  refreshUi();
}

void AgentBridge::showActivity() {
  ensurePanel();
  m_unseen = 0;
  if (m_services) m_services->openPanel(m_panel);
  else {
    m_panel->anchorTo(QRect(m_viewport->mapToGlobal(QPoint()), m_viewport->size()));
    m_panel->show();
  }
  m_panel->raise();
  refreshUi();
  QTimer::singleShot(0, m_feed, &ActivityPanel::scrollToEnd);
}

void AgentBridge::toggleActivity() {
  if (m_panel && m_panel->isVisible()) {
    m_panel->hide();
    hoverRow(0);
    refreshUi();
  } else {
    showActivity();
  }
}

quint64 AgentBridge::post(ActivityEvent e) {
  ensurePanel();
  const bool fresh = e.id == 0 || !m_feed->event(e.id);
  if (e.id == 0) e.id = ++m_eventSeq;
  if (fresh && e.kind != ActivityEvent::Reads) m_fold = 0;
  if (fresh && e.client.isEmpty()) e.client = connectedName().brief;
  m_feed->post(e);
  return e.id;
}

ActivityEvent* AgentBridge::row(quint64 id) { return m_feed && id ? m_feed->event(id) : nullptr; }

void AgentBridge::addRead(const QString& text, const QString& name) {
  if (m_fold && !m_feed->events().empty() && m_feed->events().back().id == m_fold)
    if (auto* f = row(m_fold)) {
      ++f->reads;
      if (!f->children.contains(text)) f->children << text;
      f->at = QDateTime::currentDateTime();
      m_feed->post(*f);
      return;
    }
  ActivityEvent e;
  e.kind = ActivityEvent::Reads;
  e.icon = QStringLiteral("eye");
  e.text = tr("Looked over the model");
  e.reads = 1;
  e.children << text;
  e.client = name;
  e.canShow = false;
  const auto id = post(e);
  m_fold = id;
}

void AgentBridge::divider(const QString& text) {
  ActivityEvent e;
  e.kind = ActivityEvent::Divider;
  e.text = text;
  e.canShow = false;
  post(e);
}

void AgentBridge::needsYou(const QString& headline, const QString& second, const QString& action, const QString& actionText, const QString& details) {
  ActivityEvent e;
  if (auto* old = row(m_needs)) e = *old;
  e.kind = ActivityEvent::NeedsYou;
  e.text = headline;
  e.second = second;
  e.action = action;
  e.actionText = actionText;
  e.details = details;
  e.canShow = false;
  e.at = QDateTime::currentDateTime();
  m_needs = post(e);
  // A question toast only while the panel is closed; one at a time.
  if ((!m_panel || !m_panel->isVisible()) && m_services && !m_needsToast)
    if (auto* stack = m_services->toasts()) {
      const auto id = m_needs;
      QList<Toast::Action> answers;
      if (!actionText.isEmpty()) answers.append({actionText, [this, id, action] { rowAction(id, action); }});
      answers.append({actionText.isEmpty() ? tr("OK") : tr("Not now"), [this, id] { rowAction(id, QStringLiteral("notNow")); }});
      m_needsToast = stack->ask(headline, second, answers, 0);
    }
  emit statusChanged();
}

void AgentBridge::failed(const std::string& code, const QString& message) {
  const bool human = m_doc->annotationEditing || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane();
  // The running call is the one that failed only once it stopped running (dispatch refusals meanwhile are other calls).
  const quint64 running = m_busy ? 0 : m_running;
  const auto dropRunning = [&] {
    if (running && m_feed) m_feed->remove(running);
    if (running) m_running = 0;
  };
  if (code == "edit_session_busy" && human) {  // no row: the chip and the strip say it waits for the user
    m_waiting.start();
    dropRunning();
    emit statusChanged();
    return;
  }
  if (agentsay::retryable(code)) {
    auto& times = m_retries[code];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    times.push_back(now);
    std::erase_if(times, [now](qint64 t) { return now - t > 20000; });
    if (running && times.size() >= 3 && code != "cancelled") {
      if (auto* r = row(running)) {
        r->second = tr("Retrying…");
        m_feed->post(*r);
      }
      m_running = 0;
    } else {
      dropRunning();
    }
    return;
  }
  const auto names = connectedName();
  const QString name = names.brief.isEmpty() ? tr("The AI assistant") : names.brief;
  QString doing = m_call.running;
  if (doing.isEmpty() && !m_call.name.empty() && m_doc->hasDocument) doing = agentsay::running(m_call.name, m_call.args, m_doc->scene, units::current());
  if (code == "read_only") {
    dropRunning();
    QString tried = agentsay::plain(doing);
    if (tried.endsWith(QChar(0x2026))) tried.chop(1);
    needsYou(tr("%1 can look but not change anything").arg(name), tried.isEmpty() ? tr("It tried to make a change.") : tr("It tried this: %1.").arg(tried),
             QStringLiteral("allowEdit"), tr("Allow changes"), message);
    return;
  }
  const QString text = agentsay::failure(code, message, doing);
  if (auto* r = row(running)) {
    r->kind = ActivityEvent::Quiet;
    r->text = text;
    r->second.clear();
    r->details = message;
    r->state = ActivityEvent::Done;
    r->canShow = false;
    m_feed->post(*r);
    m_running = 0;
  } else {
    ActivityEvent e;
    e.kind = ActivityEvent::Quiet;
    e.text = text;
    e.details = message;
    e.canShow = false;
    post(e);
  }
}

void AgentBridge::rowAction(quint64 id, const QString& action) {
  auto* found = row(id);
  if (!found) return;
  const ActivityEvent e = *found;
  if (action == QLatin1String("show")) {
    std::vector<std::string> here;
    for (const auto& s : e.subjects)
      if (m_doc->scene.node(s)) here.push_back(s);
    if (!here.empty()) m_viewport->fitNodes(here, true);
  } else if (action == QLatin1String("select")) {
    std::vector<opad::Ref> refs;
    for (const auto& s : e.subjects)
      if (m_doc->scene.node(s)) {
        opad::Ref r;
        r.body = s;
        refs.push_back(r);
      }
    if (m_services && !refs.empty() && e.canShow) m_services->select(refs);
  } else if (action == QLatin1String("undo")) {
    if (m_doc->undoTopHas(e.op)) m_doc->undo();
  } else if (action == QLatin1String("timeline")) {
    if (m_services && !e.op.empty()) m_services->findInTimeline(e.op);
  } else if (action == QLatin1String("copy")) {
    QStringList lines{agentsay::plain(e.text)};
    if (!e.second.isEmpty()) lines << agentsay::plain(e.second);
    for (const auto& c : e.children) lines << QStringLiteral("  ") + agentsay::plain(c);
    if (!e.details.isEmpty()) lines << e.details;
    lines << e.at.toString(Qt::ISODate) + (e.ms >= 0 ? QStringLiteral(" · %1 ms").arg(e.ms) : QString());
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
  } else if (action == QLatin1String("dismiss") || action == QLatin1String("notNow")) {
    m_feed->remove(id);
    if (id == m_needs) {
      m_needs = 0;
      if (m_needsToast) m_needsToast->dismiss();
    }
    emit statusChanged();
  } else if (action == QLatin1String("discard")) {
    if (m_staged == id && m_prepared) {
      m_dropReason = tr("Discarded by you. Nothing changed.");
      clearPrepared();
      emit statusChanged();
    }
  } else if (action == QLatin1String("allowEdit") || action == QLatin1String("save")) {
    m_feed->remove(id);
    if (id == m_needs) m_needs = 0;
    if (m_needsToast) m_needsToast->dismiss();
    if (action == QLatin1String("allowEdit")) allowChanges();
    else if (m_services)
      if (auto* save = m_services->action("file.save")) save->trigger();
    emit statusChanged();
  }
}

void AgentBridge::hoverRow(quint64 id) {
  if (id == m_hoverId) return;
  m_hoverId = id;
  std::vector<opad::Ref> refs;
  const auto* e = row(id);
  if (e && e->canShow)
    for (const auto& s : e->subjects)
      if (m_doc->scene.node(s)) {
        opad::Ref r;
        r.body = s;
        refs.push_back(r);
      }
  if (!refs.empty()) {
    m_viewport->showCandidateRefs(refs);
    m_hoverTint = true;
    if (m_services && !e->op.empty())
      if (auto* timeline = m_services->timeline()) timeline->pulse(e->op);
  } else if (m_hoverTint) {
    m_viewport->showCandidateRefs({});
    m_hoverTint = false;
  }
}

void AgentBridge::allowChanges() {
  const bool wasEnabled = m_enabled, wasEdit = m_edit, wasPaused = m_paused;
  m_paused = false;
  setAccess(true, true);
  const auto names = connectedName();
  const QString name = names.brief.isEmpty() ? tr("AI assistants") : names.brief;
  ActivityEvent e;
  e.kind = ActivityEvent::Result;
  e.icon = QStringLiteral("check");
  e.text = tr("You allowed %1 to change your designs").arg(name);
  e.canShow = false;
  post(e);
  if (m_services)
    m_services->toast(tr("%1 can now change your designs. Change this from the AI chip.").arg(name), tr("Undo"), [this, wasEnabled, wasEdit, wasPaused] {
      setAccess(wasEnabled, wasEdit);
      m_paused = wasPaused;
      publish();
    }, 6000);
}

void AgentBridge::userStop() {
  const auto names = connectedName();
  const QString name = names.brief.isEmpty() ? tr("the AI assistant") : names.brief;
  if (m_prepared) m_dropReason = tr("Dropped: you stopped %1. Nothing changed.").arg(name);
  stop();
  divider(tr("You stopped %1 · %2").arg(name, QTime::currentTime().toString(QStringLiteral("HH:mm"))));
  if (m_services) m_services->toast(tr("Stopped %1. Changes it already made stay; Undo removes them.").arg(name), QString(), {}, 4000);
}

void AgentBridge::setPaused(bool on) {
  if (m_paused == on) return;
  if (on && m_prepared) m_dropReason = tr("Dropped when you paused changes. Nothing changed.");
  if (on) stop();
  m_paused = on;
  ActivityEvent e;
  e.kind = ActivityEvent::Result;
  e.icon = on ? QStringLiteral("eye") : QStringLiteral("check");
  e.text = on ? tr("You paused changes") : tr("You resumed changes");
  e.canShow = false;
  post(e);
  publish();
}

void AgentBridge::refreshUi() {
  if (m_chip) m_chip->setState(chipState());
  if (!m_feed) return;
  const int n = boundCount();
  const auto name = connectedName();
  const bool human = m_doc->annotationEditing || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane();
  ActivityPanel::Strip st;
  if (n > 0) {
    st.shown = true;
    if (m_doc->browse) {
      st.line1 = tr("%1 is open for viewing").arg(fileLabel());
      st.line2 = tr("%1 can look but not change it.").arg(name.brief);
      st.link = tr("Save as OPAD to edit");
    } else if (human && m_waiting.isValid() && m_waiting.elapsed() < 60000) {
      st.amber = true;
      st.line1 = m_design->sketchActive() ? tr("Waiting for you to finish the sketch") : tr("Waiting for you to finish editing");
      st.line2 = tr("%1 will continue after that.").arg(name.brief);
    } else if (m_prepared) {
      st.line1 = tr("%1 is showing a preview").arg(name.brief);
      st.line2 = tr("Not applied yet. Your design hasn't changed.");
    } else if (m_running) {
      st.line1 = tr("%1 is working").arg(name.brief);
      if (auto* e = row(m_running)) st.line2 = agentsay::plain(e->text);
    } else if (m_paused) {
      st.line1 = tr("Changes paused");
      st.line2 = tr("%1 can still look.").arg(name.brief);
      st.link = tr("Resume");
    } else if (n > 1) {
      st.line1 = tr("%1 assistants connected").arg(n);
      QStringList names;
      for (const auto& s : m_sessions)
        if (s->bound && s->socket && s->socket->state() == QLocalSocket::ConnectedState) names << who(s).brief;
      st.line2 = names.join(tr(", "));
    } else {
      st.line1 = tr("%1 is connected").arg(name.brief);
      st.pill = m_edit ? tr("Can edit") : tr("Look only");
      st.line2 = fileLabel();
    }
  }
  m_feed->setStrip(st);
  ActivityPanel::Empty em;
  if (!m_enabled) {
    em.title = tr("Work with an AI assistant");
    em.text = tr("Claude Code, Claude Desktop or Codex can look at your design and make changes you can undo.");
    em.button = tr("Connect an AI assistant…");
  } else if (n == 0) {
    em.title = tr("Waiting for an AI assistant");
    em.text = tr("In your AI app, ask:");
    em.prompt = tr("Open \"%1\" in OPAD and tell me what you see.").arg(fileLabel());
  } else {
    em.title = tr("%1 is connected to %2").arg(name.brief, fileLabel());
    em.text = tr("Ask %1 something. What it does shows up here in plain words.").arg(name.brief);
    em.example = tr("Try: Add 2 mm fillets to the outer edges of your part.");
  }
  m_feed->setEmpty(em);
  m_feed->setBusy(m_running || m_prepared || m_busy);
  m_feed->setMultiple(n > 1);
  m_panel->setContext(n > 1 ? tr("%1 assistants").arg(n) : n == 1 ? tr("%1 · %2").arg(name.brief, fileLabel()) : tr("Not connected"));
}

void AgentBridge::refreshRows() {
  if (!m_feed || m_feed->events().empty()) return;
  std::unordered_set<std::string> wanted, there;
  for (const auto& e : m_feed->events())
    if (!e.stale && !e.op.empty()) wanted.insert(e.op);
  if (wanted.empty()) return;
  for (const auto& op : m_doc->doc.ops)
    if (wanted.count(op.id)) there.insert(op.id);
  for (auto e : m_feed->events()) {
    if (e.stale || e.op.empty()) continue;
    if (e.kind != ActivityEvent::Change && e.kind != ActivityEvent::Group && e.kind != ActivityEvent::Note && e.kind != ActivityEvent::Result) continue;
    if (e.kind == ActivityEvent::Group && e.state != ActivityEvent::Applied && e.state != ActivityEvent::Undone && e.state != ActivityEvent::Gone) continue;
    const bool here = there.count(e.op) > 0;
    const bool undo = here && m_doc->undoTopHas(e.op);
    auto state = e.state;
    QString tag = e.tag;
    if (!here) {
      if (m_doc->redoHas(e.op)) {
        state = ActivityEvent::Undone;
        tag = tr("Undone");
      } else {
        state = ActivityEvent::Gone;
        tag = tr("No longer in the design");
      }
    } else if (state == ActivityEvent::Undone || state == ActivityEvent::Gone) {
      state = ActivityEvent::Applied;
      tag.clear();
    }
    if (undo != e.canUndo || state != e.state || tag != e.tag) {
      e.canUndo = undo;
      e.state = state;
      e.tag = tag;
      m_feed->post(e);
    }
  }
}
