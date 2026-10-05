#include "Jobs.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QStringList>
#include <QThread>
#include <QtGlobal>
#include <algorithm>
#include <array>
#include <cstdio>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "ProgressStrip.hpp"

namespace {
constexpr int kStripDelayMs = 500;  // how long an operation may run before progress UI appears
constexpr int kSliceMs = 10;        // budget per UI-thread slice; leaves room for input and painting at 60 Hz
constexpr int kWarnMs = 250;        // the watchdog without OPAD_TRACE: stderr only, over this
constexpr int kTickMs = 16;         // the watchdog's tick while tracing (a stall reads at most this much long)
constexpr int kBusyMs = 150;        // a job the user waits for this long turns the busy cursor on
}  // namespace

// ---------------------------------------------------------------- JobState / Progress
void detail::JobState::post(std::function<void(Job&)> fn) {
  Job* j = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!job) return;
    j = job;
    if (QThread::currentThread() != j->thread()) {
      // Queued to the Job's thread; Qt drops it if the Job is deleted first.
      QMetaObject::invokeMethod(j, [j, fn = std::move(fn)] { fn(*j); }, Qt::QueuedConnection);
      return;
    }
  }
  fn(*j);  // on the UI thread: the Job can only be deleted here, so no lock is needed while fn runs
}

void Progress::setPhase(const QString& text, int percent) const {
  m_s->post([text, percent](Job& j) { emit j.phaseChanged(text, percent); });
}

void Progress::setOverall(int percent) const {
  m_s->post([percent](Job& j) { emit j.overallChanged(percent); });
}

// ---------------------------------------------------------------- Job
Job::Job(const QString& title, bool twoBars, QObject* parent)
    : QObject(parent), m_title(title), m_twoBars(twoBars), m_state(std::make_shared<detail::JobState>()) {
  m_state->job = this;
  m_clock.start();
}

Job::~Job() {
  std::lock_guard<std::mutex> lock(m_state->mu);
  m_state->job = nullptr;
}

void Job::setPhase(const QString& text, int percent) { emit phaseChanged(text, percent); }
void Job::setOverall(int percent) { emit overallChanged(percent); }

void Job::cancel() {
  if (!m_active) return;
  m_state->cancel = true;
  emit cancelRequested();
  finish(false, QStringLiteral("cancelled"));
}

void Job::finish(bool ok, const QString& error) {
  m_state->post([ok, error](Job& j) {
    if (!j.m_active) return;
    j.m_active = false;
    auto done = std::move(j.m_stepDone);  // sliced jobs: done(completed) runs exactly once, before finished()
    j.m_step = {};
    j.m_stepDone = {};
    if (trace::enabled()) trace::log(QStringLiteral("job \"%1\": %2 in %3 ms").arg(j.m_title, ok ? "done" : error).arg(j.m_clock.elapsed()));
    if (done) done(ok);
    emit j.finished(ok, error);
  });
}

// ---------------------------------------------------------------- JobRunner
JobRunner::JobRunner(ProgressStrip* strip, QObject* parent) : QObject(parent), m_strip(strip) {
  m_showTimer.setSingleShot(true);
  m_showTimer.setInterval(kStripDelayMs);
  connect(&m_showTimer, &QTimer::timeout, this, &JobRunner::refreshStrip);
  m_activityTimer.setSingleShot(true);
  connect(&m_activityTimer, &QTimer::timeout, this, &JobRunner::refreshActivity);
  connect(m_strip, &ProgressStrip::cancelRequested, this, [this] { if (Job* c = current()) c->cancel(); });
  m_busyTimer.setSingleShot(true);
  connect(&m_busyTimer, &QTimer::timeout, this, &JobRunner::updateBusy);
}

JobRunner::~JobRunner() {
  if (m_busyCursor) QGuiApplication::restoreOverrideCursor();
}

QStringList JobRunner::titles() const {
  QStringList out;
  for (Job* j : m_jobs) out << j->title();
  return out;
}

// On once the oldest job someone waits for has run kBusyMs and no mouse button is down (a drag would flicker); off when
// no such job is left. A background job never counts.
void JobRunner::updateBusy() {
  qint64 oldest = -1;
  for (Job* j : m_jobs)
    if (j->active() && !j->background()) oldest = std::max(oldest, j->elapsedMs());
  const bool on = oldest >= kBusyMs && (m_busyCursor || QGuiApplication::mouseButtons() == Qt::NoButton);
  if (on != m_busyCursor) {
    m_busyCursor = on;
    if (on) QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    else QGuiApplication::restoreOverrideCursor();
  }
  if (oldest < 0 || on) m_busyTimer.stop();
  else m_busyTimer.start(oldest >= kBusyMs ? 50 : static_cast<int>(kBusyMs - oldest));
}

