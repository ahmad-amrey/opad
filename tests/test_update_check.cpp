// The update check's reading of GitHub's answer and its version comparison (app/UpdateCheck.hpp); and a check against a
// file:// answer through the real network code (OPAD_UPDATE_URL), the way the app runs it.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <memory>

#include "UpdateCheck.hpp"
#include "check.hpp"

using namespace updates;

namespace {
QByteArray release(const char* tag, bool draft = false, bool pre = false) {
  return QString(R"({"tag_name":"%1","name":"OPAD %2","body":"## Added\n- Align","html_url":"https://github.com/x/y/releases/tag/%1",)"
                 R"("published_at":"2026-12-01T10:00:00Z","draft":%3,"prerelease":%4})")
      .arg(tag, QString(tag).mid(1), draft ? "true" : "false", pre ? "true" : "false")
      .toUtf8();
}
}  // namespace

TEST(versions_compare_by_their_numbers) {
  CHECK(compareVersions("0.2.0", "0.1.9") > 0);
  CHECK(compareVersions("v0.10.0", "0.9.0") > 0);  // numbers, not text
  CHECK(compareVersions("0.2", "0.2.0") == 0);
  CHECK(compareVersions("1.0.0", "1.0.0-rc1") > 0);  // the release after its candidate
  CHECK(compareVersions("1.0.0-rc1", "1.0.0-rc2") < 0);
  CHECK(compareVersions("0.1.0", "0.1.1") < 0);
}

TEST(latest_release_is_read_and_drafts_are_not) {
  const auto r = parseLatest(release("v0.2.0"));
  CHECK(r && r->version == "0.2.0" && r->tag == "v0.2.0" && r->name == "OPAD 0.2.0");
  CHECK(r->notes.contains("Align") && r->page.endsWith("/v0.2.0") && r->published.isValid());
  CHECK(!parseLatest(release("v0.3.0", true)));
  CHECK(!parseLatest(release("v0.3.0", false, true)));
  CHECK(!parseLatest("not json"));
  CHECK(!parseLatest(R"({"tag_name":"nightly"})"));  // not a version
}

TEST(the_daily_check_is_due_after_a_day) {
  const QDateTime now = QDateTime::fromString("2026-12-02T12:00:00Z", Qt::ISODate);
  CHECK(dueForCheck(QDateTime(), now));
  CHECK(!dueForCheck(now.addSecs(-3600), now));
  CHECK(dueForCheck(now.addSecs(-25 * 3600), now));
  CHECK(dueForCheck(now.addSecs(3600), now));  // the clock went back
}

TEST(checker_reports_a_newer_release_and_the_same_one) {
  QTemporaryDir dir;
  const QString file = dir.filePath("latest.json");
  auto answer = [&](const QByteArray& body) {
    QFile f(file);
    f.open(QIODevice::WriteOnly);
    f.write(body);
  };
  qputenv("OPAD_UPDATE_URL", QUrl::fromLocalFile(file).toString().toUtf8());
  Checker checker("0.1.0");
  QString got;
  QObject::connect(&checker, &Checker::available, [&](const Release& r, bool asked) { got = "available " + r.version + (asked ? " asked" : ""); });
  QObject::connect(&checker, &Checker::upToDate, [&](const QString& v, bool) { got = "up to date " + v; });
  QObject::connect(&checker, &Checker::failed, [&](const QString& why, bool) { got = "failed " + why; });
  auto run = [&](bool asked) {
    got.clear();
    checker.check(asked);
    QEventLoop loop;
    QElapsedTimer clock;
    clock.start();
    while (got.isEmpty() && clock.elapsed() < 5000) loop.processEvents(QEventLoop::WaitForMoreEvents, 50);
    return got;
  };
  answer(release("v0.2.0"));
  CHECK(run(true) == "available 0.2.0 asked");
  answer(release("v0.1.0"));
  CHECK(run(false) == "up to date 0.1.0");
  answer("garbage");
  CHECK(run(false).startsWith("failed"));
}

