// The job runner's feedback (UI-109), offscreen: the busy cursor after 150 ms for jobs someone waits for (never for a
// background one, never flickering on during a drag), "+N" beside the job in the strip (the oldest Foreground one,
// UI-40), the done signal with what a completion toast needs (background flag, done text, run time, whether the strip
// showed it).
#include "Jobs.hpp"
#include "ProgressStrip.hpp"
#include "check.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QThread>
#include <QTest>

namespace {
// Runs the event loop until `done` holds or `ms` pass.
bool wait(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QTest::qWait(10);
  return done();
}
void sleepFor(int ms) { QThread::msleep(ms); }
}  // namespace

TEST(busy_cursor_after_150_ms) {
  ProgressStrip strip;
  JobRunner jobs(&strip);
  Job* job = jobs.async("Wait", [](Progress) { sleepFor(500); });
  QPointer<Job> watched(job);
  QTest::qWait(60);
  CHECK(!jobs.busyCursor() && !QGuiApplication::overrideCursor());
  CHECK(wait([&] { return jobs.busyCursor(); }, 400));
  CHECK(watched && watched->elapsedMs() >= 150);
  CHECK(QGuiApplication::overrideCursor() && QGuiApplication::overrideCursor()->shape() == Qt::BusyCursor);
  CHECK(wait([&] { return !jobs.busy(); }, 2000));
  CHECK(!jobs.busyCursor() && !QGuiApplication::overrideCursor());
}

TEST(background_jobs_leave_the_cursor_alone) {
  ProgressStrip strip;
  JobRunner jobs(&strip);
  jobs.backgroundNext();
  Job* quiet = jobs.async("Snapshot", [](Progress) { sleepFor(400); });
  Job* next = jobs.begin("Next");
  CHECK(quiet->background() && !next->background());  // only the next job
  next->finish();
  QTest::qWait(300);
  CHECK(!jobs.busyCursor() && !QGuiApplication::overrideCursor());
  CHECK(wait([&] { return !jobs.busy(); }, 2000));
}

TEST(no_busy_cursor_starts_while_a_button_is_down) {
  ProgressStrip strip;
  strip.show();
  JobRunner jobs(&strip);
  QWidget area;
  area.resize(100, 100);
  area.show();
  QTest::mousePress(&area, Qt::LeftButton);  // a drag: the cursor must not flicker to busy under it
  Job* job = jobs.begin("Preview");
  QTest::qWait(300);
  const bool duringDrag = jobs.busyCursor();
  QTest::mouseRelease(&area, Qt::LeftButton);
  CHECK(wait([&] { return jobs.busyCursor(); }, 300));  // released: it turns on
  job->finish();
  QTest::qWait(10);
  CHECK(!duringDrag && !jobs.busyCursor() && !QGuiApplication::overrideCursor());
}

TEST(others_beside_the_oldest_job) {
  // The oldest Foreground job keeps the strip (UI-40: a load's Cancel stays where it is while others begin); a Background
  // job is the activity dot's, never counted beside it.
  ProgressStrip strip;
  JobRunner jobs(&strip);
  Job* a = jobs.begin("First");
  Job* b = jobs.begin("Second");
  Job* c = jobs.begin("Third");
  Job* tidy = jobs.begin("Tidy", false, JobKind::Background);
  CHECK(strip.othersText().isEmpty());  // nothing shows before 0.5 s
  QTest::qWait(600);
  CHECK_EQ(strip.othersText(), QString("+2"));
  const QString tip = strip.findChild<QLabel*>("progressOthers")->toolTip();
  CHECK(!tip.contains("First") && tip.contains("Second") && tip.contains("Third") && !tip.contains("Tidy"));
  CHECK(a->wasShown() && !c->wasShown() && !tidy->wasShown());
  a->finish();
  QTest::qWait(10);
  CHECK_EQ(strip.othersText(), QString("+1"));  // Second shows now, Third beside it
  CHECK(b->wasShown());
  c->finish();
  QTest::qWait(10);
  CHECK(strip.othersText().isEmpty());
  b->finish();
  tidy->finish();
  QTest::qWait(10);
  CHECK(!jobs.busy() && strip.isHidden() && strip.othersText().isEmpty());
}

TEST(done_signal_carries_what_a_toast_needs) {
  ProgressStrip strip;
  JobRunner jobs(&strip);
  struct Seen { QString title, text, error; bool ok = false, background = false, shown = false; qint64 ms = 0; };
  QList<Seen> seen;
  QObject::connect(&jobs, &JobRunner::done, [&](Job* j, bool ok, const QString& error) {
    seen << Seen{j->title(), j->doneText(), error, ok, j->background(), j->wasShown(), j->elapsedMs()};
  });
  Job* load = jobs.begin("Opening box.step", true);
  load->setDoneText("Opened box.step · 3 bodies");
  QTest::qWait(650);
  load->finish();
  jobs.backgroundNext();
  jobs.async("Snapshot", [](Progress) {});
  Job* failing = jobs.async("Failing", [](Progress) { throw std::runtime_error("no such face"); });
  Q_UNUSED(failing);
  Job* cancelled = jobs.begin("Cancelled");
  cancelled->cancel();
  CHECK(wait([&] { return seen.size() == 4; }, 2000));
  auto find = [&](const QString& title) { for (const Seen& s : seen) if (s.title == title) return s; throw check::Failure("no done for " + title.toStdString()); };
  const Seen opened = find("Opening box.step");
  CHECK(opened.ok && opened.shown && !opened.background && opened.ms >= 600 && opened.text == "Opened box.step · 3 bodies");
  CHECK(find("Snapshot").background && find("Snapshot").ok);
  CHECK(!find("Failing").ok && find("Failing").error == "no such face");
  CHECK(!find("Cancelled").ok && find("Cancelled").error == "cancelled");
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return check::run_all(argc, argv);
}
