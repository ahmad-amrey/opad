// OPAD_BENCH_RECENT_PATHS=1 on tools/bench_cases/help.py's recent-paths document, opened from the command line (native
// separators) while the settings list it already with forward slashes (as an agent or another path recorded it), with a
// second file twice the same way and once in another letter case: the recent files are one entry per file, in the system's
// form, at startup (the start page's cards, File > Recent), after the open, after adding a spelling of a listed file again
// and after removing one by another spelling.
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "EmptyState.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"

OPAD_BENCH(OPAD_BENCH_RECENT_PATHS, recentPaths) {
  auto fail = [](const QString& why) {
    trace::log("bench: recent-paths: FAIL " + why);
    QCoreApplication::exit(2);
    return true;
  };
  auto pass = [](const QString& what) { trace::log("bench: recent-paths: " + what + " PASS"); };
  const QString doc = QDir::toNativeSeparators(QFileInfo(w.m_doc->path()).absoluteFilePath());
  const QString other = QDir::toNativeSeparators(QFileInfo(QFileInfo(doc).absolutePath() + "/recent-other.opad").absoluteFilePath());
  auto cards = [&w] {
    QStringList out;
    for (RecentCard* c : w.m_empty->cards()) out << c->path();
    return out;
  };
  auto once = [](const QStringList& list, const QString& file) {
    int n = 0;
    for (const QString& p : list) n += QFileInfo(p) == QFileInfo(file);
    return n == 1;
  };
  auto native = [](const QStringList& list) {
    for (const QString& p : list)
      if (p != QDir::toNativeSeparators(QDir::cleanPath(p))) return false;
    return true;
  };
  const QStringList atStart = w.recent();
  if (!once(atStart, doc) || !once(atStart, other) || !native(atStart) || atStart.size() != 2)
    return fail("one entry per file after the open from the command line: " + atStart.join(" | "));
  if (!once(cards(), doc) || !once(cards(), other) || cards().size() != 2) return fail("the start page's cards: " + cards().join(" | "));
  pass("opened from the command line while listed with forward slashes: one entry per file, native, one card each");
  w.addRecent(QDir::fromNativeSeparators(other));
  if (w.recent().size() != 2 || !once(w.recent(), other) || QFileInfo(w.recent().front()) != QFileInfo(other) || !native(w.recent()))
    return fail("another spelling of a listed file added: " + w.recent().join(" | "));
  pass("another spelling of a listed file added: moved to the top, not listed twice");
  w.removeRecent(QDir::fromNativeSeparators(doc).toUpper());
  if (w.recent().size() != 1 || once(w.recent(), doc) || cards().size() != 1) return fail("removed by another spelling: " + w.recent().join(" | "));
  pass("removed by another spelling of it");
  QCoreApplication::exit(0);
  return true;
}
