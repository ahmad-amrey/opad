// Live agents' git tools (git_status ... git_push, GitAgent.hpp) on the bound document's repository. git runs on a
// worker (an agent's write shows in the status strip with Cancel, as the panel's commands do); reads need agent access,
// writes edit permission and a request_id (request_status finds their receipt). What rewrites the file (a switch, a
// merge, a pull, a resolve, an abort) refuses while the document has unsaved changes, a transaction or preview is
// staged, or a sketch or feature is open; afterwards gitChanged lets the Version control area refresh the chip and the
// panel and take the file in (DiskSync::adopt). Each action is a line in Agent activity.
#include <QFileInfo>
#include <QLocalSocket>

#include "AgentBridge.hpp"
#include "GitAgent.hpp"
#include "opad/live.hpp"

using opad::json;
using namespace opad::agent;

namespace {
QString field(const json& v, const char* key) { return v.contains(key) && v[key].is_string() ? QString::fromStdString(v[key].get<std::string>()) : QString(); }
}  // namespace

QStringList AgentBridge::activityLines() const {
  QStringList lines;
  if (m_activity)
    for (int i = 0; i < m_activity->count(); ++i) lines << m_activity->item(i)->text();
  return lines;
}

void AgentBridge::gitTool(const std::shared_ptr<Session>& s, const std::string& name, json args, const std::string& receipt, const std::string& hash) {
  const bool write = gitagent::writes(name), files = gitagent::changesFiles(name);
  auto refuse = [this, s](const std::string& code, const QString& message, const std::string& receiptKey = {}, const std::string& next = {}) {
    if (!receiptKey.empty()) m_receipts[receiptKey].state = "failed";
    auto error = live_error(code, message.toStdString());
    error["structuredContent"]["state"] = "failed";
    error["structuredContent"]["error"]["next"] = next.empty() ? json("git_status") : json(next);
    activity(tr("Agent git: %1").arg(message));
    reply(s, std::move(error), receiptKey);
  };
  if (write) {
    if (auto found = m_receipts.find(receipt); found != m_receipts.end()) {
      if (found->second.hash != hash) return fail(s, "request_id_reused", tr("This request ID already belongs to different arguments."));
      return replyReceipt(s, found->second);
    }
    if (!m_edit) return fail(s, "read_only", tr("Agent access is view-only. Git commands that change the repository need editing permission (Settings > AI integration)."));
    if (m_busy) return fail(s, "busy", tr("An agent operation is still running. Wait or use Stop."));
    if (files && editorBusy()) return fail(s, "edit_session_busy", tr("Finish the active sketch or feature operation before agent edits."));
    if (files && m_prepared) return fail(s, "prepared_active", tr("Commit or cancel the current transaction first."));
    if (m_receipts.size() >= 10000) return fail(s, "session_limit", tr("This application session has reached its request limit. Save and restart OPAD."));
  }
  if (!m_doc->hasDocument || m_doc->browse || m_doc->doc.path.empty())
    return refuse("no_repository", tr("The open document is not an .opad file on disk yet: save it (save, with a path inside a git repository) first."), {}, "save");
  gitagent::Call call;
  call.repo = call.document = m_doc->path();
  call.documentDirty = m_doc->isDirty();
  call.policy = gitagent::Policy::read();
  if (name == "git_init") call.repo = args.contains("folder") ? QString::fromStdString(args["folder"].get<std::string>()) : QFileInfo(m_doc->path()).absolutePath();
  args.erase("request_id");
  args.erase("folder");
  if (write) {
    m_receipts.emplace(receipt, Receipt{hash});
    m_busy = true;
    m_owner = s->socket;
  }
  activity(tr("Agent: %1").arg(QString::fromStdString(name)));
  struct Out {
    json result;
    std::string code, message, next;
  };
  auto out = std::make_shared<Out>();
  const auto epoch = m_epoch;
  auto started = std::make_shared<QElapsedTimer>();
  started->start();
  if (!write) m_jobs->backgroundNext();  // a read: no busy cursor
  auto* job = m_jobs->async(tr("Agent: %1").arg(QString::fromStdString(name)), [out, name, args, call](Progress p) mutable {
    call.cancelled = [p] { return p.cancelled(); };
    call.progress = [p](const QString& phase) { p.setPhase(phase); };
    try {
      out->result = gitagent::run(name, args, call);
    } catch (const gitagent::Refused& e) {
      out->code = e.code;
      out->message = e.what();
      out->next = e.next;
    } catch (const std::exception& e) {
      out->code = "git_failed";
      out->message = e.what();
    }
  }, [this, s, out, name, write, files, receipt, epoch, started, refuse](bool ok, const QString& error) {
    if (write && epoch == m_epoch) {
      m_busy = false;
      m_owner.clear();
      m_job.clear();
    }
    if (!ok && out->code.empty()) {
      out->code = error == "cancelled" ? "cancelled" : "git_failed";
      out->message = error.toStdString();
    }
    if (!out->code.empty()) {
      refuse(out->code, QString::fromStdString(out->message), write ? receipt : std::string(), out->next);
      if (write) emit gitChanged(files);  // a refused or failed command may still have run git (a stopped merge)
      emit statusChanged();
      return;
    }
    const json& r = out->result;
    if (write) {
      m_receipts[receipt].state = "committed";
      m_receipts[receipt].revision = m_doc->revision;
    }
    reply(s, live_result({{"state", write ? "committed" : "read"}, {"revision", m_doc->revision}, {"result", r}, {"elapsed_ms", started->elapsed()}}), write ? receipt : std::string());
    const QString state = field(r, "state"), branch = field(r, "branch");
    QString line;
    if (name == "git_commit") line = tr("Agent committed %1 on %2: %3").arg(field(r, "short"), branch, field(r, "subject"));
    else if (name == "git_branch_create") line = r.value("switched", false) ? tr("Agent created the branch %1 and switched to it").arg(field(r, "created")) : tr("Agent created the branch %1").arg(field(r, "created"));
    else if (name == "git_switch") line = tr("Agent switched to %1").arg(branch);
    else if (name == "git_merge" || name == "git_pull") {
      const QString source = field(r, "source").isEmpty() ? field(r, "upstream") : field(r, "source");
      if (state == "preview") line = tr("Agent previewed %1").arg(source);
      else if (state == "conflicts") line = tr("Agent's merge of %1 stopped on conflicts: %2").arg(source, QString::fromStdString(r.value("conflicts", json::array()).dump()));
      else if (state == "up_to_date") line = tr("%1 is up to date").arg(branch);
      else line = tr("Agent merged %1 into %2").arg(source, branch);
    } else if (name == "git_merge_abort") line = tr("Agent aborted the merge");
    else if (name == "git_resolve") line = state == "resolved" ? tr("Agent resolved the conflicts in %1").arg(field(r, "path")) : tr("Agent listed the conflicts in %1").arg(field(r, "path"));
    else if (name == "git_push") line = tr("Agent pushed %1 to %2").arg(branch, field(r, "remote"));
    else if (name == "git_fetch") line = tr("Agent fetched");
    else if (name == "git_init") line = tr("Agent made %1 a git repository (branch %2)").arg(field(r, "repo"), branch);
    else if (name == "git_tag") line = tr("Agent tagged %1 as %2").arg(field(r, "target").left(7), field(r, "tag"));
    else line = tr("Completed: %1 (%2 ms)").arg(QString::fromStdString(name)).arg(started->elapsed());
    activity(line);
    trace::log(QStringLiteral("agent: %1 %2 ms").arg(QString::fromStdString(name)).arg(started->elapsed()));
    if (write && state != "preview") emit gitChanged(files);
    emit statusChanged();
  });
  if (write) m_job = job;
}
