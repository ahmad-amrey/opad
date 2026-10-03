// OPAD_BENCH_SELPUBLISH=<prefix> (UI-06): the GUI selection is published for agents (selection.json) only while agent
// access is on. Case in tools/bench_cases/viewer.py on 1,000 boxes: with access off, selecting everything writes nothing
// and starts no job; turned on, the current selection is published at once (ref, node, type, body key, world box); a
// rubber band over every face is capped at 2,000 refs (truncated, with the total) and keeps the UI thread free while it
// is written; turned off, the file goes and a selection publishes nothing again.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>

#include <functional>

#include "AgentBridge.hpp"
#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "MainWindow.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_SELPUBLISH, selpublish) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: publish: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] {
    QCoreApplication::exit(all ? 0 : 2);
    return true;
  };
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  const QString file = QString::fromStdU16String((opad::cache_dir() / "selection.json").u16string());
  auto settled = [&] { return !w.m_jobs->busy() && !w.m_selFileTimer.isActive() && !w.m_displayJob && w.m_meshRemaining == 0; };
  auto published = [&] {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? opad::json::parse(f.readAll().toStdString(), nullptr, false) : opad::json();
  };
  if (!require(waitUntil([&] { return settled() && v->displayedCount() > 0; }, 120000), QString("%1 bodies shown").arg(v->displayedCount()))) return finish();
  QFile::remove(file);
  w.m_agent->setAccess(false, false);
  const auto roots = doc->scene.roots;
  bool applied = false;
  QObject::connect(v, &Viewport::selectionApplied, &w, [&applied] { applied = true; });
  const int begun = w.m_jobs->begun();
  w.m_browser->setSelectedIds(roots);
  w.onBrowserSelection(roots);
  waitUntil([&] { return applied && settled(); }, 30000);
  waitUntil([] { return false; }, 400);  // past the 250 ms debounce
  require(!QFile::exists(file) && !w.m_selFileJob, QString("agent access off: nothing published (%1 jobs for the selection)").arg(w.m_jobs->begun() - begun));

  w.m_agent->setAccess(true, false);
  waitUntil([&] { return QFile::exists(file) && settled(); }, 10000);
  opad::json j = published();
  bool fields = j.is_object() && j.value("total", 0) == w.m_selRefs.size() && !j.value("truncated", true) && j["selection"].size() == w.m_selRefs.size();
  for (const auto& e : j.value("selection", opad::json::array()))
    fields = fields && e.contains("ref") && e.contains("node") && e.contains("type");
  require(fields, QString("agent access on: the current selection is published at once (%1 refs)").arg(j.value("selection", opad::json::array()).size()));

  // Every face in a rubber band: capped, and written off the UI thread.
  w.action("select.faces")->trigger();
  waitUntil(settled, 60000);
  QFile::remove(file);
  QElapsedTimer clock;
  clock.start();
  const qint64 cpu0 = trace::threadCpuMs();
  v->benchBand();
  const qint64 bandCpu = trace::threadCpuMs() - cpu0;
  waitUntil([] { return false; }, 30);  // the watchdog's tick that saw the band itself (OCCT's pick, not the publishing)
  trace::resetStalls();
  waitUntil([&] { return QFile::exists(file) && settled(); }, 60000);
  j = published();
  const size_t picked = w.m_selRefs.size();
  bool faces = j.is_object() && j["selection"].size() == std::min<size_t>(picked, 2000) && j.value("total", size_t(0)) == picked && j.value("truncated", false) == (picked > 2000);
  if (faces && !j["selection"].empty()) faces = j["selection"][0].value("type", "") == "face";
  require(faces && picked > 2000, QString("%1 faces picked, %2 published, truncated %3").arg(picked).arg(j.value("selection", opad::json::array()).size()).arg(j.value("truncated", false)));
  const trace::Stalls s = trace::stalls();
  require(s.longestCpu < 150, QString("the UI thread is free while it is published: longest stall %1 ms (%2 ms CPU), the band itself %3 ms CPU, %4 ms in all")
                                  .arg(s.longest).arg(s.longestCpu).arg(bandCpu).arg(clock.elapsed()));

  w.m_agent->setAccess(false, false);
  waitUntil([&] { return !QFile::exists(file); }, 5000);
  require(!QFile::exists(file), "agent access off again: the published selection goes");
  w.action("select.bodies")->trigger();
  waitUntil(settled, 60000);
  w.m_browser->setSelectedIds(roots);
  w.onBrowserSelection(roots);
  waitUntil(settled, 30000);
  waitUntil([] { return false; }, 400);
  require(!QFile::exists(file), "and a new selection publishes nothing");
  return finish();
}