// The notice says what a release brings: the changelog section's opening sentences, else its first entries.
TEST(notes_are_summed_up_for_the_notice) {
  const QString section = "Fix release for the Arabic interface.\n\n### Fixed\n\n- **Arabic interface:** Qt's texts are Arabic now.\n";
  CHECK(summarize(section) == "Fix release for the Arabic interface.");
  const QString lists = "### Added\n\n- **Offset relationship:** curves keep their distance.\n- Align takes **flat arcs**. More text.\n"
                        "- [Rigid groups](https://x.y) move as one\n- A fourth one\n";
  CHECK(summarize(lists) == "Offset relationship; Align takes flat arcs; Rigid groups move as one");
  const QString lone = "One sentence\nwrapped over two lines with *emphasis* and `code`.\n\nAnother paragraph.";
  CHECK(summarize(lone) == "One sentence wrapped over two lines with emphasis and code.");
  const QString longer = QString("word ").repeated(100);
  const QString cut = summarize(longer, 40);
  CHECK(cut.size() <= 41 && cut.endsWith(QChar(0x2026)) && !cut.contains("  "));
  CHECK(summarize("").isEmpty());
  CHECK(summarize("## Only a heading\n---\n").isEmpty());
  const auto r = parseLatest(release("v0.2.0"));
  CHECK(r && r->summary == "Align");  // the test release's notes are "## Added / - Align"
}

// Download takes this computer's installer straight from the release: the names the Build workflow publishes.
TEST(each_system_downloads_its_own_installer) {
  CHECK(installerAssetName("windows", "x86_64", false) == "OPAD-windows-x64-setup.exe");
  CHECK(installerAssetName("windows", "arm64", false) == "OPAD-windows-x64-setup.exe");
  CHECK(installerAssetName("windows", "x86_64", false, true) == "OPAD-windows-x64.exe");  // the uninstalled single exe
  CHECK(installerAssetName("macos", "arm64", false) == "OPAD-macos.dmg");
  CHECK(installerAssetName("macos", "x86_64", false).isEmpty());  // the dmg is Apple silicon only: the page opens
  CHECK(installerAssetName("linux", "x86_64", true) == "OPAD-linux-amd64.deb");
  CHECK(installerAssetName("linux", "x86_64", false).isEmpty());  // not Debian/Ubuntu: the page (AppImage) opens
  CHECK(installerAssetName("linux", "arm64", true).isEmpty());
  CHECK(assetDownloadUrl("v0.10.0", "OPAD-macos.dmg") == "https://github.com/ahmad-amrey/opad/releases/download/v0.10.0/OPAD-macos.dmg");
  const QByteArray withAssets = R"({"tag_name":"v0.10.0","html_url":"https://github.com/ahmad-amrey/opad/releases/tag/v0.10.0","assets":[)"
                                R"({"name":"OPAD-macos.dmg","browser_download_url":"https://github.com/ahmad-amrey/opad/releases/download/v0.10.0/OPAD-macos.dmg"},)"
                                R"({"name":"OPAD-windows-x64-setup.exe","browser_download_url":"https://github.com/ahmad-amrey/opad/releases/download/v0.10.0/OPAD-windows-x64-setup.exe"}]})";
  const auto r = parseLatest(withAssets, "OPAD-windows-x64-setup.exe");
  CHECK(r && r->download.endsWith("/v0.10.0/OPAD-windows-x64-setup.exe"));
  CHECK(parseLatest(withAssets, "OPAD-linux-amd64.deb")->download.isEmpty());
  CHECK(parseLatest(withAssets)->download.isEmpty());
}

TEST(only_opads_release_addresses_are_opened) {
  const QString latest = kLatestPageUrl;
  CHECK(safeReleaseUrl("https://github.com/ahmad-amrey/opad/releases/tag/v0.10.0").toString() == "https://github.com/ahmad-amrey/opad/releases/tag/v0.10.0");
  CHECK(safeReleaseUrl("https://github.com/ahmad-amrey/opad/releases/download/v0.10.0/OPAD-macos.dmg").toString().endsWith("/OPAD-macos.dmg"));
  CHECK(safeReleaseUrl("http://github.com/ahmad-amrey/opad/releases/tag/v1").toString() == latest);  // not https
  CHECK(safeReleaseUrl("https://evil.example/ahmad-amrey/opad/releases/tag/v1").toString() == latest);
  CHECK(safeReleaseUrl("https://github.com.evil.example/ahmad-amrey/opad/releases/tag/v1").toString() == latest);
  CHECK(safeReleaseUrl("https://user@github.com/ahmad-amrey/opad/releases/tag/v1").toString() == latest);
  CHECK(safeReleaseUrl("https://github.com:8443/ahmad-amrey/opad/releases/tag/v1").toString() == latest);
  CHECK(safeReleaseUrl("https://github.com/someone/else/releases/tag/v1").toString() == latest);
  CHECK(safeReleaseUrl("file:///C:/Windows/System32/calc.exe").toString() == latest);
  CHECK(safeReleaseUrl("").toString() == latest);
  CHECK(tagFromReleasePageUrl(QUrl("https://github.com/ahmad-amrey/opad/releases/tag/v0.10.0")) == "v0.10.0");
  CHECK(tagFromReleasePageUrl(QUrl("https://github.com/ahmad-amrey/opad/releases/latest")).isEmpty());
}

