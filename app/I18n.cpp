#include "I18n.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace {
QJsonObject readJson(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}

// Source text -> translation. An unknown string returns empty, and Qt falls back to the source text.
class JsonTranslator : public QTranslator {
 public:
  using QTranslator::QTranslator;
  void add(const QJsonObject& o) {
    for (auto it = o.begin(); it != o.end(); ++it)
      if (!it.key().startsWith('@') && !it.value().toString().isEmpty()) m_map.insert(it.key().toUtf8(), it.value().toString());
  }
  QString translate(const char*, const char* sourceText, const char*, int) const override {
    return m_map.value(QByteArray::fromRawData(sourceText, static_cast<qsizetype>(qstrlen(sourceText))));
  }
  bool isEmpty() const override { return m_map.isEmpty(); }
 private:
  QHash<QByteArray, QString> m_map;
};

QString g_current = "en";
}  // namespace

namespace i18n {

QList<Language> languages() {
  QList<Language> out{{"en", "English", false}};
  for (const QString& file : QDir(":/i18n").entryList({"*.json"}, QDir::Files, QDir::Name)) {
    const QJsonObject o = readJson(":/i18n/" + file);
    const QString code = file.chopped(5);
    out.append({code, o.value("@name").toString(code), o.value("@rtl").toBool()});
  }
  return out;
}

QString current() { return g_current; }

void install(QApplication& app) {
  const QList<Language> known = languages();
  QString code = qEnvironmentVariable("OPAD_LANG");  // for a one-off run, e.g. screenshots
  if (code.isEmpty()) code = QSettings().value("ui/language").toString();
  if (code.isEmpty()) code = QLocale::system().name().section('_', 0, 0);
  for (const Language& l : known) {
    if (l.code != code || l.code == "en") continue;
    auto* tr = new JsonTranslator(&app);
    tr->add(readJson(":/i18n/" + code + ".json"));
    tr->add(readJson(QCoreApplication::applicationDirPath() + "/i18n/" + code + ".json"));
    app.installTranslator(tr);
    app.setLayoutDirection(l.rtl ? Qt::RightToLeft : Qt::LeftToRight);
    g_current = code;
  }
}

void setLanguage(const QString& code) { QSettings().setValue("ui/language", code); }

QString t(const char* source) { return QCoreApplication::translate("i18n", source); }
QString t(const QString& source) { return t(source.toUtf8().constData()); }

QString localTime(const std::string& iso) {
  const QString text = QString::fromStdString(iso);
  const QDateTime at = QDateTime::fromString(text, Qt::ISODate);
  if (!at.isValid()) return text.left(16).replace('T', ' ');  // not a time stamp after all: as written
  return at.toLocalTime().toString("yyyy-MM-dd HH:mm");
}

}  // namespace i18n
