// Feedback when work ends (UI-109): a job someone waited for that ran at least ui/doneToastSeconds (3 s; 0: never) ends in
// a toast at the bottom of the view, "Opened engine.step · 1,295 bodies" or "<title> · done in 4.2 s", so a long run is
// noticed when the user looked away. Background jobs (recovery snapshots, zoom refinement, agent traffic), failures (their
// owners say why) and cancelled ones end quietly. The busy cursor and the strip's "+N" are JobRunner's (Jobs.hpp).
#include <QLocale>
#include <QSettings>

#include "AreaController.hpp"
#include "Jobs.hpp"

class FeedbackArea : public AreaController {
 public:
  using AreaController::AreaController;

  void ready() override {
    connect(services().jobs(), &JobRunner::done, this, [this](Job* job, bool ok, const QString&) {
      const QString text = doneText(job, ok);
      if (!text.isEmpty()) services().toast(text);
    });
  }

  static QString doneText(const Job* job, bool ok) {
    const double seconds = QSettings().value("ui/doneToastSeconds", 3.0).toDouble();
    if (!ok || job->background() || seconds <= 0 || job->elapsedMs() < seconds * 1000) return {};
    if (!job->doneText().isEmpty()) return job->doneText();
    QString title = job->title();
    title.remove(QString::fromUtf8("…"));
    return tr("%1 · done in %2 s").arg(title.trimmed(), QLocale().toString(job->elapsedMs() / 1000.0, 'f', 1));
  }
};

OPAD_AREA(FeedbackArea)
