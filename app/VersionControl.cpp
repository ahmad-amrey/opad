#include "VersionControl.hpp"

#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <filesystem>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "CompareMode.hpp"
#include "DesignController.hpp"
#include "DiskSync.hpp"
#include "GitWatch.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "ToolPanel.hpp"
#include "OpProvenance.hpp"
#include "VersionPanel.hpp"
#include "opad/diff.hpp"
#include "opad/geometry.hpp"
#include "opad/merge.hpp"

// Version control's icons: a branch, push and pull (an arrow to and from a line), a merge, history (a clock turned back).
OPAD_ICON_TABLE(version,
                {"branch", R"(<circle cx="7" cy="5" r="2"/><circle cx="7" cy="19" r="2"/><circle cx="17" cy="7" r="2"/><path d="M7 7v10M17 9c0 4-3 5-6 5.5S7 16 7 17"/>)"},
                {"push", R"(<path d="M12 17V4M7 9l5-5 5 5M5 20h14"/>)"},
                {"pull", R"(<path d="M12 4v13M7 12l5 5 5-5M5 20h14"/>)"},
                {"merge", R"(<circle cx="6" cy="5" r="2"/><circle cx="6" cy="19" r="2"/><circle cx="18" cy="12" r="2"/><path d="M6 7v10M6 7c0 3 4 5 10 5"/>)"},
                {"history", R"(<path d="M3 12a9 9 0 1 0 2.6-6.4L3 8"/><path d="M3 3v5h5"/><path d="M12 7v5l3 2"/>)"});

namespace {
constexpr int kPage = 200;                // commits read at a time
constexpr qint64 kPackKiB = 64 * 1024;    // loose objects worth packing (a big document's commit adds about its size)
std::filesystem::path fsPath(const QString& file) { return std::filesystem::path(file.toStdU16String()); }

// Freed on a thread of its own: a big document is work that scales with the model (CompareMode does the same).
void dispose(std::shared_ptr<void> value) {
  if (!value) return;
  auto* thread = QThread::create([value = std::move(value)]() mutable { value.reset(); });
  QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start(QThread::LowPriority);
}

[[noreturn]] void fail(const QString& text) { throw std::runtime_error(text.toUtf8().toStdString()); }

// What both sides of a merge changed, as words: the thing's name in `doc` (a body, a feature, a sketch, a parameter).
QString conflictText(const opad::MergeConflict& c, const opad::Scene& s) {
  QString what = QString::fromStdString(c.target.substr(0, 8));
  if (c.target.rfind("parameter:", 0) == 0) what = QString::fromStdString(c.target.substr(10));
  else if (const opad::Node* n = s.node(c.target)) what = QString::fromStdString(n->name);
  else if (const opad::Feature* f = s.feature(c.target)) what = QString::fromStdString(f->name);
  else if (const opad::SketchItem* k = s.sketch(c.target)) what = QString::fromStdString(k->name);
  return VersionControl::tr("%1: %2").arg(what, c.field == "*" ? VersionControl::tr("everything") : QString::fromStdString(c.field));
}

// What merging `in.target` into HEAD brings, on a worker: the commits, and the document as the driver would merge it.
void readIncoming(const git::Context& c, const QString& rel, VersionControl::Incoming& in, const QString& folder, const git::RunOptions& o) {
  auto phase = [&o](const QString& text) {
    if (o.cancelled && o.cancelled()) fail(VersionControl::tr("Cancelled."));
    if (o.progress) o.progress(text, -1);
  };
  in.head = git::revParse(c, "HEAD");
  in.theirs = git::revParse(c, in.target);
  if (in.theirs.isEmpty()) fail(VersionControl::tr("%1 was not found.").arg(in.label));
  const git::Result base = git::run(c, {"merge-base", "HEAD", in.theirs});
  in.base = base.ok() ? QString::fromLatin1(base.out).trimmed() : QString();
  in.commits = git::log(c, "HEAD.." + in.theirs, {}, kPage);
  if (in.commits.empty()) return;  // up to date
  in.fastForward = in.base == in.head;
  if (in.base.isEmpty()) {
    in.error = VersionControl::tr("The two branches have no commit in common.");
    return;
  }
  phase(VersionControl::tr("Reading the versions of %1").arg(QFileInfo(rel).fileName()));
  auto blob = [&](const QString& rev) {
    try {
      return git::show(c, rev, rel).toStdString();
    } catch (const std::exception&) {
      return std::string();  // not in that commit
    }
  };
  std::string baseText = blob(in.base), oursText = blob(in.head), theirsText = blob(in.theirs);
  in.documentChanged = theirsText != baseText;
  if (!in.documentChanged) return;
  if (baseText.empty() || oursText.empty() || theirsText.empty()) {  // added or deleted on a side: git merges the file as a whole
    in.summary = oursText.empty() ? VersionControl::tr("it comes in new") : theirsText.empty() ? VersionControl::tr("they delete it") : VersionControl::tr("both sides added it");
    return;
  }
  const auto origin = fsPath(QDir(c.dir).filePath(rel));
  opad::Document baseDoc = opad::Document::parse_index(baseText, origin), ours = opad::Document::parse_index(oursText, origin),
                 theirs = opad::Document::parse_index(theirsText, origin);
  in.design = opad::changes_design(baseDoc, ours) && opad::changes_design(baseDoc, theirs);
  std::string merged;
  if (oursText == baseText) merged = theirsText;  // only they changed it
  else {
    phase(VersionControl::tr("Merging %1").arg(QFileInfo(rel).fileName()));
    const opad::FileMerge m = opad::merge_files(std::move(baseText), std::move(oursText), std::move(theirsText));
    if (!m.error.empty()) {
      in.error = QString::fromStdString(m.error);
      const opad::Scene scene = opad::resolve(ours);
      for (const auto& conflict : m.conflicts) in.conflicts << conflictText(conflict, scene);
    } else {
      merged = m.text();
    }
  }
  phase(VersionControl::tr("Comparing"));
  if (merged.empty()) {  // the driver would stop: what they changed, then
    const opad::json diff = opad::semantic_diff(baseDoc, theirs);
    in.summary = QString::fromStdString(diff.value("summary", ""));
    in.changes = diff.value("changes", opad::json::array());
    return;
  }
  QDir().mkpath(folder);
  const QString file = QDir(folder).filePath(VersionControl::tr("%1 merged with %2.opad").arg(QFileInfo(rel).completeBaseName(), in.theirs.left(7)));
  QSaveFile out(file);
  if (!out.open(QIODevice::WriteOnly) || out.write(merged.data(), qint64(merged.size())) != qint64(merged.size()) || !out.commit())
    fail(VersionControl::tr("Could not write %1: %2").arg(QDir::toNativeSeparators(file), out.errorString()));
  in.merged = file;
  const opad::Document after = opad::Document::parse_index(std::move(merged), origin);
  const opad::json diff = opad::semantic_diff(ours, after);
  in.summary = QString::fromStdString(diff.value("summary", ""));
  in.changes = diff.value("changes", opad::json::array());
}
}  // namespace

