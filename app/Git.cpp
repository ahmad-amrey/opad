#include "Git.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <stdexcept>

namespace git {
namespace {
const char* const kContext = "Git";
QString tr(const char* text) { return QCoreApplication::translate(kContext, text); }

[[noreturn]] void fail(const QString& text) { throw std::runtime_error(text.toUtf8().toStdString()); }

QString executable(const QString& path) {
  const QFileInfo fi(path);
  return fi.isFile() && fi.isExecutable() ? QDir::cleanPath(fi.absoluteFilePath()) : QString();
}

// Kills git and everything it started (git-remote-https, ssh, git-lfs, an askpass): killing git alone leaves them.
class Tree {
 public:
  explicit Tree(const QProcess& p) {
#ifdef _WIN32
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(p.processId()))) {
      if (m_job && !AssignProcessToJobObject(m_job, h)) {
        CloseHandle(m_job);
        m_job = nullptr;
      }
      CloseHandle(h);
    }
#else
    m_pid = p.processId();
#endif
  }
  ~Tree() {
#ifdef _WIN32
    if (m_job) CloseHandle(m_job);  // without KILL_ON_JOB_CLOSE: a detached `git maintenance` lives on
#endif
  }
  Tree(const Tree&) = delete;
  Tree& operator=(const Tree&) = delete;
  void kill(QProcess& p) {
#ifdef _WIN32
    if (m_job) TerminateJobObject(m_job, 1);
#else
    if (m_pid > 0) ::kill(-pid_t(m_pid), SIGKILL);  // its process group (setpgid in the child)
#endif
    p.kill();
  }
 private:
#ifdef _WIN32
  HANDLE m_job = nullptr;
#else
  qint64 m_pid = 0;
#endif
};

void progressLine(const QByteArray& line, const RunOptions& o) {
  static const QRegularExpression re(QStringLiteral("^(?:remote: )?([A-Za-z][A-Za-z ]*[a-z]):\\s+(\\d+)%"));
  const auto m = re.match(QString::fromUtf8(line).trimmed());
  if (m.hasMatch()) o.progress(phaseText(m.captured(1)), m.captured(2).toInt());
}

// git's own words for what failed: its fatal/error line, else the last line it wrote.
QString message(const QByteArray& err) {
  const QStringList lines = QString::fromUtf8(err).remove('\r').split('\n', Qt::SkipEmptyParts);
  for (const QString& line : lines)
    for (const char* prefix : {"fatal: ", "error: "})
      if (line.startsWith(QLatin1String(prefix))) return line.mid(int(qstrlen(prefix))).trimmed();
  return lines.isEmpty() ? QString() : lines.last().trimmed();
}

struct Tools {
  QString version, lfs, error;
};
QMutex g_toolsMutex;
QHash<QString, Tools> g_tools;  // per program (and its time stamp): git --version and git lfs version

Tools tools(const Context& c) {
  const QString key = c.program + '|' + QString::number(QFileInfo(c.program).lastModified().toMSecsSinceEpoch());
  {
    QMutexLocker lock(&g_toolsMutex);
    if (auto it = g_tools.find(key); it != g_tools.end()) return *it;
  }
  Tools t;
  const Result v = run(c, {"--version"}, RunOptions{10000});
  if (!v.ok() || !v.out.startsWith("git version ")) {
    t.error = v.ok() ? tr("%1 is not git.").arg(QDir::toNativeSeparators(c.program)) : v.error();
    return t;  // not kept: the next look tries again
  }
  t.version = QString::fromUtf8(v.out.mid(12)).trimmed();
  if (const Result l = run(c, {"lfs", "version"}, RunOptions{10000}); l.ok() && l.out.startsWith("git-lfs/"))
    t.lfs = QString::fromUtf8(l.out.mid(8)).section(' ', 0, 0).trimmed();
  QMutexLocker lock(&g_toolsMutex);
  g_tools.insert(key, t);
  return t;
}

QString readFile(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

void writeFile(const QString& path, const QString& text) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
    fail(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), f.errorString()));
}

QString quoted(const QString& path) { return '"' + QDir::fromNativeSeparators(path) + '"'; }

