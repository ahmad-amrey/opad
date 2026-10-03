#include "Jobs.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QThread>
#include <QtGlobal>
#include <algorithm>
#include <cstdio>
#include <thread>

#include "Panels.hpp"

namespace {
constexpr int kStripDelayMs = 500;  // how long an operation may run before progress UI appears
constexpr int kSliceMs = 10;        // budget per UI-thread slice; leaves room for input and painting at 60 Hz
constexpr int kStallMs = 250;       // watchdog threshold
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
  connect(m_strip, &ProgressStrip::cancelRequested, this, [this] { if (Job* c = current()) c->cancel(); });
}

Job* JobRunner::begin(const QString& title, bool twoBars) {
  Job* j = new Job(title, twoBars, this);
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
  connect(j, &Job::finished, this, [this, j] { onFinished(j); });
  if (!m_shown && !m_showTimer.isActive()) m_showTimer.start();
  else if (m_shown) refreshStrip();  // a newer job takes over the strip
  return j;
}

Job* JobRunner::async(const QString& title, std::function<void(Progress)> work, std::function<void(bool, const QString&)> done) {
  Job* j = begin(title, false);
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

Job* JobRunner::sliced(const QString& title, std::function<bool(Job&)> step, std::function<void(bool)> done) {
  Job* j = begin(title, false);
  j->m_step = std::move(step);
  j->m_stepDone = std::move(done);
  // The first slice runs on the next event-loop turn, never inside this call: a job that completed
  // synchronously would run done() before the caller had even stored the returned Job*.
  QTimer::singleShot(0, j, [this, j] { slice(j); });
  return j;
}

// One time-boxed slice of a sliced job, then yield to the event loop and reschedule.
void JobRunner::slice(Job* j) {
  if (!j->active() || !j->m_step) return;
  QElapsedTimer t;
  t.start();
  bool more = true;
  const bool tracing = trace::enabled();
  while (more && !j->cancelled() && t.elapsed() < kSliceMs) {
    const qint64 before = t.elapsed();
    more = j->m_step(*j);
    const qint64 took = t.elapsed() - before;
    if (tracing && took > 50) trace::log(QStringLiteral("job \"%1\": one step took %2 ms (steps must be small)").arg(j->m_title).arg(took));
  }
  if (!j->active()) return;  // cancelled from inside step (or via the strip) - finish already ran
  if (more && !j->cancelled()) {
    QTimer::singleShot(0, j, [this, j] { slice(j); });
    return;
  }
  const bool completed = !j->cancelled();
  j->finish(completed, completed ? QString() : QStringLiteral("cancelled"));
}

void JobRunner::onFinished(Job* j) {
  m_jobs.erase(std::remove(m_jobs.begin(), m_jobs.end(), j), m_jobs.end());
  j->deleteLater();
  if (m_jobs.empty()) {
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

// Shows the most recent job in the strip (only after the 0.5 s grace period has elapsed once).
void JobRunner::refreshStrip() {
  Job* c = current();
  if (!c) return;
  if (!m_shown && m_showTimer.isActive()) return;
  if (c == m_shown) return;
  const bool wasShown = m_shown != nullptr;
  m_shown = c;
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

void installUiWatchdog(QObject* parent) {
  auto* timer = new QTimer(parent);
  auto last = std::make_shared<QElapsedTimer>();
  last->start();
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, parent, [last] {
    const qint64 lag = last->restart() - 100;
    if (lag > kStallMs) {
      const QString msg = QStringLiteral("UI thread stalled for %1 ms").arg(lag + 100);
      qWarning("%s", msg.toUtf8().constData());
      log(msg);
    }
  });
  timer->start();
}
}  // namespace trace