VersionControl::VersionControl(AreaServices& services, GitWatch* git, CompareMode* compare, DiskSync* disk)
    : QObject(services.window()), m_services(services), m_git(git), m_compare(compare), m_disk(disk),
      m_provenance(new OpProvenance(git, services.jobs(), this)) {
  m_reload.setSingleShot(true);
  m_reload.setInterval(150);
  connect(&m_reload, &QTimer::timeout, this, &VersionControl::reload);
  connect(git, &GitWatch::changed, this, [this] {
    const QString listed = m_git->repo().top + '|' + documentPath();
    if (listed != m_listed) {  // another repository or document: the old lists are no use
      ++m_generation;
      m_history.clear();
      m_branches.clear();
      m_remotes.clear();
      m_moreHistory = false;
      m_listed = listed;
      m_filterLabel.clear();
      m_filterIds.clear();
      m_filtered.clear();
      if (m_panel) m_panel->showLists();
    } else if (!m_filterLabel.isEmpty() && m_git->repo().status.oid != m_filterHead) {
      applyFilter();  // HEAD moved: what touched it may have grown
    }
    if (m_panel) m_panel->showState();
    scheduleReload();
    m_services.updateCommands();
  });
  connect(services.document(), &AppDocument::changed, this, [this] {
    if (m_panel && m_tool->isVisible()) m_panel->showState();
  });
  git->setMenuExtension([this](QMenu* m) { extendMenu(m); });
  if (const int minutes = QSettings().value("git/fetchMinutes", 10).toInt(); minutes > 0) {
    connect(&m_fetch, &QTimer::timeout, this, &VersionControl::backgroundFetch);
    m_fetch.start(minutes * 60000);
  }
  if (compare)  // Compare opened from the panel took its place: the panel comes back when it ends
    connect(compare, &CompareMode::activeChanged, this, [this](bool on) {
      if (!on && std::exchange(m_reopen, false)) openPanel();
    });
}

QString VersionControl::documentPath() const { return m_git->repo().state == git::Repo::State::Ready ? m_git->repo().rel : QString(); }

bool VersionControl::ready() const {
  const AppDocument* doc = m_services.document();
  return doc->hasDocument && !doc->viewOnly() && !doc->doc.path.empty() && !documentPath().isEmpty() &&
         QFileInfo(m_git->repo().file) == QFileInfo(doc->path());
}

bool VersionControl::settled() const { return !m_reading && !m_reload.isActive() && !m_running; }

QString VersionControl::versionsFolder() const { return QDir::temp().filePath(QStringLiteral("opad-versions")); }

void VersionControl::makePanel() {
  if (m_tool) return;
  QWidget* window = m_services.window();
  m_panel = new VersionPanel(this, window);
  m_tool = new ToolPanel("version", "git", &Tokens::sel, tr("Version control"), m_panel, 560, window);
  m_tool->setEscapeHandler([this] { m_tool->hide(); });
  m_services.addPanel(m_tool);
  connect(m_panel, &VersionPanel::closeRequested, m_tool, &QWidget::hide);
  connect(m_tool, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (on) scheduleReload();
  });
}

void VersionControl::openPanel(int page) {
  makePanel();
  if (page >= 0) m_page = page;
  m_panel->setPage(m_page);
  m_panel->showState();
  m_panel->showLists();
  m_services.openPanel(m_tool);
  reload();
}

void VersionControl::scheduleReload() {
  if (m_tool && m_tool->isVisible()) m_reload.start();
}

void VersionControl::reload() {
  if (!m_panel || !m_tool->isVisible()) return;
  if (!ready()) {
    m_panel->showState();
    return;
  }
  if (m_reading) {
    m_again = true;
    return;
  }
  m_reading = true;
  struct Out {
    std::vector<git::Commit> history;
    std::vector<git::Branch> branches;
    QStringList remotes;
  };
  auto out = std::make_shared<Out>();
  const git::Context c = m_git->context();
  const QString rel = documentPath();
  const int count = std::max(kPage, int(m_history.size()));  // the pages shown so far
  const unsigned generation = m_generation;
  m_services.jobs()->quiet(tr("Reading the history"), [out, c, rel, count](Progress) {
    out->history = git::log(c, {}, rel, count);
    out->branches = git::branches(c);
    out->remotes = git::remotes(c);
  }, [this, self = QPointer<VersionControl>(this), out, generation, count](bool ok, const QString& error) {
    if (!self) return;
    m_reading = false;
    if (generation == m_generation) {
      if (ok) {
        m_history = std::move(out->history);
        m_moreHistory = int(m_history.size()) == count;
        m_branches = std::move(out->branches);
        m_remotes = std::move(out->remotes);
      } else if (trace::enabled()) {
        trace::log("version: " + error);
      }
      if (m_panel) {
        m_panel->showState();
        m_panel->showLists();
      }
      emit listsChanged();
    }
    if (std::exchange(m_again, false)) reload();
  });
}

void VersionControl::moreHistory() {
  if (!ready() || m_reading || !m_moreHistory) return;
  m_reading = true;
  auto out = std::make_shared<std::vector<git::Commit>>();
  const git::Context c = m_git->context();
  const QString rel = documentPath();
  const int skip = int(m_history.size());
  const unsigned generation = m_generation;
  m_services.jobs()->quiet(tr("Reading the history"), [out, c, rel, skip](Progress) { *out = git::log(c, {}, rel, kPage, skip); },
                           [this, self = QPointer<VersionControl>(this), out, generation](bool ok, const QString&) {
                             if (!self) return;
                             m_reading = false;
                             if (generation != m_generation || !ok) return;
                             m_moreHistory = int(out->size()) == kPage;
                             m_history.insert(m_history.end(), out->begin(), out->end());
                             if (m_panel) m_panel->showLists();
                             emit listsChanged();
                           });
}

void VersionControl::showHistoryOf(const QString& label, const std::vector<std::string>& ids) {
  m_filterLabel = ids.empty() ? QString() : label;
  m_filterIds = ids;
  m_filtered.clear();
  if (!ids.empty()) openPanel(History);
  applyFilter();
}

void VersionControl::applyFilter() {
  m_filterRead = false;
  m_filterHead = m_git->repo().status.oid;
  if (m_panel) m_panel->showLists();  // "reading" meanwhile
  if (m_filterLabel.isEmpty()) return;
  m_provenance->whenReady([this, self = QPointer<VersionControl>(this), label = m_filterLabel, ids = m_filterIds] {
    if (!self || m_filterLabel != label || m_filterIds != ids) return;
    m_filtered = m_provenance->commitsTouching(ids);
    m_filterRead = true;
    if (m_panel) m_panel->showLists();
    emit listsChanged();
  });
}

void VersionControl::failed(const QString& title, const QString& text) {
  m_lastFailure = title + ": " + text;
  if (trace::enabled()) trace::log("version: " + m_lastFailure);
  auto* box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Ok, m_services.window());
  box->setObjectName("vcsFailed");
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->open();
}

void VersionControl::say(const QString& text, const QString& action, std::function<void()> fn, int ms) {
  m_said << text;
  m_services.toast(text, action, std::move(fn), m_benching ? 0 : ms);  // a bench reads them when it gets there
}

void VersionControl::done(const QString& what, bool ok, const QString& text) {
  m_lastDone = what + (ok ? " ok" : " failed");
  if (trace::enabled()) trace::log(QStringLiteral("version: %1 %2%3").arg(what, ok ? "done" : "failed", text.isEmpty() ? QString() : ": " + text));
  emit finished(what, ok);
}

