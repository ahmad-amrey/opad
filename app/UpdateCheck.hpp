#pragma once
// Is there a newer OPAD on GitHub? Step 1 of updates: check and tell; nothing is downloaded or installed here.
//
// The latest published release of the repository, from GitHub's REST API (anonymous: 60 requests an hour per address;
// OPAD asks at most once a day, or when the user chooses Help > Check for updates), its tag compared with this build's
// version. Nothing about the user or their files is sent; GitHub sees the address, as with any download. HTTPS needs Qt's
// TLS plugin: the single-file build links Windows' Schannel backend, the portable folder stages it (app/CMakeLists.txt,
// cmake/portable_stage.cmake). OPAD_UPDATE_URL points the check elsewhere (a file:// JSON for the bench and the tests).
#include <QDateTime>
#include <QObject>
#include <QString>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace updates {
inline constexpr const char* kLatestUrl = "https://api.github.com/repos/ahmad-amrey/opad/releases/latest";

struct Release {
  QString version;  // the tag without its "v": "0.2.0"
  QString tag;      // "v0.2.0"
  QString name;     // "OPAD 0.2.0"
  QString notes;    // the release's description (its CHANGELOG.md section), Markdown
  QString page;     // the release's page on GitHub
  QDateTime published;
};

// The release in GitHub's "latest release" answer; empty when it is not one (a draft, a prerelease, no tag, not JSON).
std::optional<Release> parseLatest(const QByteArray& json);
// <0, 0, >0 as a is older than, the same as, or newer than b. A leading "v" is ignored, missing parts count as 0
// (0.2 == 0.2.0), and a "-suffix" (0.2.0-rc1) comes before the same version without one.
int compareVersions(const QString& a, const QString& b);
// Whether the automatic check is due: never checked, a day or more since, or the clock went back.
bool dueForCheck(const QDateTime& last, const QDateTime& now);

// Settings: updates/autoCheck (default on), updates/lastCheck, updates/skipVersion.
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
  QString m_current;
  QNetworkAccessManager* m_net;
  QNetworkReply* m_reply = nullptr;
};
}  // namespace updates
