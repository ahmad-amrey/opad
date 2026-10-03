// Benches of the core area (T0), registered through BenchRegistry; cases in tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "MainWindow.hpp"

// OPAD_BENCH_SEAMS=1 [OPAD_LANG=ar]: the extension seams (UI-119) in the running app. This bench is itself dispatched
// by the registry; no two files claim the same bench switch or icon name; the fragments app/i18n/<code>/*.json are
// embedded but are not languages; with a language on, tr() gives exactly the merged table of <code>.json and its
// fragments (so every fragment string came through).
OPAD_BENCH(OPAD_BENCH_SEAMS, seams) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: seams: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  require(bench::pending() == "OPAD_BENCH_SEAMS" && bench::variables().contains("OPAD_BENCH_SEAMS") && w.m_doc->hasDocument,
          "registered bench runs with the window loaded");
  auto named = [](const QStringList& names) { return names.isEmpty() ? QString() : " (" + names.join(' ') + ")"; };
  require(bench::clashes().isEmpty(), "bench switches claimed once" + named(bench::clashes()));
  require(icons::clashes().isEmpty() && icons::has("open"), "icon names taken once" + named(icons::clashes()));
  bool languages = true;
  for (const i18n::Language& l : i18n::languages())
    languages = languages && !l.code.contains('/') && (l.code == "en" || QFile::exists(":/i18n/" + l.code + ".json"));
  int fragments = 0;
  for (const QString& code : QDir(":/i18n").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    fragments += QDir(":/i18n/" + code).entryList({"*.json"}, QDir::Files).size();
    languages = languages && QFile::exists(":/i18n/" + code + ".json");
  }
  require(languages, QString("languages are <code>.json, %1 fragments are not").arg(fragments));
  if (const QString code = i18n::current(); code != "en") {
    const QHash<QString, QString> table = i18n::table(code, {":/i18n", QCoreApplication::applicationDirPath() + "/i18n"});
    int wrong = 0;
    for (auto it = table.begin(); it != table.end(); ++it)
      if (i18n::t(it.key()) != it.value()) ++wrong;
    require(table.size() > 1000 && wrong == 0, QString("%1: %2 strings of %3.json and %4 fragments translate as merged (%5 wrong)")
                                                    .arg(code).arg(table.size()).arg(code).arg(QDir(":/i18n/" + code).entryList({"*.json"}, QDir::Files).size()).arg(wrong));
  }
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
