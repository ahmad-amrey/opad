#pragma once
// The open document's op history in git (UI-64, OpHistory.hpp), on the UI thread: the index of its repository at HEAD,
// read on a worker when first asked (a hover, a menu) and again once HEAD has moved. tip() is asked on every hover move
// of the timeline, so a lookup is a hash map's.
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <string>
#include <vector>

#include "OpHistory.hpp"

class GitWatch;
class Job;
class JobRunner;
namespace opad {
struct Op;
}

class OpProvenance : public QObject {
  Q_OBJECT
 public:
  OpProvenance(GitWatch* git, JobRunner* jobs, QObject* parent);
  // Lines for an op's tooltip (HTML): who added it in which commit and when (and under which name, the document renamed
  // since), its later edits, "not committed yet", or that the history is being read (which starts the read). Empty outside
  // a repository.
  QString tip(const opad::Op& op);
  void ensure();  // reads the index unless it is there for HEAD (or being read)
  bool ready() const;  // the index is the one of HEAD
  const ophistory::Index& index() const { return m_index; }
  // The commits that touched any of these ops or nodes, newest first; ready() first. One under an older name of the document
  // counts as the commit that renamed it (the History page lists the document under its name).
  std::vector<git::Commit> commitsTouching(const std::vector<std::string>& ids) const;
  void whenReady(std::function<void()> then);  // now when ready, else once the read ends (dropped when it fails); retries a failed read
  QString error() const { return m_error; }    // why the last read failed
 signals:
  void built(bool ok);
 private:
  QString key() const;  // the repository, document and HEAD the index must be of
  GitWatch* m_git;
  JobRunner* m_jobs;
  ophistory::Index m_index;
  QString m_built, m_building, m_failed, m_error;  // m_failed: not read again by itself (hovers) until HEAD moves
  QPointer<Job> m_job;
  std::vector<std::function<void()>> m_waiting;
};
