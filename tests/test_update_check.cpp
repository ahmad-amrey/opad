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
  CHECK(!parseLatest(R"({"tag_name":"engines-openfoam-v2512-1-windows-x64","body":"OpenFOAM for OPAD"})"));  // a solver pack
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

// The development channel's list (UpdateCheck.hpp): stable vX.Y.Z releases and prereleases tagged vA.B.C-dev.N or
// vX.Y.Z-rc.N, keyed by (version, build) from CI's marker (the version the binary says: a dev build after 0.10.0 is tagged
// v0.10.1-dev.N and marked 0.10.0); a stable release without a marker is (X.Y.Z, 0); a prerelease without one, with another
// tag, or whose tag's N is not the marker's, is left out; drafts too.
namespace {
QByteArray listEntry(const char* tag, bool pre, const char* body, const char* name = "", bool draft = false) {
  return QString(R"({"tag_name":"%1","name":"%2","body":"%3","html_url":"https://github.com/ahmad-amrey/opad/releases/tag/%1",)"
                 R"("published_at":"2026-12-01T10:00:00Z","draft":%4,"prerelease":%5,"assets":[]})")
      .arg(tag, name, body, draft ? "true" : "false", pre ? "true" : "false")
      .toUtf8();
}
QByteArray releaseList(std::initializer_list<QByteArray> entries) {
  QByteArray out = "[";
  for (const QByteArray& e : entries) out += (out.size() > 1 ? "," : "") + e;
  return out + "]";
}
}  // namespace

TEST(the_marker_names_the_build_and_version) {
  int build = 0;
  QString version;
  CHECK(parseMarker("Notes\n\n<!-- opad-build: 123; opad-version: 0.10.1 -->\n", build, version) && build == 123 && version == "0.10.1");
  CHECK(parseMarker("<!--opad-build:7;opad-version:v1.2.3-->", build, version) && build == 7 && version == "1.2.3");
  CHECK(!parseMarker("<!-- something else -->", build, version));
  CHECK(compareBuilds("0.10.1", 5, "0.10.0", 900) > 0 && compareBuilds("0.10.1", 5, "0.10.1", 4) > 0 && compareBuilds("0.10.1", 5, "0.10.1", 5) == 0);
  CHECK(displayVersion("0.10.1", 123) == "0.10.1 (build 123)" && displayVersion("0.10.1", 0) == "0.10.1");
}

TEST(the_list_keys_releases_and_development_builds) {
  const QByteArray list = releaseList({
      listEntry("v0.10.2-rc.130", true, "<!-- opad-build: 130; opad-version: 0.10.2 -->", "OPAD 0.10.2 release candidate (build 130)"),
      listEntry("v0.10.2-dev.126", true, "<!-- opad-build: 126; opad-version: 0.10.1 -->", "OPAD 0.10.2 development build 126"),
      listEntry("v0.10.1", false, "## Fixed\\n- Trim\\n\\n<!-- opad-build: 125; opad-version: 0.10.1 -->"),
      listEntry("v0.10.1-rc.125", true, "<!-- opad-build: 125; opad-version: 0.10.1 -->"),
      listEntry("v0.10.0", false, "## Added\\n- Measure"),                     // before the marker: build 0
      listEntry("v0.10.1-dev.99", true, "no marker"),                         // left out
      listEntry("v0.10.1-dev.98", true, "<!-- opad-build: 97; opad-version: 0.10.0 -->"),  // its N and the marker's differ: left out
      listEntry("dev-96", true, "<!-- opad-build: 96; opad-version: 0.10.0 -->"),         // the old tag: left out
      listEntry("v0.11.0", false, "draft", "", true),                         // a draft: left out
      listEntry("nightly", true, "<!-- opad-build: 5; opad-version: 0.1.0 -->"),          // another prerelease: left out
      // Solver packs (engines.yml): releases of their own, never OPAD versions, whatever their notes say.
      listEntry("engines-openfoam-v2512-1-windows-x64", false, "OpenFOAM v2512 for OPAD", "OpenFOAM v2512 for OPAD (Windows x64)"),
      listEntry("engines-calculix-2.22-1-linux-x64", false, "<!-- opad-build: 999; opad-version: 9.9.9 -->"),
      listEntry("engines-calculix-2.22-1-macos-arm64", true, "<!-- opad-build: 999; opad-version: 9.9.9 -->"),
  });
  const auto all = parseList(list, "OPAD-windows-x64-setup.exe");
  CHECK(all.size() == 5);
  const Release *rc = nullptr, *dev = nullptr, *stable = nullptr;
  for (const Release& r : all) {
    if (r.tag == "v0.10.2-rc.130") rc = &r;
    if (r.tag == "v0.10.2-dev.126") dev = &r;
    if (r.tag == "v0.10.0") stable = &r;
  }
  CHECK(rc && rc->development && rc->candidate && rc->version == "0.10.2" && rc->build == 130);
  CHECK(rc->download == assetDownloadUrl("v0.10.2-rc.130", "OPAD-windows-x64-setup.exe"));  // no asset listed: its address
  CHECK(dev && dev->development && !dev->candidate && dev->version == "0.10.1" && dev->build == 126);  // the marker's version, not the tag's
  CHECK(stable && !stable->development && stable->build == 0);
}

