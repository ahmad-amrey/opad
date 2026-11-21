#include "UpdateCheck.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSysInfo>

namespace updates {

namespace {
constexpr int kTimeoutMs = 15000;

QUrl fromEnvironment(const char* name, const char* fallback) {
  return QUrl(qEnvironmentVariableIsSet(name) ? qEnvironmentVariable(name) : QString::fromLatin1(fallback));
}

QString versionOfTag(QString tag) {
  tag = tag.trimmed();
  const QString v = tag.startsWith('v') || tag.startsWith('V') ? tag.mid(1) : tag;
  return QRegularExpression(R"(^\d+(\.\d+)*(-[0-9A-Za-z.]+)?$)").match(v).hasMatch() ? v : QString();
}

// A readable sentence when GitHub refused because of its anonymous rate limit; empty for any other refusal.
QString rateLimitMessage(const QNetworkReply* reply) {
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status != 429 && !(status == 403 && reply->rawHeader("X-RateLimit-Remaining") == "0")) return {};
  QString message = QObject::tr("GitHub is limiting update checks from your network right now.");
  bool ok = false;
  const qint64 reset = reply->rawHeader("X-RateLimit-Reset").toLongLong(&ok);
  if (ok && reset > 0)
    message += ' ' + QObject::tr("Try again after %1.").arg(QDateTime::fromSecsSinceEpoch(reset).toLocalTime().toString("HH:mm"));
  return message;
}

// Whether this Linux system installs .deb packages: ID or ID_LIKE in /etc/os-release names debian or ubuntu.
bool debianFamily() {
  QFile file("/etc/os-release");
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
  QStringList families;
  for (QString line : QString::fromUtf8(file.readAll()).split('\n')) {
    line = line.trimmed();
    if (!line.startsWith("ID=") && !line.startsWith("ID_LIKE=")) continue;
    QString value = line.section('=', 1);
    value.remove('"');
    value.remove('\'');
    families << value.split(' ', Qt::SkipEmptyParts);
  }
  return families.contains("debian") || families.contains("ubuntu");
}
}  // namespace

std::optional<Release> parseLatest(const QByteArray& json, const QString& asset) {
  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
  const QJsonObject o = doc.object();
  if (o.value("draft").toBool() || o.value("prerelease").toBool()) return std::nullopt;
  Release r;
  r.tag = o.value("tag_name").toString().trimmed();
  r.version = versionOfTag(r.tag);
  if (r.version.isEmpty()) return std::nullopt;
  r.name = o.value("name").toString();
  if (r.name.isEmpty()) r.name = "OPAD " + r.version;
  r.notes = o.value("body").toString();
  r.summary = summarize(r.notes);
  r.page = o.value("html_url").toString();
  r.published = QDateTime::fromString(o.value("published_at").toString(), Qt::ISODate);
  if (!asset.isEmpty())
    for (const QJsonValue& file : o.value("assets").toArray())
      if (file.toObject().value("name").toString() == asset) r.download = file.toObject().value("browser_download_url").toString();
  return r;
}

namespace {
// Markdown out of one line of text: links keep their words, emphasis and code marks go.
QString plainText(QString s) {
  s.replace(QRegularExpression(R"(!?\[([^\]]*)\]\([^)]*\))"), QStringLiteral("\\1"));
  s.remove(QRegularExpression(R"((\*\*|__|`))"));
  s.replace(QRegularExpression(R"((^|\s)[*_]([^*_]+)[*_](?=\s|[.,;:!?]|$))"), QStringLiteral("\\1\\2"));
  return s.simplified();
}
}  // namespace

