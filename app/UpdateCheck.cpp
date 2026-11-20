#include "UpdateCheck.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace updates {

std::optional<Release> parseLatest(const QByteArray& json) {
  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
  const QJsonObject o = doc.object();
  if (o.value("draft").toBool() || o.value("prerelease").toBool()) return std::nullopt;
  Release r;
  r.tag = o.value("tag_name").toString().trimmed();
  if (r.tag.isEmpty()) return std::nullopt;
  r.version = r.tag.startsWith('v') || r.tag.startsWith('V') ? r.tag.mid(1) : r.tag;
  if (!QRegularExpression(R"(^\d+(\.\d+)*(-[0-9A-Za-z.]+)?$)").match(r.version).hasMatch()) return std::nullopt;
  r.name = o.value("name").toString();
  if (r.name.isEmpty()) r.name = "OPAD " + r.version;
  r.notes = o.value("body").toString();
  r.summary = summarize(r.notes);
  r.page = o.value("html_url").toString();
  r.published = QDateTime::fromString(o.value("published_at").toString(), Qt::ISODate);
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

Checker::Checker(QString currentVersion, QObject* parent) : QObject(parent), m_current(std::move(currentVersion)), m_net(new QNetworkAccessManager(this)) {}

void Checker::check(bool userAsked) {
  if (m_reply) return;
  const QString url = qEnvironmentVariableIsSet("OPAD_UPDATE_URL") ? qEnvironmentVariable("OPAD_UPDATE_URL") : QString(kLatestUrl);
  QNetworkRequest request{QUrl(url)};
  request.setHeader(QNetworkRequest::UserAgentHeader, "OPAD/" + m_current);  // GitHub refuses requests without one
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setTransferTimeout(15000);
  m_reply = m_net->get(request);
  connect(m_reply, &QNetworkReply::finished, this, [this, userAsked] {
    QNetworkReply* reply = std::exchange(m_reply, nullptr);
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
      // 404: the repository has no published release yet.
      if (status == 404) emit upToDate(m_current, userAsked);
      else emit failed(reply->errorString(), userAsked);
      return;
    }
    const std::optional<Release> release = parseLatest(reply->readAll());
    if (!release) return emit failed(tr("GitHub's answer was not a release"), userAsked);
    if (compareVersions(release->version, m_current) > 0) emit available(*release, userAsked);
    else emit upToDate(m_current, userAsked);
  });
}

}  // namespace updates
