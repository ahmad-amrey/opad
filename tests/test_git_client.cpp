// GitClient (app/Git.cpp, UI-61): porcelain v2, the repository as the chip sees it in a fresh, committed, pushed and
// cloned repository, setting one up (init -b main, .gitattributes, .gitignore, LFS hooks, the managed driver) and that
// driver merging and diffing for real through opad-cli and through opad.exe alone, timeouts and Cancel ending git with
// everything it started, clone progress. UI-136: errors as sentences, safe.directory, the environment (BatchMode ssh,
// no inherited GIT_DIR), opad.exe answering git's sign-in prompts as GIT_ASKPASS, the author, Locate git, warnings
// before a push. Temporary repositories only; git's global and system config are left out.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include "Git.hpp"
#include "check.hpp"
#include "opad/core.hpp"

namespace {
QString bin(const QString& name) {
#ifdef _WIN32
  return QCoreApplication::applicationDirPath() + "/" + name + ".exe";
#else
  return QCoreApplication::applicationDirPath() + "/" + name;
#endif
}

git::Context in(const QString& dir) {
  git::Context c;
  c.program = git::findProgram();
  c.dir = dir;
  return c;
}

QByteArray git_(const QString& dir, const QStringList& args) {
  const git::Result r = git::run(in(dir), args);
  if (!r.ok()) throw check::Failure(("git " + args.join(' ') + ": " + r.error()).toStdString());
  return r.out;
}

void cli(const QStringList& args) {
  QProcess p;
  p.start(bin("opad-cli"), args);
  if (!p.waitForFinished(60000) || p.exitCode() != 0)
    throw check::Failure(("opad-cli " + args.join(' ') + ": " + QString::fromUtf8(p.readAllStandardError())).toStdString());
}

void box(const QString& doc, int x) {
  cli({"feature", doc, "--kind", "box", "--inputs", QStringLiteral(R"({"x":"%1 mm","length":"5 mm","width":"5 mm","height":"5 mm"})").arg(x)});
}

QString read(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

size_t features(const QString& doc) {
  size_t n = 0;
  for (const auto& op : opad::Document::load(doc.toStdU16String()).ops) n += op.type == "feature";
  return n;
}

git::Install fromBuild(bool cli) {
  git::Install i;
  if (cli) i.cli = bin("opad-cli");
  i.app = bin("opad");
  return i;
}

// A document in a fresh repository set up by OPAD (no LFS: the hooks would want git-lfs on every push of the tests).
QString setUpRepository(const QString& dir, const git::Install& install) {
  const QString doc = dir + "/model.opad";
  cli({"new", doc});
  git::SetupOptions o;
  o.lfs = false;
  git::setUp(in(dir), dir, install, o);
  return doc;
}
}  // namespace

TEST(parse_porcelain_v2) {
  const QByteArray z = QByteArrayLiteral(
      "# branch.oid 1234567890abcdef1234567890abcdef12345678\0"
      "# branch.head feature/x\0"
      "# branch.upstream origin/feature/x\0"
      "# branch.ab +2 -3\0"
      "1 .M N... 100644 100644 100644 aaaa bbbb dir/with space.opad\0"
      "1 A. N... 000000 100644 100644 0000 cccc new.opad\0"
      "2 R. N... 100644 100644 100644 dddd dddd R100 renamed.opad\0old name.opad\0"
      "u UU N... 100644 100644 100644 100644 e1 e2 e3 both.opad\0"
      "? untracked dir/\0"
      "? loose.txt\0");
  const git::Status s = git::parseStatus(z);
  CHECK(s.oid.startsWith("1234567"));
  CHECK_EQ(s.branch, QString("feature/x"));
  CHECK_EQ(s.upstream, QString("origin/feature/x"));
  CHECK(s.tracking && s.ahead == 2 && s.behind == 3);
  CHECK_EQ(s.entries.size(), size_t(6));
  CHECK_EQ(s.entries[0].path, QString("dir/with space.opad"));
  CHECK(s.entries[0].x == '.' && s.entries[0].y == 'M');
  CHECK(s.entries[1].x == 'A');
  CHECK(s.entries[2].kind == '2' && s.entries[2].path == "renamed.opad" && s.entries[2].orig == "old name.opad");
  CHECK(s.entries[3].kind == 'u' && s.entries[3].path == "both.opad");
  CHECK_EQ(s.entries[4].path, QString("untracked dir/"));
  CHECK_EQ(s.count('?'), 2);

  git::Repo r;
  r.state = git::Repo::State::Ready;
  r.status = s;
  for (const auto& [rel, doc] : std::vector<std::pair<QString, git::Repo::Doc>>{{"dir/with space.opad", git::Repo::Doc::Modified},
                                                                                {"new.opad", git::Repo::Doc::Added},
                                                                                {"both.opad", git::Repo::Doc::Conflict},
                                                                                {"untracked dir/deep/a.opad", git::Repo::Doc::Untracked},
                                                                                {"clean.opad", git::Repo::Doc::Clean}}) {
    r.rel = rel;
    CHECK(r.doc() == doc);
  }
  CHECK(r.sync() == git::Repo::Sync::Diverged);
  r.status.tracking = false;
  CHECK(r.sync() == git::Repo::Sync::Gone);

  const git::Status fresh = git::parseStatus(QByteArrayLiteral("# branch.oid (initial)\0# branch.head main\0? model.opad\0"));
  CHECK(fresh.oid == "(initial)" && fresh.branch == "main" && fresh.upstream.isEmpty() && !fresh.tracking);
}

TEST(attributes_and_ignore_are_idempotent) {
  const QString lfs = git::attributesText("", true);
  CHECK_EQ(lfs, git::attributesText(lfs, true));
  CHECK(lfs.indexOf("assets/** filter=lfs") >= 0 && lfs.indexOf("assets/**") < lfs.indexOf("*.opad text eol=lf merge=opad diff=opad -filter"));
  // The README's older line is replaced, other lines stay, CRLF goes; without LFS the .opad line needs no -filter.
  const QString old = git::attributesText("*.png binary\r\n*.opad text eol=lf merge=opad\r\n", false);
  CHECK(old.startsWith("*.png binary\n") && old.endsWith("*.opad text eol=lf merge=opad diff=opad\n") && !old.contains('\r'));
  CHECK_EQ(old.count("*.opad"), 1);
  CHECK(!old.contains("assets/**"));
  // A user's own LFS line for assets/ is kept, and OPAD documents in it are still kept out of LFS.
  const QString theirs = git::attributesText("assets/** filter=lfs diff=lfs merge=lfs -text\n", false);
  CHECK_EQ(theirs.count("assets/**"), 1);
  CHECK(theirs.endsWith("*.opad text eol=lf merge=opad diff=opad -filter\n"));

  const QString ignore = git::ignoreText("build/\n");
  CHECK(ignore.startsWith("build/\n\n# OPAD") && ignore.contains("*.opad.tmp\n") && ignore.contains("opad-data/\n"));
  CHECK_EQ(git::ignoreText(ignore), ignore);
  const QString crlf = QString(ignore).replace("\n", "\r\n");
  CHECK_EQ(git::ignoreText(crlf), crlf);  // nothing missing: the user's file as it was
}

TEST(fresh_repository_set_up) {
  QTemporaryDir tmp;
  const QString dir = tmp.path() + "/project";
  QDir().mkpath(dir);
  const git::Install install = fromBuild(true);
  const QString doc = dir + "/model.opad";
  cli({"new", doc});
  git::Repo r = git::probe(in(dir), doc);
  CHECK(r.state == git::Repo::State::NotRepo);
  CHECK(!r.version.isEmpty());
  git::SetupOptions o;
  QStringList phases;
  git::RunOptions ro;
  ro.progress = [&phases](const QString& phase, int) { phases << phase; };
  const QStringList done = git::setUp(in(dir), dir, install, o, ro);
  CHECK(done.size() >= 4 && phases.size() >= 4);
  r = git::probe(in(dir), doc);
  CHECK(r.state == git::Repo::State::Ready);
  CHECK(QFileInfo(r.top).canonicalFilePath() == QFileInfo(dir).canonicalFilePath());
  CHECK(r.status.branch == "main" && r.status.oid == "(initial)");  // rev-parse --abbrev-ref HEAD fails here (exit 128)
  CHECK(r.rel == "model.opad" && r.doc() == git::Repo::Doc::Untracked);
  CHECK(r.sync() == git::Repo::Sync::Local);
  CHECK(r.wantsDriver && r.managed && !r.needsDriver() && !r.driverStale());
  CHECK_EQ(r.driver, install.mergeDriver());
  CHECK_EQ(r.textconv, install.textconv());
  CHECK(r.driver.startsWith('"' + QDir::fromNativeSeparators(install.cli) + "\" merge-driver"));
  const QString attributes = read(dir + "/.gitattributes");
  CHECK(attributes.contains("*.opad text eol=lf merge=opad diff=opad"));
  CHECK_EQ(attributes.contains("assets/** filter=lfs"), !r.lfsVersion.isEmpty());
  CHECK_EQ(r.lfsHooks, !r.lfsVersion.isEmpty());
  for (auto p = git::kIgnored; *p; ++p) CHECK(read(dir + "/.gitignore").contains(QString::fromLatin1(*p) + "\n"));
  // Set up again: nothing changes, nothing fails.
  git::setUp(in(dir), dir, install, o);
  CHECK_EQ(read(dir + "/.gitattributes"), attributes);
  // The document committed: clean; a temporary save beside it is ignored, not "another change".
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "-m", "first"});
  QFile tmpSave(doc + ".tmp");
  CHECK(tmpSave.open(QIODevice::WriteOnly));
  tmpSave.close();
  r = git::probe(in(dir), doc);
  CHECK(r.doc() == git::Repo::Doc::Clean && r.status.entries.empty() && r.status.oid != "(initial)");
  box(doc, 0);
  git::readStatus(in(dir), r);
  CHECK(r.doc() == git::Repo::Doc::Modified);
}