QString summarize(const QString& notes, int maxChars) {
  QStringList paragraph, items;
  bool inParagraph = false, done = false;
  QString text = notes;
  text.remove(QChar(13));
  for (QString line : text.split(QChar(10))) {
    line = line.trimmed();
    const bool heading = line.startsWith('#'), rule = line.startsWith("---") || line.startsWith("***");
    const bool item = line.startsWith("- ") || line.startsWith("* ") || QRegularExpression(R"(^\d+\.\s)").match(line).hasMatch();
    if (line.isEmpty() || heading || rule || item) {
      if (inParagraph) done = true;  // the opening paragraph ended
      inParagraph = false;
      if (item && items.size() < 3) {
        QString text = line.mid(line.indexOf(' ') + 1);
        // "**Arabic interface:** Qt's texts ..." -> "Arabic interface"
        if (const auto m = QRegularExpression(R"(^\*\*([^*]+?):?\*\*)").match(text); m.hasMatch()) text = m.captured(1);
        else if (const qsizetype stop = text.indexOf(QRegularExpression(R"([.:;](\s|$))")); stop > 0) text = text.left(stop);
        items << plainText(text).remove(QRegularExpression(R"([:.]$)"));
      }
      continue;
    }
    if (!done) {
      inParagraph = true;
      paragraph << line;
    }
  }
  QString out = paragraph.isEmpty() ? items.join("; ") : plainText(paragraph.join(' '));
  if (out.size() > maxChars) {
    qsizetype cut = out.lastIndexOf(' ', maxChars);
    out = out.left(cut > maxChars / 2 ? cut : maxChars).trimmed() + QChar(0x2026);
  }
  return out;
}

int compareVersions(const QString& a, const QString& b) {
  auto split = [](QString v, QList<int>& parts, QString& suffix) {
    v = v.trimmed();
    if (v.startsWith('v') || v.startsWith('V')) v = v.mid(1);
    const qsizetype dash = v.indexOf('-');
    suffix = dash < 0 ? QString() : v.mid(dash + 1);
    for (const QString& p : (dash < 0 ? v : v.left(dash)).split('.')) parts << p.toInt();
  };
  QList<int> pa, pb;
  QString sa, sb;
  split(a, pa, sa);
  split(b, pb, sb);
  for (qsizetype i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const int x = pa.value(i, 0), y = pb.value(i, 0);
    if (x != y) return x < y ? -1 : 1;
  }
  if (sa == sb) return 0;
  if (sa.isEmpty()) return 1;  // 0.2.0 is newer than 0.2.0-rc1
  if (sb.isEmpty()) return -1;
  return QString::compare(sa, sb) < 0 ? -1 : 1;
}

bool dueForCheck(const QDateTime& last, const QDateTime& now) {
  return !last.isValid() || last > now || last.secsTo(now) >= 24 * 3600;
}

QString installerAssetName() {
#if defined(Q_OS_WIN)
#ifdef OPAD_SINGLE_FILE  // the setup installs this same exe beside its uninstaller: without one, it runs uninstalled
  const bool singleFile = !QFile::exists(QCoreApplication::applicationDirPath() + "/unins000.exe");
#else
  const bool singleFile = false;
#endif
  return installerAssetName("windows", QSysInfo::currentCpuArchitecture(), false, singleFile);
#elif defined(Q_OS_MACOS)
  return installerAssetName("macos", QSysInfo::currentCpuArchitecture(), false);
#else
  return installerAssetName("linux", QSysInfo::currentCpuArchitecture(), debianFamily());
#endif
}

QString installerAssetName(const QString& os, const QString& cpu, bool debian, bool windowsSingleFile) {
  // Windows on Arm runs the x64 build through emulation.
  if (os == "windows") return windowsSingleFile ? "OPAD-windows-x64.exe" : "OPAD-windows-x64-setup.exe";
  if (os == "macos" && cpu == "arm64") return "OPAD-macos.dmg";  // Apple silicon only (docs/releasing.md)
  if (os == "linux" && debian && cpu == "x86_64") return "OPAD-linux-amd64.deb";
  return {};
}

QString assetDownloadUrl(const QString& tag, const QString& asset) {
  return QString("https://github.com/ahmad-amrey/opad/releases/download/%1/%2")
      .arg(QString::fromLatin1(QUrl::toPercentEncoding(tag)), QString::fromLatin1(QUrl::toPercentEncoding(asset)));
}

