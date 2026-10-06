// GitClient (app/Git.cpp, UI-61): porcelain v2, the repository as the chip sees it in a fresh, committed, pushed and
// cloned repository, setting one up (init -b main, .gitattributes, .gitignore, LFS hooks, the managed driver) and that
// driver merging and diffing for real through opad-cli and through opad.exe alone, timeouts and Cancel ending git with
// everything it started, clone progress, a new clone set up for OPAD and its documents found. UI-136: errors as
// sentences, safe.directory, the environment (BatchMode ssh, no inherited GIT_DIR), opad.exe answering git's sign-in
// prompts as GIT_ASKPASS, the author, Locate git, warnings before a push. UI-62 against a host: Git LFS files going up
// to a remote's LFS store and down into a new clone; push, clone, fetch and pull over smart HTTP behind a sign-in (a
// host on this machine), OPAD answering git's prompts, a refused sign-in as a sentence. UI-64: who brought which op in
// which commit, read once per version (OpHistory.cpp). Temporary repositories only;
// git's global and system config are left out.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <atomic>

#include "Git.hpp"
#include "OpHistory.hpp"
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

QByteArray noise(int size) {
  QByteArray bytes(size, '\0');
  for (char& b : bytes) b = char(QRandomGenerator::global()->generate());
  return bytes;
}

// A git host on this machine: smart HTTP through git http-backend behind a Basic sign-in (`account`: "user:password"),
// served on a thread of its own, one request per connection. As a hosting service, minus TLS: the first request of a git
// command is refused (401), git asks its credential helper or askpass, and asks again signed in.
class Host {
 public:
  Host(const QString& root, const QByteArray& account) : m_root(root), m_account("Basic " + account.toBase64()) {
    m_thread = QThread::create([this] { serve(); });
    m_thread->start();
    while (m_port == 0) QThread::msleep(5);
  }
  ~Host() {
    m_stop = true;
    m_thread->wait();
    delete m_thread;
  }
  QString url(const QString& repository) const { return QStringLiteral("http://127.0.0.1:%1/%2").arg(m_port.load()).arg(repository); }
  bool listening() const { return m_port > 0; }
  std::atomic<int> refused{0}, served{0};