TEST(managed_driver_merges_and_diffs) {
  for (const bool withCli : {true, false}) {  // opad-cli, or opad.exe alone (--merge-driver, --textconv)
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const git::Install install = fromBuild(withCli);
    const QString doc = setUpRepository(dir, install);
    CHECK_EQ(git::probe(in(dir), doc).driver, install.mergeDriver());
    git_(dir, {"add", "-A"});
    git_(dir, {"commit", "-q", "-m", "base"});
    git_(dir, {"switch", "-q", "-c", "a"});
    box(doc, 0);
    git_(dir, {"commit", "-q", "-am", "a box"});
    git_(dir, {"switch", "-q", "main"});
    box(doc, 20);
    git_(dir, {"commit", "-q", "-am", "another box"});
    git_(dir, {"merge", "-q", "--no-edit", "a"});  // two appended features: a text merge would conflict
    CHECK_EQ(features(doc), size_t(2));
    const QString shown = QString::fromUtf8(git_(dir, {"diff", "HEAD~1", "HEAD"}));
    CHECK(!shown.isEmpty() && !shown.contains("CASCADE") && !shown.contains("#body "));
  }
}

TEST(upstream_ahead_and_clone) {
  QTemporaryDir tmp;
  const QString dir = tmp.path() + "/work", remote = tmp.path() + "/remote.git";
  QDir().mkpath(dir);
  const git::Install install = fromBuild(true);
  const QString doc = setUpRepository(dir, install);
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "-m", "base"});
  git_(tmp.path(), {"init", "-q", "--bare", "-b", "main", remote});
  git_(dir, {"remote", "add", "origin", remote});
  git_(dir, {"push", "-q", "-u", "origin", "main"});
  git::Repo r = git::probe(in(dir), doc);
  CHECK(r.sync() == git::Repo::Sync::Synced && r.status.upstream == "origin/main");
  box(doc, 0);
  git_(dir, {"commit", "-q", "-am", "box"});
  git::readStatus(in(dir), r);
  CHECK(r.sync() == git::Repo::Sync::Ahead && r.status.ahead == 1);
  git_(dir, {"mv", "model.opad", "renamed.opad"});
  git::readStatus(in(dir), r);
  CHECK(r.status.entries.size() == 1 && r.status.entries[0].kind == '2' && r.status.entries[0].orig == "model.opad");

  // A clone has the attributes but not the driver config, which never travels: the chip offers to set it up.
  const QString copy = tmp.path() + "/copy";
  QStringList phases;
  git::RunOptions o;
  o.progress = [&phases](const QString& phase, int percent) { phases << phase + " " + QString::number(percent); };
  const git::Result cloned = git::clone(in(tmp.path()), QUrl::fromLocalFile(remote).toString(), copy, o);
  CHECK(cloned.ok());
  CHECK(!phases.isEmpty());
  r = git::probe(in(copy), copy + "/model.opad");
  CHECK(r.state == git::Repo::State::Ready && r.wantsDriver && r.needsDriver() && !r.managed);
  git::configureDriver(in(copy), install);
  r = git::probe(in(copy), copy + "/model.opad");
  CHECK(!r.needsDriver() && r.managed && r.doc() == git::Repo::Doc::Clean && r.sync() == git::Repo::Sync::Synced);
  // A managed config whose program is gone (OPAD moved) reads as stale.
  git_(copy, {"config", "merge.opad.driver", "\"C:/nowhere/opad-cli.exe\" merge-driver %O %A %B %P"});
  CHECK(git::probe(in(copy), copy + "/model.opad").driverStale());
}