void VersionControl::ask(const QString& name, const QString& title, const QString& text, const std::vector<Answer>& answers) {
  auto* box = new QMessageBox(QMessageBox::Question, title, text, QMessageBox::Cancel, m_services.window());
  box->setObjectName(name);
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->button(QMessageBox::Cancel)->setProperty("answer", "cancel");
  std::vector<std::pair<QAbstractButton*, std::function<void()>>> runs;
  for (size_t i = 0; i < answers.size(); ++i) {
    QPushButton* b = box->addButton(answers[i].text, i == 0 ? QMessageBox::AcceptRole : QMessageBox::ActionRole);
    b->setProperty("answer", answers[i].id);
    if (i == 0) box->setDefaultButton(b);
    runs.emplace_back(b, answers[i].run);
  }
  connect(box, &QMessageBox::buttonClicked, this, [runs](QAbstractButton* clicked) {
    for (const auto& [button, run] : runs)
      if (button == clicked && run) QTimer::singleShot(0, run);  // once the box has closed
  });
  box->open();
  if (const QString id = m_answers.take(name); m_benching && !id.isEmpty())  // a bench's answer, before its quiet mode closes the box
    for (QAbstractButton* b : box->buttons())
      if (b->property("answer") == id) {
        trace::log(QStringLiteral("version: %1 answered %2").arg(name, id));
        b->click();
        break;
      }
}

void VersionControl::whenClean(const QString& what, std::function<void()> then, bool commitAllowed) {
  AppDocument* doc = m_services.document();
  if (DesignController* d = m_services.design(); d && (d->sketchActive() || d->featureActive())) {
    m_services.showMessage(tr("Finish the sketch or the feature first."));
    return;
  }
  const QString name = QFileInfo(doc->path()).fileName();
  if (doc->isDirty()) {
    ask("vcsUnsaved", what, tr("%1 has unsaved changes: save them first?").arg(name), {{"save", tr("Save"), [this, doc, what, then, commitAllowed] {
          try {
            doc->saveAsync(m_services.jobs(), QString(), false, [this, self = QPointer<VersionControl>(this), what, then, commitAllowed](bool saved, const QString& why) {
              if (!self) return;
              if (!saved) return failed(tr("Could not save"), why);
              // git's view of the file after the save, then on
              auto* link = new QMetaObject::Connection;
              *link = connect(m_git, &GitWatch::changed, this, [this, link, what, then, commitAllowed] {
                disconnect(*link);
                delete link;
                whenClean(what, then, commitAllowed);
              });
              m_git->refresh();
            });
          } catch (const std::exception& e) {
            failed(tr("Could not save"), QString::fromUtf8(e.what()));
          }
        }}});
    return;
  }
  using D = git::Repo::Doc;
  const D state = m_git->repo().doc();
  if (commitAllowed && (state == D::Modified || state == D::Added)) {
    ask("vcsUncommitted", what, tr("%1 has changes that are not committed. Commit them first, or go on and let git carry them over if it can.").arg(name),
        {{"commit", tr("Commit…"), [this] { commit(); }}, {"anyway", tr("Go on"), then}});
    return;
  }
  then();
}

// ---------------------------------------------------------------- commit
void VersionControl::commit() {
  AppDocument* doc = m_services.document();
  if (!doc->hasDocument || doc->browse) return;
  if (!ready()) return openPanel();  // it says why, and offers to set up a repository
  if (DesignController* d = m_services.design(); d && (d->sketchActive() || d->featureActive())) {
    m_services.showMessage(tr("Finish the sketch or the feature first."));
    return;
  }
  const git::Repo& r = m_git->repo();
  const QString rel = r.rel, name = QFileInfo(rel).fileName();
  const bool merging = r.merging, fresh = r.status.oid == "(initial)";
  if (merging && r.status.count('u'))  // git add would take the files as they are now: one side's version, silently
    return failed(tr("Commit the merge"), tr("Files are still in conflict: resolve them first, or abort the merge."));
  auto* d = new QDialog(m_services.window());
  d->setObjectName("vcsCommit");
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowTitle(merging ? tr("Commit the merge") : tr("Commit"));
  d->setMinimumWidth(540);
  auto* col = new QVBoxLayout(d);
  auto* where = new QLabel(tr("On branch %1 of %2").arg(r.status.branch, QDir::toNativeSeparators(r.top)), d);
  where->setObjectName("secondary");
  where->setWordWrap(true);
  col->addWidget(where);
  col->addWidget(new QLabel(tr("Files"), d));
  auto* files = new QListWidget(d);
  files->setObjectName("files");
  using D = git::Repo::Doc;
  auto add = [files](const QString& path, const QString& text, bool checked) {
    auto* item = new QListWidgetItem(text, files);
    item->setData(Qt::UserRole, path);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
  };
  const D state = r.doc();
  QString docText = rel;
  if (doc->isDirty()) docText += ' ' + tr("(unsaved changes: saved first)");
  else if (state == D::Untracked) docText += ' ' + tr("(new)");
  else if (state == D::Clean) docText += ' ' + tr("(as committed)");
  add(rel, docText, doc->isDirty() || state != D::Clean || merging);
  for (const git::Entry& e : r.status.entries) {
    if (e.kind == '!' || e.path.compare(rel, Qt::CaseInsensitive) == 0) continue;
    const bool ours = e.path == ".gitattributes" || e.path == ".gitignore";
    QString what = e.kind == '?' ? tr("new") : e.kind == 'u' ? tr("in conflict") : (e.x == 'D' || e.y == 'D') ? tr("deleted") : tr("changed");
    add(e.path, QStringLiteral("%1 (%2)").arg(e.path, what), ours || merging);
  }
  files->setMaximumHeight(std::min(6, files->count()) * (files->fontMetrics().height() + 8) + 8);
  col->addWidget(files);
  if (merging) {
    auto* note = new QLabel(tr("A merge is committed as a whole: everything it staged goes in, with the files ticked here."), d);
    note->setObjectName("secondary");
    note->setWordWrap(true);
    col->addWidget(note);
  }
  col->addWidget(new QLabel(tr("Message"), d));
  auto* message = new QPlainTextEdit(d);
  message->setObjectName("message");
  message->setPlaceholderText(tr("What changed, in one line; more below it if needed"));
  message->setTabChangesFocus(true);
  message->setMinimumHeight(90);
  col->addWidget(message);
  auto* suggestion = new QLabel(tr("Reading the changes since the last commit…"), d);
  suggestion->setObjectName("suggestion");
  suggestion->setProperty("state", "reading");
  suggestion->setWordWrap(true);
  col->addWidget(suggestion);
  auto* who = new QHBoxLayout;
  auto* author = new QLabel(d);
  author->setObjectName("author");
  auto showAuthor = [this, author] {
    const git::Repo& now = m_git->repo();
    author->setText(now.userName.trimmed().isEmpty() || now.userEmail.trimmed().isEmpty() ? tr("Author: not set yet (asked when you commit)")
                                                                                          : tr("Author: %1 <%2>").arg(now.userName, now.userEmail));
  };
  showAuthor();
  connect(m_git, &GitWatch::changed, author, showAuthor);
  auto* change = new QPushButton(tr("Change…"), d);
  change->setProperty("action", "vcsAuthor");
  connect(change, &QPushButton::clicked, d, [this] { m_git->editIdentity(); });
  who->addWidget(author, 1);
  who->addWidget(change);
  col->addLayout(who);
  auto* amend = new QCheckBox(tr("Amend the last commit (it has not been pushed)"), d);
  amend->setObjectName("amend");
  amend->setEnabled(false);
  amend->setToolTip(tr("Replaces the last commit with one that holds these changes too; only while no remote has it"));
  col->addWidget(amend);
  auto* push = new QCheckBox(tr("Push after committing"), d);
  push->setObjectName("push");
  push->setChecked(QSettings().value("git/pushAfterCommit", false).toBool());
  push->setEnabled(!m_remotes.isEmpty() || !r.status.upstream.isEmpty());
  col->addWidget(push);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, d);
  auto* run = new QPushButton(merging ? tr("Commit the merge") : tr("Commit"), d);
  run->setObjectName("primary");
  run->setProperty("action", "vcsCommitRun");
  run->setDefault(true);
  run->setToolTip(tr("Ctrl+Enter"));
  buttons->addButton(run, QDialogButtonBox::AcceptRole);
  col->addWidget(buttons);
  auto valid = [files, message, run] {
    bool any = false;
    for (int i = 0; i < files->count(); ++i) any = any || files->item(i)->checkState() == Qt::Checked;
    run->setEnabled(any && !message->toPlainText().trimmed().isEmpty());
  };
  connect(files, &QListWidget::itemChanged, d, valid);
  connect(message, &QPlainTextEdit::textChanged, d, valid);
  valid();
  connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
  auto go = [this, d, files, message, amend, push, run] {
    if (!run->isEnabled()) return;
    QStringList paths;
    for (int i = 0; i < files->count(); ++i)
      if (files->item(i)->checkState() == Qt::Checked) paths << files->item(i)->data(Qt::UserRole).toString();
    const QString text = message->toPlainText().trimmed();
    const bool amending = amend->isChecked(), pushing = push->isEnabled() && push->isChecked();
    QSettings().setValue("git/pushAfterCommit", push->isChecked());
    d->accept();
    m_git->ensureIdentity([this, paths, text, amending, pushing] { runCommit(paths, text, amending, pushing); });
  };
  connect(run, &QPushButton::clicked, d, go);
  auto* key = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), d);
  connect(key, &QShortcut::activated, d, go);
  // The suggestion: the semantic diff of the last commit's document and this session's (read on workers), a merge's own
  // message, and whether the last commit can be amended.
  struct Out {
    QString summary, headMessage;
    bool pushed = true;
  };
  auto out = std::make_shared<Out>();
  const git::Context c = m_git->context();
  const QString file = doc->path(), gitDir = r.gitDir;
  const bool dirty = doc->isDirty();
  QPointer<QDialog> dialog(d);
  auto suggest = [this, dialog, out, c, rel, name, file, gitDir, merging, fresh, message, suggestion, amend](std::shared_ptr<opad::Document> session) {
    m_services.jobs()->quiet(tr("Reading the changes"), [out, c, rel, name, file, gitDir, merging, fresh, session](Progress) mutable {
      if (merging) {
        QFile m(gitDir + "/MERGE_MSG");
        if (m.open(QIODevice::ReadOnly)) {
          QStringList lines;
          for (const QString& line : QString::fromUtf8(m.readAll()).split('\n'))
            if (!line.startsWith('#')) lines << line;
          out->summary = lines.join('\n').trimmed();
        }
        return;
      }
      if (!fresh) {
        out->pushed = !git::run(c, {"branch", "-r", "--contains", "HEAD"}).out.trimmed().isEmpty();
        out->headMessage = QString::fromUtf8(git::run(c, {"log", "-1", "--format=%B"}).out).trimmed();
      }
      std::string head;
      try {
        if (!fresh) head = git::show(c, "HEAD", rel).toStdString();
      } catch (const std::exception&) {  // not committed yet
      }
      if (head.empty()) {
        out->summary = VersionControl::tr("Add %1").arg(name);
        return;
      }
      const opad::Document before = opad::Document::parse_index(std::move(head), fsPath(file));
      const opad::Document now = session ? std::move(*session) : opad::Document::load_index(fsPath(file));
      session.reset();
      out->summary = QString::fromStdString(opad::semantic_diff(before, now).value("summary", ""));
      if (out->summary.isEmpty()) out->summary = VersionControl::tr("Update %1").arg(name);
    }, [dialog, out, message, suggestion, amend, merging, fresh](bool ok, const QString& error) {
      if (!dialog) return;
      if (ok && message->toPlainText().trimmed().isEmpty()) message->setPlainText(out->summary);
      suggestion->setText(!ok ? tr("No suggestion: %1").arg(error)
                              : merging ? tr("The merge's own message.") : tr("Suggested from what changed since the last commit."));
      suggestion->setProperty("state", ok ? "suggested" : "failed");
      amend->setEnabled(ok && !merging && !fresh && !out->pushed);
      if (!amend->isEnabled()) amend->setText(tr("Amend the last commit"));
      QObject::connect(amend, &QCheckBox::toggled, message, [message, out](bool on) {  // the last commit's message to start from
        if (on && !out->headMessage.isEmpty() && !message->toPlainText().contains(out->headMessage)) message->setPlainText(out->headMessage);
      });
    });
  };
  if (dirty && !merging) {  // the session as it is now, copied on a worker
    if (!doc->captureSnapshot(m_services.jobs(), [suggest](std::shared_ptr<opad::Document> session, const QString&) { suggest(std::move(session)); }))
      suggest(nullptr);
  } else {
    suggest(nullptr);
  }
  d->open();
  message->setFocus();
}