// The program a driver command line names: the quoted first word, or the first word.
QString commandProgram(const QString& command) {
  const QString c = command.trimmed();
  if (c.startsWith('"')) return c.mid(1, c.indexOf('"', 1) - 1);
  return c.section(' ', 0, 0);
}

const QString kAttributesNote = QStringLiteral("# OPAD documents: record-aware merges and readable diffs; never in Git LFS (the merge driver would be lost)");
const QString kIgnoreNote = QStringLiteral("# OPAD: temporary saves, merge leftovers, portable data, caches and recovery snapshots");
const Qt::CaseSensitivity kPathCase =
#if defined(_WIN32) || defined(__APPLE__)
    Qt::CaseInsensitive;
#else
    Qt::CaseSensitive;
#endif
}  // namespace

const char* const kIgnored[] = {"*.opad.tmp", "*.opad.orig", "opad-data/", "/data/cache/", "/data/recovery/", nullptr};

QString findProgram() {
  if (qEnvironmentVariableIsSet("OPAD_GIT")) return executable(qEnvironmentVariable("OPAD_GIT"));
  const QString dir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
  for (const QString& p : {dir + "/git/cmd/git.exe", dir + "/PortableGit/cmd/git.exe"})
    if (const QString f = executable(p); !f.isEmpty()) return f;
  if (const QString f = QStandardPaths::findExecutable("git"); !f.isEmpty()) return QDir::cleanPath(f);
  QStringList roots;
  for (const char* v : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"})
    if (qEnvironmentVariableIsSet(v)) roots << qEnvironmentVariable(v) + "/Git";
  const QString local = qEnvironmentVariable("LOCALAPPDATA");
  if (!local.isEmpty()) roots << local + "/Programs/Git";
  for (const QString& r : roots)
    if (const QString f = executable(r + "/cmd/git.exe"); !f.isEmpty()) return f;
  if (!local.isEmpty()) {  // GitHub Desktop's own, newest first
    const QDir desktop(local + "/GitHubDesktop");
    for (const QString& app : desktop.entryList({"app-*"}, QDir::Dirs, QDir::Name | QDir::Reversed))
      if (const QString f = executable(desktop.filePath(app + "/resources/app/git/cmd/git.exe")); !f.isEmpty()) return f;
  }
  return {};
#else
  if (const QString f = executable(dir + "/git/bin/git"); !f.isEmpty()) return f;
  if (const QString f = QStandardPaths::findExecutable("git"); !f.isEmpty()) {
#ifdef __APPLE__
    // /usr/bin/git is a shim that offers to install the developer tools when they are missing: only with them.
    if (f != "/usr/bin/git" || QFileInfo::exists("/Library/Developer/CommandLineTools/usr/bin/git") ||
        QFileInfo::exists("/Applications/Xcode.app/Contents/Developer/usr/bin/git"))
      return f;
#else
    return f;
#endif
  }
  for (const char* p : {"/opt/homebrew/bin/git", "/usr/local/bin/git", "/Library/Developer/CommandLineTools/usr/bin/git", "/usr/bin/git"})
    if (const QString f = executable(p); !f.isEmpty()) {
#ifdef __APPLE__
      if (f == "/usr/bin/git" && !QFileInfo::exists("/Library/Developer/CommandLineTools/usr/bin/git")) continue;
#endif
      return f;
    }
  return {};
#endif
}

QProcessEnvironment Context::environment(bool optionalLocks) const {
  QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
  // A hook or a terminal that started OPAD may have pointed git elsewhere.
  for (const char* k : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX", "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY",
                        "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_NAMESPACE", "GIT_QUARANTINE_PATH"})
    e.remove(QString::fromLatin1(k));
  e.insert("GIT_TERMINAL_PROMPT", "0");
  e.insert("LC_ALL", "C");
  if (!optionalLocks) e.insert("GIT_OPTIONAL_LOCKS", "0");
  return e;
}

QString Result::error() const {
  if (!started) return err.isEmpty() ? tr("git could not be started.") : tr("git could not be started: %1").arg(QString::fromUtf8(err));
  if (cancelled) return tr("Cancelled.");
  if (timedOut) return tr("git did not finish in time and was stopped.");
  if (code == 0) return {};
  const QString m = message(err);
  return m.isEmpty() ? tr("git failed (exit code %1).").arg(code) : m;
}