Job* JobRunner::current() const {
  for (Job* j : m_jobs)
    if (j->m_kind == JobKind::Foreground) return j;
  return nullptr;
}

QStringList JobRunner::background() const {
  QStringList out;
  for (Job* j : m_jobs)
    if (j->m_kind == JobKind::Background || (j->m_kind == JobKind::Child && !j->m_parent)) out << j->title();
  return out;
}

Job* JobRunner::begin(const QString& title, bool twoBars, JobKind kind, Job* parent) {
  Job* j = new Job(title, twoBars, this);
  ++m_begun;
  j->m_background = std::exchange(m_backgroundNext, false);
  j->m_kind = kind == JobKind::Child && !parent ? JobKind::Background : kind;
  if (j->m_kind == JobKind::Child) {
    j->m_parent = parent;
    connect(parent, &Job::cancelRequested, j, &Job::cancel);  // the strip's Cancel reaches the children
  }
  m_jobs.push_back(j);
  connect(j, &Job::phaseChanged, this, [this, j](const QString& text, int pct) {
    j->m_lastPhase = text;
    j->m_lastPct = pct;
    if (j == m_shown) m_strip->setPhase(text, pct);
  });
  connect(j, &Job::overallChanged, this, [this, j](int pct) {
    j->m_lastOverall = pct;
    if (j == m_shown) m_strip->setOverall(pct);
  });
  connect(j, &Job::finished, this, [this, j](bool ok, const QString& error) { onFinished(j, ok, error); });
  if (j->m_kind != JobKind::Foreground) {
    if (!m_activityTimer.isActive()) m_activityTimer.start(kStripDelayMs);
  } else if (!m_shown && !m_showTimer.isActive()) {
    m_showTimer.start();  // the oldest Foreground job keeps the strip once it shows
  } else if (m_shown) {
    refreshStrip();  // counted among the others beside it
  }
  if (!m_busyCursor && !m_busyTimer.isActive()) m_busyTimer.start(kBusyMs);  // marked background by then, or it counts
  return j;
}

Job* JobRunner::async(const QString& title, std::function<void(Progress)> work, std::function<void(bool, const QString&)> done, JobKind kind,
                      Job* parent) {
  Job* j = begin(title, false, kind, parent);
  launch(j, std::move(work), std::move(done));
  return j;
}

Job* JobRunner::quiet(const QString& title, std::function<void(Progress)> work, std::function<void(bool, const QString&)> done) {
  Job* j = new Job(title, false, this);
  connect(j, &Job::finished, j, &QObject::deleteLater);
  launch(j, std::move(work), std::move(done));
  return j;
}

void JobRunner::launch(Job* j, std::function<void(Progress)> work, std::function<void(bool, const QString&)> done) {
  if (done) connect(j, &Job::finished, j, [done](bool ok, const QString& e) { done(ok, e); });
  Progress p = j->progress();
  // Qt adopts std::threads that post progress. On MinGW/Qt 6.10 their TLS cleanup can fault on exit,
  // pausing the whole process in Windows Error Reporting after the result has already arrived.
  // Let Qt own the native thread and its teardown. Keep it parentless: cancelling a Job must not wait.
  auto* worker = QThread::create([p, work = std::move(work)]() {
    bool ok = true;
    QString err;
    try {
      work(p);
    } catch (const std::exception& e) {
      ok = false;
      err = QString::fromUtf8(e.what());
    } catch (...) {
      ok = false;
      err = QStringLiteral("unknown error");
    }
    if (ok && p.cancelled()) {
      ok = false;
      err = QStringLiteral("cancelled");
    }
    // Finish through the shared state: the Job may already be gone (window closed), in which case this is a no-op.
    p.m_s->post([ok, err](Job& j) { j.finish(ok, err); });
  });
  connect(worker, &QThread::finished, worker, &QObject::deleteLater);
  worker->start();
}

Job* JobRunner::sliced(const QString& title, std::function<bool(Job&)> step, std::function<void(bool)> done, JobKind kind, Job* parent) {
  Job* j = begin(title, false, kind, parent);
  j->m_step = std::move(step);
  j->m_stepDone = std::move(done);
  // The first slice runs on the next event-loop turn, never inside this call: a job that completed
  // synchronously would run done() before the caller had even stored the returned Job*.
  QTimer::singleShot(0, j, [this, j] { slice(j); });
  return j;
}

void JobRunner::resume(Job* j) {
  if (!j || !j->active() || !std::exchange(j->m_paused, false)) return;
  QTimer::singleShot(0, j, [this, j] { slice(j); });
}