namespace {
// A minimal HTTP server standing in for api.github.com and github.com: each "METHOD /path" gets a canned raw answer.
class FakeGitHub : public QObject {
 public:
  explicit FakeGitHub(QHash<QByteArray, QByteArray> routes) : m_routes(std::move(routes)) {
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        auto request = std::make_shared<QByteArray>();  // per connection: a reused socket address must not inherit one
        auto answered = std::make_shared<bool>(false);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request, answered] {
          *request += socket->readAll();
          if (*answered || !request->contains("\r\n\r\n")) return;
          *answered = true;
          const QList<QByteArray> line = request->left(request->indexOf("\r\n")).split(' ');
          socket->write(m_routes.value(line.value(0) + ' ' + line.value(1), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    m_server.listen(QHostAddress::LocalHost);
  }
  QByteArray url(const char* path) const { return QString("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path).toUtf8(); }

 private:
  QTcpServer m_server;
  QHash<QByteArray, QByteArray> m_routes;
};

const QByteArray kRateLimited = "HTTP/1.1 403 rate limit exceeded\r\nX-RateLimit-Limit: 60\r\nX-RateLimit-Remaining: 0\r\n"
                                "X-RateLimit-Reset: 4102444800\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
const QByteArray kRedirectToTag = "HTTP/1.1 302 Found\r\nLocation: https://github.com/ahmad-amrey/opad/releases/tag/v0.10.0\r\n"
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
const QByteArray kServerError = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

// Runs one check and waits for its answer: "available <version> <page> <download>", "up to date", "failed <why>".
QString runCheck(Checker& checker) {
  QString got;
  QMetaObject::Connection a = QObject::connect(&checker, &Checker::available, [&](const Release& r, bool) { got = "available " + r.version + " " + r.page + " " + r.download; });
  QMetaObject::Connection b = QObject::connect(&checker, &Checker::upToDate, [&](const QString&, bool) { got = "up to date"; });
  QMetaObject::Connection c = QObject::connect(&checker, &Checker::failed, [&](const QString& why, bool) { got = "failed " + why; });
  checker.check(false);
  QEventLoop loop;
  QElapsedTimer clock;
  clock.start();
  while (got.isEmpty() && clock.elapsed() < 5000) loop.processEvents(QEventLoop::WaitForMoreEvents, 50);
  QObject::disconnect(a);
  QObject::disconnect(b);
  QObject::disconnect(c);
  return got;
}
}  // namespace

// GitHub's API allows 60 anonymous requests an hour per address, shared by everyone behind it: when it refuses, the
// release page's /latest redirect still names the newest version.
TEST(a_rate_limited_api_falls_back_to_the_release_page) {
  FakeGitHub github({{"GET /api", kRateLimited}, {"HEAD /latest", kRedirectToTag}});
  qputenv("OPAD_UPDATE_URL", github.url("/api"));
  qputenv("OPAD_UPDATE_PAGE_URL", github.url("/latest"));
  Checker checker("0.9.0");
  const QString asset = installerAssetName();
  const QString download = asset.isEmpty() ? QString() : assetDownloadUrl("v0.10.0", asset);
  CHECK(runCheck(checker) == ("available 0.10.0 https://github.com/ahmad-amrey/opad/releases/tag/v0.10.0 " + download));
  Checker newest("0.10.0");
  CHECK(runCheck(newest) == "up to date");
}

TEST(rate_limited_everywhere_says_so) {
  FakeGitHub github({{"GET /api", kRateLimited}, {"HEAD /latest", kServerError}});
  qputenv("OPAD_UPDATE_URL", github.url("/api"));
  qputenv("OPAD_UPDATE_PAGE_URL", github.url("/latest"));
  Checker checker("0.9.0");
  const QString got = runCheck(checker);
  CHECK(got.contains("limiting update checks") && got.contains("Try again after"));
}

TEST(no_release_yet_is_up_to_date) {
  FakeGitHub github({});
  qputenv("OPAD_UPDATE_URL", github.url("/api"));
  Checker checker("0.9.0");
  CHECK(runCheck(checker) == "up to date");
  qunsetenv("OPAD_UPDATE_URL");
  qunsetenv("OPAD_UPDATE_PAGE_URL");
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return check::run_all(argc, argv);
}