TEST(timeout_and_cancel_end_the_whole_tree) {
  QTemporaryDir tmp;
  const QString marker = tmp.path() + "/marker";
  // git runs the alias through sh, which outlives git when only git is killed: the marker would appear 2 s later.
  const QStringList nap{"-c", "alias.nap=!sleep 2 && echo late > marker", "nap"};
  git::RunOptions o;
  o.timeoutMs = 400;
  QElapsedTimer clock;
  clock.start();
  git::Result r = git::run(in(tmp.path()), nap, o);
  CHECK(r.started && r.timedOut && !r.ok() && r.code == -1);
  CHECK(clock.elapsed() < 1900);
  CHECK(!r.error().isEmpty());
  o.timeoutMs = 0;
  o.cancelled = [&clock] { return clock.elapsed() > 300; };
  clock.restart();
  r = git::run(in(tmp.path()), nap, o);
  CHECK(r.cancelled && !r.ok() && clock.elapsed() < 1900);
  QThread::msleep(2600);
  CHECK(!QFileInfo::exists(marker));
  // Not found and failing: no exception, a sentence.
  git::Context none = in(tmp.path());
  none.program = tmp.path() + "/no-git.exe";
  r = git::run(none, {"--version"});
  CHECK(!r.started && !r.error().isEmpty());
  r = git::run(in(tmp.path()), {"rev-parse", "--show-toplevel"});
  CHECK(r.started && r.code == 128 && r.error() == "This folder is not in a git repository.");
  CHECK_THROWS(git::check(in(tmp.path()), {"rev-parse", "--show-toplevel"}));
}