void VersionControl::runCommit(const QStringList& files, const QString& message, bool amend, bool pushAfter) {
  AppDocument* doc = m_services.document();
  if (doc->snapshotBusy()) {  // the suggestion's copy of the session is still being taken: Save waits for it
    QTimer::singleShot(100, this, [this, files, message, amend, pushAfter] { runCommit(files, message, amend, pushAfter); });
    return;
  }
  const QString rel = documentPath();
  const bool merging = m_git->repo().merging;
  auto go = [this, files, message, amend, pushAfter, merging] {
    struct Out {
      QString hash;
      git::Objects objects;
    };
    auto out = std::make_shared<Out>();
    ++m_running;
    m_git->job(tr("Committing"), [out, files, message, amend, merging](const git::Context& c, const git::RunOptions& o) {
      git::RunOptions step = o;
      step.timeoutMs = 0;  // a big document takes a while to hash; Cancel still ends it
      if (o.progress) o.progress(tr("Adding the files"), -1);
      git::check(c, QStringList{"add", "--"} + files, step);
      if (o.progress) o.progress(tr("Writing the commit"), -1);
      QStringList args{"commit", "-q", "-F", "-"};
      if (amend) args << "--amend";
      if (!merging) args << "--" << files;
      step.input = message.toUtf8();
      git::check(c, args, step);
      out->hash = git::revParse(c, "HEAD");
      out->objects = git::countObjects(c);
    }, [this, self = QPointer<VersionControl>(this), out, message, pushAfter](bool ok, const QString& error) {
      if (!self) return;
      --m_running;
      if (!ok) {
        if (error != "cancelled") failed(tr("Could not commit"), error);
        return done("commit", false, error);
      }
      say(tr("Committed %1: %2").arg(out->hash.left(7), message.section('\n', 0, 0)));
      if (out->objects.looseKiB > kPackKiB)  // a big document adds about its size per commit until git packs it
        say(tr("The repository holds %1 MB in loose objects: packing them saves space and makes git faster.").arg(out->objects.looseKiB / 1024),
                         tr("Pack"), [this] { pack(); }, 0);
      done("commit", true, out->hash);
      if (pushAfter) push();
    });
  };
  if (doc->isDirty() && files.contains(rel)) {
    try {
      doc->saveAsync(m_services.jobs(), QString(), false, [this, self = QPointer<VersionControl>(this), go](bool saved, const QString& why) {
        if (!self) return;
        if (saved) return go();
        failed(tr("Could not save"), why);
        done("commit", false, why);
      });
    } catch (const std::exception& e) {
      failed(tr("Could not save"), QString::fromUtf8(e.what()));
      done("commit", false);
    }
    return;
  }
  go();
}