Result run(const Context& c, const QStringList& args, const RunOptions& o) {
  Result r;
  QElapsedTimer clock;
  clock.start();
  if (c.program.isEmpty()) {
    r.err = "git was not found";
    return r;
  }
  QProcess p;
  p.setProgram(c.program);
  p.setArguments(QStringList{"-c", "core.quotepath=off"} + args);
  if (!c.dir.isEmpty()) p.setWorkingDirectory(c.dir);
  p.setProcessEnvironment(c.environment(o.optionalLocks));
#ifndef _WIN32
  p.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
  p.start();
  if (!p.waitForStarted(15000)) {
    r.err = p.errorString().toUtf8();
    r.ms = clock.elapsed();
    return r;
  }
  r.started = true;
  Tree tree(p);
  if (!o.input.isEmpty()) p.write(o.input);
  p.closeWriteChannel();
  QElapsedTimer quiet;
  quiet.start();
  QByteArray pending;  // stderr not split into lines yet
  auto drain = [&] {
    const QByteArray out = p.readAllStandardOutput(), err = p.readAllStandardError();
    if (out.isEmpty() && err.isEmpty()) return;
    quiet.restart();
    r.out += out;
    r.err += err;
    if (!o.progress || err.isEmpty()) return;
    pending += err;
    for (;;) {  // --progress rewrites its line with \r
      const qsizetype a = pending.indexOf('\r'), b = pending.indexOf('\n');
      const qsizetype cut = a < 0 ? b : b < 0 ? a : qMin(a, b);
      if (cut < 0) break;
      progressLine(pending.left(cut), o);
      pending.remove(0, cut + 1);
    }
  };
  while (!p.waitForFinished(50)) {
    if (p.state() == QProcess::NotRunning) break;
    drain();
    if (o.cancelled && o.cancelled()) r.cancelled = true;
    else if (o.timeoutMs > 0 && clock.elapsed() > o.timeoutMs) r.timedOut = true;
    else if (o.idleMs > 0 && quiet.elapsed() > o.idleMs) r.timedOut = true;
    else continue;
    tree.kill(p);
    p.waitForFinished(5000);
    break;
  }
  drain();
  r.code = r.cancelled || r.timedOut || p.exitStatus() != QProcess::NormalExit ? -1 : p.exitCode();
  r.ms = clock.elapsed();
  return r;
}

Result check(const Context& c, const QStringList& args, const RunOptions& o) {
  Result r = run(c, args, o);
  if (!r.ok()) fail(r.error());
  return r;
}

int Status::count(char kind) const {
  return int(std::count_if(entries.begin(), entries.end(), [kind](const Entry& e) { return e.kind == kind; }));
}

Status parseStatus(const QByteArray& z) {
  Status s;
  const QList<QByteArray> fields = z.split('\0');
  for (qsizetype i = 0; i < fields.size(); ++i) {
    const QByteArray& f = fields[i];
    if (f.size() < 3) continue;
    if (f.startsWith("# ")) {
      const QByteArray head = f.mid(2);
      const qsizetype space = head.indexOf(' ');
      const QByteArray key = head.left(space), value = space < 0 ? QByteArray() : head.mid(space + 1);
      if (key == "branch.oid") s.oid = QString::fromUtf8(value);
      else if (key == "branch.head") s.branch = QString::fromUtf8(value);
      else if (key == "branch.upstream") s.upstream = QString::fromUtf8(value);
      else if (key == "branch.ab") {
        const QList<QByteArray> ab = value.split(' ');
        if (ab.size() == 2) {
          s.ahead = ab[0].mid(1).toInt();
          s.behind = ab[1].mid(1).toInt();
          s.tracking = true;
        }
      }
      continue;
    }
    auto after = [&f](int spaces) {  // the path is what follows the fixed fields; it may hold spaces itself
      qsizetype at = 0;
      for (int n = 0; n < spaces; ++n) {
        at = f.indexOf(' ', at) + 1;
        if (at <= 0) return QString();
      }
      return QString::fromUtf8(f.mid(at));
    };
    Entry e;
    e.kind = f[0];
    switch (e.kind) {
      case '1': e.path = after(8); break;
      case '2':
        e.path = after(9);
        if (i + 1 < fields.size()) e.orig = QString::fromUtf8(fields[++i]);
        break;
      case 'u': e.path = after(10); break;
      case '?':
      case '!': e.path = QString::fromUtf8(f.mid(2)); break;
      default: continue;
    }
    if (e.kind != '?' && e.kind != '!' && f.size() > 3) {
      e.x = f[2];
      e.y = f[3];
    }
    if (!e.path.isEmpty()) s.entries.push_back(e);
  }
  return s;
}