// UI-136: what git says, as sentences; the folder a safe.directory refusal names, and trusting it.
TEST(explain_and_safe_directory) {
  CHECK(git::explain("fatal: could not read Username for 'https://github.com': terminal prompts disabled").contains("credential helper"));
  CHECK(git::explain("git@github.com: Permission denied (publickey).\r\nfatal: Could not read from remote repository.").contains("ssh-agent"));
  CHECK(git::explain("Host key verification failed.\nfatal: Could not read from remote repository.").contains("host key"));
  CHECK(git::explain("ssh: connect to host 127.0.0.1 port 1: Connection refused\nfatal: Could not read from remote repository.").contains("did not answer"));
  CHECK(git::explain("fatal: unable to access 'https://nowhere.invalid/x.git/': Could not resolve host: nowhere.invalid").contains("host name"));
  CHECK(git::explain("remote: error: GH001: Large files detected. You may want to try Git Large File Storage").contains("Git LFS"));
  CHECK(git::explain("batch response: This repository is over its data quota.").contains("quota"));
  CHECK(git::explain("fatal: Unable to create 'C:/x/.git/index.lock': File exists.").contains("index.lock"));
  CHECK(git::explain("Author identity unknown\n\n*** Please tell me who you are.").contains("name and email"));
  CHECK(git::explain(" ! [rejected]        main -> main (fetch first)\nerror: failed to push some refs").contains("pull first"));
  CHECK(git::explain("fatal: Authentication failed for 'https://example.com/x.git/'").contains("refused the sign-in"));
  CHECK_EQ(git::explain("hint: something\nfatal: something new\n"), QString("something new"));  // anything else: git's own line
  const QString owned =
      "fatal: detected dubious ownership in repository at 'D:/shared/project'\n'D:/shared/project' is owned by:\n\tBUILTIN/Administrators (S-1-5-32-544)\n"
      "but the current user is:\n\tPC/me (S-1-5-21-1)\nTo add an exception for this directory, call:\n\n\tgit config --global --add safe.directory D:/shared/project\n";
  CHECK(git::explain(owned).contains("does not trust"));
  CHECK_EQ(git::unsafeDirectory(owned), QString("D:/shared/project"));
  CHECK_EQ(git::unsafeDirectory("fatal: detected dubious ownership in repository at '//server/share/p'\n"), QString("//server/share/p"));
  QTemporaryDir tmp;
  git::trust(in(tmp.path()), "D:/shared/project");
  CHECK(QString::fromUtf8(git_(tmp.path(), {"config", "--global", "--get-all", "safe.directory"})).contains("D:/shared/project"));
}