// ---------------------------------------------------------------- push, pull, fetch
void VersionControl::push() {
  if (!ready()) return openPanel();
  const git::Status& s = m_git->repo().status;
  if (s.branch == "(detached)") return failed(tr("Push"), tr("HEAD names no branch: switch to a branch, or make one here, to push."));
  if (s.oid == "(initial)") return failed(tr("Push"), tr("Nothing to push yet: commit first."));
  auto list = std::make_shared<QStringList>();
  const git::Context c = m_git->context();
  ++m_running;
  m_services.jobs()->quiet(tr("Reading the remotes"), [list, c](Progress) { *list = git::remotes(c); },
                           [this, self = QPointer<VersionControl>(this), list](bool ok, const QString& error) {
                             if (!self) return;
                             --m_running;
                             if (!ok) {
                               failed(tr("Could not push"), error);
                               return done("push", false, error);
                             }
                             m_remotes = *list;
                             if (!m_git->repo().status.upstream.isEmpty()) return runPush({}, false);
                             if (list->isEmpty()) return addRemote([this](const QString& remote) { runPush(remote, false); });
                             runPush(list->contains("origin") ? QStringLiteral("origin") : list->front(), false);
                           });
}

void VersionControl::addRemote(std::function<void(const QString&)> then) {
  auto* d = new QDialog(m_services.window());
  d->setObjectName("vcsRemote");
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowTitle(tr("Add a remote"));
  d->setMinimumWidth(500);
  auto* col = new QVBoxLayout(d);
  auto* why = new QLabel(tr("This repository has no remote yet: where should it push? An address (https://…, git@host:…) or a folder of a shared or bare repository."), d);
  why->setWordWrap(true);
  col->addWidget(why);
  auto* form = new QFormLayout;
  auto* name = new QLineEdit(QStringLiteral("origin"), d);
  name->setObjectName("name");
  auto* url = new QLineEdit(d);
  url->setObjectName("url");
  url->setPlaceholderText(QStringLiteral("https://github.com/team/project.git"));
  form->addRow(tr("Name"), name);
  form->addRow(tr("Address"), url);
  col->addLayout(form);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, d);
  auto* run = new QPushButton(tr("Add and push"), d);
  run->setObjectName("primary");
  run->setProperty("action", "vcsRemoteRun");
  run->setDefault(true);
  buttons->addButton(run, QDialogButtonBox::AcceptRole);
  col->addWidget(buttons);
  auto valid = [name, url, run] { run->setEnabled(git::validBranchName(name->text().trimmed()) && !url->text().trimmed().isEmpty()); };
  connect(name, &QLineEdit::textChanged, d, valid);
  connect(url, &QLineEdit::textChanged, d, valid);
  valid();
  connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
  connect(run, &QPushButton::clicked, d, [this, d, name, url, then] {
    const QString remote = name->text().trimmed(), address = url->text().trimmed();
    d->accept();
    ++m_running;
    m_git->command(tr("Adding the remote %1").arg(remote), {"remote", "add", remote, address}, [this, remote, then](const git::Result& r) {
      --m_running;
      if (!r.ok()) {
        failed(tr("Could not add the remote"), r.error());
        return done("push", false, r.error());
      }
      m_remotes << remote;
      then(remote);
    });
  });
  d->open();
  url->setFocus();
}

void VersionControl::runPush(const QString& remote, bool warned) {
  if (!warned) {  // big files first: a host refuses them, or they cost LFS quota
    auto warnings = std::make_shared<QStringList>();
    ++m_running;
    m_git->job(tr("Checking what goes up"), [warnings](const git::Context& c, const git::RunOptions&) { *warnings = git::pushWarnings(c); },
               [this, self = QPointer<VersionControl>(this), warnings, remote](bool ok, const QString&) {
                 if (!self) return;
                 --m_running;
                 if (ok && !warnings->isEmpty())
                   return ask("vcsPushWarnings", tr("Push"), warnings->join("\n\n"), {{"push", tr("Push anyway"), [this, remote] { runPush(remote, true); }}});
                 runPush(remote, true);
               });
    return;
  }
  const QString branch = m_git->repo().status.branch;
  QStringList args{"push", "--progress"};
  if (!remote.isEmpty()) args << "-u" << remote << branch;
  ++m_running;
  m_git->command(tr("Pushing %1").arg(branch), args, [this, branch, remote](const git::Result& r) {
    --m_running;
    if (r.ok()) {
      say(remote.isEmpty() ? tr("Pushed %1").arg(branch) : tr("Pushed %1 to %2: it follows %2/%1 from now on").arg(branch, remote));
      return done("push", true);
    }
    if (r.cancelled) return done("push", false);
    failed(tr("Could not push %1").arg(branch), r.error());
    if (r.error() == git::explain("non-fast-forward")) say(tr("The remote has commits this clone lacks."), tr("Pull"), [this] { pull(); }, 0);
    done("push", false, r.error());
  }, git::RunOptions::network());
}

void VersionControl::fetch() {
  if (!ready()) return openPanel();
  ++m_running;
  m_git->command(tr("Fetching"), {"fetch", "--progress", "--all", "--prune"}, [this](const git::Result& r) {
    --m_running;
    if (!r.ok()) {
      if (!r.cancelled) failed(tr("Could not fetch"), r.error());
      return done("fetch", false, r.error());
    }
    // The chip reads ahead/behind again by itself; say it once it has.
    auto* link = new QMetaObject::Connection;
    *link = connect(m_git, &GitWatch::changed, this, [this, link] {
      disconnect(*link);
      delete link;
      const git::Status& s = m_git->repo().status;
      say(s.upstream.isEmpty() ? tr("Fetched") : s.behind ? tr("Fetched: %n to pull", nullptr, s.behind) : tr("Fetched: nothing new for %1").arg(s.branch));
      done("fetch", true);
    });
  }, git::RunOptions::network());
}

void VersionControl::backgroundFetch() {
  const git::Repo& r = m_git->repo();
  if (!ready() || r.status.upstream.isEmpty() || r.merging || m_running || m_fetching) return;
  QString remote = r.status.upstream.section('/', 0, 0);
  for (const QString& name : m_remotes)
    if (r.status.upstream.startsWith(name + '/')) remote = name;
  git::Context c = m_git->context();
  c.askpass.clear();  // nobody is asked to sign in for a fetch they did not start
  const int before = r.status.behind;
  const QString upstream = r.status.upstream;
  m_fetching = true;
  m_services.jobs()->quiet(tr("Fetching in the background"), [c, remote](Progress p) {
    git::RunOptions o = git::RunOptions::network();
    o.idleMs = 60000;
    o.cancelled = [p] { return p.cancelled(); };
    git::check(c, {"-c", "credential.interactive=never", "fetch", "--quiet", remote}, o);
  }, [this, self = QPointer<VersionControl>(this), before, upstream](bool ok, const QString& error) {
    if (!self) return;
    m_fetching = false;
    if (!ok) {
      if (trace::enabled()) trace::log("version: background fetch: " + error);
      return done("background fetch", false, error);
    }
    auto* link = new QMetaObject::Connection;  // the chip's next read has the count
    *link = connect(m_git, &GitWatch::changed, this, [this, link, before, upstream] {
      disconnect(*link);
      delete link;
      const git::Status& s = m_git->repo().status;
      if (s.upstream == upstream && s.behind > before) say(tr("%1 has new commits: %n to pull.", nullptr, s.behind).arg(upstream), tr("Pull"), [this] { pull(); }, 8000);
      done("background fetch", true);
    });
    m_git->refresh();
  });
}

void VersionControl::pull() {
  if (!ready()) return openPanel();
  const git::Repo& r = m_git->repo();
  if (r.merging) return failed(tr("Pull"), git::explain("You have not concluded your merge"));
  if (r.status.upstream.isEmpty())
    return failed(tr("Pull"), tr("Branch %1 follows no remote branch yet: Push sets one up.").arg(r.status.branch));
  incoming(QStringLiteral("@{u}"), r.status.upstream, true);
}

