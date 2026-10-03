#pragma once
// Git for the open document (UI-61): the git command line, never a library (libgit2 runs no merge drivers or LFS
// filters). Everything in namespace git that runs git *blocks*: call it on a worker (JobRunner::async/quiet), never on
// the UI thread. GitWatch (GitWatch.hpp) is the UI side: the status chip, its event-driven refresh and the jobs.
//
// Every process gets GIT_TERMINAL_PROMPT=0 (git never waits on a terminal nobody sees), LC_ALL=C (messages that can be
// read), core.quotepath=off, and no GIT_DIR / GIT_WORK_TREE inherited from a hook that started OPAD; a background status
// also GIT_OPTIONAL_LOCKS=0, so it never holds index.lock against the user's own git nor wakes the watcher by itself.
// Cancel and timeouts end the whole process tree (a Windows job object, a process group elsewhere): killing git alone
// leaves git-remote-https, ssh or git-lfs running.
#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

namespace git {

// Where git is: OPAD_GIT (only that one, for tests), git beside OPAD (a portable git/ or PortableGit/ folder), PATH,
// then the usual install folders. Empty when there is none.
QString findProgram();

struct Context {
  QString program;  // git
  QString dir;      // working directory
  QProcessEnvironment environment(bool optionalLocks = true) const;
};

struct RunOptions {
  int timeoutMs = 60000;  // the whole run (0: none)
  int idleMs = 0;         // nothing written for this long: stalled (network commands with --progress; 0: no limit)
  bool optionalLocks = true;
  QByteArray input;       // written to stdin, which is closed either way
  std::function<bool()> cancelled;                                  // asked every 50 ms
  std::function<void(const QString& phase, int percent)> progress;  // from git's --progress lines, translated
};

struct Result {
  int code = -1;
  QByteArray out, err;
  bool started = false, timedOut = false, cancelled = false;
  qint64 ms = 0;
  bool ok() const { return started && !timedOut && !cancelled && code == 0; }
  QString error() const;  // what went wrong, as a sentence
};

// git <args> in c.dir. Never throws.
Result run(const Context& c, const QStringList& args, const RunOptions& o = {});
// The same, throwing std::runtime_error(error()) unless it succeeded.
Result check(const Context& c, const QStringList& args, const RunOptions& o = {});

// `git status --porcelain=v2 --branch -z`
struct Entry {
  char kind = '1';        // '1' changed, '2' renamed or copied, 'u' unmerged, '?' untracked, '!' ignored
  char x = '.', y = '.';  // index, work tree ('.' unchanged)
  QString path, orig;     // relative to the top, '/' separated; orig: a rename's source
};
struct Status {
  QString oid;       // "(initial)" before the first commit
  QString branch;    // "(detached)" when HEAD names no branch
  QString upstream;  // empty: none set
  int ahead = 0, behind = 0;
  bool tracking = false;  // branch.ab was given: the upstream exists
  std::vector<Entry> entries;
  int count(char kind) const;
};
Status parseStatus(const QByteArray& porcelainV2z);

// One look at the repository of a file: what the chip shows and the menus offer.
struct Repo {
  enum class State { None, GitMissing, NotRepo, Untrusted, Failed, Ready };
  enum class Doc { None, Clean, Untracked, Added, Modified, Conflict, Ignored };
  enum class Sync { Local, Synced, Ahead, Behind, Diverged, Gone };
  State state = State::None;
  QString error;  // Untrusted, Failed: why
  QString program, version, lfsVersion;  // lfsVersion empty: no git-lfs
  QString file, top, gitDir, commonDir, rel;  // rel: the file relative to top
  QString helper, sshCommand, userName, userEmail;  // effective config
  QString driver, textconv;  // merge.opad.driver, diff.opad.textconv
  bool managed = false;      // opad.managed: OPAD wrote the driver config and keeps it working
  bool wantsDriver = false;  // the attributes say merge=opad for the file
  bool lfsHooks = false;     // the clone has git-lfs's hooks (git lfs install --local or global)
  bool ignored = false, merging = false, rebasing = false;
  Status status;
  Doc doc() const;
  Sync sync() const;
  bool needsDriver() const { return state == State::Ready && wantsDriver && driver.isEmpty(); }
  bool driverStale() const;  // managed, and the program it names is gone (OPAD moved or was updated elsewhere)
};
Repo probe(const Context& c, const QString& file);  // c.dir is ignored: the file's folder
void forgetTools();  // probe asks git and git-lfs for their versions again (they are kept per program)
void readStatus(const Context& c, Repo& r);         // the status alone: one process

// This installation's driver commands: opad-cli beside the app, else the app (--merge-driver, --textconv).
struct Install {
  QString cli, app;
  static Install here();
  QString mergeDriver() const;
  QString textconv() const;
};
QString attributesText(const QString& existing, bool lfs);  // .gitattributes with OPAD's lines (idempotent)
QString ignoreText(const QString& existing);                // .gitignore with OPAD's entries (idempotent)
extern const char* const kIgnored[];                        // those entries, null-terminated

struct SetupOptions {
  bool init = true, attributes = true, ignore = true, lfs = true, driver = true;
};
// Init (-b main) when the folder is not in a repository, .gitattributes, .gitignore, git lfs install --local, the
// managed driver config. Returns what it did, as sentences; throws std::runtime_error.
QStringList setUp(const Context& c, const QString& folder, const Install& in, const SetupOptions& o, const RunOptions& ro = {});
void configureDriver(const Context& c, const Install& in);  // merge.opad.*, diff.opad.textconv, opad.managed (local)

// git clone --progress (idle timeout 2 min, no overall limit).
Result clone(const Context& c, const QString& url, const QString& folder, const RunOptions& o = {});

QString phaseText(const QString& gitPhase);  // "Receiving objects" -> its translation
}  // namespace git