Repo::Doc Repo::doc() const {
  if (state != State::Ready || rel.isEmpty()) return Doc::None;
  for (const Entry& e : status.entries) {
    const bool inside = e.kind == '?' && e.path.endsWith('/') && rel.startsWith(e.path, kPathCase);  // an untracked folder
    if (!inside && e.path.compare(rel, kPathCase) != 0) continue;
    switch (e.kind) {
      case 'u': return Doc::Conflict;
      case '?': return Doc::Untracked;
      case '!': return Doc::Ignored;
      default: return e.x == 'A' ? Doc::Added : Doc::Modified;
    }
  }
  return ignored ? Doc::Ignored : Doc::Clean;
}

Repo::Sync Repo::sync() const {
  if (status.upstream.isEmpty()) return Sync::Local;
  if (!status.tracking) return Sync::Gone;
  if (status.ahead && status.behind) return Sync::Diverged;
  if (status.ahead) return Sync::Ahead;
  if (status.behind) return Sync::Behind;
  return Sync::Synced;
}

bool Repo::driverStale() const {
  if (state != State::Ready || !managed) return false;
  for (const QString& command : {driver, textconv})
    if (!command.isEmpty() && !QFileInfo::exists(commandProgram(command))) return true;
  return driver.isEmpty();
}

void readStatus(const Context& c, Repo& r) {
  RunOptions o;
  o.timeoutMs = 30000;
  o.optionalLocks = false;
  const Result s = run(c, {"status", "--porcelain=v2", "--branch", "-z"}, o);
  if (!s.ok()) {
    r.state = QString::fromUtf8(s.err).contains("not a git repository") ? Repo::State::NotRepo : Repo::State::Failed;
    r.error = s.error();
    return;
  }
  r.status = parseStatus(s.out);
  r.merging = QFileInfo::exists(r.gitDir + "/MERGE_HEAD");
  r.rebasing = QFileInfo::exists(r.gitDir + "/rebase-merge") || QFileInfo::exists(r.gitDir + "/rebase-apply");
}

