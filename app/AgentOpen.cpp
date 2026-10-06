// Live agents' open_document / new_document: a file opened in the bound window as File > Open does (an .opad, or a STEP,
// a drawing, ... in viewer mode), or a new empty .opad made at a path and opened, so an agent can start without a person
// or the CLI. As safe as the window: unsaved changes are never discarded (refused, the agent saves or asks the user), an
// open sketch or feature, a staged transaction or a running agent operation refuse too. The window opens it
// (openRequested -> MainWindow::openPath); once loaded, this connection is bound to the new document, the others are
// not (they bind again, as after any document change).
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>

#include "AgentBridge.hpp"
#include "opad/commands.hpp"
#include "opad/live.hpp"

using opad::json;
using namespace opad::agent;

// The open document's unsaved changes are never discarded by an agent: refused, with save as the next step (or the user's
// answer). True when it refused.
bool AgentBridge::unsaved(const std::shared_ptr<Session>& s) {
  if (!m_doc->hasDocument || !m_doc->isDirty()) return false;
  const QString name = m_doc->path().isEmpty() ? tr("The open document") : QFileInfo(m_doc->path()).fileName();
  auto error = live_error("unsaved_changes", tr("%1 has unsaved changes, and opening another document would discard them: nothing was changed. Save it first (save), or ask the user what to do with them.")
                                                 .arg(name).toStdString());
  error["structuredContent"]["state"] = "failed";
  error["structuredContent"]["error"]["next"] = json{"save"};
  activity(tr("Agent error: %1").arg(QString::fromStdString(error["structuredContent"]["error"]["message"].get<std::string>())));
  reply(s, std::move(error));
  return true;
}

void AgentBridge::openDocument(const std::shared_ptr<Session>& s, const std::string& name, const json& args, const std::string& receipt, const std::string& hash) {
  if (auto found = m_receipts.find(receipt); found != m_receipts.end()) {
    if (found->second.hash != hash) return fail(s, "request_id_reused", tr("This request ID already belongs to different arguments."));
    return replyReceipt(s, found->second);
  }
  const bool create = name == "new_document";
  if (!m_edit) return fail(s, "read_only", tr("Agent access is view-only. Opening or creating a document needs editing permission (Settings > AI integration)."));
  if (m_busy) return fail(s, "busy", tr("An agent operation is still running. Wait or use Stop."));
  if (editorBusy() || m_doc->snapshotBusy()) return fail(s, "edit_session_busy", tr("Finish the active sketch or feature operation before agent edits."));
  if (m_prepared) return fail(s, "prepared_active", tr("Commit or cancel the current transaction first."));
  const QString path = QDir::cleanPath(QString::fromStdString(args.at("path").get<std::string>()));
  const QFileInfo file(path);
  if (!file.isAbsolute()) return fail(s, "invalid_arguments", tr("path must be an absolute path."));
  if (create) {
    if (file.suffix().compare("opad", Qt::CaseInsensitive) != 0) return fail(s, "invalid_arguments", tr("A new document is an .opad file: end the path with .opad."));
    if (file.exists()) return fail(s, "file_exists", tr("%1 exists already: open_document opens it, or choose another path. Nothing was changed.").arg(QDir::toNativeSeparators(path)));
  } else if (!file.isFile()) {
    return fail(s, "not_found", tr("%1 does not exist.").arg(QDir::toNativeSeparators(path)));
  }
  if (unsaved(s)) return;
  m_receipts.emplace(receipt, Receipt{hash});
  m_busy = true;
  m_owner = s->socket;
  m_activeOperation = args.value("request_id", name);
  activity(create ? tr("Agent: new document %1").arg(QDir::toNativeSeparators(path)) : tr("Agent: open %1").arg(QDir::toNativeSeparators(path)));
  const auto epoch = ++m_epoch;
  // The window opens it; the reply comes once it is loaded, with the new target this connection is now bound to.
  auto open = [this, s, path, receipt, create, epoch] {
    if (epoch != m_epoch || !s->socket) return;
    if (m_doc->hasDocument && m_doc->isDirty()) {  // changed while the file was being made
      m_busy = false;
      m_owner.clear();
      m_receipts[receipt].state = "failed";
      return void(unsaved(s));
    }
    auto link = std::make_shared<QMetaObject::Connection>();
    *link = connect(m_doc, &AppDocument::loadFinished, this, [this, s, path, receipt, create, link](bool ok, const QString& error) {
      disconnect(*link);
      m_busy = false;
      m_owner.clear();
      if (!s->socket) return;
      if (!ok) return fail(s, "open_failed", tr("OPAD could not open %1: %2").arg(QDir::toNativeSeparators(path), error), receipt);
      s->bound = true;  // the document this connection asked for: bound to it, as live_bind would
      s->target = target();
      m_receipts[receipt].state = "committed";
      m_receipts[receipt].revision = m_doc->revision;
      const std::string again = (target() + "/").toStdString() + receipt.substr(receipt.find('/') + 1);  // request_status, bound to it
      json result = {{"path", m_doc->path().toStdString()}, {"target", target().toStdString()}, {"document", m_doc->doc.header.uuid},
                     {"title", m_doc->title().toStdString()}, {"created", create}, {"viewer", m_doc->browse}, {"read_only", m_doc->readOnly},
                     {"bodies", m_doc->scene.all_bodies().size()}, {"units", m_doc->scene.units}};
      if (m_doc->browse) result["next"] = "Viewer mode: the file is shown read-only; save it as an .opad (save with a path) to edit it.";
      reply(s, live_result({{"state", "committed"}, {"revision", m_doc->revision}, {"result", result}}), receipt);
      m_receipts[again] = m_receipts[receipt];
      const QString shown = QFileInfo(path).fileName();
      activity(create ? tr("Agent made and opened %1").arg(shown) : tr("Agent opened %1").arg(shown));
      emit statusChanged();
    });
    emit openRequested(path);
    if (!m_doc->loading) {  // the window kept the document (unfinished work in it asked to stay)
      disconnect(*link);
      m_busy = false;
      m_owner.clear();
      fail(s, "open_refused", tr("OPAD did not open %1: the window has unfinished work that keeps the current document. Ask the user.").arg(QDir::toNativeSeparators(path)), receipt);
    }
  };
  if (!create) return open();
  // A new document's file is made on a worker (opad-cli new does the same), then opened.
  auto made = std::make_shared<QString>();
  m_jobs->backgroundNext();
  m_jobs->async(tr("Agent: new document"), [path, made](Progress) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw opad::Error("could not create the folder " + QFileInfo(path).absolutePath().toStdString());
    opad::commands::run("new", {{"doc", path.toStdString()}});
  }, [this, s, receipt, epoch, open](bool ok, const QString& error) {
    if (epoch != m_epoch || !s->socket) return;
    if (!ok) {
      m_busy = false;
      m_owner.clear();
      return fail(s, "create_failed", tr("Could not create the document: %1").arg(error), receipt);
    }
    open();
  });
}
