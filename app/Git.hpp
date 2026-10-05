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
//
// The environment it meets (UI-136): git found where a portable build keeps it or where the user pointed ("Locate
// git…", the setting git/path), git-lfs detected; sign-in through the credential helper, else this opad.exe as
// GIT_ASKPASS (a dialog, never a terminal); SSH in BatchMode unless the user set their own ssh command, so a key that
// wants a passphrase fails with a sentence instead of hanging; errors as sentences (explain): sign-in, SSH host keys,
// network, safe.directory ownership, index.lock, identity, size limits and LFS quotas.
#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

namespace git {

// Where git is: OPAD_GIT (only that one, for tests), the setting git/path ("Locate git…", relative to OPAD's folder when
// inside it, so a portable copy can move), git beside OPAD (a portable git/ or PortableGit/ folder), PATH, then the
// usual install folders. Empty when there is none.
QString findProgram();
// Whether `path` runs as git: empty, or why not. Fills `version`. Runs it: workers only.
QString checkProgram(const QString& path, QString* version = nullptr);

struct Context {
  QString program;       // git
  QString dir;           // working directory
  QString askpass;       // GIT_ASKPASS: this opad.exe when no credential helper is configured (OPAD_ASKPASS=1 tells it so)
  bool sshBatch = true;  // ssh -o BatchMode=yes, unless GIT_SSH(_COMMAND) or core.sshCommand is the user's own
  QProcessEnvironment environment(bool optionalLocks = true) const;
};

struct RunOptions {
  int timeoutMs = 60000;  // the whole run (0: none)
  int idleMs = 0;         // nothing written for this long: stalled (network commands with --progress; 0: no limit)
  bool optionalLocks = true;
  QByteArray input;       // written to stdin, which is closed either way
  std::function<bool()> cancelled;                                  // asked every 50 ms
  std::function<void(const QString& phase, int percent)> progress;  // from git's --progress lines, translated
  static RunOptions network() {  // fetch, pull, push, clone: no overall limit, but 2 min without a word is a stall
    RunOptions o;
    o.timeoutMs = 0;
    o.idleMs = 120000;
    return o;
  }
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
  QString error;   // Untrusted, Failed: why
  QString unsafe;  // Untrusted: the folder git would need in safe.directory
  QString program, version, lfsVersion;  // lfsVersion empty: no git-lfs
  QString file, top, gitDir, commonDir, rel;  // rel: the file relative to top
  QString helper, sshCommand, userName, userEmail;  // effective config
  QString driver, textconv, difftool;  // merge.opad.driver, diff.opad.textconv, difftool.opad.cmd
  bool managed = false;      // opad.managed: OPAD wrote the driver config and keeps it working
  bool wantsDriver = false;  // the attributes say merge=opad for the file
  bool lfsHooks = false;     // the clone has git-lfs's hooks (git lfs install --local or global)
  bool ignored = false, merging = false, rebasing = false;
  Status status;
  Doc doc() const;
  Sync sync() const;
  bool needsDriver() const { return state == State::Ready && wantsDriver && driver.isEmpty(); }
  bool driverStale() const;  // managed, and a program it names is gone (OPAD moved or was updated elsewhere) or a key missing
};
Repo probe(const Context& c, const QString& file);  // c.dir is ignored: the file's folder
void forgetTools();  // probe asks git and git-lfs for their versions again (they are kept per program)
void readStatus(const Context& c, Repo& r);         // the status alone: one process

// This installation's driver commands: opad-cli beside the app, else the app (--merge-driver, --textconv); git difftool -t
// opad opens Compare in the app (--compare $LOCAL $REMOTE).
struct Install {
  QString cli, app;
  static Install here();
  QString mergeDriver() const;
  QString textconv() const;
  QString difftool() const;  // empty without the app
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
// merge.opad.*, diff.opad.textconv and cachetextconv (git log -p converts each version once), difftool.opad.cmd,
// opad.managed (local)
void configureDriver(const Context& c, const Install& in);

// git clone --progress (idle timeout 2 min, no overall limit).
Result clone(const Context& c, const QString& url, const QString& folder, const RunOptions& o = {});
// The folder name a clone address suggests, as git picks it ("git@host:team/robot.git" -> "robot"); empty: none.
QString cloneName(const QString& url);
// What never travels with a clone, for a new one in `folder`: the managed driver config when its attributes say
// merge=opad for an .opad file, the Git LFS hooks and files when they use LFS. Sentences of what it did; throws.
QStringList afterClone(const Context& c, const QString& folder, const Install& in, const RunOptions& ro = {});
// The OPAD documents under `folder` (absolute), shallowest first, then by name; hidden folders (.git) skipped, 8 deep.
QStringList documentsIn(const QString& folder, int limit = 200);
// git for `dir` outside the open document's repository (a clone): its config decides the askpass and BatchMode ssh.
Context contextFor(const QString& program, const QString& dir, const QString& askpass);

QString phaseText(const QString& gitPhase);  // "Receiving objects" -> its translation

QString explain(const QString& gitStderr);          // what git said, as a sentence a user can act on
QString unsafeDirectory(const QString& gitStderr);  // the folder a "dubious ownership" refusal names
void trust(const Context& c, const QString& folder);  // git config --global --add safe.directory
void setIdentity(const Context& c, const QString& name, const QString& email, bool global);

// History and branches (UI-62).
struct Commit {
  QString hash, shortHash, author, email, date, subject;  // date: the author date, ISO 8601
  QStringList parents;
  QStringList refs;  // what points at it: "HEAD -> main", "origin/main", "tag: v1"
};
extern const char* const kLogFormat;  // git log -z --format=<this>: what parseLog reads
std::vector<Commit> parseLog(const QByteArray& z);
// The commits of `range` ("": HEAD; "HEAD..@{u}") that changed `path` (relative to c.dir; "": any), newest first,
// `count` from `skip`. None before the first commit.
std::vector<Commit> log(const Context& c, const QString& range, const QString& path, int count, int skip = 0);
struct Branch {
  QString name;  // "main", "origin/main"
  QString ref;   // "refs/heads/main"
  QString oid, upstream, subject, date;
  int ahead = 0, behind = 0;
  bool head = false, remote = false, gone = false;  // gone: its upstream was deleted
};
extern const char* const kBranchFormat;  // git for-each-ref --format=<this>: what parseBranches reads
std::vector<Branch> parseBranches(const QByteArray& out);
std::vector<Branch> branches(const Context& c);  // local ones, then remote-tracking ones (no remote HEAD), each by name
QStringList remotes(const Context& c);
// A file at a revision (git cat-file blob <rev>:<path>, `path` relative to the repository's top). Throws when it has none.
QByteArray show(const Context& c, const QString& rev, const QString& path);
QString revParse(const Context& c, const QString& rev);  // the commit's hash; empty when there is none
// Loose objects and packs (git count-objects -v), in KiB.
struct Objects {
  qint64 loose = 0, looseKiB = 0, packs = 0, packKiB = 0;
};
Objects countObjects(const Context& c);
// Whether git takes `name` for a new branch (the rules of check-ref-format --branch, checked here without git).
bool validBranchName(const QString& name);

// Before a push: what goes up that is big. Files over `fileLimit` stored in the history itself (hosting services refuse
// 100 MB), and the Git LFS files going up when they come to more than `lfsLimit` (they count against the remote's
// quota). Commits not on any remote-tracking branch; local reads only. Sentences, empty when nothing is big.
QStringList pushWarnings(const Context& c, qint64 fileLimit = qint64(50) << 20, qint64 lfsLimit = qint64(500) << 20);
}  // namespace git