TEST(the_newest_build_above_this_one_is_offered) {
  const QByteArray list = releaseList({
      listEntry("v0.10.2-dev.126", true, "<!-- opad-build: 126; opad-version: 0.10.1 -->"),
      listEntry("v0.10.1", false, "<!-- opad-build: 125; opad-version: 0.10.1 -->"),
      listEntry("v0.10.1-rc.125", true, "<!-- opad-build: 125; opad-version: 0.10.1 -->"),
      listEntry("v0.10.0", false, "no marker"),
  });
  const auto all = parseList(list);
  auto pick = [&](const char* version, int build) { const auto r = newestAbove(all, version, build); return r ? r->tag : QString(); };
  CHECK(pick("0.10.0", 0) == "v0.10.2-dev.126");    // the release 0.10.0 (marker-less, build 0): the newest build after it
  CHECK(pick("0.10.1", 125) == "v0.10.2-dev.126");  // the release (or its candidate): the build after it
  CHECK(pick("0.10.1", 126).isEmpty());             // the newest itself: nothing
  CHECK(pick("0.10.1", 0) == "v0.10.2-dev.126");    // a local build of 0.10.1 is build 0: everything of 0.10.1 is newer
  const auto sameKey = parseList(releaseList({listEntry("v0.10.1-rc.125", true, "<!-- opad-build: 125; opad-version: 0.10.1 -->"),
                                              listEntry("v0.10.1", false, "<!-- opad-build: 125; opad-version: 0.10.1 -->")}));
  const auto chosen = newestAbove(sameKey, "0.10.0", 0);
  CHECK(chosen && chosen->tag == "v0.10.1" && !chosen->development);  // a release and its candidate: the release
}

TEST(the_development_channel_checks_the_list_and_falls_back_when_refused) {
  const QByteArray list = releaseList({listEntry("v0.10.2-rc.140", true, "<!-- opad-build: 140; opad-version: 0.10.2 -->"),
                                       listEntry("v0.10.1", false, "<!-- opad-build: 125; opad-version: 0.10.1 -->")});
  FakeGitHub github({{"GET /list", "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(list.size()) +
                                       "\r\nConnection: close\r\n\r\n" + list},
                     {"GET /limited", kRateLimited}, {"GET /api", kRateLimited}, {"HEAD /latest", kRedirectToTag}});
  qputenv("OPAD_UPDATE_LIST_URL", github.url("/list"));
  Checker checker("0.10.1");
  checker.setBuild(125);
  checker.setDevelopment(true);
  CHECK(runCheck(checker).startsWith("available 0.10.2 "));
  Checker newest("0.10.2");
  newest.setBuild(140);
  newest.setDevelopment(true);
  CHECK(runCheck(newest) == "up to date");
  // The list refused (GitHub's limit): the stable check answers, here through the release page (v0.10.0).
  qputenv("OPAD_UPDATE_LIST_URL", github.url("/limited"));
  qputenv("OPAD_UPDATE_URL", github.url("/api"));
  qputenv("OPAD_UPDATE_PAGE_URL", github.url("/latest"));
  Checker fallback("0.9.0");
  fallback.setDevelopment(true);
  CHECK(runCheck(fallback).startsWith("available 0.10.0 "));
  qunsetenv("OPAD_UPDATE_LIST_URL");
  qunsetenv("OPAD_UPDATE_URL");
  qunsetenv("OPAD_UPDATE_PAGE_URL");
}


int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return check::run_all(argc, argv);
}