Repo probe(const Context& base, const QString& file) {
  Repo r;
  r.file = file;
  r.program = base.program;
  if (base.program.isEmpty()) {
    r.state = Repo::State::GitMissing;
    return r;
  }
  Context c = base;
  c.dir = QFileInfo(file).absolutePath();
  const Tools t = tools(c);
  if (t.version.isEmpty()) {
    r.state = Repo::State::GitMissing;
    r.error = t.error;
    return r;
  }
  r.version = t.version;
  r.lfsVersion = t.lfs;
  RunOptions quick;
  quick.timeoutMs = 20000;
  quick.optionalLocks = false;
  const Result where = run(c, {"rev-parse", "--show-toplevel", "--absolute-git-dir", "--git-common-dir"}, quick);
  if (!where.ok()) {
    const QString err = QString::fromUtf8(where.err);
    r.state = err.contains("not a git repository") ? Repo::State::NotRepo
              : err.contains("dubious ownership")  ? Repo::State::Untrusted
                                                   : Repo::State::Failed;
    if (r.state != Repo::State::NotRepo) r.error = where.error();
    return r;
  }
  const QStringList lines = QString::fromUtf8(where.out).split('\n', Qt::SkipEmptyParts);
  if (lines.size() < 3) {
    r.state = Repo::State::Failed;
    r.error = tr("git rev-parse said: %1").arg(QString::fromUtf8(where.out).trimmed());
    return r;
  }
  r.top = QDir::cleanPath(lines[0].trimmed());
  r.gitDir = QDir::cleanPath(lines[1].trimmed());
  r.commonDir = QDir::cleanPath(QDir(c.dir).absoluteFilePath(lines[2].trimmed()));
  if (!QFileInfo(r.commonDir + "/objects").isDir()) r.commonDir = QDir::cleanPath(QDir(r.top).absoluteFilePath(lines[2].trimmed()));  // older git: relative to the top
  r.rel = QDir(QFileInfo(r.top).canonicalFilePath()).relativeFilePath(QFileInfo(file).canonicalFilePath());
  if (const Result config = run(c, {"config", "-l", "-z"}, quick); config.ok()) {
    for (const QByteArray& record : config.out.split('\0')) {
      const qsizetype nl = record.indexOf('\n');
      const QString key = QString::fromUtf8(record.left(nl < 0 ? record.size() : nl));
      const QString value = nl < 0 ? QStringLiteral("true") : QString::fromUtf8(record.mid(nl + 1));
      if (key == "credential.helper") r.helper = value;  // later ones win; an empty one clears the list
      else if (key == "user.name") r.userName = value;
      else if (key == "user.email") r.userEmail = value;
      else if (key == "core.sshcommand") r.sshCommand = value;
      else if (key == "merge.opad.driver") r.driver = value;
      else if (key == "diff.opad.textconv") r.textconv = value;
      else if (key == "opad.managed") r.managed = value == "true";
    }
  }
  const QString name = QFileInfo(file).fileName();  // pathspecs are relative to c.dir, the file's folder
  if (const Result attr = run(c, {"check-attr", "-z", "merge", "--", name}, quick); attr.ok()) {
    const QList<QByteArray> f = attr.out.split('\0');
    r.wantsDriver = f.size() >= 3 && f[2] == "opad";
  }
  r.ignored = run(c, {"check-ignore", "-q", "--", name}, quick).code == 0;
  const QString hook = readFile(r.commonDir + "/hooks/pre-push");
  r.lfsHooks = hook.contains("git lfs") || hook.contains("git-lfs");
  r.state = Repo::State::Ready;
  readStatus(c, r);
  return r;
}

void forgetTools() {
  QMutexLocker lock(&g_toolsMutex);
  g_tools.clear();
}

Install Install::here() {
  Install in;
  const QString dir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
  in.cli = dir + "/opad-cli.exe";
#else
  in.cli = dir + "/opad-cli";
#endif
  if (!QFileInfo::exists(in.cli)) in.cli.clear();
  in.app = QCoreApplication::applicationFilePath();
  return in;
}

QString Install::mergeDriver() const {
  return cli.isEmpty() ? quoted(app) + " --merge-driver %O %A %B %P" : quoted(cli) + " merge-driver %O %A %B %P";
}

QString Install::textconv() const { return cli.isEmpty() ? quoted(app) + " --textconv" : quoted(cli) + " textconv"; }

QString attributesText(const QString& existing, bool lfs) {
  QStringList lines = QString(existing).remove('\r').split('\n');
  while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
  static const QRegularExpression space(QStringLiteral("\\s+"));
  bool assets = false;
  QStringList out;
  for (const QString& line : lines) {
    const QString pattern = line.trimmed().section(space, 0, 0);
    if (pattern == "*.opad" || line == kAttributesNote) continue;  // ours: written again below, last (later lines win)
    assets = assets || pattern == "assets/**";
    out << line;
  }
  if (lfs && !assets) out << QStringLiteral("assets/** filter=lfs diff=lfs merge=lfs -text");
  out << kAttributesNote << QStringLiteral("*.opad text eol=lf merge=opad diff=opad") + (lfs || assets ? " -filter" : "");
  return out.join('\n') + '\n';
}

QString ignoreText(const QString& existing) {
  QStringList lines = QString(existing).remove('\r').split('\n');
  while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
  QStringList missing;
  for (auto p = kIgnored; *p; ++p)
    if (!lines.contains(QString::fromLatin1(*p))) missing << QString::fromLatin1(*p);
  if (missing.isEmpty()) return existing;
  if (!lines.contains(kIgnoreNote)) {
    if (!lines.isEmpty()) lines << QString();
    lines << kIgnoreNote;
  }
  return (lines + missing).join('\n') + '\n';
}