void VersionControl::mergeBranch(const QString& name) {
  if (!ready()) return;
  if (m_git->repo().merging) return failed(tr("Merge"), git::explain("You have not concluded your merge"));
  incoming(name, name, false);
}

void VersionControl::incoming(const QString& target, const QString& label, bool fetchFirst) {
  auto in = std::make_shared<Incoming>();
  in->target = target;
  in->label = label;
  const QString rel = documentPath(), folder = versionsFolder();
  QString remote = m_git->repo().status.upstream.section('/', 0, 0);
  for (const QString& name : m_remotes)  // a remote's name may hold a '/'
    if (m_git->repo().status.upstream.startsWith(name + '/')) remote = name;
  ++m_running;
  const QString what = fetchFirst ? QStringLiteral("pull") : QStringLiteral("incoming");
  m_git->job(fetchFirst ? tr("Pulling from %1").arg(label) : tr("Reading %1").arg(label), [in, rel, folder, remote, fetchFirst](const git::Context& c, const git::RunOptions& o) {
    if (fetchFirst) {
      git::RunOptions net = git::RunOptions::network();
      net.cancelled = o.cancelled;
      net.progress = o.progress;
      if (o.progress) o.progress(tr("Fetching from %1").arg(remote), -1);
      git::check(c, {"fetch", "--progress", remote}, net);
    }
    readIncoming(c, rel, *in, folder, o);
  }, [this, self = QPointer<VersionControl>(this), in, what, fetchFirst](bool ok, const QString& error) {
    if (!self) return;
    --m_running;
    if (!ok) {
      if (error != "cancelled") failed(fetchFirst ? tr("Could not pull") : tr("Could not read %1").arg(in->label), error);
      return done(what, false, error);
    }
    if (trace::enabled())
      trace::log(QStringLiteral("version: incoming %1: %2 commits, document %3, %4%5").arg(in->label).arg(in->commits.size())
                     .arg(in->documentChanged ? "changed" : "unchanged", in->fastForward ? "fast-forward" : "merge", in->error.isEmpty() ? QString() : ", stops: " + in->error));
    showIncoming(in, fetchFirst);
    done(what, true);
  });
}

void VersionControl::showIncoming(std::shared_ptr<Incoming> in, bool pull) {
  const QString branch = m_git->repo().status.branch, name = QFileInfo(documentPath()).fileName();
  if (in->commits.empty()) {
    say(pull ? tr("Already up to date with %1").arg(in->label) : tr("%1 has nothing that %2 lacks").arg(in->label, branch));
    return;
  }
  if (m_incoming) m_incoming->close();
  auto* d = new QDialog(m_services.window());
  m_incoming = d;
  d->setObjectName("vcsIncoming");
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setModal(false);  // Compare can be looked at meanwhile
  d->setWindowTitle(pull ? tr("Pull from %1").arg(in->label) : tr("Merge %1 into %2").arg(in->label, branch));
  d->resize(600, 480);
  auto* col = new QVBoxLayout(d);
  auto* head = new QLabel(tr("Commits that come in from %1: %n.", nullptr, int(in->commits.size())).arg(in->label) + ' ' +
                              (in->fastForward ? tr("%1 has nothing they lack: it moves up to them (fast-forward).").arg(branch)
                                               : tr("Both sides have commits of their own: git makes a merge commit.")),
                          d);
  head->setObjectName("incomingHead");
  head->setWordWrap(true);
  col->addWidget(head);
  auto* commits = new QTreeWidget(d);
  commits->setObjectName("commits");
  commits->setColumnCount(3);
  commits->setHeaderHidden(true);
  commits->setRootIsDecorated(false);
  commits->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  commits->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  commits->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  for (const git::Commit& c : in->commits) {
    auto* item = new QTreeWidgetItem(commits, {c.shortHash, c.subject, tr("%1, %2").arg(c.author, VersionPanel::ago(c.date))});
    item->setFont(0, theme::mono(12));
    item->setToolTip(1, c.hash);
  }
  commits->setMaximumHeight(std::min(5, int(in->commits.size())) * 24 + 8);
  col->addWidget(commits);
  auto* what = new QLabel(d);
  what->setObjectName("incomingDocument");
  what->setWordWrap(true);
  what->setText(!in->documentChanged ? tr("%1 is not changed by them.").arg(name)
                : in->error.isEmpty() ? tr("What changes in %1: %2").arg(name, in->summary.isEmpty() ? tr("nothing it shows") : in->summary)
                                      : tr("What they change in %1: %2").arg(name, in->summary));
  col->addWidget(what);
  if (in->documentChanged) {
    QTreeWidget* list = ComparePanel::makeList(d);
    list->setObjectName("incomingChanges");
    std::vector<QTreeWidgetItem*> rows;
    std::vector<int> order;
    ComparePanel::listChanges(list, in->changes, rows, order);
    list->expandAll();
    col->addWidget(list, 1);
  }
  const Tokens& t = theme::current();
  if (!in->error.isEmpty()) {
    auto* stop = new QLabel(tr("The merge stops: both sides changed the same things in %1, and OPAD's merge leaves that to you. Git keeps your version and the merge waits "
                               "until you abort it or resolve it.").arg(name) +
                                (in->conflicts.isEmpty() ? QString() : "\n" + in->conflicts.mid(0, 8).join('\n')),
                            d);
    stop->setObjectName("incomingConflicts");
    stop->setWordWrap(true);
    stop->setStyleSheet(QStringLiteral("color: %1").arg(theme::css(t.red)));
    col->addWidget(stop);
  }
  if (in->design) {
    auto* regen = new QLabel(tr("Both sides changed the design: after the merge OPAD offers to regenerate it, so that what each side computed follows the other's changes."), d);
    regen->setObjectName("incomingDesign");
    regen->setWordWrap(true);
    regen->setStyleSheet(QStringLiteral("color: %1").arg(theme::css(t.amber)));
    col->addWidget(regen);
  }
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, d);
  auto* preview = new QPushButton(tr("Preview in Compare"), d);
  preview->setProperty("action", "vcsIncomingCompare");
  preview->setEnabled(!in->merged.isEmpty() && m_compare);
  preview->setToolTip(tr("The document as it is against the document as the merge leaves it"));
  buttons->addButton(preview, QDialogButtonBox::ActionRole);
  auto* merge = new QPushButton(!in->error.isEmpty() ? tr("Merge anyway") : pull ? tr("Pull") : tr("Merge"), d);
  if (in->error.isEmpty()) merge->setObjectName("primary");  // a merge that will stop is no default
  merge->setProperty("action", "vcsIncomingMerge");
  merge->setDefault(in->error.isEmpty());
  buttons->addButton(merge, QDialogButtonBox::AcceptRole);
  col->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::close);
  connect(preview, &QPushButton::clicked, d, [this, in] {
    m_reopen = m_reopen || (m_tool && m_tool->isVisible());
    m_compare->compare({CompareVersion::Kind::Session, QString(), tr("This session"), QString()},
                       {CompareVersion::Kind::File, in->merged, tr("After merging %1").arg(in->label), QDir::toNativeSeparators(in->merged)});
  });
  connect(merge, &QPushButton::clicked, d, [this, in] { runMerge(in); });
  d->show();
}

