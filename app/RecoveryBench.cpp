// OPAD_BENCH_RECOVERY_DIFF=<prefix> (UI-59) on tools/bench_cases/vcs.py's recovery/model.opad: three boxes, saved. The
// bench makes Box1 longer and moves Box2 without saving and takes a recovery snapshot: its .meta names the changes and its
// base (the file's ops and hash). The offer of that snapshot (built, never shown) checks the file by its hash, lists the
// changes as Compare does and offers every answer; a file that is gone and a document never saved change what it offers.
// Back at the saved state, Restore into file brings the changes back under the file's path, unsaved, and Save writes them
// without a refusal. Then a snapshot renames Box3, the session undoes that and saves another change: the offer reads it
// as one newer change on disk, Merge into current adds the rename as one undo step, and Restore into file puts the rename
// after the file's change. Show unsaved changes is Review changes… in the unsaved-changes question, and Discard deletes a
// snapshot with its .meta. perf:<prefix> times a snapshot, the offer's preview and Restore into file on any saved document.
#include <QAction>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <functional>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "CompareMode.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "RecoveryManager.hpp"
#include "Viewport.hpp"

OPAD_BENCH(OPAD_BENCH_RECOVERY_DIFF, recovery_diff) {
  using Entry = RecoveryManager::Entry;
  struct State {
    size_t step = 0;
    int wait = 0;
    QString file, prefix;
    std::map<std::string, std::string> body, feature;  // by name
    std::unique_ptr<QDialog> offer;
    Entry first, second;  // the snapshots: Box1 and Box2 changed; Box3 renamed
    bool snapped = false, answered = false, ok = false;
    QString answer;
    unsigned long long generation = 0;
  };
  auto st = std::make_shared<State>();
  st->prefix = value;
  MainWindow* win = &w;
  AppDocument* doc = w.m_doc;
  RecoveryManager* rec = w.m_recovery;
  auto* compare = w.findChild<CompareMode*>();
  const bool arabic = qEnvironmentVariable("OPAD_LANG") == "ar";
  auto require = [](bool ok, const QString& why) { if (!ok) throw opad::Error(why.toStdString()); };
  auto pass = [](const QString& what) { trace::log("bench: recovery diff: " + what + " PASS"); };
  QObject::connect(rec, &RecoveryManager::answered, rec, [st](bool ok, const QString& text) { st->answered = true; st->ok = ok; st->answer = text; });
  auto snap = [rec, st] {  // a snapshot now (a save's checkpoint may be writing one: then again)
    st->snapped = false;
    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [rec, st, attempt](int left) {
      rec->saveNow([rec, st, attempt, left](bool ok, const QString& error) {
        if (ok) st->snapped = true;
        else if (left > 0) QTimer::singleShot(200, rec, [attempt, left] { (*attempt)(left - 1); });
        else trace::log("bench: recovery diff: FAIL no snapshot: " + error);
      });
    };
    (*attempt)(50);
  };
  auto newest = [doc] {  // the newest snapshot of this document as the offer lists it, from its .meta
    const auto all = RecoveryManager::snapshotsOf(RecoveryManager::recoveryRoot(), doc->doc.header.uuid);
    if (all.empty()) throw opad::Error("no recovery snapshot");
    QFile file(all.front().file + ".meta");
    file.open(QIODevice::ReadOnly);
    const opad::json meta = opad::json::parse(file.readAll().toStdString());
    return Entry{all.front().file, all.front().title, all.front().time, QString::fromStdString(meta.value("source", "")),
                 QString::fromStdString(meta.value("summary", "")), meta.value("changes", -1)};
  };
  auto meta = [](const Entry& e) {
    QFile file(e.file + ".meta");
    file.open(QIODevice::ReadOnly);
    return opad::json::parse(file.readAll().toStdString());
  };
  auto open = [rec, st](std::vector<Entry> entries) {
    st->offer.reset(rec->offerDialog(entries));
    st->offer->layout()->activate();
  };
  auto child = [st](RecoveryManager::Answer answer) {  // by what it answers: the default one is named "primary" (styled)
    for (auto* b : st->offer->findChildren<QPushButton*>())
      if (b->property("answer").toInt() == answer) return b;
    throw opad::Error("no button for answer " + std::to_string(answer));
  };
  auto base = [st] { return st->offer->findChild<QLabel*>("recoveryBase"); };
  auto read = [base] { return !base()->property("state").toString().isEmpty(); };  // the preview is in
  auto rows = [st] {
    QStringList out;
    auto* tree = st->offer->findChild<QTreeWidget*>("recoveryChanges");
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      if (tree->topLevelItem(i)->data(0, Qt::UserRole).toInt() >= 0) out << tree->topLevelItem(i)->text(1);
    return out;
  };
  auto press = [st, rec, child](RecoveryManager::Answer name, const Entry& e) {
    st->answered = false;
    child(name)->click();
    const int result = st->offer->result();
    st->offer.reset();
    rec->answerOffer(result, e);
    return result;
  };
  auto idle = [win, doc] { return !doc->loading && !doc->designBusy && !doc->snapshotBusy() && !win->m_jobs->busy(); };
  auto name = [doc](const std::string& id) { const opad::Node* n = doc->scene.node(id); return n ? n->name : std::string(); };
  auto shot = [st](const QString& what) { st->offer->grab().save(st->prefix + "." + what + ".png"); };
  std::vector<std::function<bool()>> steps;
  auto clock = std::make_shared<QElapsedTimer>();
  if (st->prefix.startsWith("perf:")) {  // perf:<prefix> on any saved document (the Engine): timings; nothing is saved
    st->prefix = st->prefix.mid(5);
    steps = {
        [=] {
          if (doc->loading || doc->designBusy || win->m_jobs->busy() || doc->scene.all_bodies().empty()) return false;
          st->file = doc->path();
          const std::string id = doc->scene.all_bodies().front();
          doc->run("rename", {{"target", id}, {"name", name(id) + " (renamed)"}});
          clock->start();
          snap();
          return true;
        },
        [=] {
          if (!st->snapped) return false;
          st->first = newest();
          trace::log(QStringLiteral("bench: recovery diff: perf: a snapshot with its summary and its base's hash in %1 ms: \"%2\" PASS").arg(clock->elapsed()).arg(st->first.summary));
          require(st->first.summary.contains("(renamed)"), "summary " + st->first.summary);
          clock->start();
          open({st->first});
          return true;
        },
        [=] {
          if (!read()) return false;
          trace::log(QStringLiteral("bench: recovery diff: perf: the offer's preview in %1 ms (%2, hash %3, %4 rows) PASS")
                         .arg(clock->elapsed()).arg(base()->property("state").toString(), base()->property("hash").toString()).arg(rows().size()));
          require(base()->property("state") == "same" && base()->property("hash") == "same", "the base check: " + base()->text());
          doc->undo();
          clock->start();
          press(RecoveryManager::RestoreFile, st->first);
          return true;
        },
        [=] {
          if (!st->answered) return false;
          require(st->ok && doc->isDirty() && QFileInfo(doc->path()) == QFileInfo(st->file) && !doc->diskChanged(), "restored: " + st->answer);
          trace::log(QStringLiteral("bench: recovery diff: perf: Restore into file in %1 ms PASS").arg(clock->elapsed()));
          return true;
        },
    };
  } else {
    steps = {
        [=] {  // unsaved: Box1 longer, Box2 moved; a snapshot
          if (doc->loading || doc->designBusy || win->m_viewport->displayedCount() < 3) return false;
          st->file = doc->path();
          for (const auto& id : doc->scene.all_bodies()) st->body[doc->scene.node(id)->name] = id;
          for (const auto& f : doc->scene.features) st->feature[f.name] = f.id;
          for (const char* n : {"Box1", "Box2", "Box3"}) require(st->body.count(n) && st->feature.count(n), QString("Box %1").arg(n));
          doc->run("feature_edit", {{"target", st->feature["Box1"]}, {"inputs", {{"length", "40 mm"}}}});
          doc->run("transform", {{"target", st->body["Box2"]}, {"matrix", {1, 0, 0, 0, 0, 1, 0, 20, 0, 0, 1, 0, 0, 0, 0, 1}}});
          snap();
          return true;
        },
        [=] {  // its .meta: what it has over the file, the file's ops and hash
          if (!st->snapped || !idle()) return false;
          st->first = newest();
          const opad::json m = meta(st->first);
          QFile file(st->file);
          file.open(QIODevice::ReadOnly);
          const std::string hash = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
          const auto saved = opad::Document::load_index(std::filesystem::path(st->file.toStdU16String())).ops.size();
          require(st->first.summary.contains("Box1") && st->first.summary.contains("Box2") && st->first.changes >= 2, "summary " + st->first.summary);
          require(m["disk"].value("sha256", "") == hash && m["disk"].value("ops", size_t(0)) == saved && QFileInfo(QString::fromStdString(m["disk"].value("file", ""))) == QFileInfo(st->file),
                  "the base in .meta: " + QString::fromStdString(m["disk"].dump()));
          require(QFileInfo(st->first.source) == QFileInfo(st->file), "source " + st->first.source);
          pass("the snapshot's .meta: \"" + st->first.summary + "\", its base's ops and hash");
          Entry gone = st->first, never = st->first;
          gone.source = QFileInfo(st->file).absolutePath() + "/gone.opad";
          never.source.clear();
          open({st->first, gone, never});
          return true;
        },
        [=] {  // the offer of it: the file unchanged by its hash, the changes, every answer
          if (!read()) return false;
          require(base()->property("state") == "same" && base()->property("hash") == "same", "the base check: " + base()->property("state").toString() + " / " +
                                                                                                base()->property("hash").toString() + ": " + base()->text());
          const QStringList r = rows();
          require(r.filter("Box1").size() >= 1 && r.filter("Box2").size() >= 1, "the changes listed: " + r.join(" | "));
          for (auto b : {RecoveryManager::RestoreFile, RecoveryManager::MergeCurrent, RecoveryManager::RestoreCopy, RecoveryManager::Compare, RecoveryManager::Discard})
            require(child(b)->isEnabled(), QString("answer %1 offered").arg(int(b)));
          require(child(RecoveryManager::RestoreFile)->isDefault() && child(RecoveryManager::RestoreFile)->objectName() == "primary", "Restore into file is the default");
          require(child(RecoveryManager::Compare)->objectName() == "recoveryCompare", "Compare… keeps its name (the compare bench presses it)");
          if (arabic) require(child(RecoveryManager::RestoreFile)->text() != "Restore into file" && child(RecoveryManager::MergeCurrent)->text() != "Merge into current", "the answers in Arabic");
          auto* tree = st->offer->findChild<QTreeWidget*>("recoveryChanges");
          for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(1).contains("Box1")) {
              tree->setCurrentItem(tree->topLevelItem(i));
              break;
            }
          require(!st->offer->findChild<QTableWidget*>("recoveryDetails")->isHidden(), "a row shows what it changed");
          shot("offer");
          pass("the offer: " + base()->text() + " " + st->offer->findChild<QLabel*>("recoverySummary")->text());
          st->offer->findChild<QListWidget*>("recoveryList")->setCurrentRow(1);
          return true;
        },
        [=] {  // its file gone: written again; nothing to compare with
          if (!read()) return false;
          require(base()->property("state") == "missing" && child(RecoveryManager::RestoreFile)->isEnabled() && !child(RecoveryManager::Compare)->isEnabled(), "gone: " + base()->text());
          st->offer->findChild<QListWidget*>("recoveryList")->setCurrentRow(2);
          return true;
        },
        [=] {
          if (!read()) return false;
          require(base()->property("state") == "unsaved" && !child(RecoveryManager::RestoreFile)->isEnabled() && child(RecoveryManager::RestoreCopy)->isDefault() && !child(RecoveryManager::RestoreFile)->isDefault(), "never saved: " + base()->text());
          pass("a file that is gone, a document never saved");
          st->offer->findChild<QListWidget*>("recoveryList")->setCurrentRow(0);
          doc->undo(2);  // back to the saved state, as after a restart
          require(!doc->isDirty(), "undone to the file");
          return true;
        },
        [=] {  // Restore into file
          if (!read()) return false;
          st->generation = doc->generation;
          require(press(RecoveryManager::RestoreFile, st->first) == RecoveryManager::RestoreFile, "Restore into file answers RestoreFile");
          return true;
        },
        [=] {
          if (!st->answered || !idle()) return false;
          require(st->ok, "restored: " + st->answer);
          require(doc->generation != st->generation && QFileInfo(doc->path()) == QFileInfo(st->file) && QFileInfo(doc->diskFile()) == QFileInfo(st->file) && doc->isDirty() &&
                      !doc->diskChanged() && doc->title().contains('*'),
                  "the file's path kept, the changes unsaved: " + doc->title());
          require(doc->scene.feature(st->feature["Box1"])->inputs.value("length", "") == "40 mm" && std::abs(doc->scene.world(st->body["Box2"]).at(1, 3) - 20) < 1e-9,
                  "Box1 40 mm long, Box2 moved");
          require(doc->save(), "Save writes into the file (no refusal)");
          require(opad::Document::load_index(std::filesystem::path(st->file.toStdU16String())).ops.size() == doc->doc.ops.size() && !doc->isDirty(), "the file has them");
          pass("Restore into file keeps the path: " + st->answer);
          doc->run("rename", {{"target", st->body["Box3"]}, {"name", "Lid"}});
          snap();
          return true;
        },
        [=] {  // undone, another change saved: one newer change on disk
          if (!st->snapped || !idle()) return false;
          st->second = newest();
          require(st->second.summary.contains("Lid"), "the second snapshot: " + st->second.summary);
          doc->undo();
          doc->run("appearance", {{"target", st->body["Box1"]}, {"color", {1.0, 0.0, 0.0}}});
          require(doc->save(), "saved another change");
          open({st->second});
          return true;
        },
        [=] {
          if (!read()) return false;
          require(base()->property("state") == "extends" && base()->property("hash") == "changed", "newer changes: " + base()->property("state").toString() + ": " + base()->text());
          const QStringList r = rows();
          require(r.filter("Lid").size() == 1 && r.filter("Box1").isEmpty(), "only the rename to restore, the file's colour not undone: " + r.join(" | "));
          shot("newer");
          pass("one newer change on disk: " + base()->text());
          require(press(RecoveryManager::MergeCurrent, st->second) == RecoveryManager::MergeCurrent, "Merge into current answers MergeCurrent");
          return true;
        },
        [=] {  // Merge into current: one undo step
          if (!st->answered || !idle()) return false;
          require(st->ok, "merged: " + st->answer);
          require(name(st->body["Box3"]) == "Lid" && doc->scene.node(st->body["Box1"])->has_color && doc->isDirty(), "the rename after the file's colour");
          require(doc->undoLabels().value(0) == QObject::tr("merge recovered changes"), "one step: " + doc->undoLabels().join(", "));
          doc->undo();
          require(name(st->body["Box3"]) == "Box3" && doc->scene.node(st->body["Box1"])->has_color && !doc->isDirty(), "undone: the file as saved");
          pass("Merge into current: " + st->answer);
          open({st->second});
          return true;
        },
        [=] {  // Restore into file after the file's own newer change
          if (!read()) return false;
          require(press(RecoveryManager::RestoreFile, st->second) == RecoveryManager::RestoreFile, "Restore into file");
          return true;
        },
        [=] {
          if (!st->answered || !idle()) return false;
          require(st->ok, "restored: " + st->answer);
          require(QFileInfo(doc->path()) == QFileInfo(st->file) && name(st->body["Box3"]) == "Lid" && doc->scene.node(st->body["Box1"])->has_color && doc->isDirty() &&
                      !doc->diskChanged() && !doc->canUndo(),
                  "the file's change kept, the rename unsaved after it");
          pass("Restore into file after a newer change: " + st->answer);
          // Review changes… in the unsaved-changes question: Compare, the saved file against this session.
          require(win->action("vcs.unsavedChanges") && win->action("vcs.unsavedChanges")->isEnabled(), "Show unsaved changes on");
          std::unique_ptr<QMessageBox> box(win->unsavedPrompt());
          auto* review = box->findChild<QPushButton*>("reviewChanges");
          require(review && !review->text().isEmpty(), "Review changes… in the question");
          review->click();
          require(box->clickedButton() == review, "it answers the question");
          return true;
        },
        [=] {
          if (!compare || !compare->active() || !compare->settled()) return false;
          const auto& v = compare->versions();
          QStringList r;
          auto* list = compare->panel()->list();
          for (int i = 0; i < list->topLevelItemCount(); ++i)
            if (list->topLevelItem(i)->data(0, Qt::UserRole).toInt() >= 0) r << list->topLevelItem(i)->text(1);
          require(v.size() >= 2 && r.filter("Lid").size() == 1 && r.filter("Box1").isEmpty(), "Compare: the saved file against this session: " + r.join(" | "));
          pass("Review changes… opens Compare on the unsaved changes");
          compare->close();
          snap();
          return true;
        },
        [=] {  // Discard: the snapshot and its .meta go
          if (!st->snapped || !idle() || (compare && compare->active())) return false;
          const Entry e = newest();
          st->first = e;
          open({e});
          return true;
        },
        [=] {
          if (!read()) return false;
          require(press(RecoveryManager::Discard, st->first) == RecoveryManager::Discard, "Discard answers Discard");
          return true;
        },
        [=] {
          if (!st->answered || !idle()) return false;
          require(st->ok && !QFileInfo::exists(st->first.file) && !QFileInfo::exists(st->first.file + ".meta"), "discarded: " + st->answer);
          pass("Discard deletes the snapshot and its .meta");
          return true;
        },
    };
  }
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [st, steps, timer] {
    try {
      if (st->step >= steps.size()) {
        timer->stop();
        QCoreApplication::exit(0);
        return;
      }
      if (steps[st->step]()) {
        ++st->step;
        st->wait = 0;
      } else if (++st->wait > 600) {
        throw opad::Error("timed out in step " + std::to_string(st->step));
      }
    } catch (const std::exception& e) {
      timer->stop();
      st->offer.reset();
      trace::log(QStringLiteral("bench: recovery diff: FAIL %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