void configureDriver(const Context& c, const Install& in) {
  if (in.cli.isEmpty() && in.app.isEmpty()) fail(tr("This OPAD has no program to give git as its merge driver."));
  const std::pair<const char*, QString> config[] = {{"merge.opad.name", QStringLiteral("OPAD record-aware merge")},
                                                     {"merge.opad.driver", in.mergeDriver()},
                                                     {"diff.opad.textconv", in.textconv()},
                                                     {"opad.managed", QStringLiteral("true")}};
  for (const auto& [key, value] : config) check(c, {"config", "--local", QString::fromLatin1(key), value});
}

QStringList setUp(const Context& base, const QString& folder, const Install& in, const SetupOptions& o, const RunOptions& ro) {
  QStringList done;
  Context c = base;
  c.dir = folder;
  auto phase = [&ro](const QString& text) {
    if (ro.cancelled && ro.cancelled()) fail(tr("Cancelled."));
    if (ro.progress) ro.progress(text, -1);
  };
  QString top;
  if (const Result where = run(c, {"rev-parse", "--show-toplevel"}); where.ok()) top = QString::fromUtf8(where.out).trimmed();
  else if (!QString::fromUtf8(where.err).contains("not a git repository")) fail(where.error());
  if (top.isEmpty()) {
    if (!o.init) fail(tr("%1 is not in a git repository.").arg(QDir::toNativeSeparators(folder)));
    phase(tr("Creating the repository"));
    if (!run(c, {"init", "-b", "main"}).ok()) {  // git before 2.28 has no -b
      check(c, {"init"});
      check(c, {"symbolic-ref", "HEAD", "refs/heads/main"});
    }
    done << tr("Created a git repository in %1 (branch main).").arg(QDir::toNativeSeparators(folder));
    top = folder;
  }
  c.dir = top;
  const bool lfs = o.lfs && run(c, {"lfs", "version"}).ok();
  if (o.attributes) {
    phase(tr("Writing .gitattributes"));
    const QString path = top + "/.gitattributes", existing = readFile(path), text = attributesText(existing, lfs);
    if (text != existing) writeFile(path, text);
    done << tr(".gitattributes: .opad files merge and diff through OPAD.");
    if (lfs) done << tr(".gitattributes: files under assets/ go to Git LFS.");
  }
  if (o.ignore) {
    phase(tr("Writing .gitignore"));
    const QString path = top + "/.gitignore", existing = readFile(path), text = ignoreText(existing);
    if (text != existing) writeFile(path, text);
    done << tr(".gitignore: temporary saves, portable data, caches and recovery snapshots stay out.");
  }
  if (lfs) {
    phase(tr("Installing the Git LFS hooks"));
    const Result r = run(c, {"lfs", "install", "--local"});
    done << (r.ok() ? tr("Git LFS is set up for this clone.") : tr("Git LFS could not be set up: %1").arg(r.error()));
  } else if (o.lfs) {
    done << tr("Git LFS is not installed: big files go into the history itself.");
  }
  if (o.driver) {
    phase(tr("Setting up the OPAD merge driver"));
    configureDriver(c, in);
    done << tr("This clone merges and diffs .opad files with %1.").arg(QDir::toNativeSeparators(in.cli.isEmpty() ? in.app : in.cli));
  }
  return done;
}

Result clone(const Context& base, const QString& url, const QString& folder, const RunOptions& given) {
  RunOptions o = given;
  o.timeoutMs = 0;
  if (o.idleMs <= 0) o.idleMs = 120000;
  Context c = base;
  c.dir = QFileInfo(folder).absolutePath();
  QDir().mkpath(c.dir);
  return run(c, {"clone", "--progress", "--", url, folder}, o);
}

QString phaseText(const QString& p) {
  if (p == "Enumerating objects") return tr("Enumerating objects");
  if (p == "Counting objects") return tr("Counting objects");
  if (p == "Compressing objects") return tr("Compressing objects");
  if (p == "Writing objects") return tr("Writing objects");
  if (p == "Receiving objects") return tr("Receiving objects");
  if (p == "Resolving deltas") return tr("Resolving deltas");
  if (p == "Updating files") return tr("Updating files");
  if (p == "Checking out files") return tr("Checking out files");
  if (p == "Uploading LFS objects") return tr("Uploading LFS objects");
  if (p == "Downloading LFS objects") return tr("Downloading LFS objects");
  if (p == "Filtering content") return tr("Filtering content");
  return p;
}
}  // namespace git