// One time-boxed slice of a sliced job, then yield to the event loop and reschedule.
void JobRunner::slice(Job* j) {
  if (!j->active() || !j->m_step) return;
  QElapsedTimer t;
  t.start();
  bool more = true;
  const bool tracing = trace::enabled();
  while (more && !j->m_paused && !j->cancelled() && t.elapsed() < kSliceMs) {
    const qint64 before = t.elapsed();
    more = j->m_step(*j);
    const qint64 took = t.elapsed() - before;
    if (tracing && took > 50) trace::log(QStringLiteral("job \"%1\": one step took %2 ms (steps must be small)").arg(j->m_title).arg(took));
  }
  if (!j->active()) return;  // cancelled from inside step (or via the strip) - finish already ran
  if (more && !j->cancelled()) {
    if (!j->m_paused) QTimer::singleShot(0, j, [this, j] { slice(j); });  // paused: resume() slices again
    return;
  }
  const bool completed = !j->cancelled();
  j->finish(completed, completed ? QString() : QStringLiteral("cancelled"));
}

void JobRunner::onFinished(Job* j, bool ok, const QString& error) {
  m_jobs.erase(std::remove(m_jobs.begin(), m_jobs.end(), j), m_jobs.end());
  emit done(j, ok, error);
  j->deleteLater();
  updateBusy();
  refreshActivity();
  if (!current()) {
    m_showTimer.stop();
    if (m_shown) {
      m_shown = nullptr;
      m_strip->finish();
      emit stripShown(false);
    }
    return;
  }
  refreshStrip();
}

// The Background jobs (and children whose parent has gone) that have run for the grace period, for the activity dot.
void JobRunner::refreshActivity() {
  QStringList titles;
  qint64 wait = -1;
  for (Job* j : m_jobs) {
    if (j->m_kind == JobKind::Foreground || (j->m_kind == JobKind::Child && j->m_parent)) continue;
    const qint64 left = kStripDelayMs - j->elapsedMs();
    if (left <= 0) titles << j->title();
    else wait = wait < 0 ? left : std::min(wait, left);
  }
  titles.removeDuplicates();
  if (wait >= 0) m_activityTimer.start(static_cast<int>(wait) + 1);
  if (titles == m_activity) return;
  m_activity = titles;
  emit activityChanged(titles);
}

// Shows the oldest Foreground job in the strip (only after the 0.5 s grace period has elapsed once), and how many other
// Foreground jobs run beside it (Background ones are the activity dot's).
void JobRunner::refreshStrip() {
  Job* c = current();
  if (!c) return;
  if (!m_shown && m_showTimer.isActive()) return;
  QStringList others;
  for (Job* j : m_jobs)
    if (j != c && j->m_kind == JobKind::Foreground) others << j->title();
  m_strip->setOthers(others);
  if (c == m_shown) return;
  const bool wasShown = m_shown != nullptr;
  m_shown = c;
  c->m_wasShown = true;
  m_strip->begin(c->title(), c->m_twoBars);
  if (!c->m_lastPhase.isEmpty()) m_strip->setPhase(c->m_lastPhase, c->m_lastPct);
  if (c->m_lastOverall >= 0) m_strip->setOverall(c->m_lastOverall);
  if (!wasShown) emit stripShown(true);
}

// ---------------------------------------------------------------- trace
namespace trace {
namespace {
struct Sink {
  bool enabled = false;
  QFile file;
  std::mutex mu;
  Sink() {
    const QByteArray v = qgetenv("OPAD_TRACE");
    if (v.isEmpty()) return;
    enabled = true;
    if (v != "1" && v != "stderr") {
      file.setFileName(QString::fromLocal8Bit(v));
      file.open(QIODevice::Append | QIODevice::Text);
    }
  }
};
Sink& sink() {
  static Sink s;
  return s;
}
}  // namespace

bool enabled() { return sink().enabled; }

void log(const QString& line) {
  Sink& s = sink();
  if (!s.enabled) return;
  const QString out = QDateTime::currentDateTime().toString("HH:mm:ss.zzz ") + line + "\n";
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.file.isOpen()) {
    s.file.write(out.toUtf8());
    s.file.flush();
  } else {
    std::fputs(out.toUtf8().constData(), stderr);
    std::fflush(stderr);
  }
}

Scope::~Scope() {
  if (enabled()) log(QStringLiteral("%1: %2 ms").arg(m_name).arg(m_t.elapsed()));
}