 private:
  void serve() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
      m_port = -1;
      return;
    }
    m_port = server.serverPort();
    while (!m_stop)
      if (server.waitForNewConnection(50))
        while (QTcpSocket* s = server.nextPendingConnection()) {
          answer(s);
          delete s;
        }
  }
  void answer(QTcpSocket* s) {
    QByteArray in;
    auto more = [&] {
      if (!s->bytesAvailable() && !s->waitForReadyRead(10000)) return false;
      in += s->readAll();
      return true;
    };
    qsizetype end;
    while ((end = in.indexOf("\r\n\r\n")) < 0)
      if (!more()) return;
    const QList<QByteArray> lines = in.left(end).split('\n');
    in.remove(0, end + 4);
    const QList<QByteArray> request = lines[0].trimmed().split(' ');
    if (request.size() < 2) return;
    QHash<QByteArray, QByteArray> h;
    for (qsizetype i = 1; i < lines.size(); ++i)
      if (const qsizetype colon = lines[i].indexOf(':'); colon > 0) h[lines[i].left(colon).trimmed().toLower()] = lines[i].mid(colon + 1).trimmed();
    if (h.value("expect").toLower() == "100-continue") {
      s->write("HTTP/1.1 100 Continue\r\n\r\n");
      s->flush();
    }
    QByteArray body;
    if (h.value("transfer-encoding").toLower() == "chunked")
      for (;;) {
        qsizetype eol;
        while ((eol = in.indexOf("\r\n")) < 0)
          if (!more()) return;
        const qsizetype size = in.left(eol).split(';')[0].trimmed().toLongLong(nullptr, 16);
        while (in.size() < eol + 2 + size + 2)
          if (!more()) return;
        body += in.mid(eol + 2, size);
        in.remove(0, eol + 2 + size + 2);
        if (size == 0) break;
      }
    else {
      const qsizetype size = h.value("content-length").toLongLong();
      while (in.size() < size)
        if (!more()) return;
      body = in.left(size);
    }
    QByteArray reply;
    if (h.value("authorization") != m_account) {
      ++refused;
      reply = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"host\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
      ++served;
      const QByteArray target = request[1];
      const qsizetype q = target.indexOf('?');
      QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
      e.insert("GIT_PROJECT_ROOT", m_root);
      e.insert("GIT_HTTP_EXPORT_ALL", "1");
      e.insert("REMOTE_USER", "s3cret");
      e.insert("REMOTE_ADDR", "127.0.0.1");
      e.insert("REQUEST_METHOD", QString::fromLatin1(request[0]));
      e.insert("PATH_INFO", QString::fromUtf8(QByteArray::fromPercentEncoding(q < 0 ? target : target.left(q))));
      e.insert("QUERY_STRING", q < 0 ? QString() : QString::fromLatin1(target.mid(q + 1)));
      e.insert("CONTENT_TYPE", QString::fromLatin1(h.value("content-type")));
      e.insert("CONTENT_LENGTH", QString::number(body.size()));
      if (h.contains("content-encoding")) e.insert("HTTP_CONTENT_ENCODING", QString::fromLatin1(h.value("content-encoding")));
      if (h.contains("git-protocol")) e.insert("GIT_PROTOCOL", QString::fromLatin1(h.value("git-protocol")));
      QProcess cgi;
      cgi.setProcessEnvironment(e);
      cgi.start(git::findProgram(), {"http-backend"});
      cgi.write(body);
      cgi.closeWriteChannel();
      cgi.waitForFinished(60000);
      const QByteArray out = cgi.readAllStandardOutput();
      const qsizetype split = out.indexOf("\r\n\r\n");
      QByteArray status = "200 OK", headers;
      for (const QByteArray& line : out.left(std::max<qsizetype>(split, 0)).split('\n'))
        if (line.trimmed().isEmpty()) continue;
        else if (line.startsWith("Status:")) status = line.mid(7).trimmed();
        else headers += line.trimmed() + "\r\n";
      const QByteArray content = split < 0 ? QByteArray() : out.mid(split + 4);
      reply = "HTTP/1.1 " + status + "\r\n" + headers + "Content-Length: " + QByteArray::number(content.size()) + "\r\nConnection: close\r\n\r\n" + content;
    }
    s->write(reply);
    while (s->bytesToWrite() && s->waitForBytesWritten(10000)) {}
    s->disconnectFromHost();
    if (s->state() != QAbstractSocket::UnconnectedState) s->waitForDisconnected(2000);
  }
  QString m_root;
  QByteArray m_account;
  QThread* m_thread = nullptr;
  std::atomic<int> m_port{0};
  std::atomic<bool> m_stop{false};
};

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
  CHECK_EQ(r.difftool, install.difftool());  // git difftool -t opad: Compare
  CHECK(r.difftool.startsWith('"' + QDir::fromNativeSeparators(install.app) + "\" --compare \"$LOCAL\" \"$REMOTE\""));
  CHECK(!git::run(in(dir), {"config", "--get", "diff.opad.cachetextconv"}).ok());  // no textconv cache: its notes ref showed in git log --all
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

