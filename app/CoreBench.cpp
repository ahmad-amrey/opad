// Benches of the core area (T0), registered through BenchRegistry; cases in tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <set>

#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "MainWindow.hpp"
#include "TimelineWidget.hpp"

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

// OPAD_BENCH_TOLERANT=<path>: a file of a newer build (tools/bench_cases/core.py adds a one-line and a multi-line record of
// unknown types) opens (UI-65): the records are reported as needing a newer OPAD, the timeline never steps onto them, an
// edit still works, and Save As writes them back as they were read.
OPAD_BENCH(OPAD_BENCH_TOLERANT, tolerant) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: tolerant: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  std::vector<const opad::Op*> opaque;
  for (const auto& op : w.m_doc->doc.ops)
    if (!opad::Document::known_type(op.type)) opaque.push_back(&op);
  int newer = 0;
  for (const auto& u : w.m_doc->scene.unresolved) newer += u.reason.find("needs a newer OPAD") != std::string::npos;
  require(w.m_doc->hasDocument && opaque.size() == 2 && newer == 2 && !w.m_doc->scene.all_bodies().empty(),
          QString("opened with %1 records of a newer build, %2 reported, %3 bodies").arg(opaque.size()).arg(newer).arg(w.m_doc->scene.all_bodies().size()));
  std::set<std::string> visited;
  w.m_timeline->setCurrentOp({});
  for (int i = 0; i <= int(w.m_doc->doc.ops.size()); ++i) {
    w.m_timeline->step(1);
    visited.insert(w.m_timeline->currentOp());
  }
  bool hidden = !visited.empty();
  for (const auto* op : opaque) hidden = hidden && !visited.count(op->id);
  require(hidden, QString("the timeline steps through %1 ops, none of a newer build").arg(visited.size()));
  std::string records;
  for (const auto* op : opaque) records += op->raw + "\n";
  const auto body = w.m_doc->scene.all_bodies().front();
  bool edited = true;
  try {
    w.m_doc->run("rename", {{"target", body}, {"name", "Renamed here"}});
  } catch (const std::exception&) {
    edited = false;
  }
  w.m_doc->saveAs(value);
  QFile file(value);
  const std::string text = file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string();
  require(edited && w.m_doc->scene.node(body)->name == "Renamed here" && !records.empty() && text.find(records) != std::string::npos &&
              opad::Document::parse(text).ops.size() == w.m_doc->doc.ops.size(),
          "an edit, then Save As writes the newer records back byte for byte");
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