void VersionControl::runMerge(std::shared_ptr<Incoming> in) {
  whenClean(tr("Merge %1").arg(in->label), [this, in] {
    if (m_compare && m_compare->active()) m_compare->close();  // the preview: the document changes under it
    git::RunOptions o;
    o.timeoutMs = 0;  // the driver on a big document
    ++m_running;
    m_git->command(tr("Merging %1").arg(in->label), {"merge", "--no-edit", in->target}, [this, in](const git::Result& r) {
      --m_running;
      m_disk->adopt();  // the file as git left it comes in
      if (!r.ok()) {
        if (!r.cancelled) failed(tr("Could not merge %1").arg(in->label), r.error());
        return done("merge", false, r.error());
      }
      if (m_incoming) m_incoming->close();
      if (in->design)
        say(tr("Merged %1. Both sides changed the design: regenerate it so that each side's results follow the other's changes.").arg(in->label),
                         tr("Regenerate"), [this] {
                           if (QAction* a = m_services.action("design.regenerate")) a->trigger();
                         }, 0);
      else
        say(tr("Merged %1").arg(in->label));
      done("merge", true);
    }, o);
  });
}

void VersionControl::abortMerge() {
  if (!m_git->repo().merging) return;
  ask("vcsAbortMerge", tr("Abort merge"), tr("Abort the merge? The files go back to how they were before it began."), {{"abort", tr("Abort merge"), [this] {
        ++m_running;
        m_git->command(tr("Aborting the merge"), {"merge", "--abort"}, [this](const git::Result& r) {
          --m_running;
          m_disk->adopt();
          if (!r.ok()) failed(tr("Could not abort the merge"), r.error());
          else say(tr("Merge aborted"));
          done("abort", r.ok());
        });
      }}});
}

// ---------------------------------------------------------------- branches
void VersionControl::runSwitch(const QStringList& args, const QString& name) {
  ++m_running;
  m_git->command(tr("Switching to %1").arg(name), args, [this, name](const git::Result& r) {
    --m_running;
    m_disk->adopt();  // the branch's version of the document comes in
    if (!r.ok()) {
      if (!r.cancelled) failed(tr("Could not switch to %1").arg(name), r.error());
      return done("switch", false, r.error());
    }
    say(tr("Switched to %1").arg(name));
    done("switch", true);
  });
}

void VersionControl::switchTo(const git::Branch& b) {
  if (b.head || !ready()) return;
  QStringList args{"switch", "-q"};
  QString name = b.name;
  if (b.remote) {  // its local branch, made to follow it when there is none yet
    name = b.name.section('/', 1, -1);
    const bool local = std::any_of(m_branches.begin(), m_branches.end(), [&name](const git::Branch& x) { return !x.remote && x.name == name; });
    if (local) args << name;
    else args << "-c" << name << "--track" << b.name;
  } else {
    args << b.name;
  }
  whenClean(tr("Switch to %1").arg(name), [this, args, name] { runSwitch(args, name); });
}

void VersionControl::newBranch(const QString& start, const QString& startLabel) {
  if (!ready()) return openPanel();
  if (m_git->repo().status.oid == "(initial)") return failed(tr("New branch"), tr("Commit first: a branch starts at a commit."));
  auto* d = new QDialog(m_services.window());
  d->setObjectName("vcsNewBranch");
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowTitle(tr("New branch"));
  d->setMinimumWidth(440);
  auto* col = new QVBoxLayout(d);
  auto* form = new QFormLayout;
  auto* name = new QLineEdit(d);
  name->setObjectName("name");
  name->setPlaceholderText(tr("feature/longer-arm"));
  form->addRow(tr("Name"), name);
  auto* from = new QLabel(start.isEmpty() ? tr("The current commit (%1)").arg(m_git->repo().status.branch) : startLabel, d);
  from->setObjectName("from");
  from->setWordWrap(true);
  form->addRow(tr("Starts at"), from);
  col->addLayout(form);
  auto* problem = new QLabel(d);
  problem->setObjectName("problem");
  problem->setWordWrap(true);
  problem->setStyleSheet(QStringLiteral("color: %1").arg(theme::css(theme::current().red)));
  col->addWidget(problem);
  auto* switchTo = new QCheckBox(tr("Switch to it"), d);
  switchTo->setObjectName("switch");
  switchTo->setChecked(true);
  col->addWidget(switchTo);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, d);
  auto* run = new QPushButton(tr("Create"), d);
  run->setObjectName("primary");
  run->setProperty("action", "vcsNewBranchRun");
  run->setDefault(true);
  buttons->addButton(run, QDialogButtonBox::AcceptRole);
  col->addWidget(buttons);
  auto valid = [this, name, problem, run] {
    const QString n = name->text().trimmed();
    const bool taken = std::any_of(m_branches.begin(), m_branches.end(), [&n](const git::Branch& b) { return !b.remote && b.name == n; });
    problem->setText(n.isEmpty() ? QString() : !git::validBranchName(n) ? git::explain("is not a valid branch name") : taken ? git::explain("a branch named x already exists") : QString());
    problem->setVisible(!problem->text().isEmpty());
    run->setEnabled(!n.isEmpty() && problem->text().isEmpty());
  };
  connect(name, &QLineEdit::textChanged, d, valid);
  valid();
  connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
  connect(run, &QPushButton::clicked, d, [this, d, name, switchTo, start] {
    const QString n = name->text().trimmed();
    const bool go = switchTo->isChecked();
    d->accept();
    if (!go) {
      ++m_running;
      QStringList args{"branch", n};
      if (!start.isEmpty()) args << start;
      m_git->command(tr("Creating %1").arg(n), args, [this, n](const git::Result& r) {
        --m_running;
        if (!r.ok()) failed(tr("Could not create %1").arg(n), r.error());
        else say(tr("Created the branch %1").arg(n));
        done("branch", r.ok());
      });
      return;
    }
    QStringList args{"switch", "-q", "-c", n};
    if (start.isEmpty()) return runSwitch(args, n);  // from here: the files stay as they are
    args << start;
    whenClean(tr("Switch to %1").arg(n), [this, args, n] { runSwitch(args, n); });
  });
  d->open();
  name->setFocus();
}

void VersionControl::deleteBranch(const QString& name, bool force) {
  auto run = [this, name, force] {
    ++m_running;
    m_git->command(tr("Deleting %1").arg(name), {"branch", force ? "-D" : "-d", name}, [this, name, force](const git::Result& r) {
      --m_running;
      if (r.ok()) {
        say(tr("Deleted the branch %1").arg(name));
        return done("delete", true);
      }
      if (!force && QString::fromUtf8(r.err).contains("not fully merged")) {
        done("delete", false, "unmerged");
        return ask("vcsDeleteUnmerged", tr("Delete branch"), tr("%1 holds commits no other branch has: deleting it loses them. Delete it anyway?").arg(name),
                   {{"force", tr("Delete anyway"), [this, name] { deleteBranch(name, true); }}});
      }
      failed(tr("Could not delete %1").arg(name), r.error());
      done("delete", false, r.error());
    });
  };
  if (force) return run();
  ask("vcsDeleteBranch", tr("Delete branch"), tr("Delete the branch %1? Its commits stay in the branches that hold them.").arg(name), {{"delete", tr("Delete"), run}});
}