// Clone repository… (UI-61): the folder name an address suggests, a new clone set up for OPAD (the driver config when the
// attributes want it, the Git LFS hooks when they use LFS) and its documents found, shallowest first.
TEST(clone_set_up_for_opad) {
  for (const auto& [url, name] : std::vector<std::pair<QString, QString>>{{"https://github.com/team/robot.git", "robot"},
                                                                         {"git@github.com:team/robot.git", "robot"},
                                                                         {"ssh://git@host:2222/team/robot/", "robot"},
                                                                         {"https://host/team/robot/.git", "robot"},
                                                                         {"C:\\work\\remote.git\\", "remote"},
                                                                         {"file:///C:/work/arm", "arm"},
                                                                         {"  ", ""}})
    CHECK_EQ(git::cloneName(url), name);
  QTemporaryDir tmp;
  const QString dir = tmp.path() + "/work", remote = tmp.path() + "/remote.git";
  QDir().mkpath(dir + "/parts/deep");
  const git::Install install = fromBuild(true);
  setUpRepository(dir, install);
  {  // assets/ in Git LFS (no LFS objects: nothing for the hooks to push)
    QFile attributes(dir + "/.gitattributes");
    const QString text = git::attributesText(read(attributes.fileName()), true);
    CHECK(attributes.open(QIODevice::WriteOnly | QIODevice::Truncate) && attributes.write(text.toUtf8()) > 0);
  }
  cli({"new", dir + "/parts/deep/b.opad"});
  cli({"new", dir + "/parts/a.opad"});
  QDir().mkpath(dir + "/.hidden");
  cli({"new", dir + "/.hidden/x.opad"});
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "-m", "base"});
  git_(tmp.path(), {"init", "-q", "--bare", "-b", "main", remote});
  git_(dir, {"remote", "add", "origin", remote});
  git_(dir, {"push", "-q", "-u", "origin", "main"});
  CHECK_EQ(git::documentsIn(dir), (QStringList{dir + "/model.opad", dir + "/parts/a.opad", dir + "/parts/deep/b.opad"}));
  CHECK_EQ(git::documentsIn(dir, 2).size(), qsizetype(2));

  // Outside any repository, with no credential helper: OPAD answers git's prompts, ssh in BatchMode.
  const git::Context c = git::contextFor(git::findProgram(), tmp.path(), bin("opad"));
  CHECK(c.askpass == bin("opad") && c.sshBatch);
  git_(tmp.path(), {"config", "--global", "credential.helper", "store"});
  git_(tmp.path(), {"config", "--global", "core.sshCommand", "ssh -i key"});
  const git::Context helped = git::contextFor(git::findProgram(), tmp.path(), bin("opad"));
  CHECK(helped.askpass.isEmpty() && !helped.sshBatch);
  git_(tmp.path(), {"config", "--global", "--unset", "credential.helper"});
  git_(tmp.path(), {"config", "--global", "--unset", "core.sshCommand"});

  const QString copy = tmp.path() + "/copy";
  CHECK(git::clone(c, QDir::toNativeSeparators(remote), copy).ok());
  CHECK(git::probe(in(copy), copy + "/parts/a.opad").needsDriver());
  QStringList phases;
  git::RunOptions o;
  o.progress = [&phases](const QString& phase, int) { phases << phase; };
  const QStringList done = git::afterClone(c, copy, install, o);
  CHECK(phases.contains("Setting up the OPAD merge driver"));
  const git::Repo r = git::probe(in(copy), copy + "/parts/deep/b.opad");
  CHECK(r.managed && !r.needsDriver() && r.driver == install.mergeDriver() && r.textconv == install.textconv());
  CHECK(r.doc() == git::Repo::Doc::Clean && r.sync() == git::Repo::Sync::Synced);
  CHECK_EQ(git::afterClone(c, copy, install), done);  // again: the same, nothing breaks
  // The attributes send assets/ to Git LFS: the clone gets the hooks, or is told LFS is missing.
  if (!r.lfsVersion.isEmpty()) CHECK(r.lfsHooks && done.contains("Git LFS is set up for this clone."));
  else CHECK(done.join('\n').contains("not installed"));

  // A repository whose attributes say nothing of OPAD: its config is left alone. Its bare remote's HEAD names a branch
  // it does not have, so git checks nothing out: its main branch is.
  const QString plain = tmp.path() + "/plain", plainRemote = tmp.path() + "/plain.git";
  QDir().mkpath(plain);
  git_(plain, {"init", "-q", "-b", "main"});
  cli({"new", plain + "/model.opad"});
  git_(plain, {"add", "-A"});
  git_(plain, {"commit", "-q", "-m", "plain"});
  git_(tmp.path(), {"init", "-q", "--bare", "-b", "nothing", plainRemote});
  git_(plain, {"push", "-q", plainRemote, "main"});
  const QString plainCopy = tmp.path() + "/plain-copy";
  CHECK(git::clone(c, plainRemote, plainCopy).ok());
  CHECK(git::documentsIn(plainCopy).isEmpty());
  CHECK_EQ(git::afterClone(c, plainCopy, install), QStringList{"The remote's HEAD names no branch it has: checked out main."});
  CHECK_EQ(git::documentsIn(plainCopy), QStringList{plainCopy + "/model.opad"});
  const git::Repo p = git::probe(in(plainCopy), plainCopy + "/model.opad");
  CHECK(!p.wantsDriver && !p.managed && p.driver.isEmpty());
  CHECK(p.status.branch == "main" && p.status.upstream == "origin/main" && p.sync() == git::Repo::Sync::Synced && p.doc() == git::Repo::Doc::Clean);
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
  git_(dir, {"push", "-q", "--no-verify", "-u", "origin", "main"});  // the LFS upload itself: lfs_push_and_clone
  CHECK(git::pushWarnings(in(dir), 4096, 1024).isEmpty());
}

