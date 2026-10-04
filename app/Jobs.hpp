#pragma once
// Long operations. The rule (see CLAUDE.md): nothing that scales with model size runs on the UI thread in
// one go. Everything that might take longer than a frame goes through JobRunner, which gives it a cancel
// flag, a phase/percent, the status-bar ProgressStrip after 0.5 s, and a Cancel button:
//
//   JobRunner::async  - work on a worker thread (core geometry, file IO); progress marshalled to the UI
//   JobRunner::sliced - UI-thread-only work (OCCT AIS calls, Qt widgets) run in ~10 ms slices between events
//   JobRunner::begin  - a job driven by something with its own threading (AppDocument loads)
//
// Each has a kind (UI-40). The strip shows the oldest Foreground job, and its Cancel cancels that job and its children;
// a Child job is part of another one (cancelled with it, never shown on its own); a Background job (housekeeping the
// user did not ask for: highlights, looks, refinement, clearing the view) never takes the strip, only the status bar's
// activity dot, whose tooltip lists the ones running for 0.5 s.
//
// A UI watchdog logs any event-loop stall over OPAD_TRACE_STALL_MS (default 50) so violations are visible; set
// OPAD_TRACE=1 (stderr) or OPAD_TRACE=<file> to see those and the timing scopes.
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QString>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class ProgressStrip;
class Job;

enum class JobKind { Foreground, Child, Background };

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
  JobKind kind() const { return m_kind; }
  Job* parentJob() const { return m_parent; }  // a Child's, while it exists
  const QString& title() const { return m_title; }
  qint64 elapsedMs() const { return m_clock.elapsed(); }
  Progress progress() const { return Progress(m_state); }
  void setPhase(const QString& text, int percent = -1);
  void setOverall(int percent);
  void cancel();                                           // sets the flag, emits cancelRequested, finishes the job
  void finish(bool ok = true, const QString& error = {});  // idempotent; emits finished once, on the UI thread
  // A sliced job waiting for work that arrives from elsewhere (meshes from workers): from inside its step, no slice runs
  // until JobRunner::resume. One job then lives as long as the stream instead of one per batch (UI-40).
  void pause() { m_paused = true; }
  bool paused() const { return m_paused; }
  // A job nobody waits for (recovery snapshots, publishing the selection, agent traffic): it may show in the strip like
  // the others but never turns the busy cursor on or ends in a completion toast (UI-109). Background and Child jobs
  // (UI-40) are such jobs whatever the flag says.
  void setBackground(bool on) { m_background = on; }
  bool background() const { return m_background || m_kind != JobKind::Foreground; }
  // What the completion toast says when the job ran long ("Opened engine.step · 1,295 bodies"); empty: from the title.
  void setDoneText(const QString& text) { m_doneText = text; }
  const QString& doneText() const { return m_doneText; }
  bool wasShown() const { return m_wasShown; }  // the strip showed it at some point
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
  bool m_active = true, m_paused = false;
  JobKind m_kind = JobKind::Foreground;
  QPointer<Job> m_parent;
  std::shared_ptr<detail::JobState> m_state;
  QElapsedTimer m_clock;
  QString m_lastPhase;                    // what the strip shows when it appears later than the update
  int m_lastPct = -1, m_lastOverall = -1;
  bool m_background = false, m_wasShown = false;
  QString m_doneText;
  std::function<bool(Job&)> m_step;       // sliced jobs only
  std::function<void(bool)> m_stepDone;
};

