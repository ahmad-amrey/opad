#include "Git.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
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
#include <QSettings>
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

QString selfFile() {
  if (QCoreApplication::instance()) return QCoreApplication::applicationFilePath();
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  return n && n < buffer.size() ? QDir::cleanPath(QString::fromWCharArray(buffer.data(), int(n))) : QString();
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  return _NSGetExecutablePath(path.data(), &size) == 0 ? QFileInfo(QString::fromUtf8(path.c_str())).canonicalFilePath() : QString();
#else
  return QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
#endif
}

QString findProgram() {
  if (qEnvironmentVariableIsSet("OPAD_GIT")) return executable(qEnvironmentVariable("OPAD_GIT"));
  const QString dir = QFileInfo(selfFile()).absolutePath();
  if (const QString located = QSettings().value("git/path").toString(); !located.isEmpty())  // Locate git…
    if (const QString f = executable(QDir(dir).absoluteFilePath(located)); !f.isEmpty()) return f;
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

QString checkProgram(const QString& path, QString* version) {
  Context c;
  c.program = executable(path);
  if (c.program.isEmpty()) return tr("%1 is not a program.").arg(QDir::toNativeSeparators(path));
  const Result v = run(c, {"--version"}, RunOptions{10000});
  if (!v.ok()) return v.error();
  if (!v.out.startsWith("git version ")) return tr("%1 is not git.").arg(QDir::toNativeSeparators(path));
  if (version) *version = QString::fromUtf8(v.out.mid(12)).trimmed();
  return {};
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
  if (!askpass.isEmpty()) {  // a dialog instead of the terminal nobody sees; OPAD_ASKPASS tells opad.exe what it is for
    e.insert("GIT_ASKPASS", QDir::toNativeSeparators(askpass));
    e.insert("OPAD_ASKPASS", "1");
  }
  // A key with a passphrase, an unknown host key: ssh would ask on a terminal and wait forever. BatchMode fails at once.
  if (sshBatch && !e.contains("GIT_SSH_COMMAND") && !e.contains("GIT_SSH")) e.insert("GIT_SSH_COMMAND", "ssh -o BatchMode=yes");
  return e;
}

QString Result::error() const {
  if (!started) return err.isEmpty() ? tr("git was not found.") : tr("git could not be started: %1").arg(QString::fromUtf8(err));
  if (cancelled) return tr("Cancelled.");
  if (timedOut) return tr("git did not finish in time and was stopped.");
  if (code == 0) return {};
  const QString m = explain(QString::fromUtf8(err.trimmed().isEmpty() ? out.left(4096) : err));  // git commit says "nothing to commit" on stdout
  return m.isEmpty() ? tr("git failed (exit code %1).").arg(code) : m;
}

Result run(const Context& c, const QStringList& args, const RunOptions& o) {
  Result r;
  QElapsedTimer clock;
  clock.start();
  if (c.program.isEmpty()) return r;
  QProcess p;
  p.setProgram(c.program);
  p.setArguments(QStringList{"-c", "core.quotepath=off"} + args);
  if (!c.dir.isEmpty()) p.setWorkingDirectory(c.dir);
  QProcessEnvironment env = c.environment(o.optionalLocks);
  // git-lfs is silent without a terminal: an upload of big assets said nothing for minutes and read as a stall (idleMs).
  if (o.progress || o.idleMs > 0) env.insert("GIT_LFS_FORCE_PROGRESS", "1");
  p.setProcessEnvironment(env);
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
  QByteArray pendingOut, pendingErr;  // not split into lines yet
  auto lines = [&o](QByteArray& pending, const QByteArray& more) {
    pending += more;
    for (;;) {  // --progress rewrites its line with \r
      const qsizetype a = pending.indexOf('\r'), b = pending.indexOf('\n');
      const qsizetype cut = a < 0 ? b : b < 0 ? a : qMin(a, b);
      if (cut < 0) break;
      progressLine(pending.left(cut), o);
      pending.remove(0, cut + 1);
    }
  };
  auto drain = [&] {
    const QByteArray out = p.readAllStandardOutput(), err = p.readAllStandardError();
    if (out.isEmpty() && err.isEmpty()) return;
    quiet.restart();
    r.out += out;
    r.err += err;
    if (!o.progress) return;
    lines(pendingErr, err);
    lines(pendingOut, out);  // git-lfs's pre-push hook writes its upload progress there
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
  for (const QString& command : {driver, textconv, difftool})
    if (!command.isEmpty() && !QFileInfo::exists(commandProgram(command))) return true;
  return driver.isEmpty() || difftool.isEmpty();  // set up before OPAD wrote the difftool: written now
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

namespace {
RunOptions quickRead() {
  RunOptions o;
  o.timeoutMs = 20000;
  o.optionalLocks = false;
  return o;
}

// Outside a repository it is the global config (the identity, a credential helper, an ssh command).
void readConfig(const Context& c, Repo& r) {
  const Result config = run(c, {"config", "-l", "-z"}, quickRead());
  if (!config.ok()) return;
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
    else if (key == "difftool.opad.cmd") r.difftool = value;
    else if (key == "opad.managed") r.managed = value == "true";
  }
}
}  // namespace

Context contextFor(const QString& program, const QString& dir, const QString& askpass) {
  Context c;
  c.program = program;
  c.dir = dir;
  Repo r;
  readConfig(c, r);
  if (r.helper.isEmpty()) c.askpass = askpass;
  c.sshBatch = r.sshCommand.isEmpty();
  return c;
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
  const RunOptions quick = quickRead();
  readConfig(c, r);
  const Result where = run(c, {"rev-parse", "--show-toplevel", "--absolute-git-dir", "--git-common-dir"}, quick);
  if (!where.ok()) {
    const QString err = QString::fromUtf8(where.err);
    r.state = err.contains("not a git repository") ? Repo::State::NotRepo
              : err.contains("dubious ownership")  ? Repo::State::Untrusted
                                                   : Repo::State::Failed;
    if (r.state != Repo::State::NotRepo) r.error = where.error();
    if (r.state == Repo::State::Untrusted) r.unsafe = unsafeDirectory(err);
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
  const QString self = selfFile(), dir = QFileInfo(self).absolutePath();
#ifdef _WIN32
  const QString suffix = QStringLiteral(".exe");
#else
  const QString suffix;
#endif
  in.cli = dir + "/opad-cli" + suffix;
  if (!QFileInfo::exists(in.cli)) in.cli.clear();
  in.app = self;
  if (QFileInfo(self).completeBaseName().startsWith(QLatin1String("opad-cli"))) {  // opad-cli's own git tools (GitAgent.cpp)
    in.cli = self;
    in.app = QFileInfo::exists(dir + "/opad" + suffix) ? dir + "/opad" + suffix : QString();
  }
  return in;
}

QString Install::mergeDriver() const {
  return cli.isEmpty() ? quoted(app) + " --merge-driver %O %A %B %P" : quoted(cli) + " merge-driver %O %A %B %P";
}

QString Install::textconv() const { return cli.isEmpty() ? quoted(app) + " --textconv" : quoted(cli) + " textconv"; }

QString Install::difftool() const { return app.isEmpty() ? QString() : quoted(app) + " --compare \"$LOCAL\" \"$REMOTE\""; }

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
                                                     {"difftool.opad.cmd", in.difftool()},
                                                     {"opad.managed", QStringLiteral("true")}};
  for (const auto& [key, value] : config)
    if (!value.isEmpty()) check(c, {"config", "--local", QString::fromLatin1(key), value});
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
    const QString branch = validBranchName(o.branch) ? o.branch : QStringLiteral("main");
    if (!run(c, {"init", "-b", branch}).ok()) {  // git before 2.28 has no -b
      check(c, {"init"});
      check(c, {"symbolic-ref", "HEAD", "refs/heads/" + branch});
    }
    done << (branch == "main" ? tr("Created a git repository in %1 (branch main).").arg(QDir::toNativeSeparators(folder))
                              : tr("Created a git repository in %1 (branch %2).").arg(QDir::toNativeSeparators(folder), branch));
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
  if (o.idleMs <= 0) o.idleMs = RunOptions::network().idleMs;
  Context c = base;
  c.dir = QFileInfo(folder).absolutePath();
  QDir().mkpath(c.dir);
  return run(c, {"clone", "--progress", "--", url, folder}, o);
}

QString cloneName(const QString& url) {
  QString s = url.trimmed().replace('\\', '/');  // a Windows path names its folder on every system
  auto trim = [&s] {
    while (s.endsWith('/')) s.chop(1);
  };
  trim();
  if (s.endsWith(".git", Qt::CaseInsensitive)) s.chop(4);  // "x.git", "x/.git"
  trim();
  static const QRegularExpression cut(QStringLiteral("[/:]")), bad(QStringLiteral("[<>\"|?*\\x00-\\x1f]"));
  s = s.section(cut, -1).remove(bad).trimmed();
  return s == "." || s == ".." ? QString() : s;
}

QStringList afterClone(const Context& base, const QString& folder, const Install& in, const RunOptions& ro) {
  Context c = base;
  c.dir = folder;
  QStringList done;
  auto phase = [&ro](const QString& text) {
    if (ro.cancelled && ro.cancelled()) fail(tr("Cancelled."));
    if (ro.progress) ro.progress(text, -1);
  };
  // A remote whose HEAD names a branch it does not have (a bare repository made with another default branch) leaves
  // the clone empty: its main branch then, or its only one.
  if (!run(c, {"rev-parse", "-q", "--verify", "HEAD"}, quickRead()).ok()) {
    QStringList branches =
        QString::fromUtf8(run(c, {"for-each-ref", "--format=%(refname:lstrip=3)", "refs/remotes/origin"}, quickRead()).out).split('\n', Qt::SkipEmptyParts);
    branches.removeAll(QStringLiteral("HEAD"));
    QString pick = branches.size() == 1 ? branches.front() : QString();
    for (const char* b : {"main", "master", "trunk"})
      if (branches.contains(QLatin1String(b))) {
        pick = QString::fromLatin1(b);
        break;
      }
    if (!pick.isEmpty()) {
      phase(tr("Checking out %1").arg(pick));
      RunOptions o;
      o.timeoutMs = 0;
      o.cancelled = ro.cancelled;
      check(c, {"checkout", "-q", "-b", pick, "--track", "origin/" + pick}, o);
      done << tr("The remote's HEAD names no branch it has: checked out %1.").arg(pick);
    }
  }
  QStringList paths;  // the documents (attributes may differ per folder), else any .opad at the top
  for (const QString& d : documentsIn(folder)) paths << QDir(folder).relativeFilePath(d);
  if (paths.isEmpty()) paths << QStringLiteral("model.opad");
  bool wants = false;
  if (const Result attr = run(c, QStringList{"check-attr", "-z", "merge", "--"} + paths, quickRead()); attr.ok()) {
    const QList<QByteArray> f = attr.out.split('\0');
    for (qsizetype i = 2; i < f.size(); i += 3) wants = wants || f[i] == "opad";
  }
  if (wants) {
    phase(tr("Setting up the OPAD merge driver"));
    configureDriver(c, in);
    done << tr("This clone merges and diffs .opad files with %1.").arg(QDir::toNativeSeparators(in.cli.isEmpty() ? in.app : in.cli));
  }
  // Attribute files anywhere in the tree that send something to Git LFS.
  if (run(c, {"grep", "-q", "-e", "filter=lfs", "--", ":(glob)**/.gitattributes"}, quickRead()).code != 0) return done;
  if (!run(c, {"lfs", "version"}, quickRead()).ok()) {
    done << tr("This repository keeps big files in Git LFS, which is not installed: they stay small pointer files until it is.");
    return done;
  }
  phase(tr("Installing the Git LFS hooks"));
  check(c, {"lfs", "install", "--local"});
  phase(tr("Downloading LFS objects"));
  RunOptions o = ro;  // the files themselves come now: what the checkout left as pointers when LFS was not set up
  o.timeoutMs = 0;
  if (o.idleMs <= 0) o.idleMs = RunOptions::network().idleMs;
  const Result pull = run(c, {"lfs", "pull"}, o);
  done << (pull.ok() ? tr("Git LFS is set up for this clone.") : tr("Git LFS files could not be downloaded: %1").arg(pull.error()));
  return done;
}

QStringList documentsIn(const QString& folder, int limit) {
  QStringList out, level{folder};
  for (int depth = 0; depth < 8 && !level.isEmpty() && out.size() < limit; ++depth) {
    QStringList next, here;
    for (const QString& dir : level) {
      const QDir d(dir);
      for (const QFileInfo& f : d.entryInfoList({"*.opad"}, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase)) here << f.absoluteFilePath();
      for (const QFileInfo& f : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase))
        if (!f.fileName().startsWith('.')) next << f.absoluteFilePath();
    }
    std::sort(here.begin(), here.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    out += here.mid(0, limit - out.size());
    level = next;
  }
  return out;
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
QString explain(const QString& text) {
  auto has = [&text](const char* s) { return text.contains(QLatin1String(s), Qt::CaseInsensitive); };
  if (has("dubious ownership"))
    return tr("Git does not trust this folder: it belongs to another user account, and its settings could run commands as you. Trust it only if you know where it came from.");
  if (has("terminal prompts disabled") || has("could not read Username") || has("could not read Password"))
    return tr("The remote wants a user name and a password, and none was given: set up a credential helper (Git Credential Manager) or answer when OPAD asks.");
  if (has("Authentication failed") || has("HTTP Basic: Access denied") || has("Invalid username or password") || has("returned error: 401") ||
      has("returned error: 403"))
    return tr("The remote refused the sign-in: check the user name and the password or access token.");
  if (has("Permission denied (publickey") || has("Permission denied, please try again"))
    return tr("The SSH server refused the key. OPAD cannot type a key's passphrase: load the key into ssh-agent or Pageant, or use the remote's HTTPS address.");
  if (has("Host key verification failed"))
    return tr("This computer does not know the SSH server's host key yet: connect once from a terminal (ssh -T and the host) and accept it.");
  if (has("Could not resolve host")) return tr("The remote's host name could not be found: check the address and the network.");
  if (has("Connection refused") || has("Connection timed out") || has("Failed to connect") || has("Operation timed out") || has("Network is unreachable"))
    return tr("The remote did not answer: check the address, the network, a proxy or a VPN.");
  if (has("Repository not found") || has("does not appear to be a git repository"))
    return tr("The remote repository was not found, or this account cannot see it.");
  if (has("index.lock"))
    return tr("Another git command is running in this repository, or one stopped halfway and left .git/index.lock behind: wait, or delete that file when no git runs.");
  if (has("Please tell me who you are") || has("Author identity unknown") || has("empty ident name"))
    return tr("Git does not know who you are yet: set your name and email address first.");
  if (has("non-fast-forward") || (has("[rejected]") && has("fetch first")))
    return tr("The remote has commits this clone does not have yet: pull first, then push.");
  if (has("GH001") || has("exceeds GitHub's file size limit") || has("file size limit"))
    return tr("The remote refuses files this large: keep them in Git LFS.");
  if (has("over its data quota") || has("LFS budget") || has("bandwidth quota") || has("exceeded its LFS"))
    return tr("The remote's Git LFS quota is used up: big files cannot go up or come down until it is raised.");
  if (has("'lfs' is not a git command") || has("git-lfs: command not found") || has("git-lfs was not found"))
    return tr("Git LFS is not installed: install it (git-lfs.com) to work with files kept in LFS.");
  // UI-62: branches, merges and commits
  if (has("would be overwritten by") || has("Please commit your changes or stash them"))
    return tr("Files with uncommitted changes would be overwritten: commit them first.");
  if (has("not fully merged")) return tr("The branch has commits that no other branch holds: deleting it loses them.");
  if (has("cannot delete branch") && (has("checked out at") || has("used by worktree")))
    return tr("That branch is checked out: switch to another one first.");
  if (has("a branch named") && has("already exists")) return tr("A branch of that name exists already.");
  if (has("is not a valid branch name")) return tr("That is not a name git takes for a branch: no spaces, no backslash, none of ~ ^ : ? * [ and no two dots.");
  if (has("refusing to merge unrelated histories")) return tr("The two branches have no commit in common.");
  if (has("Automatic merge failed") || has("CONFLICT (") || has("OPAD merge conflict"))
    return tr("The merge stopped on changes both sides made to the same things: abort it, or resolve them and commit.");
  if (has("You have not concluded your merge") || has("MERGE_HEAD exists") || has("merging is not possible"))
    return tr("A merge is under way: commit it or abort it first.");
  if (has("cannot do a partial commit during a merge")) return tr("A merge is under way: it is committed as a whole.");
  if (has("has no upstream branch") || has("no tracking information")) return tr("This branch follows no remote branch yet: push it first.");
  if (has("No configured push destination") || has("No such remote")) return tr("This repository has no remote to push to yet.");
  if (has("nothing to commit") || has("no changes added to commit")) return tr("Nothing to commit: the files are as committed.");
  if (has("not a git repository")) return tr("This folder is not in a git repository.");
  return message(text.toUtf8());
}

QString unsafeDirectory(const QString& text) {
  static const QRegularExpression add(QStringLiteral("safe\\.directory\\s+(\\S[^\\r\\n]*)")), at(QStringLiteral("repository at '([^']+)'"));
  if (const auto m = add.match(text); m.hasMatch()) return m.captured(1).trimmed();
  if (const auto m = at.match(text); m.hasMatch()) return m.captured(1);
  return {};
}

void trust(const Context& c, const QString& folder) { check(c, {"config", "--global", "--add", "safe.directory", QDir::fromNativeSeparators(folder)}); }

void setIdentity(const Context& c, const QString& name, const QString& email, bool global) {
  const QString scope = global ? QStringLiteral("--global") : QStringLiteral("--local");
  check(c, {"config", scope, "user.name", name.trimmed()});
  check(c, {"config", scope, "user.email", email.trimmed()});
}

const char* const kLogFormat = "%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%P%x1f%D%x1f%s";

std::vector<Commit> parseLog(const QByteArray& z) {
  std::vector<Commit> out;
  for (const QByteArray& record : z.split('\0')) {
    const QList<QByteArray> f = record.trimmed().split('\x1f');
    if (f.size() < 8 || f[0].isEmpty()) continue;
    Commit c;
    c.hash = QString::fromLatin1(f[0]);
    c.shortHash = QString::fromLatin1(f[1]);
    c.author = QString::fromUtf8(f[2]);
    c.email = QString::fromUtf8(f[3]);
    c.date = QString::fromLatin1(f[4]);
    c.parents = QString::fromLatin1(f[5]).split(' ', Qt::SkipEmptyParts);
    for (const QString& r : QString::fromUtf8(f[6]).split(QStringLiteral(", "), Qt::SkipEmptyParts)) c.refs << r.trimmed();
    c.subject = QString::fromUtf8(f.mid(7).join('\x1f'));
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<Commit> log(const Context& c, const QString& range, const QString& path, int count, int skip) {
  QStringList args{"log", "-z", QStringLiteral("--format=") + QString::fromLatin1(kLogFormat), "-n", QString::number(count)};
  if (skip > 0) args << "--skip" << QString::number(skip);
  if (!range.isEmpty()) args << range;
  args << "--";
  if (!path.isEmpty()) args << path;
  RunOptions o;
  o.timeoutMs = 60000;
  o.optionalLocks = false;
  const Result r = run(c, args, o);
  if (!r.ok()) {
    const QString err = QString::fromUtf8(r.err);
    if (err.contains("does not have any commits") || err.contains("bad default revision")) return {};
    fail(r.error());
  }
  return parseLog(r.out);
}

const char* const kBranchFormat =
    "%(refname)%1f%(refname:short)%1f%(objectname)%1f%(upstream:short)%1f%(upstream:track,nobracket)%1f%(committerdate:iso-strict)%1f%(HEAD)%1f%(subject)";

std::vector<Branch> parseBranches(const QByteArray& out) {
  std::vector<Branch> local, remote;
  static const QRegularExpression ahead(QStringLiteral("ahead (\\d+)")), behind(QStringLiteral("behind (\\d+)"));
  for (const QByteArray& line : out.split('\n')) {
    const QList<QByteArray> f = line.split('\x1f');
    if (f.size() < 8) continue;
    Branch b;
    b.ref = QString::fromUtf8(f[0]);
    if (b.ref.endsWith("/HEAD") && b.ref.startsWith("refs/remotes/")) continue;  // origin/HEAD: an alias
    b.remote = b.ref.startsWith("refs/remotes/");
    if (!b.remote && !b.ref.startsWith("refs/heads/")) continue;
    b.name = b.remote ? b.ref.mid(13) : b.ref.mid(11);  // refname:short drops "heads/" only when it is not ambiguous
    b.oid = QString::fromLatin1(f[2]);
    b.upstream = QString::fromUtf8(f[3]);
    const QString track = QString::fromUtf8(f[4]);
    b.gone = track == "gone";
    if (const auto m = ahead.match(track); m.hasMatch()) b.ahead = m.captured(1).toInt();
    if (const auto m = behind.match(track); m.hasMatch()) b.behind = m.captured(1).toInt();
    b.date = QString::fromLatin1(f[5]);
    b.head = f[6].trimmed() == "*";
    b.subject = QString::fromUtf8(f.mid(7).join('\x1f')).trimmed();
    (b.remote ? remote : local).push_back(std::move(b));
  }
  auto byName = [](const Branch& a, const Branch& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; };
  std::sort(local.begin(), local.end(), byName);
  std::sort(remote.begin(), remote.end(), byName);
  local.insert(local.end(), std::make_move_iterator(remote.begin()), std::make_move_iterator(remote.end()));
  return local;
}

std::vector<Branch> branches(const Context& c) {
  RunOptions o;
  o.timeoutMs = 30000;
  o.optionalLocks = false;
  return parseBranches(check(c, {"for-each-ref", QStringLiteral("--format=") + QString::fromLatin1(kBranchFormat), "refs/heads", "refs/remotes"}, o).out);
}

QStringList remotes(const Context& c) {
  RunOptions o;
  o.timeoutMs = 20000;
  o.optionalLocks = false;
  return QString::fromUtf8(check(c, {"remote"}, o).out).split('\n', Qt::SkipEmptyParts);
}

QByteArray show(const Context& c, const QString& rev, const QString& path) {
  RunOptions o;
  o.timeoutMs = 300000;  // a big document
  o.optionalLocks = false;
  const Result r = run(c, {"cat-file", "blob", rev + ':' + path}, o);
  if (!r.ok()) fail(tr("%1 has no %2.").arg(rev.left(12), path));
  return r.out;
}

QString revParse(const Context& c, const QString& rev) {
  RunOptions o;
  o.timeoutMs = 20000;
  o.optionalLocks = false;
  const Result r = run(c, {"rev-parse", "-q", "--verify", rev + "^{commit}"}, o);
  return r.ok() ? QString::fromLatin1(r.out).trimmed() : QString();
}

Objects countObjects(const Context& c) {
  RunOptions o;
  o.timeoutMs = 60000;
  o.optionalLocks = false;
  Objects out;
  for (const QByteArray& line : check(c, {"count-objects", "-v"}, o).out.split('\n')) {
    const qsizetype colon = line.indexOf(':');
    if (colon < 0) continue;
    const QByteArray key = line.left(colon).trimmed();
    const qint64 value = line.mid(colon + 1).trimmed().toLongLong();
    if (key == "count") out.loose = value;
    else if (key == "size") out.looseKiB = value;
    else if (key == "packs") out.packs = value;
    else if (key == "size-pack") out.packKiB = value;
  }
  return out;
}

bool validBranchName(const QString& name) {
  if (name.isEmpty() || name.startsWith('-') || name.startsWith('/') || name.endsWith('/') || name.endsWith('.') || name.endsWith(".lock") ||
      name == "@" || name == "HEAD" || name.contains("..") || name.contains("@{") || name.contains("//"))
    return false;
  for (const QChar ch : name)
    if (ch.unicode() < 0x20 || ch.unicode() == 0x7f || QStringLiteral(" ~^:?*[\\").contains(ch)) return false;
  for (const QString& part : name.split('/'))
    if (part.startsWith('.') || part.endsWith(".lock")) return false;
  return true;
}

QStringList pushWarnings(const Context& c, qint64 fileLimit, qint64 lfsLimit) {
  RunOptions o;
  o.timeoutMs = 120000;
  const Result objects = run(c, {"rev-list", "--objects", "HEAD", "--not", "--remotes"}, o);
  if (!objects.ok()) return {};  // no commit yet: nothing goes up
  QHash<QByteArray, QString> paths;  // object -> the path it is at
  QByteArray ids;
  for (const QByteArray& line : objects.out.split('\n')) {
    const qsizetype space = line.indexOf(' ');
    if (space <= 0) continue;  // commits have no path
    paths.insert(line.left(space), QString::fromUtf8(line.mid(space + 1)));
    ids += line.left(space) + '\n';
  }
  if (ids.isEmpty()) return {};
  o.input = ids;
  const Result sizes = run(c, {"cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)"}, o);
  if (!sizes.ok()) return {};
  QStringList out;
  QByteArray small;  // blobs that may be Git LFS pointers
  for (const QByteArray& line : sizes.out.split('\n')) {
    const QList<QByteArray> f = line.split(' ');
    if (f.size() != 3 || f[1] != "blob") continue;
    const qint64 size = f[2].toLongLong();
    if (size > fileLimit)
      out << tr("%1 (%2 MB) goes into the history itself: hosting services refuse files over 100 MB. Keep it in Git LFS.")
                 .arg(paths.value(f[0]))
                 .arg(double(size) / (1 << 20), 0, 'f', 1);
    else if (size < 1024)
      small += f[0] + '\n';
  }
  qint64 lfs = 0;
  int files = 0;
  if (!small.isEmpty()) {
    o.input = small;
    static const QRegularExpression pointer(QStringLiteral("^version https://git-lfs\\.github\\.com/spec/v1\\noid sha256:[0-9a-f]+\\nsize (\\d+)"));
    const Result blobs = run(c, {"cat-file", "--batch"}, o);
    for (qsizetype at = 0; blobs.ok() && at < blobs.out.size();) {  // "<oid> blob <size>\n<content>\n"
      const qsizetype nl = blobs.out.indexOf('\n', at);
      if (nl < 0) break;
      const qint64 size = blobs.out.mid(at, nl - at).split(' ').value(2).toLongLong();
      const auto m = pointer.match(QString::fromUtf8(blobs.out.mid(nl + 1, size)));
      if (m.hasMatch()) {
        lfs += m.captured(1).toLongLong();
        ++files;
      }
      at = nl + 1 + size + 1;
    }
  }
  if (files && lfs > lfsLimit)
    out << tr("%1 MB in %2 Git LFS files go up: Git LFS storage and bandwidth count against the remote's quota (1 GB on GitHub's free plan).")
               .arg(double(lfs) / (1 << 20), 0, 'f', 1)
               .arg(files);
  return out;
}
}  // namespace git
