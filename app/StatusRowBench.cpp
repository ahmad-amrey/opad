// OPAD_BENCH_STATUSROW=<prefix> (UI-08) on a document with a long path in a repository (tools/bench_cases/vcs.py
// "status-row"): the path and the git chip keep their place and width at 1600 and 1280 px wide, under a long hover text,
// while a message shows (in its own label, the hover text hidden) and while the progress strip shows. Pictures of the
// status bar at <prefix>.<step>.png.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLayout>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <memory>

#include "BenchRegistry.hpp"
#include "GitWatch.hpp"
#include "MainWindow.hpp"
#include "ProgressStrip.hpp"
#include "StatusRow.hpp"

OPAD_BENCH(OPAD_BENCH_STATUSROW, status_row) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  auto* git = w.findChild<GitWatch*>();
  struct State {
    int step = 0;
    QElapsedTimer clock;
    QStringList wrong;
  };
  auto st = std::make_shared<State>();
  st->clock.start();
  const QString prefix = value;
  auto check = [&w, git](const QString& when) {
    QStringList wrong;
    if (QLayout* layout = w.statusBar()->layout()) layout->activate();
    PathChip* path = w.m_statusPath;
    QWidget* row = w.m_statusRow;
    QWidget* chip = git ? git->chip() : nullptr;
    if (!row->isVisible()) wrong << "the row hidden";
    if (!path->isVisible() || path->width() < path->minimumSizeHint().width() || path->text().isEmpty())
      wrong << QStringLiteral("the path %1 px of %2 (\"%3\")").arg(path->width()).arg(path->minimumSizeHint().width()).arg(path->text());
    if (!chip || chip->isHidden() || chip->parentWidget() != row || chip->width() < chip->sizeHint().width())
      wrong << QStringLiteral("the git chip %1 px of %2").arg(chip ? chip->width() : -1).arg(chip ? chip->sizeHint().width() : -1);
    else if ((chip->x() > path->x()) == w.isRightToLeft()) wrong << "the git chip before the path";
    // The leading end of the bar, before the message and the hover text.
    const int lead = w.isRightToLeft() ? w.statusBar()->width() - (row->x() + row->width()) : row->x();
    if (lead > 8) wrong << QStringLiteral("the row %1 px from the leading end").arg(lead);
    for (QWidget* middle : {static_cast<QWidget*>(w.m_statusMessage), static_cast<QWidget*>(w.m_statusHover)})
      if (middle->isVisible() && (middle->x() > row->x()) == w.isRightToLeft()) wrong << middle->objectName() + " before the row";
    trace::log(QStringLiteral("bench: status row: %1: path %2/%3 px%4, git chip %5 px (%6) %7").arg(when).arg(path->width()).arg(path->sizeHint().width())
                   .arg(path->elided() ? " cut in the middle" : "").arg(chip ? chip->width() : -1).arg(chip ? chip->property("text").toString() : QString())
                   .arg(wrong.isEmpty() ? "PASS" : "FAIL (" + wrong.join(", ") + ")"));
    return wrong.isEmpty();
  };
  auto shot = [&w, prefix](const QString& step) {
    if (!prefix.isEmpty()) w.statusBar()->grab().save(prefix + "." + step + ".png");
  };
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [=, &w] {
    auto finish = [timer](int code) {
      timer->stop();
      QCoreApplication::exit(code);
    };
    auto fail = [&](const QString& why) {
      trace::log("bench: status row: " + why + " FAIL");
      finish(2);
    };
    switch (st->step) {
      case 0:  // the repository read, the chip says the branch
        if (st->clock.elapsed() > 30000) return fail("git chip never ready");
        if (!git || !git->idle() || git->repo().state != git::Repo::State::Ready) return;
        w.resize(1600, 1000);
        st->step = 1;
        return;
      case 1:
        if (!check("1600 px")) return finish(2);
        shot("1600");
        w.m_statusHover->setText(QString(40, ' ') + QString("hover text ").repeated(60));
        st->step = 2;
        return;
      case 2:
        if (!check("a long hover text")) return finish(2);
        w.statusBar()->showMessage(QStringLiteral("A message that says what just happened, long enough to want the whole bar for itself"));
        st->step = 3;
        return;
      case 3:
        if (!w.m_statusMessage->isVisible() || !w.m_statusMessage->text().startsWith("A message") || w.m_statusHover->isVisible())
          return fail(QStringLiteral("the message not in its label (shown %1, hover shown %2)").arg(w.m_statusMessage->isVisible()).arg(w.m_statusHover->isVisible()));
        if (!check("a message")) return finish(2);
        if (w.m_statusMessage->width() < 200) return fail(QStringLiteral("the message has %1 px").arg(w.m_statusMessage->width()));
        shot("message");
        w.resize(1280, 800);
        st->step = 4;
        return;
      case 4:
        if (!check("1280 px with a message")) return finish(2);
        if (w.m_statusMessage->width() < 200) return fail(QStringLiteral("the message has %1 px at 1280").arg(w.m_statusMessage->width()));
        shot("1280");
        w.m_jobs->async(QStringLiteral("Bench job"), [](Progress p) {
          for (int i = 0; i < 30 && !p.cancelled(); ++i) QThread::msleep(50);
        });
        st->clock.restart();
        st->step = 5;
        return;
      case 5:  // the strip shows after 0.5 s
        if (st->clock.elapsed() > 5000) return fail("the progress strip never showed");
        if (!w.m_progress->isVisible()) return;
        if (!check("the progress strip")) return finish(2);
        shot("strip");
        w.statusBar()->clearMessage();
        st->step = 6;
        return;
      case 6:
        if (w.m_statusMessage->isVisible()) return fail("the message label stays after clearMessage");
        if (w.m_jobs->busy()) return;
        if (!w.m_statusHover->isVisible()) return fail("the hover text stays hidden after the strip");
        if (!check("message cleared, strip gone")) return finish(2);
        trace::log("bench: status row: the path and the git chip never collapse PASS");
        return finish(0);
    }
  });
  timer->start();
  return true;
}
