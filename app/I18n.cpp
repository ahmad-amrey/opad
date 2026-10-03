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
  JsonTranslator(const QHash<QString, QString>& table, QObject* parent) : QTranslator(parent) {
    for (auto it = table.begin(); it != table.end(); ++it) m_map.insert(it.key().toUtf8(), it.value());
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

QList<Language> languages(const QString& dir) {
  QList<Language> out{{"en", "English", false}};
  for (const QString& file : QDir(dir).entryList({"*.json"}, QDir::Files, QDir::Name)) {  // not the fragment folders
    const QJsonObject o = readJson(dir + "/" + file);
    const QString code = file.chopped(5);
    out.append({code, o.value("@name").toString(code), o.value("@rtl").toBool()});
  }
  return out;
}

QHash<QString, QString> table(const QString& code, const QStringList& dirs) {
  QHash<QString, QString> out;
  auto add = [&out](const QString& path) {
    const QJsonObject o = readJson(path);
    for (auto it = o.begin(); it != o.end(); ++it)
      if (!it.key().startsWith('@') && !it.value().toString().isEmpty()) out.insert(it.key(), it.value().toString());
  };
  for (const QString& dir : dirs) {
    add(dir + "/" + code + ".json");
    for (const QString& file : QDir(dir + "/" + code).entryList({"*.json"}, QDir::Files, QDir::Name)) add(dir + "/" + code + "/" + file);
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
    app.installTranslator(new JsonTranslator(table(code, {":/i18n", QCoreApplication::applicationDirPath() + "/i18n"}), &app));
    app.setLayoutDirection(l.rtl ? Qt::RightToLeft : Qt::LeftToRight);
    g_current = code;
  }
}

void setLanguage(const QString& code) { QSettings().setValue("ui/language", code); }

QString t(const char* source) { return QCoreApplication::translate("i18n", source); }
QString t(const QString& source) { return t(source.toUtf8().constData()); }

QString message(const QString& text) {
  if (const QString whole = t(text); whole != text) return whole;
  QStringList out;
  qsizetype start = 0;
  for (qsizetype at = text.indexOf(". "); at >= 0; at = text.indexOf(". ", start)) {
    out << t(text.mid(start, at + 1 - start));
    start = at + 2;
  }
  out << t(text.mid(start));
  return out.join(' ');
}

QString localTime(const std::string& iso) {
  const QString text = QString::fromStdString(iso);
  const QDateTime at = QDateTime::fromString(text, Qt::ISODate);
  if (!at.isValid()) return text.left(16).replace('T', ' ');  // not a time stamp after all: as written
  return at.toLocalTime().toString("yyyy-MM-dd HH:mm");
}

}  // namespace i18n