namespace {
struct StallLog {
  Stalls since, all;
  std::array<int, 6> buckets{};  // up to 100, 150, 250, 500, 1000 ms, longer
};
StallLog& stallLog() {
  static StallLog s;
  return s;
}
constexpr std::array<qint64, 5> kEdges{100, 150, 250, 500, 1000};
// Frames since the watchdog's last tick and the longest stretch without one (UI thread only).
struct Frames {
  QElapsedTimer clock;
  qint64 last = 0, longest = 0;
  int count = 0;
};
Frames& frames() {
  static Frames f;
  if (!f.clock.isValid()) f.clock.start();
  return f;
}
}  // namespace

void frameDrawn() {
  Frames& f = frames();
  const qint64 now = f.clock.elapsed();
  f.longest = std::max(f.longest, now - f.last);
  f.last = now;
  ++f.count;
}

int stallThreshold() {
  static const int ms = [] {
    bool ok = false;
    const int v = qEnvironmentVariableIntValue("OPAD_TRACE_STALL_MS", &ok);
    return ok && v > 0 ? v : 50;
  }();
  return enabled() ? ms : kWarnMs;
}

qint64 threadCpuMs() {
#if defined(_WIN32)
  // Cycles the thread ran (the invariant TSC) at the nominal clock: exact, where GetThreadTimes moves in 15.6 ms ticks.
  static const double perMs = [] {
    DWORD mhz = 0, size = sizeof(mhz);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size);
    return mhz * 1000.0;
  }();
  ULONG64 cycles = 0;
  if (perMs > 0 && QueryThreadCycleTime(GetCurrentThread(), &cycles)) return static_cast<qint64>(static_cast<double>(cycles) / perMs);
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  auto ticks = [](const FILETIME& f) { return (static_cast<qint64>(f.dwHighDateTime) << 32 | f.dwLowDateTime); };  // 100 ns
  return (ticks(kernel) + ticks(user)) / 10000;
#else
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return static_cast<qint64>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
#endif
}

Stalls stalls() { return stallLog().since; }
void resetStalls() { stallLog().since = {}; }

QString stallHistogram() {
  const StallLog& s = stallLog();
  QStringList parts;
  qint64 from = stallThreshold();
  for (size_t i = 0; i < s.buckets.size(); ++i) {
    parts << (i < kEdges.size() ? QStringLiteral("%1-%2 ms: %3").arg(from).arg(kEdges[i]).arg(s.buckets[i]) : QStringLiteral("%1+ ms: %2").arg(from).arg(s.buckets[i]));
    if (i < kEdges.size()) from = std::max(from, kEdges[i]);
  }
  return QStringLiteral("stall histogram (over %1 ms): %2; %3 stalls, longest %4 ms, %5 ms in all")
      .arg(stallThreshold()).arg(parts.join(", ")).arg(s.all.count).arg(s.all.longest).arg(s.all.total);
}

void installUiWatchdog(QObject* parent) {
  auto* timer = new QTimer(parent);
  auto last = std::make_shared<QElapsedTimer>();
  auto lastCpu = std::make_shared<qint64>(threadCpuMs());
  last->start();
  timer->setTimerType(Qt::PreciseTimer);
  timer->setInterval(enabled() ? kTickMs : 100);
  // The time between two ticks is how long the event loop could not run, give or take one tick; the thread's CPU time in
  // it tells work from waiting for a core on a busy machine.
  QObject::connect(timer, &QTimer::timeout, parent, [last, lastCpu] {
    const qint64 gap = last->restart();
    const qint64 cpu = threadCpuMs(), work = cpu - std::exchange(*lastCpu, cpu);
    Frames& f = frames();
    const qint64 now = f.clock.elapsed(), quiet = std::max(f.longest, now - f.last);
    const int drawn = std::exchange(f.count, 0);
    f.longest = 0;
    f.last = now;
    if (gap <= stallThreshold()) return;
    if (drawn > 1 && quiet <= stallThreshold()) {  // frames back to back: the loop ran between them
      log(QStringLiteral("UI thread drew %1 frames in %2 ms (%3 ms of CPU): an animation, not a stall").arg(drawn).arg(gap).arg(work));
      return;
    }
    StallLog& s = stallLog();
    for (Stalls* st : {&s.since, &s.all}) {
      ++st->count;
      st->longest = std::max(st->longest, gap);
      st->longestCpu = std::max(st->longestCpu, work);
      st->total += gap;
    }
    ++s.buckets[static_cast<size_t>(std::upper_bound(kEdges.begin(), kEdges.end(), gap) - kEdges.begin())];
    const QString msg = QStringLiteral("UI thread stalled for %1 ms (%2 ms of CPU)").arg(gap).arg(work);
    if (gap > kWarnMs) qWarning("%s", msg.toUtf8().constData());
    log(msg);
  });
  if (enabled()) QObject::connect(qApp, &QCoreApplication::aboutToQuit, parent, [] { log(stallHistogram()); });
  timer->start();
}
}  // namespace trace