// UI-62: the history of a document, branches with their upstreams, a file at a revision, the object store, branch names.
TEST(history_and_branches) {
  const auto commits = git::parseLog(QByteArrayLiteral("aaaa\x1f" "aa\x1f" "Ann\x1f" "ann@x.org\x1f" "2026-10-01T10:00:00+02:00\x1f" "p1 p2\x1f"
                                                       "HEAD -> main, origin/main, tag: v1\x1f" "Merge \x1f odd\0\n"
                                                       "bbbb\x1f" "bb\x1f" "Bob\x1f" "b@x.org\x1f" "2026-09-30T10:00:00Z\x1f\x1f\x1f" "first\0"));
  CHECK_EQ(commits.size(), size_t(2));
  CHECK(commits[0].hash == "aaaa" && commits[0].shortHash == "aa" && commits[0].author == "Ann" && commits[0].parents.size() == 2);
  CHECK(commits[0].refs == (QStringList{"HEAD -> main", "origin/main", "tag: v1"}) && commits[0].subject == "Merge \x1f odd");
  CHECK(commits[1].parents.isEmpty() && commits[1].refs.isEmpty() && commits[1].subject == "first");
  const auto parsed = git::parseBranches(QByteArrayLiteral(
      "refs/remotes/origin/HEAD\x1forigin\x1f" "cccc\x1f\x1f\x1f" "2026\x1f \x1fx\n"
      "refs/remotes/origin/main\x1forigin/main\x1f" "cccc\x1f\x1f\x1f" "2026\x1f \x1fsubject\n"
      "refs/heads/zeta\x1fzeta\x1f" "dddd\x1forigin/zeta\x1fgone\x1f" "2026\x1f \x1fz\n"
      "refs/heads/main\x1fmain\x1f" "cccc\x1forigin/main\x1f" "ahead 2, behind 3\x1f" "2026\x1f*\x1fsubject\n"));
  CHECK_EQ(parsed.size(), size_t(3));
  CHECK(parsed[0].name == "main" && parsed[0].head && parsed[0].ahead == 2 && parsed[0].behind == 3 && parsed[0].upstream == "origin/main");
  CHECK(parsed[1].name == "zeta" && parsed[1].gone && !parsed[1].head);
  CHECK(parsed[2].name == "origin/main" && parsed[2].remote);
  for (const char* good : {"main", "feature/x", "fix-1.2", "José"}) CHECK(git::validBranchName(QString::fromUtf8(good)));
  for (const char* bad : {"", "-x", "a b", "a..b", "x.lock", "a/.b", "x/", "a~1", "a:b", "@", "HEAD", "a@{1}", "q?"}) CHECK(!git::validBranchName(QString::fromUtf8(bad)));
  CHECK_EQ(git::explain("error: Your local changes to the following files would be overwritten by checkout:"),
           git::explain("error: Your local changes to the following files would be overwritten by merge:"));
  CHECK(git::explain("error: the branch 'x' is not fully merged.").contains("loses"));

  QTemporaryDir tmp;
  const QString dir = tmp.path() + "/project";
  QDir().mkpath(dir);
  const QString doc = setUpRepository(dir, fromBuild(true));
  CHECK(git::log(in(dir), {}, "model.opad", 10).empty());  // no commit yet
  CHECK(git::revParse(in(dir), "HEAD").isEmpty());
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "-m", "first"});
  box(doc, 0);
  git_(dir, {"commit", "-q", "-am", "a box"});
  QFile other(dir + "/notes.txt");
  CHECK(other.open(QIODevice::WriteOnly) && other.write("n\n") > 0);
  other.close();
  git_(dir, {"add", "notes.txt"});
  git_(dir, {"commit", "-q", "-m", "notes, not the document"});
  const auto history = git::log(in(dir), {}, "model.opad", 10);
  CHECK_EQ(history.size(), size_t(2));
  CHECK(history[0].subject == "a box" && history[1].subject == "first" && history[0].parents == QStringList{history[1].hash});
  CHECK(history[0].author == "OPAD Test" && QDateTime::fromString(history[0].date, Qt::ISODate).isValid());
  CHECK_EQ(git::log(in(dir), {}, {}, 10).size(), size_t(3));
  CHECK(git::log(in(dir), {}, {}, 1, 2)[0].subject == "first");  // paged
  CHECK(git::log(in(dir), {}, {}, 10)[0].refs.contains("HEAD -> main"));
  CHECK_EQ(git::revParse(in(dir), "HEAD~1"), history[0].hash);
  // The document as each commit has it.
  const QByteArray first = git::show(in(dir), history[1].hash, "model.opad"), now = git::show(in(dir), "HEAD", "model.opad");
  CHECK(opad::Document::parse(first.toStdString()).ops.size() + 1 == opad::Document::parse(now.toStdString()).ops.size());
  CHECK_THROWS(git::show(in(dir), history[1].hash, "notes.txt"));
  // Branches, an upstream ahead and behind, a remote's branch.
  git_(tmp.path(), {"init", "-q", "--bare", "-b", "main", tmp.path() + "/remote.git"});
  git_(dir, {"remote", "add", "origin", tmp.path() + "/remote.git"});
  CHECK(git::remotes(in(dir)) == QStringList{"origin"});
  git_(dir, {"push", "-q", "--no-verify", "-u", "origin", "main"});
  git_(dir, {"branch", "feature/x"});
  box(doc, 10);
  git_(dir, {"commit", "-q", "-am", "ahead"});
  auto list = git::branches(in(dir));
  CHECK_EQ(list.size(), size_t(3));
  CHECK(list[0].name == "feature/x" && !list[0].head && list[0].upstream.isEmpty());
  CHECK(list[1].name == "main" && list[1].head && list[1].upstream == "origin/main" && list[1].ahead == 1 && list[1].behind == 0 && list[1].subject == "ahead");
  CHECK(list[2].name == "origin/main" && list[2].remote && list[2].oid == git::revParse(in(dir), "HEAD~1"));
  const git::Objects objects = git::countObjects(in(dir));
  CHECK(objects.loose > 0 && objects.looseKiB >= 0);
  git_(dir, {"gc", "-q"});
  const git::Objects packed = git::countObjects(in(dir));
  CHECK(packed.loose < objects.loose && packed.packs >= 1);
}