class JobRunner : public QObject {
  Q_OBJECT
 public:
  explicit JobRunner(ProgressStrip* strip, QObject* parent = nullptr);
  // A job whose work runs elsewhere; the caller drives setPhase/finish (or cancel via the strip). A Child needs `parent`
  // (without one it is a Background job).
  Job* begin(const QString& title, bool twoBars = false, JobKind kind = JobKind::Foreground, Job* parent = nullptr);
  // Runs `work` on a worker thread. Exceptions become a failed finish; `done` runs on the UI thread.
  Job* async(const QString& title, std::function<void(Progress)> work, std::function<void(bool ok, const QString& error)> done = {},
             JobKind kind = JobKind::Foreground, Job* parent = nullptr);
  // The same, never shown in the strip nor counted by busy(): short reads in the background (git status).
  Job* quiet(const QString& title, std::function<void(Progress)> work, std::function<void(bool ok, const QString& error)> done = {});
  // `step` does one small unit of UI-thread work and returns true while more remains. It runs in slices of
  // about 10 ms between events, starting on the next event-loop turn (never inside this call). `done(completed)`
  // runs once, before finished(); completed is false when the job was cancelled.
  Job* sliced(const QString& title, std::function<bool(Job&)> step, std::function<void(bool completed)> done = {},
              JobKind kind = JobKind::Foreground, Job* parent = nullptr);
  void resume(Job* job);  // a paused sliced job slices again from the next event-loop turn
  bool busy() const { return !m_jobs.empty(); }
  Job* current() const;  // the oldest Foreground job: what the strip shows and cancels
  Job* newest() const { return m_jobs.empty() ? nullptr : m_jobs.back(); }  // the job begun last
  QStringList titles() const;  // every job running, oldest first
  QStringList background() const;  // titles of the Background jobs (and orphaned children) running now
  int begun() const { return m_begun; }  // jobs begun so far (benches count them)
  void backgroundNext() { m_backgroundNext = true; }  // the next job begun is a background one (Job::setBackground)
  // The busy cursor (arrow and hourglass: the app still takes input) is on while a job that is not a background one has
  // run for 150 ms, until none is left; it waits while a mouse button is down, so a drag never flickers.
  bool busyCursor() const { return m_busyCursor; }
  ~JobRunner() override;
 signals:
  void stripShown(bool shown);  // the owner may hide status-bar widgets that compete for the space
  void activityChanged(const QStringList& titles);  // Background jobs running for 0.5 s or more; empty when none
  // Every job, as it ends (before it is deleted): its title, background flag, done text, elapsedMs() and wasShown().
  void done(Job* job, bool ok, const QString& error);
 private:
  void slice(Job* j);
  static void launch(Job* j, std::function<void(Progress)> work, std::function<void(bool, const QString&)> done);
  void onFinished(Job* j, bool ok, const QString& error);
  void refreshStrip();
  void updateBusy();
  void refreshActivity();
  ProgressStrip* m_strip;
  std::vector<Job*> m_jobs;  // oldest first
  QTimer m_showTimer, m_activityTimer, m_busyTimer;
  Job* m_shown = nullptr;
  QStringList m_activity;
  int m_begun = 0;
  bool m_busyCursor = false, m_backgroundNext = false;
};

// Drops the last reference to `value` on a worker thread (UI-41): freeing large data (a replaced document, its display
// arrays) is many small deallocations that would otherwise hold the UI thread. Nothing else may still use it.
inline void disposeLater(std::shared_ptr<void> value) {
  QThread* t = QThread::create([value = std::move(value)]() mutable { value.reset(); });
  QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
  t->start(QThread::LowPriority);
}

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
// Logs whenever the UI thread fails to service the event loop for longer than OPAD_TRACE_STALL_MS (default 50; 250 and
// stderr only without OPAD_TRACE), and keeps every such stall in a histogram that is logged when the app quits (UI-11).
void installUiWatchdog(QObject* parent);
struct Stalls {
  int count = 0;
  qint64 longest = 0, total = 0;  // ms, as the watchdog measured them (its tick is 16 ms while tracing)
  qint64 longestCpu = 0;          // the most CPU time the UI thread spent in one stall (less than its length on a busy machine)
};
qint64 threadCpuMs();  // CPU time of the calling thread so far
Stalls stalls();       // since the last resetStalls(): benches measure a step with it
void resetStalls();
int stallThreshold();  // ms
QString stallHistogram();  // the whole run: "stalls over 50 ms: 50-100: 4, 100-150: 1, ...; longest 130 ms"
}  // namespace trace
