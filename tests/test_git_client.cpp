// GitClient (app/Git.cpp, UI-61): porcelain v2, the repository as the chip sees it in a fresh, committed, pushed and
// cloned repository, setting one up (init -b main, .gitattributes, .gitignore, LFS hooks, the managed driver) and that
// driver merging and diffing for real through opad-cli and through opad.exe alone, timeouts and Cancel ending git with
// everything it started, clone progress. Temporary repositories only; git's global and system config are left out.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
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
  CHECK(r.started && r.code == 128 && r.error().contains("not a git repository"));
  CHECK_THROWS(git::check(in(tmp.path()), {"rev-parse", "--show-toplevel"}));
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir home;
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