// UI-64: the op records of a version up to #bodies (fed in pieces, multiline records), and the index of who brought which
// op in which commit: four commits by three authors, an edit counted per commit, a node touched by the ops that name it,
// a big version read only to its #bodies, everything from the cache the second time, an empty index before any commit.
TEST(op_history_index) {
  const std::string a = "11111111-1111-4111-8111-111111111111", b = "22222222-2222-4222-8222-222222222222", key(64, 'a');
  const std::string text = "#opad 2\n{\"uuid\":\"" + a + "\"}\n#ops\n{\"op\":\"sketch\",\"id\":\"" + a + "\",\"ts\":\"t\",\"by\":\"x\",\"geometry\":[\n" +
                           " {\"ref\":\"" + b + "\"},\n]}\n{\"op\":\"edit\",\"id\":\"" + b + "\",\"ts\":\"t\",\"by\":\"y\",\"target\":\"" + a +
                           "\",\"set\":{\"key\":\"" + key + "\"}}\n#bodies\n{\"op\":\"edit\",\"id\":\"" + a + "\"}\n";
  ophistory::Reader reader;
  for (size_t i = 0; i < text.size(); i += 7) reader.feed(std::string_view(text).substr(i, 7));
  CHECK(reader.done());
  const auto records = reader.finish();
  CHECK_EQ(records.size(), size_t(2));
  CHECK(records[0].type == "sketch" && records[0].id == a && records[0].mentions == std::vector<std::string>{b});
  CHECK(records[1].type == "edit" && records[1].target == a && records[1].mentions.empty());  // a body key is no UUID

  QTemporaryDir tmp;
  const QString dir = tmp.path() + "/project", cache = tmp.path() + "/cache";
  QDir().mkpath(dir);
  git_(dir, {"init", "-q", "-b", "main"});
  CHECK(ophistory::build(in(dir), dir, "model.opad", cache).commits.empty());  // nothing committed yet
  const QString path = dir + "/model.opad";
  opad::Document d = opad::Document::create();
  const std::string brep = "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n";
  const std::string body = opad::new_uuid(), part = d.add_body(brep, opad::json::object());
  const std::string import = d.append({{"op", "import"}, {"source", "x.step"}, {"nodes", {{{"type", "body"}, {"id", body}, {"name", "Part"}, {"key", part}}}}}).id;
  auto commit = [&](const char* author, const char* message) {
    d.save_as(path.toStdU16String());
    git_(dir, {"add", "model.opad"});
    git_(dir, {"commit", "-q", "--author", QString::fromLatin1(author), "-m", QString::fromLatin1(message)});
  };
  commit("Alice <alice@x.org>", "a part");
  const std::string rename = d.append({{"op", "rename"}, {"target", body}, {"name", "Bracket"}}).id;
  commit("Bob <bob@x.org>", "renamed");
  const std::string note = d.append({{"op", "annotation"}, {"anchor", body}, {"text", "deburr"}}).id;
  d.append({{"op", "edit"}, {"target", note}, {"set", {{"text", "deburr all"}}}});
  d.add_body(brep + std::string(1500000, ' ') + "\n", opad::json::object());  // over 1 MB: read on its own, to #bodies only
  commit("Carol <carol@x.org>", "a note");
  d.append({{"op", "edit"}, {"target", note}, {"set", {{"text", "deburr every edge"}}}});
  d.append({{"op", "edit"}, {"target", note}, {"set", {{"style", "issue"}}}});
  commit("Alice <alice@x.org>", "the note again");

  const ophistory::Index ix = ophistory::build(in(dir), dir, "model.opad", cache);
  CHECK_EQ(ix.commits.size(), size_t(4));
  CHECK(ix.commits[0].author == "Alice" && ix.commits[1].author == "Bob" && ix.commits[2].author == "Carol" && ix.commits[3].subject == "the note again");
  CHECK(ix.head == git::revParse(in(dir), "HEAD") && ix.blobs == 4 && ix.blobsRead == 4);
  CHECK(ix.bytesRead < qint64(4) * 1500000);  // the two big versions were not read to their end
  CHECK(ix.find(import)->added == 0 && ix.find(rename)->added == 1 && ix.find(note)->added == 2);
  CHECK(ix.find(note)->edits == 2 && ix.find(note)->lastEdit == 3 && ix.find(import)->edits == 0);
  CHECK(ix.touching({body}) == (std::vector<int>{0, 1, 2}));  // created, renamed, noted
  CHECK(ix.touching({note}) == (std::vector<int>{2, 3}) && ix.touching({rename, note}) == (std::vector<int>{1, 2, 3}));
  CHECK(!ix.find(opad::new_uuid()));
  const ophistory::Index again = ophistory::build(in(dir), dir, "model.opad", cache);
  CHECK(again.blobsRead == 0 && again.bytesRead == 0 && again.find(note)->lastEdit == 3);  // every version from the cache
  CHECK_EQ(QDir(cache).entryList(QDir::Files).size(), 4);
  bool stopped = false;
  QDir(cache).removeRecursively();
  CHECK(ophistory::build(in(dir), dir, "model.opad", cache, [&stopped] { return stopped = true; }).ops.empty() && stopped);  // cancelled

  // Moved with an edit, then renamed alone: the commits under the older names still count (git log --follow loses them
  // with --reverse), each version read once, the renames kept per commit.
  CHECK(ophistory::build(in(dir), dir, "model.opad", cache).blobsRead == 4);
  QDir().mkpath(dir + "/parts");
  git_(dir, {"mv", "model.opad", "parts/bracket.opad"});
  const std::string moved = d.append({{"op", "rename"}, {"target", body}, {"name", "Moved bracket"}}).id;
  d.save_as((dir + "/parts/bracket.opad").toStdU16String());
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "--author", "Dave <dave@x.org>", "-m", "moved"});
  const QString last = QString::fromUtf8("parts/bracket v2,\xc3\xbc.opad");
  git_(dir, {"mv", "parts/bracket.opad", last});
  git_(dir, {"commit", "-q", "--author", "Erin <erin@x.org>", "-m", "renamed the file"});
  const ophistory::Index ren = ophistory::build(in(dir), dir, last, cache);
  CHECK_EQ(ren.commits.size(), size_t(6));
  CHECK(ren.paths == std::vector<QString>({"model.opad", "model.opad", "model.opad", "model.opad", "parts/bracket.opad", last}));
  CHECK(ren.commits[0].author == "Alice" && ren.commits[4].author == "Dave" && ren.commits[5].author == "Erin");
  CHECK(ren.find(import)->added == 0 && ren.find(rename)->added == 1 && ren.find(note)->lastEdit == 3 && ren.find(moved)->added == 4);
  CHECK(ren.blobs == 5 && ren.blobsRead == 1);  // Erin's version is Dave's; the four before from the cache
  CHECK(ren.touching({body}) == (std::vector<int>{0, 1, 2, 4}));
  CHECK(QFileInfo::exists(cache + "/renames-" + ren.commits[5].hash) && QFileInfo::exists(cache + "/renames-" + ren.commits[4].hash));
  CHECK(!QFileInfo::exists(cache + "/renames-" + ren.commits[0].hash));  // a root commit: nothing asked
  const ophistory::Index cached = ophistory::build(in(dir), dir, last, cache);
  CHECK(cached.commits.size() == 6 && cached.blobsRead == 0 && cached.paths == ren.paths);
}

