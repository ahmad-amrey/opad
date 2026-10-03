#pragma once
// Long operations. The rule (see CLAUDE.md): nothing that scales with model size runs on the UI thread in
// one go. Everything that might take longer than a frame goes through JobRunner, which gives it a cancel
// flag, a phase/percent, the status-bar ProgressStrip after 0.5 s, and a Cancel button:
//
//   JobRunner::async  - work on a worker thread (core geometry, file IO); progress marshalled to the UI
//   JobRunner::sliced - UI-thread-only work (OCCT AIS calls, Qt widgets) run in ~10 ms slices between events
//   JobRunner::begin  - a job driven by something with its own threading (AppDocument loads)
//
// A UI watchdog logs any event-loop stall over 250 ms so violations are visible; set OPAD_TRACE=1 (stderr)
// or OPAD_TRACE=<file> to see those and the timing scopes.
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class ProgressStrip;
class Job;

namespace detail {
// Shared between a Job (UI thread) and the Progress handles a worker holds; outlives the Job so a worker
// can never touch a deleted QObject.
struct JobState {
  std::mutex mu;
  Job* job = nullptr;
  std::atomic<bool> cancel{false};
  void post(std::function<void(Job&)> fn);  // runs fn on the UI thread if the Job still exists
};
}  // namespace detail

// Handle a worker thread uses to report progress and poll for cancellation.
class Progress {
 public:
  bool cancelled() const { return m_s->cancel.load(); }
  void setPhase(const QString& text, int percent = -1) const;  // percent < 0: indeterminate
  void setOverall(int percent) const;
 private:
  friend class Job;
  friend class JobRunner;
  explicit Progress(std::shared_ptr<detail::JobState> s) : m_s(std::move(s)) {}
  std::shared_ptr<detail::JobState> m_s;
};

class Job : public QObject {
  Q_OBJECT
 public:
  ~Job() override;
  bool cancelled() const { return m_state->cancel.load(); }
  bool active() const { return m_active; }
  const QString& title() const { return m_title; }
  qint64 elapsedMs() const { return m_clock.elapsed(); }
  Progress progress() const { return Progress(m_state); }
  void setPhase(const QString& text, int percent = -1);
  void setOverall(int percent);
  void cancel();                                           // sets the flag, emits cancelRequested, finishes the job
  void finish(bool ok = true, const QString& error = {});  // idempotent; emits finished once, on the UI thread
 signals:
  void phaseChanged(const QString& text, int percent);
  void overallChanged(int percent);
  void cancelRequested();  // hook extra teardown here (stop a worker, abandon a mesh queue)
  void finished(bool ok, const QString& error);
 private:
  friend class JobRunner;
  friend struct detail::JobState;
  Job(const QString& title, bool twoBars, QObject* parent);
  QString m_title;
  bool m_twoBars;
  bool m_active = true;
  std::shared_ptr<detail::JobState> m_state;
  QElapsedTimer m_clock;
  QString m_lastPhase;                    // what the strip shows when it appears later than the update
  int m_lastPct = -1, m_lastOverall = -1;
  std::function<bool(Job&)> m_step;       // sliced jobs only
  std::function<void(bool)> m_stepDone;
};

class JobRunner : public QObject {
  Q_OBJECT
 public:
  explicit JobRunner(ProgressStrip* strip, QObject* parent = nullptr);
  // A job whose work runs elsewhere; the caller drives setPhase/finish (or cancel via the strip).
  Job* begin(const QString& title, bool twoBars = false);
  // Runs `work` on a worker thread. Exceptions become a failed finish; `done` runs on the UI thread.
  Job* async(const QString& title, std::function<void(Progress)> work, std::function<void(bool ok, const QString& error)> done = {});
  // The same, never shown in the strip nor counted by busy(): short reads in the background (git status).
  Job* quiet(const QString& title, std::function<void(Progress)> work, std::function<void(bool ok, const QString& error)> done = {});
  // `step` does one small unit of UI-thread work and returns true while more remains. It runs in slices of
  // about 10 ms between events, starting on the next event-loop turn (never inside this call). `done(completed)`
  // runs once, before finished(); completed is false when the job was cancelled.
  Job* sliced(const QString& title, std::function<bool(Job&)> step, std::function<void(bool completed)> done = {});
  bool busy() const { return !m_jobs.empty(); }
  Job* current() const { return m_jobs.empty() ? nullptr : m_jobs.back(); }
 signals:
  void stripShown(bool shown);  // the owner may hide status-bar widgets that compete for the space
 private:
  void slice(Job* j);
  static void launch(Job* j, std::function<void(Progress)> work, std::function<void(bool, const QString&)> done);
  void onFinished(Job* j);
  void refreshStrip();
  ProgressStrip* m_strip;
  std::vector<Job*> m_jobs;
  QTimer m_showTimer;
  Job* m_shown = nullptr;
};

// Timing/diagnostic output, enabled by OPAD_TRACE (see above). Cheap no-ops otherwise.
namespace trace {
bool enabled();
void log(const QString& line);
struct Scope {  // logs "<name>: N ms" when it goes out of scope
  explicit Scope(QString name) : m_name(std::move(name)) { m_t.start(); }
  ~Scope();
  QString m_name;
  QElapsedTimer m_t;
};
// Logs whenever the UI thread fails to service the event loop for more than 250 ms.
void installUiWatchdog(QObject* parent);
}  // namespace trace
