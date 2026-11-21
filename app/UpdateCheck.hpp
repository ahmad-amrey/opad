#pragma once
// Is there a newer OPAD on GitHub? Check and tell; nothing is downloaded or installed here (Download opens this
// computer's installer, or the release page, in the browser; the user installs it).
//
// The latest published release of the repository, from GitHub's REST API (anonymous: 60 requests an hour per address;
// OPAD asks at most once a day, or when the user chooses Help > Check for updates), its tag compared with this build's
// version. When the API refuses (its limit is shared by everyone behind the same address), the version is read from
// where the release page's /releases/latest redirect points, without the notes. Nothing about the user or their files
// is sent; GitHub sees the address, as with any download. HTTPS needs Qt's TLS plugin: the single-file build links
// Windows' Schannel backend, the portable folder stages it (app/CMakeLists.txt, cmake/portable_stage.cmake).
// OPAD_UPDATE_URL and OPAD_UPDATE_PAGE_URL point the two requests elsewhere (file:// or a local http:// server, for the
// bench and the tests).
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace updates {
inline constexpr const char* kLatestUrl = "https://api.github.com/repos/ahmad-amrey/opad/releases/latest";
inline constexpr const char* kLatestPageUrl = "https://github.com/ahmad-amrey/opad/releases/latest";

struct Release {
  QString version;   // the tag without its "v": "0.2.0"
  QString tag;       // "v0.2.0"
  QString name;      // "OPAD 0.2.0"
  QString notes;     // the release's description (its CHANGELOG.md section), Markdown; empty when only the page answered
  QString summary;   // a few plain sentences out of the notes for the notice (summarize)
  QString page;      // the release's page on GitHub
  QString download;  // this computer's installer in that release; empty when none fits (Download opens the page then)
  QDateTime published;
};

// The release in GitHub's "latest release" answer; empty when it is not one (a draft, a prerelease, no tag, not JSON).
// `asset` names the installer to look for among its files (installerAssetName()).
std::optional<Release> parseLatest(const QByteArray& json, const QString& asset = QString());
// What a release brings, in a line or two of plain text for the notice that offers it: the notes' opening paragraph (a
// CHANGELOG.md section starts with one or two sentences on what the release is about, docs/releasing.md), else the
// first entries of its lists ("Arabic interface; Align takes arcs; ..."), Markdown taken out, cut at about maxChars on a
// word. Empty when the notes have neither.
QString summarize(const QString& notes, int maxChars = 220);
// <0, 0, >0 as a is older than, the same as, or newer than b. A leading "v" is ignored, missing parts count as 0
// (0.2 == 0.2.0), and a "-suffix" (0.2.0-rc1) comes before the same version without one.
int compareVersions(const QString& a, const QString& b);
// Whether the automatic check is due: never checked, a day or more since, or the clock went back.
bool dueForCheck(const QDateTime& last, const QDateTime& now);

// The release file that updates this copy: on Windows the setup exe for an installed copy (or a build from source),
// the single-file exe for one that runs without installing; the dmg on an Apple-silicon Mac; the .deb on x86-64
// Debian/Ubuntu-family Linux (/etc/os-release). Empty elsewhere (an Intel Mac, other Linux: the page opens).
QString installerAssetName();
// The same choice for a given system, for tests: os "windows", "macos" or "linux", cpu as QSysInfo names it,
// windowsSingleFile: the single-file exe not installed by the setup.
QString installerAssetName(const QString& os, const QString& cpu, bool debianFamily, bool windowsSingleFile = false);
// github.com/ahmad-amrey/opad/releases/download/<tag>/<asset>: release files keep their names in every release.
QString assetDownloadUrl(const QString& tag, const QString& asset);
// The tag a ".../releases/tag/<tag>" URL points at; empty for anything else.
QString tagFromReleasePageUrl(const QUrl& url);
// `url` when it is one of this repository's release pages or downloads on github.com (https, no port or user), else
// the latest-release page: the Download and What's new buttons open only these, whatever an answer said (links inside
// the notes, which are the project's own changelog, open as they are).
QUrl safeReleaseUrl(const QString& url);

// Settings: updates/autoCheck (default on), updates/lastCheck (the last check that got an answer), updates/skipVersion.
inline constexpr const char* kAutoCheckKey = "updates/autoCheck";
inline constexpr const char* kLastCheckKey = "updates/lastCheck";
inline constexpr const char* kSkipKey = "updates/skipVersion";

// One check at a time, asynchronous (the UI thread never waits on the network).
class Checker : public QObject {
  Q_OBJECT
 public:
  explicit Checker(QString currentVersion, QObject* parent = nullptr);
  // Asks GitHub. userAsked: the user chose the command (the answers say so; a skipped version is still offered then).
  void check(bool userAsked);
  bool running() const { return m_reply != nullptr; }
  QString current() const { return m_current; }
 signals:
  void available(const updates::Release& release, bool userAsked);
  void upToDate(const QString& current, bool userAsked);
  void failed(const QString& why, bool userAsked);

 private:
  void apiAnswered();
  void askReleasePage();
  void pageAnswered();
  void finish(const Release& release);

  QString m_current;
  QNetworkAccessManager* m_net;
  QNetworkReply* m_reply = nullptr;
  QString m_apiError;  // why the API refused, said if the release page does not answer either
  bool m_userAsked = false;  // the user asked during this check (also when the automatic one was already running)
};
}  // namespace updates