QString tagFromReleasePageUrl(const QUrl& url) {
  static const QRegularExpression pattern(R"(/releases/tag/([^/]+)/?$)");
  const QRegularExpressionMatch m = pattern.match(url.path());
  return m.hasMatch() ? m.captured(1) : QString();
}

QUrl safeReleaseUrl(const QString& url) {
  const QUrl u(url, QUrl::StrictMode);
  const bool trusted = u.isValid() && u.scheme() == "https" && u.host() == "github.com" && u.port() == -1 && u.userInfo().isEmpty() &&
                       u.path().startsWith("/ahmad-amrey/opad/releases/") && !u.path().contains("/../");
  return trusted ? u : QUrl(QString::fromLatin1(kLatestPageUrl));
}

Checker::Checker(QString currentVersion, QObject* parent) : QObject(parent), m_current(std::move(currentVersion)), m_net(new QNetworkAccessManager(this)) {}

void Checker::check(bool userAsked) {
  m_userAsked = m_userAsked || userAsked;  // asked while the automatic check runs: that check answers the user
  if (m_reply) return;
  m_apiError.clear();
  QNetworkRequest request{fromEnvironment("OPAD_UPDATE_URL", kLatestUrl)};
  request.setHeader(QNetworkRequest::UserAgentHeader, "OPAD/" + m_current);  // GitHub refuses requests without one
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setTransferTimeout(kTimeoutMs);
  m_reply = m_net->get(request);
  connect(m_reply, &QNetworkReply::finished, this, [this] { apiAnswered(); });
}

void Checker::apiAnswered() {
  QNetworkReply* reply = std::exchange(m_reply, nullptr);
  if (!reply) return;
  reply->deleteLater();
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status == 404) return emit upToDate(m_current, std::exchange(m_userAsked, false));  // no published release yet
  if (reply->error() != QNetworkReply::NoError) {
    if (status < 400) return emit failed(reply->errorString(), std::exchange(m_userAsked, false));  // offline, timeout
    // GitHub refused, usually because the anonymous limit is used up: the release page still names the latest version.
    m_apiError = rateLimitMessage(reply);
    if (m_apiError.isEmpty()) m_apiError = reply->errorString();
    return askReleasePage();
  }
  const std::optional<Release> release = parseLatest(reply->readAll(), installerAssetName());
  if (!release) return emit failed(tr("GitHub's answer was not a release"), std::exchange(m_userAsked, false));
  finish(*release);
}

void Checker::askReleasePage() {
  QNetworkRequest request{fromEnvironment("OPAD_UPDATE_PAGE_URL", kLatestPageUrl)};
  request.setHeader(QNetworkRequest::UserAgentHeader, "OPAD/" + m_current);
  // Only the redirect matters: /releases/latest -> /releases/tag/vX.Y.Z.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  request.setTransferTimeout(kTimeoutMs);
  m_reply = m_net->head(request);
  connect(m_reply, &QNetworkReply::finished, this, [this] { pageAnswered(); });
}

void Checker::pageAnswered() {
  QNetworkReply* reply = std::exchange(m_reply, nullptr);
  if (!reply) return;
  reply->deleteLater();
  const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
  const QUrl target = redirect.isEmpty() ? QUrl() : reply->url().resolved(redirect);
  Release r;
  r.tag = tagFromReleasePageUrl(target);
  r.version = versionOfTag(r.tag);
  if (r.version.isEmpty()) return emit failed(m_apiError.isEmpty() ? reply->errorString() : m_apiError, std::exchange(m_userAsked, false));
  r.name = "OPAD " + r.version;
  r.page = target.toString();
  if (const QString asset = installerAssetName(); !asset.isEmpty()) r.download = assetDownloadUrl(r.tag, asset);
  finish(r);
}

void Checker::finish(const Release& release) {
  const bool userAsked = std::exchange(m_userAsked, false);
  if (compareVersions(release.version, m_current) > 0) emit available(release, userAsked);
  else emit upToDate(m_current, userAsked);
}

}  // namespace updates