// UI-136: the environment of every git: no terminal prompt, no inherited GIT_DIR, ssh in BatchMode unless the user set an
// ssh command, OPAD as the askpass when asked; ssh to nowhere fails at once with a sentence.
TEST(environment_and_ssh_batch_mode) {
  QTemporaryDir tmp;
  git::Context c = in(tmp.path());
  QProcessEnvironment e = c.environment(false);
  CHECK(e.value("GIT_TERMINAL_PROMPT") == "0" && e.value("GIT_OPTIONAL_LOCKS") == "0" && e.value("LC_ALL") == "C");
  CHECK(e.value("GIT_SSH_COMMAND") == "ssh -o BatchMode=yes" && !e.contains("OPAD_ASKPASS"));
  qputenv("GIT_DIR", "C:/elsewhere/.git");  // a hook that started OPAD
  CHECK(!c.environment().contains("GIT_DIR"));
  qunsetenv("GIT_DIR");
  qputenv("GIT_SSH_COMMAND", "ssh -i mykey");
  CHECK(c.environment().value("GIT_SSH_COMMAND") == "ssh -i mykey");  // the user's own
  qunsetenv("GIT_SSH_COMMAND");
  c.sshBatch = false;  // core.sshCommand set
  CHECK(!c.environment().contains("GIT_SSH_COMMAND"));
  c.askpass = bin("opad");
  e = c.environment();
  CHECK(e.value("OPAD_ASKPASS") == "1" && QDir::fromNativeSeparators(e.value("GIT_ASKPASS")) == bin("opad"));
  QElapsedTimer clock;
  clock.start();
  const git::Result r = git::run(in(tmp.path()), {"ls-remote", "ssh://git@127.0.0.1:1/none.git"}, git::RunOptions{30000});
  CHECK(r.started && !r.ok() && !r.timedOut && clock.elapsed() < 20000);
  CHECK(r.error().contains("did not answer"));
}

// UI-136: no credential helper, so git asks opad.exe (GIT_ASKPASS) for the user name and the password; it answers
// in its dialog (offscreen here, answered by OPAD_BENCH_ASKPASS) and git gets both. Without it: a sentence, no prompt.
TEST(askpass_answers_git) {
  QTemporaryDir tmp;
  const QString log = tmp.filePath("askpass.log");
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("OPAD_BENCH_ASKPASS", "s3cret");
  qputenv("OPAD_TRACE", QFile::encodeName(log));
  git::Context c = in(tmp.path());
  c.askpass = bin("opad");
  git::RunOptions o;
  o.input = "protocol=https\nhost=example.com\n\n";
  const git::Result r = git::run(c, {"credential", "fill"}, o);
  for (const char* k : {"QT_QPA_PLATFORM", "OPAD_BENCH_ASKPASS", "OPAD_TRACE"}) qunsetenv(k);
  CHECK(r.ok());
  CHECK(r.out.contains("username=s3cret\n") && r.out.contains("password=s3cret\n"));
  const QString trace = read(log);
  CHECK(trace.contains("askpass: text field") && trace.contains("askpass: password field"));
  const git::Result none = git::run(in(tmp.path()), {"credential", "fill"}, o);
  CHECK(!none.ok() && none.error().contains("credential helper"));
}

