// The update check's reading of GitHub's answer and its version comparison (app/UpdateCheck.hpp); and a check against a
// file:// answer through the real network code (OPAD_UPDATE_URL), the way the app runs it.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

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

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return check::run_all(argc, argv);
}
