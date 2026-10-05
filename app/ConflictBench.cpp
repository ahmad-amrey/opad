// OPAD_BENCH_CONFLICT=<prefix> (UI-63) on tools/bench_cases/vcs.py's conflict/model.opad: main renamed the box "Ours",
// coloured it blue and locked it, and added a parameter; the branch theirs renamed it "Theirs", coloured it red, locked it
// too and added a note. Merging theirs stops on the document (OPAD's driver, set up first); the toast's Resolve… reads the
// index stages and lists the name and the colour (the lock both made alike is no conflict); theirs for the name, mine for
// the colour; the file comes in with both sides' other changes, git no longer has it in conflict, and Commit… from the
// toast makes the merge commit (two parents). With OPAD_BENCH_CONFLICT_MARKERS=1 the clone's driver is never set up: git
// merges the text and writes conflict markers into the file: the bar over the view offers Resolve conflicts… (not
// Overwrite…) as its first choice, and the stages still resolve it. Pictures at
// <prefix>.<step>.png.
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <algorithm>
#include <memory>

#include <QSettings>

#include "AppDocument.hpp"
#include "Banner.hpp"
#include "BenchRegistry.hpp"
#include "GitAgent.hpp"
#include "GitWatch.hpp"
#include "MainWindow.hpp"
#include "Toast.hpp"
#include "VersionControl.hpp"
#include "VersionPanel.hpp"
#include "Viewport.hpp"

