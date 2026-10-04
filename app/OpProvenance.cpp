#include "OpProvenance.hpp"

#include <QElapsedTimer>
#include <algorithm>

#include "GitWatch.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "VersionPanel.hpp"
#include "opad/document.hpp"

OpProvenance::OpProvenance(GitWatch* git, JobRunner* jobs, QObject* parent) : QObject(parent), m_git(git), m_jobs(jobs) {}

QString OpProvenance::key() const {
  const git::Repo& r = m_git->repo();
  if (r.state != git::Repo::State::Ready || r.rel.isEmpty() || r.status.oid.isEmpty() || r.status.oid == "(initial)") return {};
  return r.top + '|' + r.rel + '|' + r.status.oid;
}

bool OpProvenance::ready() const { return !m_built.isEmpty() && m_built == key(); }

void OpProvenance::ensure() {
  const QString k = key();
  if (k.isEmpty() || k == m_built || k == m_building || k == m_failed) return;
  if (m_job) m_job->cancel();
  m_building = k;
  const git::Repo& r = m_git->repo();
  const git::Context c = m_git->context();
  const QString top = r.top, rel = r.rel, cache = ophistory::cacheFolder();
  auto out = std::make_shared<ophistory::Index>();
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  m_job = m_jobs->async(tr("Reading who changed what in git"), [c, top, rel, cache, out](Progress p) {
    *out = ophistory::build(c, top, rel, cache, [p] { return p.cancelled(); }, [p](int done, int total) {
      p.setPhase(tr("Reading version %1 of %2").arg(done).arg(total), total ? done * 100 / total : -1);
    });
  }, [self = QPointer<OpProvenance>(this), out, k, clock](bool ok, const QString& error) {
    if (!self || self->m_building != k) return;  // superseded by a newer HEAD
    self->m_building.clear();
    if (ok) {
      self->m_index = std::move(*out);
      self->m_built = k;
      self->m_error.clear();
    } else {
      self->m_failed = k;
      self->m_error = error;
    }
    if (trace::enabled())
      trace::log(QStringLiteral("git: op history %1: %2 commits, %3 versions, %4 read (%5 KB) in %6 ms%7").arg(ok ? "read" : "failed")
                     .arg(self->m_index.commits.size()).arg(self->m_index.blobs).arg(self->m_index.blobsRead).arg(self->m_index.bytesRead / 1024)
                     .arg(clock->elapsed()).arg(ok ? QString() : ": " + error));
    auto waiting = std::exchange(self->m_waiting, {});
    if (ok)
      for (auto& then : waiting) then();
    emit self->built(ok);
  });
}

void OpProvenance::whenReady(std::function<void()> then) {
  if (ready()) return then();
  m_waiting.push_back(std::move(then));
  m_failed.clear();  // asked for: try again
  ensure();
}

std::vector<git::Commit> OpProvenance::commitsTouching(const std::vector<std::string>& ids) const {
  // The History page lists the document under its name: a commit under an older one counts as the commit that renamed it.
  const int renamed = int(std::find(m_index.paths.begin(), m_index.paths.end(), m_index.rel) - m_index.paths.begin());
  std::vector<int> at = m_index.touching(ids);
  for (int& k : at) k = std::max(k, renamed);
  at.erase(std::unique(at.begin(), at.end()), at.end());
  std::vector<git::Commit> out;
  for (auto it = at.rbegin(); it != at.rend(); ++it)
    if (*it < int(m_index.commits.size())) out.push_back(m_index.commits[size_t(*it)]);
  return out;
}

QString OpProvenance::tip(const opad::Op& op) {
  const git::Repo& r = m_git->repo();
  if (r.state != git::Repo::State::Ready || r.rel.isEmpty()) return {};
  const Tokens& t = theme::current();
  auto line = [](const QColor& colour, const QString& text) { return QStringLiteral("<div style='color:%1'>%2</div>").arg(colour.name(), text.toHtmlEscaped()); };
  const QString k = key();
  if (k.isEmpty()) return line(t.fg3, tr("Not committed yet"));  // no commit in the repository
  if (k != m_built) {
    ensure();
    if (m_built.isEmpty() || m_index.top + '|' + m_index.rel != r.top + '|' + r.rel)
      return line(t.fg3, m_error.isEmpty() ? tr("Reading who added it in git…") : tr("The git history could not be read: %1").arg(m_error));
  }
  // An index of an older HEAD meanwhile: still right for everything it holds.
  const ophistory::Provenance* p = m_index.find(op.id);
  if (!p || p->added < 0) return line(t.fg3, tr("Not committed yet"));
  const git::Commit& added = m_index.commits[size_t(p->added)];
  const QString as = m_index.paths[size_t(p->added)];  // a document renamed since: the name it had then
  QString html = line(t.fg2, as == m_index.rel ? tr("Added by %1 in %2 · %3").arg(added.author, added.shortHash, VersionPanel::ago(added.date))
                                               : tr("Added by %1 in %2 · %3, as %4").arg(added.author, added.shortHash, VersionPanel::ago(added.date), as));
  if (p->edits > 0) {
    const git::Commit& last = m_index.commits[size_t(p->lastEdit)];
    html += line(t.fg3, tr("Edited in %n commit(s), last by %1 in %2 · %3", nullptr, p->edits).arg(last.author, last.shortHash, VersionPanel::ago(last.date)));
  }
  if (p->deleted >= 0) {
    const git::Commit& gone = m_index.commits[size_t(p->deleted)];
    html += line(t.fg3, tr("Deleted by %1 in %2").arg(gone.author, gone.shortHash));
  }
  return html;
}
