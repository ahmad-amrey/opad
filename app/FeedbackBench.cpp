// Benches of the feedback layer (UI-109): hints and results as toasts, the busy cursor, "+N" in the strip, the load
// shade's text and completion toasts. The completion toasts are FeedbackArea.cpp's, the rest MainWindow's and Jobs.cpp's.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "LoadShade.hpp"
#include "ProgressStrip.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QSettings>
#include <QStatusBar>
#include <QThread>
#include <QToolButton>

namespace {
// A sequence of steps, each after its delay and, with `until`, once that holds (polled every 50 ms, 20 s at most: a busy
// machine stalls the event loop for seconds); a step that throws fails the bench.
struct Steps {
  struct Step { int delay; std::function<void()> fn; std::function<bool()> until; };
  std::shared_ptr<std::vector<Step>> list = std::make_shared<std::vector<Step>>();
  void add(int delay, std::function<void()> fn, std::function<bool()> until = {}) { list->push_back({delay, std::move(fn), std::move(until)}); }
  void run(QObject* context, std::function<void(bool, const QString&)> check) {
    auto next = std::make_shared<std::function<void(size_t, int)>>();
    *next = [context, list = list, next, check](size_t i, int waited) {
      if (i >= list->size()) return;
      const Step& s = (*list)[i];
      QTimer::singleShot(waited ? 50 : s.delay, context, [list, next, check, i, waited] {
        const Step& s = (*list)[i];
        if (s.until && !s.until() && waited < 400) return (*next)(i, waited + 1);
        try { s.fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
        (*next)(i + 1, 0);
      });
    };
    (*next)(0, 0);
  }
};

Toast* toastWith(ToastStack* stack, const QString& text) {
  for (Toast* t : stack->toasts())
    if (t->text() == text) return t;
  return nullptr;
}
}  // namespace

// OPAD_BENCH_FEEDBACK=<prefix> (a document with a box, saved as box.opad): what used to be a modal warning is a toast. Lock
// with nothing selected says what it needs, waits, and locks the body once it is selected; Esc or the toast's Cancel
// drop the wait, a hint without a pick goes by itself and nothing waits; Save ends in a toast with Open folder. Then the
// jobs: a background job leaves the cursor alone, one the user waits for turns the busy cursor on after 150 ms; three at
// once show "+2" beside the newest in the strip; those that ran long enough end in a completion toast (the background one
// does not); a change that fails is an error toast, a cancelled one says nothing; reopening the file names it and its
// phase on the load shade and ends in "Opened box.opad · 1 bodies". No
// message box may open on the way. <prefix>.hint.png (the waiting toast), <prefix>.failed.png, <prefix>.strip.png,
// <prefix>.shade.png.
OPAD_BENCH(OPAD_BENCH_FEEDBACK, feedback) {
  static bool started = false;  // reopening the file below finishes a load, which asks the benches again
  if (std::exchange(started, true)) return true;
  const QString prefix = value;
  QSettings().setValue("ui/doneToastSeconds", 0.5);
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: feedback: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  // A message box here would wait for a click on the user's desktop: close it and fail.
  auto* modal = new QTimer(&w);
  QObject::connect(modal, &QTimer::timeout, &w, [check] {
    if (QWidget* m = QApplication::activeModalWidget()) {
      check(false, "no modal window (" + m->windowTitle() + ")");
      m->close();
    }
  });
  modal->start(100);
  const std::string body = w.m_doc->scene.all_bodies().front();
  ToastStack* stack = w.m_toasts;
  auto select = [&w](const std::vector<std::string>& ids) {
    w.m_browser->setSelectedIds(ids);
    w.onBrowserSelection(ids);
  };
  Steps steps;
  steps.add(600, [=, &w] {
    stack->clear();
    select({});
    w.action("design.lock")->trigger();
    const QString lock = i18n::t("Select the objects to lock or unlock first.");
    Toast* t = toastWith(stack, lock);
    check(t && t->actionButton() && t->timeout() == 0 && w.m_pendingPick == "design.lock", "Lock with nothing selected: a toast that waits, with Cancel (" + lock + ")");
    if (t) t->grab().save(prefix + ".hint.png");
    w.action("view.fit")->trigger();
    check(w.m_pendingPick == "design.lock", "looking around keeps the wait");
    select({body});
  });
  auto idle = [&w] { return !w.m_jobs->busy(); };  // the selection settled, nothing else running
  steps.add(400, [=, &w] {
    check(w.m_doc->node(body)->locked && w.m_pendingPick.isEmpty() && !toastWith(stack, i18n::t("Select the objects to lock or unlock first.")),
          "selecting the body runs Lock on it and takes the toast away");
    w.m_doc->undo();
    select({});
  }, idle);
  steps.add(100, [=, &w] {
    w.action("inspect.properties")->trigger();
    check(w.m_pendingPick == "inspect.properties" && toastWith(stack, i18n::t("Select something to see its properties.")), "Properties waits for a selection");
    w.action("inspect.clear")->trigger();  // Esc
    check(w.m_pendingPick.isEmpty() && !toastWith(stack, i18n::t("Select something to see its properties.")), "Esc drops the wait and its toast");
    w.action("design.reparent")->trigger();
    Toast* t = toastWith(stack, i18n::t("Select the objects to move under another component first."));
    check(t && w.m_pendingPick == "design.reparent", "Reparent waits");
    if (t) t->actionButton()->click();
    check(w.m_pendingPick.isEmpty(), "the toast's Cancel drops the wait");
    select({body});
  }, idle);
  steps.add(400, [=, &w] {
    check(!QApplication::activeModalWidget() && !w.m_doc->node(body)->locked, "a cancelled command does not run on the next selection");
    select({});
    w.action("annotate.resolve")->trigger();
    Toast* t = toastWith(stack, i18n::t("Select a note in the Annotations panel or on the timeline first."));
    check(t && !t->actionButton() && t->timeout() > 0 && w.m_pendingPick.isEmpty(), "a hint without a pick goes by itself and nothing waits");
    stack->clear();
    w.action("file.save")->trigger();
    const QString name = QFileInfo(w.m_doc->path()).fileName();
    Toast* saved = toastWith(stack, QCoreApplication::translate("MainWindow", "Saved %1").arg(name));
    check(saved && saved->actionButton() && saved->actionButton()->text() == QCoreApplication::translate("MainWindow", "Open folder"), "Save ends in a toast with Open folder");
    stack->clear();
    // A change that failed, the document as it was: a toast with a red edge for 10 s, no message box; cancelled: nothing.
    w.m_doc->designBusy = true;
    w.m_design->applyOps({opad::json{{"op", "units"}, {"length", "in"}}}, "bench");
    w.m_doc->designBusy = false;
    Toast* refused = toastWith(stack, QCoreApplication::translate("DesignController", "The design is still being recomputed; try again in a moment."));
    check(refused && refused->property("kind").toString() == "error" && refused->timeout() == 10000 && !QApplication::activeModalWidget() && w.m_doc->scene.units == "mm",
          "a change refused while the design recomputes: an error toast, no message box, nothing changed");
    if (refused) refused->grab().save(prefix + ".failed.png");
    const auto shown = stack->toasts().size();
    emit w.m_design->failed("cancelled");
    check(stack->toasts().size() == shown, "a cancelled change says nothing");
    stack->clear();
  });
  steps.add(100, [=, &w] {  // a background job never turns the busy cursor on
    w.m_jobs->backgroundNext();
    w.m_jobs->async("Bench background", [](Progress) { QThread::msleep(700); });
  }, idle);
  steps.add(300, [=, &w] {
    check(!w.m_jobs->busyCursor() && !QGuiApplication::overrideCursor(), "a background job leaves the cursor alone");
    // Recovery and the agent bridge copy the document in the background (every 2 min while it is dirty).
    const bool started = w.m_doc->captureSnapshot(w.m_jobs, [](std::shared_ptr<opad::Document>, const QString&) {}, true);
    Job* capture = w.m_jobs->newest();  // the job begun last (current(): the oldest Foreground one, UI-40)
    check(started && capture && capture->title() == QCoreApplication::translate("AppDocument", "Capturing document") && capture->background(),
          "a recovery snapshot's capture is a background job");
  });
  auto waiting = std::make_shared<QPointer<Job>>();
  steps.add(100, [=, &w] {
    *waiting = w.m_jobs->async("Bench wait", [](Progress) { QThread::msleep(600); });
  }, idle);
  steps.add(80, [=, &w] {
    const qint64 ran = *waiting ? (*waiting)->elapsedMs() : 0;  // a stalled event loop can come back late
    check(!w.m_jobs->busyCursor() || ran >= 150, QString("no busy cursor in the first 150 ms (%1 ms)").arg(ran));
  });
  steps.add(170, [=, &w] {
    const QCursor* c = QGuiApplication::overrideCursor();
    check(w.m_jobs->busyCursor() && c && c->shape() == Qt::BusyCursor, "the busy cursor after 150 ms");
  });
  steps.add(500, [=, &w] {
    check(!w.m_jobs->busyCursor() && !QGuiApplication::overrideCursor(), "and gone when the job ends");
    const QList<Toast*> shown = stack->toasts();
    const auto waited = std::find_if(shown.begin(), shown.end(), [](Toast* t) { return t->text().startsWith("Bench wait · "); });
    check(waited != shown.end(), "a job that ran long enough ends in a completion toast (" + (waited != shown.end() ? (*waited)->text() : QString()) + ")");
    check(std::none_of(shown.begin(), shown.end(), [](Toast* t) { return t->text().startsWith("Bench background"); }), "the background job ended quietly");
    stack->clear();
  });
  steps.add(100, [=, &w] {
    for (int i = 1; i <= 3; ++i) w.m_jobs->async(QString("Bench job %1").arg(i), [](Progress) { QThread::msleep(1400); });
  }, idle);
  steps.add(800, [=, &w] {
    const QString others = w.m_progress->othersText();
    const QString tip = w.m_progress->findChild<QLabel*>("progressOthers")->toolTip();
    // The oldest keeps the strip (UI-40: a load's Cancel stays where it is while others begin).
    check(w.m_progress->isVisible() && others == "+2" && !tip.contains("Bench job 1") && tip.contains("Bench job 2") && tip.contains("Bench job 3"),
          "three jobs: the oldest in the strip, +2 beside it, named in its tooltip (" + others + ")");
    w.statusBar()->grab().save(prefix + ".strip.png");
  });
  steps.add(1000, [=, &w] {
    check(!w.m_progress->isVisible() && w.m_progress->othersText().isEmpty(), "the strip goes when they end");
    int done = 0;
    for (Toast* t : stack->toasts()) done += t->text().startsWith("Bench job");
    check(done == 3, QString("each ends in a completion toast (%1)").arg(done));
    stack->clear();
    QSettings().setValue("ui/doneToastSeconds", 0.001);
    w.openPath(w.m_doc->path());
    const QString opening = QCoreApplication::translate("MainWindow", "Opening %1").arg(QFileInfo(w.m_doc->path()).fileName());
    check(w.m_loadShade->isVisible() && w.m_loadShade->text().startsWith(opening), "the load shade names the file (" + w.m_loadShade->text().replace('\n', " / ") + ")");
    w.setLoadPhase("Reading the bench file", 40);
    check(w.m_loadShade->text().contains("Reading the bench file · 40%"), "and the phase it is in");
    w.m_loadShade->grab().save(prefix + ".shade.png");
  });
  steps.add(300, [=, &w] {
    const QString opened = QCoreApplication::translate("MainWindow", "Opened %1 · %2 bodies").arg(QFileInfo(w.m_doc->path()).fileName(), "1");
    check(!w.m_loadJob && toastWith(stack, opened), "the load ends in its own completion toast (" + opened + ")");
    QSettings().setValue("ui/doneToastSeconds", 3);
    trace::log(QString("bench: feedback: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  }, [&w] { return !w.m_loadJob; });
  steps.run(&w, check);
  return true;
}
