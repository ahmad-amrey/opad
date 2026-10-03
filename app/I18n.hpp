#pragma once
// UI languages. A translation is one JSON file, app/i18n/<code>.json: { "source text": "translation", ... },
// compiled into the binary (i18n.qrc) and looked up by source text alone, whatever the tr() context. Keys that
// start with "@" are metadata: "@name" (the language's own name, shown in the menu) and "@rtl" (true mirrors
// the layout). A file <app dir>/i18n/<code>.json, if present, is laid over the built-in one, so a translator
// can try changes without a rebuild. Each area may keep its strings in a fragment app/i18n/<code>/<area>.json,
// merged after <code>.json (fragments by name, then the same from <app dir>/i18n), so parallel work never appends to
// one file; a fragment is not a language. No Qt Linguist tools are needed; tools/i18n_check.py lists tr() strings the
// file and its fragments do not cover. The qrc is generated from app/i18n (app/CMakeLists.txt): a language or a
// fragment is one new JSON file.
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <string>

class QApplication;

namespace i18n {
struct Language {
  QString code;  // "en", "ar"
  QString name;  // in the language itself
  bool rtl = false;
};
QList<Language> languages(const QString& dir = ":/i18n");  // English (the source strings) first; <dir>/<code>.json
// The translations of one language: <dir>/<code>.json, then its fragments <dir>/<code>/*.json by name, for each dir in
// turn; a later key wins. Metadata ("@...") and empty values are left out.
QHash<QString, QString> table(const QString& code, const QStringList& dirs);
QString current();
// Installs the language from the setting ui/language; without one, the system's language if there is a file for it.
void install(QApplication& app);
void setLanguage(const QString& code);  // saved; applies at the next start
// Run-time lookup for text that is data rather than a tr() literal: property names, core error messages.
QString t(const QString& source);
QString t(const char* source);
// An op's ISO 8601 UTC time stamp ("2026-10-01T01:32:05Z") as local "yyyy-MM-dd HH:mm" (shown raw, it was UTC).
QString localTime(const std::string& iso);
}  // namespace i18n