// UI-62 Push and UI-61 Clone with Git LFS: files under assets/ go up through git-lfs's pre-push hook into the remote's LFS
// store (a remote on this disk takes them as a host does), the progress names the upload, nothing is left to warn about
// once pushed, and a new clone gets them back through afterClone (pointers until then: no LFS in git's global config).
TEST(lfs_push_and_clone) {
  QTemporaryDir tmp;
  const QString dir = tmp.path() + "/work", remote = tmp.path() + "/remote.git";
  QDir().mkpath(dir + "/assets");
  cli({"new", dir + "/model.opad"});
  const git::Install install = fromBuild(true);
  git::setUp(in(dir), dir, install, git::SetupOptions{});
  if (git::probe(in(dir), dir + "/model.opad").lfsVersion.isEmpty()) {
    std::printf("git-lfs not found: lfs_push_and_clone skipped\n");
    return;
  }
  const QByteArray bytes = noise(300000);
  QFile asset(dir + "/assets/board.step");
  CHECK(asset.open(QIODevice::WriteOnly) && asset.write(bytes) == bytes.size());
  asset.close();
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "-m", "a board"});
  git_(tmp.path(), {"init", "-q", "--bare", "-b", "main", remote});
  git_(dir, {"remote", "add", "origin", remote});
  CHECK(git::pushWarnings(in(dir), 4096, 1024).join('\n').contains("in 1 Git LFS files go up"));
  QStringList phases;
  git::RunOptions o = git::RunOptions::network();
  o.progress = [&phases](const QString& phase, int) { phases << phase; };
  const git::Result pushed = git::run(in(dir), {"push", "--progress", "-u", "origin", "main"}, o);  // as VersionControl::runPush
  CHECK(pushed.ok());
  CHECK(phases.contains("Uploading LFS objects") && phases.contains("Writing objects"));
  const QString oid = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  CHECK(QFileInfo::exists(remote + "/lfs/objects/" + oid.left(2) + "/" + oid.mid(2, 2) + "/" + oid));
  CHECK(git::pushWarnings(in(dir), 4096, 1024).isEmpty());
  const QString copy = tmp.path() + "/copy";
  CHECK(git::clone(in(tmp.path()), QDir::toNativeSeparators(remote), copy, git::RunOptions::network()).ok());
  QFile pointer(copy + "/assets/board.step");
  CHECK(pointer.open(QIODevice::ReadOnly) && pointer.readAll().startsWith("version https://git-lfs.github.com/spec/v1"));
  pointer.close();
  CHECK(git::afterClone(in(copy), copy, install).contains("Git LFS is set up for this clone."));
  QFile cloned(copy + "/assets/board.step");
  CHECK(cloned.open(QIODevice::ReadOnly) && cloned.readAll() == bytes);
}