OPAD_BENCH(OPAD_BENCH_CONFLICT, conflict) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  auto* vc = w.findChild<VersionControl*>();
  if (!vc) return false;
  QSettings().setValue(gitagent::kBranchesKey, QString());  // the merge into main: branch protection has its bench (version-protect)
  struct State {
    int step = 0;
    QElapsedTimer clock;
    QString lastDone;
    bool driverAsked = false, busy = false;
    std::string body;
  };
  auto st = std::make_shared<State>();
  st->clock.start();
  QObject::connect(vc, &VersionControl::finished, &w, [st](const QString& what, bool ok) { st->lastDone = what + (ok ? " ok" : " failed"); });
  const QString prefix = value;
  const bool markers = qEnvironmentVariableIntValue("OPAD_BENCH_CONFLICT_MARKERS") == 1;
  GitWatch* git = vc->git();
  auto require = [](bool ok, const QString& why) {
    if (!ok) throw std::runtime_error(why.toStdString());
  };
  auto pass = [](const QString& what) { trace::log("bench: conflict: " + what + " PASS"); };
  auto dialog = [&w](const char* name) -> QDialog* {
    for (QDialog* d : w.findChildren<QDialog*>(QString::fromLatin1(name)))
      if (d->isVisible()) return d;
    return nullptr;
  };
  auto press = [](QWidget* parent, const char* action) {
    for (QPushButton* b : parent->findChildren<QPushButton*>())
      if (b->property("action") == action && b->isEnabled()) {
        b->click();
        return true;
      }
    return false;
  };
  auto toast = [&w](const QString& action) -> Toast* {
    for (Toast* t : w.m_viewport->findChildren<Toast*>())
      if (t->isVisible() && t->actionButton() && t->actionButton()->text() == action) return t;
    return nullptr;
  };
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [=, &w] {
    auto finish = [timer](int code) {
      timer->stop();
      QCoreApplication::exit(code);
    };
    if (st->busy) return;
    st->busy = true;
    struct Unbusy {
      State* s;
      ~Unbusy() { s->busy = false; }
    } unbusy{st.get()};
    try {
      if (st->clock.elapsed() > 90000) throw std::runtime_error("step " + std::to_string(st->step) + " timed out");
      AppDocument* doc = w.m_doc;
      const git::Repo& r = git->repo();
      const bool idle = git->idle() && vc->settled() && !doc->loading;
      switch (st->step) {
        case 0:  // OPAD's merge driver for this clone (the case's attributes ask for it)
          if (!idle || r.state != git::Repo::State::Ready) return;
          if (r.needsDriver() && !markers) {
            if (!std::exchange(st->driverAsked, true)) git->setUpDriver();
            return;
          }
          st->body = doc->scene.all_bodies().front();
          require(doc->nodeName(st->body) == "Ours", "main's version: " + doc->nodeName(st->body));
          vc->mergeBranch("theirs");
          st->step = 1;
          return;
        case 1: {
          QDialog* d = dialog("vcsIncoming");
          if (!idle || !d) return;
          require(press(d, "vcsIncomingMerge"), "Merge in the incoming dialog");
          st->step = 2;
          st->clock.restart();
          return;
        }
        case 2: {
          if (!idle || st->lastDone != "merge failed" || !r.merging || r.doc() != git::Repo::Doc::Conflict) return;
          Banner* bar = nullptr;  // conflict markers: the bar over the view offers Resolve conflicts… first, Overwrite… after it
          for (Banner* b : w.m_viewport->findChildren<Banner*>())
            if (b->state() == "unreadable") bar = b;
          if (markers && (!bar || !bar->button("diskResolve"))) return;  // the file read on a worker, git's view in
          require(!bar == !markers, "the unreadable banner exactly when the file holds conflict markers");
          if (bar) {
            require(bar->button("diskResolve")->objectName() == "primary" && bar->button("diskOverwrite")->objectName() != "primary", "Resolve conflicts… first, Overwrite… not");
            if (!prefix.isEmpty()) bar->grab().save(prefix + ".banner.png");
          }
          Toast* t = toast(VersionControl::tr("Resolve…"));
          require(t, "no Resolve… toast after the stopped merge");
          require(doc->nodeName(st->body) == "Ours" && !doc->isDirty(), "the document keeps ours while in conflict");
          QFile file(doc->path());
          require(file.open(QIODevice::ReadOnly) && file.readAll().contains("<<<<<<<") == markers, markers ? "git's conflict markers in the file" : "the file kept as ours");
          pass(markers ? "git merged the text and wrote conflict markers, Resolve conflicts… offered first by the bar over the view" : "the merge stopped on the document, Resolve… offered");
          if (bar) bar->button("diskResolve")->click();
          else t->actionButton()->click();
          st->step = 3;
          return;
        }
        case 3: {
          QDialog* d = dialog("vcsResolve");
          if (!d) return;
          auto* list = d->findChild<QTreeWidget*>("resolveList");
          require(list && list->topLevelItemCount() == 2, QStringLiteral("two conflicts listed (%1)").arg(list ? list->topLevelItemCount() : -1));
          QTreeWidgetItem* name = list->topLevelItem(0);
          QTreeWidgetItem* colour = list->topLevelItem(1);
          const QString nameField = VersionControl::tr("%1: %2").arg(QString(), VersionControl::fieldText("name"));  // translated field keys
          const QString colourField = VersionControl::tr("%1: %2").arg(QString(), VersionControl::fieldText("color"));
          if (!name->text(0).endsWith(nameField)) std::swap(name, colour);
          require(name->text(0).endsWith(nameField) && colour->text(0).endsWith(colourField), "what: " + name->text(0) + " / " + colour->text(0));
          require(name->text(1).startsWith("Ours") && name->text(2).startsWith("Theirs"), "the names: " + name->text(1) + " / " + name->text(2));
          require(colour->text(1).startsWith("[0,0,1]") && colour->text(2).startsWith("[1,0,0]"), "the colours: " + colour->text(1) + " / " + colour->text(2));
          static_cast<QComboBox*>(list->itemWidget(name, 3))->setCurrentIndex(1);    // theirs
          static_cast<QComboBox*>(list->itemWidget(colour, 3))->setCurrentIndex(0);  // mine
          if (!prefix.isEmpty()) d->grab().save(prefix + ".resolve.png");
          pass("Resolve conflicts lists the name and the colour (the lock both sides set alike is none), mine and theirs side by side");
          require(press(d, "vcsResolveRun"), "Resolve");
          st->step = 4;
          return;
        }
        case 4: {
          if (!idle || st->lastDone != "resolve ok" || r.doc() == git::Repo::Doc::Conflict) return;
          const opad::Node* n = doc->node(st->body);
          if (!n || doc->nodeName(st->body) != "Theirs") return;  // the file still coming in
          require(n->has_color && n->color[2] == 1 && n->color[0] == 0 && n->locked, "mine for the colour, both locked");
          const bool wall = std::any_of(doc->scene.params.begin(), doc->scene.params.end(), [](const opad::Param& p) { return p.name == "wall"; });
          require(doc->scene.annotations.size() == 1 && wall, "both sides' other changes: their note, my parameter");
          require(!doc->isDirty(), "the resolved file taken in as saved");
          Toast* t = toast(VersionControl::tr("Commit…"));
          require(t, "no Commit… toast after resolving");
          pass("resolved: theirs for the name, mine for the colour, their note and my parameter, git no longer in conflict");
          t->actionButton()->click();
          st->step = 5;
          return;
        }
        case 5: {
          QDialog* d = dialog("vcsCommit");
          if (!d) return;
          require(press(d, "vcsCommitRun"), "Commit in the commit dialog");
          st->step = 6;
          return;
        }
        case 6: {
          if (!idle || st->lastDone != "commit ok" || r.merging) return;
          const git::Result parents = git::run(git->context(), {"log", "-1", "--format=%P"});
          require(parents.ok() && QString::fromLatin1(parents.out).trimmed().split(' ').size() == 2, "the merge commit: " + QString::fromLatin1(parents.out));
          pass("the merge committed with two parents");
          trace::log("bench: conflict: a merge stopped on the document, resolved in OPAD PASS");
          return finish(0);
        }
      }
    } catch (const std::exception& e) {
      trace::log(QStringLiteral("bench: conflict: %1 FAIL").arg(QString::fromUtf8(e.what())));
      finish(2);
    }
  });
  timer->start();
  return true;
}