// UI-136: the author asked before the first commit lands in git's settings; Locate git keeps a program that runs as
// git (relative to OPAD's folder when inside it) and refuses anything else; a located git that went away falls back.
TEST(identity_and_locate_git) {
  QTemporaryDir tmp;
  const QString dir = tmp.path();
  git_(dir, {"init", "-q", "-b", "main"});
  git::setIdentity(in(dir), "Local Person", "local@example.com", false);
  git::Repo r = git::probe(in(dir), dir + "/model.opad");
  CHECK(r.state == git::Repo::State::Ready && r.userName == "Local Person" && r.userEmail == "local@example.com");
  git::setIdentity(in(dir), " Global Person ", "global@example.com", true);
  CHECK_EQ(QString::fromUtf8(git_(tmp.path(), {"config", "--global", "user.name"})).trimmed(), QString("Global Person"));
  const QString real = git::findProgram();
  QString version;
  CHECK(git::checkProgram(real, &version).isEmpty() && !version.isEmpty());
  CHECK(git::checkProgram(dir + "/nothing.exe").contains("not a program"));
  CHECK(!git::checkProgram(bin("opad-cli")).isEmpty());  // runs, but is not git
  QSettings().setValue("git/path", QDir(QCoreApplication::applicationDirPath()).relativeFilePath(real));
  CHECK_EQ(git::findProgram(), QDir::cleanPath(real));
  QSettings().setValue("git/path", dir + "/nothing.exe");
  CHECK_EQ(git::findProgram(), real);  // gone: the usual places again
  QSettings().remove("git/path");
  qputenv("OPAD_GIT", QFile::encodeName(dir + "/nothing.exe"));
  CHECK(git::findProgram().isEmpty());
  qunsetenv("OPAD_GIT");
  git::Context none = in(dir);
  none.program.clear();
  CHECK(git::probe(none, dir + "/model.opad").state == git::Repo::State::GitMissing);
}

// UI-136: before a push, files too big for the history itself are named and Git LFS uploads over the limit counted;
// once pushed, nothing is left to warn about.
TEST(push_warnings) {
  QTemporaryDir tmp;
  const QString dir = tmp.path() + "/work";
  QDir().mkpath(dir + "/assets");
  const QString doc = dir + "/model.opad";
  cli({"new", doc});
  git::setUp(in(dir), dir, fromBuild(true), git::SetupOptions{});
  auto noise = [](const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) throw check::Failure("cannot write " + path.toStdString());
    QByteArray bytes(8192, '\0');
    for (char& b : bytes) b = char(QRandomGenerator::global()->generate());
    f.write(bytes);
  };
  noise(dir + "/big.bin");
  noise(dir + "/assets/board.step");
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "-m", "files"});
  const bool lfs = !git::probe(in(dir), doc).lfsVersion.isEmpty();
  const QString warned = git::pushWarnings(in(dir), 4096, 1024).join('\n');
  CHECK(warned.contains("big.bin (") && warned.contains("Keep it in Git LFS"));
  CHECK_EQ(warned.contains("assets/board.step ("), !lfs);  // in LFS only a pointer goes into the history
  CHECK_EQ(warned.contains("in 1 Git LFS files go up"), lfs);
  CHECK(git::pushWarnings(in(dir)).isEmpty());  // the real limits: nothing big here
  git_(tmp.path(), {"init", "-q", "--bare", "-b", "main", tmp.path() + "/remote.git"});
  git_(dir, {"remote", "add", "origin", tmp.path() + "/remote.git"});
  git_(dir, {"push", "-q", "--no-verify", "-u", "origin", "main"});  // no LFS server here: the pre-push hook would want one
  CHECK(git::pushWarnings(in(dir), 4096, 1024).isEmpty());
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir home;
  QCoreApplication::setOrganizationName("opad-test");
  QCoreApplication::setApplicationName("git-client");
  QSettings::setDefaultFormat(QSettings::IniFormat);  // Locate git's setting in the temporary home, not the registry
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
  // Nothing from this machine's git config (identity, default branch, credential helper, a driver of its own).
  qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(home.filePath("gitconfig")));
  qputenv("GIT_CONFIG_NOSYSTEM", "1");
  for (const char* k : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"}) qputenv(k, "OPAD Test");
  for (const char* k : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"}) qputenv(k, "test@example.com");
  if (git::findProgram().isEmpty()) {
    std::printf("git not found: skipped\n");
    return 0;
  }
  return check::run_all(argc, argv);
}