// UI-62 against a host, UI-136 sign-in: a remote behind an HTTP sign-in. Without a credential helper or askpass git
// cannot ask and says so in a sentence; with OPAD as the askpass (offscreen, answered by OPAD_BENCH_ASKPASS) push -u,
// clone, another clone's push, fetch and a fast-forward pull go through; a wrong password is refused in a sentence.
TEST(http_host_sign_in) {
  QTemporaryDir tmp;
  git_(tmp.path(), {"init", "-q", "--bare", "-b", "main", tmp.path() + "/robot.git"});
  Host host(tmp.path(), "s3cret:s3cret");
  CHECK(host.listening());
  const QString dir = tmp.path() + "/work";
  QDir().mkpath(dir);
  const git::Install install = fromBuild(true);
  const QString doc = setUpRepository(dir, install);
  git_(dir, {"add", "-A"});
  git_(dir, {"commit", "-q", "-m", "first"});
  git_(dir, {"remote", "add", "origin", host.url("robot.git")});
  const QStringList push{"push", "--progress", "-u", "origin", "main"};
  git::Result r = git::run(in(dir), push, git::RunOptions::network());
  CHECK(!r.ok() && r.error().contains("credential helper") && host.refused > 0 && host.served == 0);
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("OPAD_BENCH_ASKPASS", "s3cret");
  auto signedIn = [](const QString& folder) {
    git::Context c = in(folder);
    c.askpass = bin("opad");
    return c;
  };
  r = git::run(signedIn(dir), push, git::RunOptions::network());
  CHECK(r.ok());
  CHECK(host.served >= 2 && git::revParse(in(dir), "origin/main") == git::revParse(in(dir), "HEAD"));
  const QString copy = tmp.path() + "/copy";
  CHECK(git::clone(signedIn(tmp.path()), host.url("robot.git"), copy, git::RunOptions::network()).ok());
  git::afterClone(in(copy), copy, install);
  box(copy + "/model.opad", 20);
  git_(copy, {"commit", "-q", "-am", "a box from the other clone"});
  CHECK(git::run(signedIn(copy), {"push", "--progress"}, git::RunOptions::network()).ok());
  CHECK(git::run(signedIn(dir), {"fetch", "--progress"}, git::RunOptions::network()).ok());
  git::Repo repo = git::probe(in(dir), doc);
  CHECK(repo.sync() == git::Repo::Sync::Behind && repo.status.behind == 1);
  CHECK(git::run(signedIn(dir), {"pull", "--progress", "--no-rebase"}, git::RunOptions::network()).ok());
  CHECK_EQ(features(doc), size_t(1));
  qputenv("OPAD_BENCH_ASKPASS", "wrong");
  r = git::run(signedIn(dir), {"fetch"}, git::RunOptions::network());
  CHECK(!r.ok() && r.error().contains("refused the sign-in"));
  for (const char* k : {"QT_QPA_PLATFORM", "OPAD_BENCH_ASKPASS"}) qunsetenv(k);
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