void VersionControl::pack() {
  if (!ready()) return openPanel();
  struct Out {
    git::Objects before, after;
  };
  auto out = std::make_shared<Out>();
  ++m_running;
  m_git->job(tr("Packing the repository"), [out](const git::Context& c, const git::RunOptions& o) {
    out->before = git::countObjects(c);
    git::RunOptions gc = o;
    gc.timeoutMs = 0;
    git::check(c, {"gc", "--quiet"}, gc);
    out->after = git::countObjects(c);
  }, [this, self = QPointer<VersionControl>(this), out](bool ok, const QString& error) {
    if (!self) return;
    --m_running;
    if (!ok) {
      if (error != "cancelled") failed(tr("Could not pack the repository"), error);
      return done("pack", false, error);
    }
    say(tr("Packed the repository: %1 MB of loose objects and packs now take %2 MB.")
                         .arg(double(out->before.looseKiB + out->before.packKiB) / 1024, 0, 'f', 1)
                         .arg(double(out->after.looseKiB + out->after.packKiB) / 1024, 0, 'f', 1));
    done("pack", true);
  });
}

// ---------------------------------------------------------------- history
void VersionControl::compareWith(const git::Commit& c) {
  if (!m_compare) return;
  m_reopen = m_tool && m_tool->isVisible();
  m_compare->compare({CompareVersion::Kind::Git, c.hash, tr("%1 · %2").arg(c.shortHash, c.subject), tr("%1\n%2, %3").arg(c.hash, c.author, c.date)},
                     {CompareVersion::Kind::Session, QString(), tr("This session"), QString()});
}

void VersionControl::comparePrevious(const git::Commit& c) {
  if (!m_compare || c.parents.isEmpty()) return;
  m_reopen = m_tool && m_tool->isVisible();
  m_compare->compare({CompareVersion::Kind::Git, c.parents.front(), tr("Before %1").arg(c.shortHash), c.parents.front()},
                     {CompareVersion::Kind::Git, c.hash, tr("%1 · %2").arg(c.shortHash, c.subject), tr("%1\n%2, %3").arg(c.hash, c.author, c.date)});
}

void VersionControl::openReadOnly(const git::Commit& c) {
  if (!ready()) return;
  const QString rel = documentPath();
  const QString file = QDir(versionsFolder()).filePath(tr("%1 at %2.opad").arg(QFileInfo(rel).completeBaseName(), c.shortHash));
  const QString hash = c.hash, shortHash = c.shortHash;
  ++m_running;
  m_git->job(tr("Reading %1").arg(shortHash), [rel, file, hash](const git::Context& ctx, const git::RunOptions&) {
    const QByteArray text = git::show(ctx, hash, rel);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (QFileInfo::exists(file)) QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);  // an older copy: replaced
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(text) != text.size() || !out.commit())
      fail(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(file), out.errorString()));
    QFile::setPermissions(file, QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther);  // Save refuses it
  }, [this, self = QPointer<VersionControl>(this), file, shortHash, rel](bool ok, const QString& error) {
    if (!self) return;
    --m_running;
    if (!ok) {
      failed(tr("Could not open %1").arg(shortHash), error);
      return done("open", false, error);
    }
    m_lastOpened = file;
    if (qEnvironmentVariableIsSet("OPAD_BENCH_VERSION")) trace::log("version: opens " + file);  // a bench starts no other window
    else if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), readOnlyArguments(file)))
      return failed(tr("Could not open %1").arg(shortHash), tr("OPAD could not be started again."));
    say(tr("%1 as it was in %2 opens in another window, read-only.").arg(QFileInfo(rel).fileName(), shortHash));
    done("open", true);
  });
}

void VersionControl::restore(const git::Commit& c) {
  AppDocument* doc = m_services.document();
  if (!ready() || doc->loading) return;
  const git::Commit commit = c;
  const QString name = QFileInfo(documentPath()).fileName();
  ask("vcsRestore", tr("Restore as new changes"),
      tr("Bring %1 back to how it was in %2 (“%3”)? Nothing leaves its history: the changes made since are undone by new records, one step that Undo takes back. Save "
         "and commit to keep it.")
          .arg(name, commit.shortHash, commit.subject),
      {{"restore", tr("Restore"), [this, doc, commit] {
          if (DesignController* d = m_services.design(); d && (d->sketchActive() || d->featureActive()))
            return m_services.showMessage(tr("Finish the sketch or the feature first."));
          const auto generation = doc->generation, revision = doc->revision;
          const git::Context ctx = m_git->context();
          const QString rel = documentPath(), file = doc->path();
          const std::string by = QSettings().value("user/name").toString().trimmed().toStdString();
          ++m_running;
          auto fail = [this](const QString& error) {
            --m_running;
            failed(tr("Could not restore the version"), error);
            done("restore", false, error);
          };
          const bool started = doc->captureSnapshot(m_services.jobs(), [=, this, self = QPointer<VersionControl>(this)](std::shared_ptr<opad::Document> copy, const QString& error) {
            if (!self) return;
            if (!copy) return fail(error.isEmpty() ? tr("The document is busy: try again in a moment.") : error);
            struct Out {
              opad::Document document;
              opad::Scene scene;
              size_t later = 0;
            };
            auto out = std::make_shared<Out>();
            m_services.jobs()->async(tr("Restoring %1").arg(commit.shortHash), [out, copy, ctx, rel, file, commit, by](Progress p) mutable {
              p.setPhase(tr("Reading %1").arg(commit.shortHash));
              opad::Document version = opad::Document::parse_index(git::show(ctx, commit.hash, rel).toStdString(), fsPath(file));
              const opad::RestorePlan plan = opad::plan_restore(*copy, version);
              using P = opad::RestorePlan::Problem;
              if (plan.problem == P::current) throw std::runtime_error(tr("The document is as it was in %1 already.").arg(commit.shortHash).toStdString());
              if (plan.problem == P::other_document) throw std::runtime_error(tr("%1 holds another document under this name.").arg(commit.shortHash).toStdString());
              if (plan.problem == P::not_ancestor)
                throw std::runtime_error(tr("The document's history does not continue %1: it holds changes made elsewhere (another branch, or a rewritten history). "
                                            "Compare with it, or open it read-only.").arg(commit.shortHash).toStdString());
              opad::apply_restore(*copy, version, plan, by);
              p.setPhase(tr("Preparing the restored bodies"));
              opad::warm_shape_cache(*copy, [p](size_t, size_t) { return !p.cancelled(); });
              out->scene = opad::resolve(*copy);
              out->later = plan.later;
              out->document = std::move(*copy);
              copy.reset();
            }, [=, this](bool ok, const QString& error) {
              if (!self) return;
              if (!ok) return fail(error);
              if (doc->generation != generation || doc->revision != revision) return fail(tr("The document changed meanwhile: try again."));
              try {
                doc->commitSnapshot(out->document, out->scene, revision, tr("restore %1").arg(commit.shortHash));
              } catch (const std::exception& e) {
                return fail(QString::fromUtf8(e.what()));
              }
              dispose(std::make_shared<std::pair<opad::Document, opad::Scene>>(std::move(out->document), std::move(out->scene)));  // what it replaced
              --m_running;
              say(tr("Restored %1 (later changes undone: %n). Save and commit to keep it.", nullptr, int(out->later)).arg(commit.shortHash), tr("Undo"),
                               [doc] { doc->undo(); }, 8000);
              done("restore", true);
            });
          });
          if (!started) fail(tr("The document is busy: try again in a moment."));
        }}});
}

// ---------------------------------------------------------------- the chip's menu
void VersionControl::extendMenu(QMenu* m) {
  const AppDocument* doc = m_services.document();
  if (!doc->hasDocument || doc->viewOnly()) return;
  for (const char* id : {"vcs.panel", "vcs.commit", "vcs.pull", "vcs.push"}) {
    QAction* a = m_services.action(QString::fromLatin1(id));
    if (a && (ready() || QString::fromLatin1(id) == "vcs.panel")) m->addAction(a);
  }
  m->addSeparator();
}
