// OPAD_BENCH_PROVENANCE=<prefix> (UI-64) on tools/bench_cases/vcs.py's provenance/model.opad: a box feature committed by
// Alice, a rename of its body by Bob, an edit of the feature by Carol. The timeline's tooltips say who added each op in
// which commit (read once, on a worker), an op of this session is not committed yet; Show in version history on the
// feature's marker and on the body's row narrows the History page to the commits that touched them, Show all widens it
// again; once a new commit moves HEAD, only that version is read (the others come from the cache) and the session's op is
// said to be added by its author. Pictures at <prefix>.<step>.png.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <memory>

#include "AppDocument.hpp"
#include "BenchMenus.hpp"
#include "BenchRegistry.hpp"
#include "GitWatch.hpp"
#include "MainWindow.hpp"
#include "OpProvenance.hpp"
#include "TimelineWidget.hpp"
#include "ToolPanel.hpp"
#include "VersionControl.hpp"
#include "VersionPanel.hpp"

using namespace benchmenus;

OPAD_BENCH(OPAD_BENCH_PROVENANCE, provenance) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  auto* vc = w.findChild<VersionControl*>();
  if (!vc) return false;
  struct State {
    int step = 0;
    QElapsedTimer clock;
    std::string feature, rename, node, session;
    QString head;  // the commit the first index was read at
    bool busy = false;  // a step that waits for a menu runs the event loop: the timer must not come back into it
    QStringList hashes;  // short, oldest first
    bool committed = false;
  };
  auto st = std::make_shared<State>();
  st->clock.start();
  const QString prefix = value;
  OpProvenance* prov = vc->provenance();
  auto require = [](bool ok, const QString& why) {
    if (!ok) throw std::runtime_error(why.toStdString());
  };
  auto pass = [](const QString& what) { trace::log("bench: provenance: " + what + " PASS"); };
  static const char* const context = "OpProvenance";  // its texts as shown (translated)
  auto tr = [](const char* text) { return QCoreApplication::translate(context, text); };
  auto listed = [vc] {  // the History page's short hashes, top to bottom
    QStringList out;
    QTreeWidget* list = vc->panel()->history();
    for (int i = 0; i < list->topLevelItemCount(); ++i)
      if (vc->panel()->commitOf(list->topLevelItem(i))) out << list->topLevelItem(i)->text(0);
    return out;
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
      if (st->clock.elapsed() > 60000) throw std::runtime_error("step " + std::to_string(st->step) + " timed out");
      AppDocument* doc = w.m_doc;
      switch (st->step) {
        case 0: {  // the repository read
          if (!vc->git()->idle() || vc->git()->repo().state != git::Repo::State::Ready || !vc->ready()) return;
          for (const auto& op : doc->doc.ops) {
            if (op.type == "feature") st->feature = op.id;
            if (op.type == "rename") st->rename = op.id, st->node = op.data.value("target", "");
          }
          require(!st->feature.empty() && !st->rename.empty() && doc->node(st->node), "the case's ops are missing");
          const QString first = w.m_timeline->tooltip(st->feature);
          require(first.contains(tr("Reading who added it in git…").toHtmlEscaped()), "the first hover reads the history: " + first);
          st->step = 1;
          st->clock.restart();
          return;
        }
        case 1: {
          if (!prov->ready()) return;
          const ophistory::Index& ix = prov->index();
          require(ix.commits.size() == 3 && ix.blobs == 3 && ix.blobsRead == 3, QStringLiteral("index: %1 commits, %2 versions, %3 read").arg(ix.commits.size()).arg(ix.blobs).arg(ix.blobsRead));
          for (const auto& c : ix.commits) st->hashes << c.shortHash;
          st->head = ix.head;
          const QString box = w.m_timeline->tooltip(st->feature), renamed = w.m_timeline->tooltip(st->rename);
          require(box.contains("Alice") && box.contains(st->hashes[0]) && box.contains("Carol") && box.contains(st->hashes[2]), "the feature's tooltip: " + box);
          require(renamed.contains("Bob") && renamed.contains(st->hashes[1]) && !renamed.contains("Carol"), "the rename's tooltip: " + renamed);
          pass(QStringLiteral("tooltips: the box added by Alice in %1 and edited by Carol in %3, the rename by Bob in %2 (read in %4 ms)")
                   .arg(st->hashes[0], st->hashes[1], st->hashes[2]).arg(st->clock.elapsed()));
          st->session = doc->run("rename", opad::json{{"target", st->node}, {"name", "Session name"}}).value("id", "");
          require(!st->session.empty(), "the session's rename");
          const QString fresh = w.m_timeline->tooltip(st->session);
          require(fresh.contains(tr("Not committed yet").toHtmlEscaped()), "an op of this session: " + fresh);
          pass("an op of this session is not committed yet");
          // The feature's marker: Show in version history.
          withPopup([&] { w.timelineMenu(st->feature, w.m_timeline->mapToGlobal(QPoint(140, 20))); }, [](QMenu* m) {
            QAction* a = named(m, "vcs.opHistory");
            if (!a) throw std::runtime_error("no Show in version history: " + names(m).join(' ').toStdString());
            a->trigger();
          });
          st->step = 2;
          st->clock.restart();
          return;
        }
        case 2: {
          if (!vc->historyFilterRead() || !vc->toolPanel() || !vc->toolPanel()->isVisible()) return;
          require(vc->panel()->filterBar()->isVisible() && listed() == QStringList({st->hashes[2], st->hashes[0]}), "the feature's history: " + listed().join(' '));
          if (!prefix.isEmpty()) vc->toolPanel()->grab().save(prefix + ".feature.png");
          pass("the feature's marker: its history is the commits that added and edited it (" + listed().join(' ') + ")");
          // The body's row in the browser: the commit that made it and the one that renamed it.
          withPopup([&] { w.showContextMenu(w.mapToGlobal(QPoint(300, 300)), {st->node}); }, [](QMenu* m) {
            QAction* a = named(m, "vcs.opHistory");
            if (!a) throw std::runtime_error("no Show in version history on the body: " + names(m).join(' ').toStdString());
            a->trigger();
          });
          st->step = 3;
          return;
        }
        case 3: {
          if (!vc->historyFilterRead()) return;
          require(listed() == QStringList({st->hashes[1], st->hashes[0]}), "the body's history: " + listed().join(' '));
          pass("the body: the commits that made and renamed it (" + listed().join(' ') + ")");
          vc->panel()->filterBar()->findChild<QToolButton*>("historyFilterClear")->click();
          st->step = 4;
          return;
        }
        case 4: {
          if (!vc->settled() || vc->panel()->filterBar()->isVisible()) return;
          if (listed().size() < 3) return;  // the whole history read again
          require(listed() == QStringList({st->hashes[2], st->hashes[1], st->hashes[0]}), "Show all: " + listed().join(' '));
          pass("Show all: every commit again");
          w.action("file.save")->trigger();
          st->step = 5;
          return;
        }
        case 5: {  // saved: Dan commits the session's rename, HEAD moves
          if (doc->isDirty() || doc->loading || !vc->git()->idle()) return;
          if (!st->committed) {
            st->committed = true;
            vc->git()->command(QStringLiteral("Bench commit"), {"-c", "user.name=Dan", "-c", "user.email=dan@example.com", "commit", "-q", "-am", "session"},
                               [](const git::Result& r) {
                                 if (!r.ok()) trace::log("bench: provenance: the commit failed: " + r.error() + " FAIL");
                               });
            return;
          }
          if (vc->git()->repo().status.oid == st->head) return;
          st->step = 6;
          st->clock.restart();
          return;
        }
        case 6: {
          const QString now = w.m_timeline->tooltip(st->session);  // HEAD moved: asking reads the index anew
          if (!prov->ready()) return;
          const ophistory::Index& ix = prov->index();
          require(ix.commits.size() == 4 && ix.blobs == 4 && ix.blobsRead == 1, QStringLiteral("after the commit: %1 commits, %2 versions, %3 read").arg(ix.commits.size()).arg(ix.blobs).arg(ix.blobsRead));
          require(now.contains("Dan") && now.contains(ix.commits.back().shortHash), "the session's op once committed: " + now);
          pass(QStringLiteral("a new commit: only its version read (%1 of %2), the session's rename added by Dan in %3").arg(ix.blobsRead).arg(ix.blobs).arg(ix.commits.back().shortHash));
          trace::log("bench: provenance: who added each op, from git, read once PASS");
          return finish(0);
        }
      }
    } catch (const std::exception& e) {
      trace::log(QStringLiteral("bench: provenance: %1 FAIL").arg(QString::fromUtf8(e.what())));
      finish(2);
    }
  });
  timer->start();
  return true;
}
