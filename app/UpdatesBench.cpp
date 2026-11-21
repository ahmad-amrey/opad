// Bench: the update check through Help > Check for updates (OPAD_BENCH_UPDATES), against answers written to a local file
// (OPAD_UPDATE_URL), never GitHub. A newer release must come as a notice with What's new, Download and Skip this version;
// What's new must show the release's notes; the same version must say it is the newest; a broken answer must say the
// check failed. Then Help > Offer development builds: the list (OPAD_UPDATE_LIST_URL) holds a release candidate of a version
// not released yet, offered as "OPAD X.Y.Z release candidate (build N) is available"; Skip this version remembers its tag.
// The automatic check stays off in benches (UpdatesArea.cpp). Case: tools/bench_cases/updates.py.
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <memory>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Toast.hpp"
#include "UpdateCheck.hpp"

OPAD_BENCH(OPAD_BENCH_UPDATES, updates) {
  const QString file = QDir::temp().filePath(QString("opad-updates-bench-%1.json").arg(QCoreApplication::applicationPid()));
  auto answer = [file](const QByteArray& body) {
    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) f.write(body);
  };
  qputenv("OPAD_UPDATE_URL", QUrl::fromLocalFile(file).toString().toUtf8());
  const QString current = QApplication::applicationVersion();
  auto release = [](const QString& tag) {
    return QString(R"({"tag_name":"%1","name":"OPAD %2","body":"## Added\n\n- The bench's own release notes","html_url":"https://example.invalid/%1","draft":false,"prerelease":false})")
        .arg(tag, tag.mid(1))
        .toUtf8();
  };
  QAction* check = w.findChild<QAction*>("help.checkUpdates");
  if (!check || !w.findChild<QAction*>("help.autoUpdates")) {
    trace::log("bench: updates: the Help commands are missing FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  struct State { int phase = 0, ticks = 0; };
  auto st = std::make_shared<State>();
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [=, &w] {
    auto finish = [timer, file](bool ok, const QString& why) {
      timer->stop();
      QFile::remove(file);
      trace::log(QString("bench: updates: %1 %2").arg(why, ok ? "PASS" : "FAIL"));
      QCoreApplication::exit(ok ? 0 : 2);
    };
    auto toastWith = [&w](const QString& part) -> Toast* {
      for (Toast* t : w.m_toasts->toasts())
        if (t->text().contains(part)) return t;
      return nullptr;
    };
    if (++st->ticks > 150) return finish(false, QString("timed out in step %1").arg(st->phase));
    switch (st->phase) {
      case 0:
        answer(release("v99.0.0"));
        check->trigger();
        st->phase = 1;
        return;
      case 1: {
        Toast* t = toastWith("99.0.0");
        if (!t) return;
        if (!t->detail().contains(current) || !t->actionButton(0) || !t->actionButton(1) || !t->actionButton(2))
          return finish(false, "the notice lacks the version you have or its three answers: " + t->text() + " / " + t->detail());
        if (!t->detail().contains("The bench's own release notes"))
          return finish(false, "the notice does not say what the release brings: " + t->detail());
        trace::log("bench: updates: a newer release is offered with what it brings, What's new, Download and Skip this version PASS");
        t->actionButton(0)->click();  // What's new
        st->phase = 2;
        return;
      }
      case 2: {
        auto* notes = w.findChild<QTextBrowser*>("updatesNotesText");
        if (!notes) return;
        if (!notes->toPlainText().contains("The bench's own release notes")) return finish(false, "What's new does not show the notes: " + notes->toPlainText());
        trace::log("bench: updates: What's new shows the release notes PASS");
        notes->window()->close();
        answer(release("v" + current));
        check->trigger();
        st->phase = 3;
        return;
      }
      case 3:
        if (!toastWith(QString("%1 is the newest").arg(current))) return;
        trace::log("bench: updates: the same version says it is the newest PASS");
        answer("not an answer");
        check->trigger();
        st->phase = 4;
        return;
      case 4: {
        if (!toastWith("Could not check for updates")) return;
        trace::log("bench: updates: a broken answer says the check failed PASS");
        // The development channel: a list with a release candidate (v99.0.1-rc.999, 99.0.1 not released), a development
        // build before it (v99.0.1-dev.998, its binary still 99.0.0) and an older release.
        const QString list = QDir::temp().filePath(QString("opad-updates-bench-list-%1.json").arg(QCoreApplication::applicationPid()));
        QFile f(list);
        if (f.open(QIODevice::WriteOnly))
          f.write(R"json([{"tag_name":"v99.0.1-rc.999","name":"OPAD 99.0.1 release candidate (build 999)","body":"<!-- opad-build: 999; opad-version: 99.0.1 -->",)json"
                  R"json("html_url":"https://github.com/ahmad-amrey/opad/releases/tag/v99.0.1-rc.999","draft":false,"prerelease":true},)json"
                  R"json({"tag_name":"v99.0.1-dev.998","name":"OPAD 99.0.1 development build 998","body":"<!-- opad-build: 998; opad-version: 99.0.0 -->",)json"
                  R"json("html_url":"https://github.com/ahmad-amrey/opad/releases/tag/v99.0.1-dev.998","draft":false,"prerelease":true},)json"
                  R"json({"tag_name":"v99.0.0","name":"OPAD 99.0.0","body":"","html_url":"https://github.com/ahmad-amrey/opad/releases/tag/v99.0.0","draft":false,"prerelease":false}])json");
        f.close();
        qputenv("OPAD_UPDATE_LIST_URL", QUrl::fromLocalFile(list).toString().toUtf8());
        QAction* dev = w.findChild<QAction*>("help.devBuilds");
        if (!dev || dev->isChecked()) return finish(false, "Offer development builds is missing, or on by default");
        dev->trigger();
        if (!dev->isChecked() || QSettings().value(updates::kChannelKey).toString() != "development") return finish(false, "Offer development builds did not set the channel");
        check->trigger();
        st->phase = 5;
        return;
      }
      case 5: {
        Toast* t = toastWith("release candidate (build 999)");
        if (!t) return;
        if (!t->text().contains("99.0.1") || !t->actionButton(2)) return finish(false, "the candidate's notice: " + t->text());
        t->actionButton(2)->click();  // Skip this version
        const QString skipped = QSettings().value(updates::kSkipKey).toString();
        QAction* dev = w.findChild<QAction*>("help.devBuilds");
        dev->trigger();  // back to releases only
        qunsetenv("OPAD_UPDATE_LIST_URL");
        if (skipped != "v99.0.1-rc.999") return finish(false, "Skip this version did not remember the build's tag: " + skipped);
        return finish(dev && !dev->isChecked() && QSettings().value(updates::kChannelKey).toString() == "stable",
                      "development builds: a release candidate offered by its version and build, skipped by its tag");
      }
    }
  });
  timer->start();
  return true;
}
