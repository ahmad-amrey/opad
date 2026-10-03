#include "DiskSync.hpp"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <algorithm>

#include "Banner.hpp"
#include "Jobs.hpp"

DiskSync::DiskSync(AppDocument* doc, JobRunner* jobs, QWidget* viewport, QWidget* window)
    : QObject(window), m_doc(doc), m_jobs(jobs), m_window(window), m_banner(new Banner(viewport)) {
  m_debounce.setSingleShot(true);
  connect(&m_debounce, &QTimer::timeout, this, &DiskSync::check);
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { schedule(); });
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { schedule(); });
  m_poll.setInterval(4000);  // one stat: network drives may not notify
  connect(&m_poll, &QTimer::timeout, this, &DiskSync::check);
  m_poll.start();
  connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) { if (s == Qt::ApplicationActive) schedule(0); });
  // Loads, saves and reloads: a new state of the file to compare with.
  connect(doc, &AppDocument::pathChanged, this, [this] {
    m_dismissed.reset();
    m_read.reset();
    m_decided = false;
    check();
  });
  connect(doc, &AppDocument::aboutToReplace, this, [this] { m_read.reset(); m_decided = false; m_banner->dismiss(); });
  connect(doc, &AppDocument::saveBlocked, this, [this] {
    m_saveAfter = true;
    m_dismissed.reset();
    m_decided = false;
    if (!m_banner->state().isEmpty()) m_banner->flash();
    schedule(0);
  });
  connect(m_banner, &Banner::closed, this, [this] {
    m_dismissed = AppDocument::statFile(m_doc->diskFile());
    m_saveAfter = false;
  });
}

void DiskSync::schedule(int ms) { m_debounce.start(ms); }

void DiskSync::watch() {
  const QString file = m_doc->hasDocument && !m_doc->browse ? m_doc->diskFile() : QString();
  QStringList want, have = m_watcher.directories() + m_watcher.files();
  if (!file.isEmpty()) {
    want << QFileInfo(file).absolutePath();
    if (QFileInfo::exists(file)) want << file;  // a save by rename drops the file's watch: added again here
  }
  std::sort(want.begin(), want.end());
  std::sort(have.begin(), have.end());
  if (want == have) return;
  if (!have.isEmpty()) m_watcher.removePaths(have);
  if (!want.isEmpty()) m_watcher.addPaths(want);
}

bool DiskSync::idle() const {
  return m_doc->hasDocument && !m_doc->loading && !m_doc->designBusy && !m_doc->snapshotBusy() && !m_doc->converting() &&
         !m_doc->annotationEditing && m_doc->rollback().empty();
}

QString DiskSync::name() const { return QFileInfo(m_doc->diskFile()).fileName(); }

void DiskSync::status(const QString& text) {
  if (auto* w = qobject_cast<QMainWindow*>(m_window)) w->statusBar()->showMessage(text, 8000);
  if (trace::enabled()) trace::log("disk: " + text);
}

void DiskSync::trigger(const char* action) {
  if (auto* a = m_window->findChild<QAction*>(QString::fromLatin1(action))) a->trigger();
}

void DiskSync::check() {
  const QString file = m_doc->hasDocument && !m_doc->browse ? m_doc->diskFile() : QString();
  const bool settled = m_banner->state() != "merged";  // the Regenerate offer outlives the change it is about
  if (file.isEmpty()) {
    m_read.reset();
    if (settled) m_banner->dismiss();
    watch();
    return;
  }
  if (m_job) return;  // its end checks again
  if (m_doc->loading || m_doc->snapshotBusy()) { schedule(500); return; }  // a save may be writing it
  watch();
  const auto now = AppDocument::statFile(file);
  if (now == m_doc->diskStat()) {  // as this session last read or wrote it
    m_read.reset();
    m_decided = false;
    if (settled && !m_banner->state().isEmpty()) m_banner->dismiss();
    return;
  }
  if (m_dismissed && now == *m_dismissed && !m_saveAfter) return;
  if (!now.exists) {
    m_read.reset();
    if (m_banner->state() != "deleted") showDeleted();
    return;
  }
  if (m_read && m_read->stat == now) {
    if (!m_decided) decide();
    return;
  }
  read();
}

void DiskSync::read(bool everyBody) {
  ++m_reads;
  const QString file = m_doc->diskFile();
  auto base = m_doc->diskBase();
  auto cache = m_doc->doc.shape_cache;
  auto out = std::make_shared<AppDocument::DiskRead>();
  m_job = m_jobs->async(tr("Reading %1 from disk").arg(name()), [out, file, base, cache, everyBody](Progress) {
    *out = AppDocument::readDisk(file, base, cache, !everyBody);
  }, [this, out, file](bool ok, const QString&) {
    m_job = nullptr;
    if (!ok) {  // cancelled: asked again when Save is refused
      m_dismissed = AppDocument::statFile(file);
      return;
    }
    if (trace::enabled())
      trace::log(QStringLiteral("disk: read %1: %2 ops, %3 bodies read, %4%5").arg(QFileInfo(file).fileName()).arg(out->doc ? out->doc->ops.size() : 0)
                     .arg(out->doc ? out->doc->body_count() : 0).arg(opad::relation_name(out->relation), out->error.isEmpty() ? QString() : ": " + out->error));
    if (out->base == m_doc->diskBase() && QFileInfo(out->file) == QFileInfo(m_doc->diskFile())) {
      m_read = out;
      m_decided = false;
    }
    check();
  });
}

void DiskSync::decide() {
  const auto& r = *m_read;
  if (std::exchange(m_reloadAfter, false) && r.doc) return reload(true);
  m_decided = true;
  if (!r.doc) return showUnreadable();
  if (r.relation == opad::Relation::same) {  // touched, or written back alike
    m_doc->acceptDisk(r);
    m_read.reset();
    m_decided = false;
    if (m_banner->state() != "merged") m_banner->dismiss();
    if (std::exchange(m_saveAfter, false)) trigger("file.save");
    return;
  }
  if (r.relation == opad::Relation::other) return showReplaced(tr("It is another document now."));
  if (r.relation == opad::Relation::rewritten)
    return showReplaced(tr("Its history no longer continues yours: another branch was checked out, or it was reset or rebased."));
  if (!m_doc->isDirty()) {  // nothing unsaved here: the file's changes come in
    if (!idle()) {  // a feature or note being edited, a plan or save running: when that is over
      m_decided = false;
      schedule(500);
      return;
    }
    return merge();
  }
  const auto plan = m_doc->planDisk(r);
  if (!plan.error.empty()) return showReplaced(tr("Changes saved before were undone here, so the two cannot be merged."));
  showMerge(plan);
}

void DiskSync::showMerge(const opad::MergePlan& plan) {
  QString text = tr("Changes there: %1, unsaved here: %2. Merge keeps both, the file's first, then yours.").arg(plan.incoming).arg(plan.mine.size());
  if (!plan.conflicts.empty()) text += ' ' + tr("Conflicting changes: %1 (yours win).").arg(plan.conflicts.size());
  QStringList details;
  for (const auto& c : plan.conflicts) {
    const opad::Node* n = m_doc->node(c.target);
    const QString what = n ? QString::fromStdString(n->name) : QString::fromStdString(c.target.rfind("parameter:", 0) == 0 ? c.target.substr(10) : c.target.substr(0, 8));
    details << tr("%1: %2").arg(what, c.field == "*" ? tr("everything") : QString::fromStdString(c.field));
  }
  m_banner->present("merge", Banner::Tone::Warning, tr("%1 changed on disk").arg(name()), text, details.join('\n'));
  m_banner->setProperty("conflicts", static_cast<int>(plan.conflicts.size()));
  m_banner->addButton("diskMerge", tr("Merge"), [this] { merge(); }, true);
  m_banner->addButton("diskReload", tr("Reload…"), [this] { reload(); });
  m_banner->addButton("diskSaveAs", tr("Save as…"), [this] { trigger("file.saveas"); });
  m_banner->addButton("diskOverwrite", tr("Overwrite…"), [this] { overwrite(); });
}

void DiskSync::showReplaced(const QString& text) {
  m_banner->present("replaced", Banner::Tone::Warning, tr("%1 was replaced on disk").arg(name()), text);
  m_banner->addButton("diskReload", m_doc->isDirty() ? tr("Reload…") : tr("Reload"), [this] { reload(); }, true);
  m_banner->addButton("diskSaveAs", tr("Save as…"), [this] { trigger("file.saveas"); });
  m_banner->addButton("diskOverwrite", tr("Overwrite…"), [this] { overwrite(); });
}

void DiskSync::showUnreadable() {
  const QString error = m_read ? m_read->error : QString();
  const QString text = error.contains("conflict marker") ? tr("It holds git conflict markers: the OPAD merge driver is not set up for this repository. Keep your version over it or under another name.")
                                                          : error;
  m_banner->present("unreadable", Banner::Tone::Danger, tr("%1 on disk cannot be read").arg(name()), text, error);
  m_banner->addButton("diskSaveAs", tr("Save as…"), [this] { trigger("file.saveas"); });
  m_banner->addButton("diskOverwrite", tr("Overwrite…"), [this] { overwrite(); }, true);
}

void DiskSync::showDeleted() {
  m_banner->present("deleted", Banner::Tone::Warning, tr("%1 was deleted or moved").arg(name()), tr("Save writes it again."));
  m_banner->addButton("diskSave", tr("Save"), [this] { trigger("file.save"); }, true);
  m_banner->addButton("diskSaveAs", tr("Save as…"), [this] { trigger("file.saveas"); });
}

void DiskSync::confirm(const QString& title, const QString& text, const QString& action, std::function<void()> fn) {
  m_banner->present("confirm", Banner::Tone::Danger, title, text);
  m_banner->addButton("diskConfirm", action, std::move(fn), true);
  m_banner->addButton("diskCancel", tr("Cancel"), [this] {  // back to the choices
    m_decided = false;
    m_banner->dismiss();
    check();
  });
}

void DiskSync::merge() {
  if (!m_read) return;
  if (AppDocument::statFile(m_read->file) != m_read->stat) {  // it moved on again: read that first
    m_decided = false;
    return check();
  }
  if (!idle()) return status(tr("The document is busy; try again in a moment."));
  for (const auto& key : m_read->bodies)
    if (!m_read->doc->has_body(key) && !m_doc->doc.has_body(key)) {  // a body this session no longer has: read them all
      m_read.reset();
      return read(true);
    }
  auto taken = std::move(m_read);
  m_decided = false;
  const bool clean = !m_doc->isDirty();
  try {
    const auto plan = m_doc->mergeDisk(std::move(*taken), clean ? tr("changes from disk") : tr("merge with disk"));
    m_saveAfter = false;
    if (clean) status(tr("%1 changed on disk: %2 new changes brought in").arg(name()).arg(plan.incoming));
    else status(tr("Merged %1 changes from disk; save to write the result").arg(plan.incoming));
    if (plan.design) {  // results each side computed did not see the other's changes
      m_banner->present("merged", Banner::Tone::Info, tr("Merged the changes from disk"),
                        tr("Both sides changed the design: regenerate it so that what each side computed follows the other's changes."));
      m_banner->addButton("diskRegenerate", tr("Regenerate"), [this] { m_banner->dismiss(); trigger("design.regenerate"); }, true);
    } else if (!m_banner->state().isEmpty()) m_banner->dismiss();
  } catch (const std::exception& e) {
    status(tr("Could not merge %1: %2").arg(name(), QString::fromUtf8(e.what())));
    schedule(2000);
  }
}

void DiskSync::reload(bool asked) {
  if (!m_read || !m_read->doc) return;
  if (!asked && m_doc->isDirty())
    return confirm(tr("Reload %1 from disk?").arg(name()), tr("Your unsaved changes are lost."), tr("Reload"), [this] { reload(true); });
  if (AppDocument::statFile(m_read->file) != m_read->stat) {
    m_decided = false;
    return check();
  }
  if (!idle()) return status(tr("The document is busy; try again in a moment."));
  for (const auto& key : m_read->bodies)
    if (!m_read->doc->has_body(key) && !m_doc->doc.has_body(key)) {
      m_reloadAfter = true;
      m_read.reset();
      return read(true);
    }
  auto taken = std::move(m_read);
  m_decided = false;
  try {
    m_doc->reloadDisk(std::move(*taken));
    m_saveAfter = false;
    m_banner->dismiss();
  } catch (const std::exception& e) {
    status(tr("Could not reload %1: %2").arg(name(), QString::fromUtf8(e.what())));
  }
}

void DiskSync::overwrite() {
  confirm(tr("Overwrite %1 with your version?").arg(name()),
          tr("What was changed on disk since you opened or saved it is lost from the file (git still has it if it was committed)."), tr("Overwrite"), [this] {
            try {
              if (!m_doc->save(true)) return;
              m_read.reset();
              m_decided = false;
              m_saveAfter = false;
              m_banner->dismiss();
            } catch (const std::exception& e) {
              status(QString::fromUtf8(e.what()));
            }
          });
}

// ---------------------------------------------------------------- bench
// OPAD_BENCH_EXTERNAL_CHANGE=<prefix> (with --bench-select on a saved document with one body; OPAD_BENCH_CLI or the
// opad-cli beside the app): opad-cli appends while the document is open and clean (it comes in), then while it has
// unsaved changes (merge banner; Save refused; Merge), a conflicting change (yours win), a reset (replaced banner,
// Reload), a reset over unsaved changes (Overwrite asks, Cancel, Overwrite), git conflict markers (unreadable, Save
// refused, Overwrite), a deleted file (Save writes it again), a touched file (Save waits for the read, then goes ahead)
// and the app's own saves (never read back).
// Banner pictures at <prefix>.<state>.png.
bool DiskSync::bench() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_EXTERNAL_CHANGE");
  if (prefix.isEmpty()) return false;
  QString cli = qEnvironmentVariable("OPAD_BENCH_CLI");
#ifdef _WIN32
  if (cli.isEmpty()) cli = QDir(QCoreApplication::applicationDirPath()).filePath("opad-cli.exe");
#else
  if (cli.isEmpty()) cli = QDir(QCoreApplication::applicationDirPath()).filePath("opad-cli");
#endif
  struct State {
    size_t step = 0;
    int wait = 0, ticks = 0, exit = 0, reads = 0;
    bool running = false;
    QString file;
    std::string a, b;
    size_t ops = 0;
    QByteArray bytes;
  };
  auto st = std::make_shared<State>();
  auto require = [](bool ok, const char* why) { if (!ok) throw opad::Error(why); };
  auto pass = [](const QString& what) { trace::log("bench: external change: " + what + " PASS"); };
  auto path = [st] { return std::filesystem::path(st->file.toStdU16String()); };
  auto run = [this, st, cli](const QStringList& args) {
    auto* p = new QProcess(this);
    st->running = true;
    connect(p, &QProcess::finished, this, [st, p](int code) { st->running = false; st->exit = code; p->deleteLater(); });
    connect(p, &QProcess::errorOccurred, this, [st, p](QProcess::ProcessError) { st->running = false; st->exit = -1; p->deleteLater(); });
    p->start(cli, args);
  };
  auto fileIds = [path] {
    std::vector<std::string> ids;
    for (const auto& o : opad::Document::load(path()).ops) ids.push_back(o.id);
    return ids;
  };
  auto sessionIds = [this] {
    std::vector<std::string> ids;
    for (const auto& o : m_doc->doc.ops) ids.push_back(o.id);
    return ids;
  };
  auto bytes = [st] {
    QFile f(st->file);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  };
  auto reset = [path] {  // as `git reset --hard HEAD~1` would leave it: the last op gone
    opad::Document d = opad::Document::load(path());
    d.truncate_ops(d.ops.size() - 1);
    opad::write_text_file(path(), d.serialize());
  };
  auto click = [this](const char* action) {
    QPushButton* b = m_banner->button(action);
    if (!b) throw opad::Error(std::string("the banner has no ") + action + " button");
    b->click();
  };
  auto shot = [this, prefix](const QString& suffix) { m_banner->grab().save(prefix + suffix); };
  auto nameOf = [this](const std::string& id) { const opad::Node* n = m_doc->node(id); return n ? n->name : std::string(); };
  auto rename = [this](const std::string& id, const char* to) { m_doc->run("rename", opad::json{{"target", id}, {"name", to}}); };
  const std::vector<std::function<bool()>> steps = {
      [=, this] {
        st->file = m_doc->diskFile();
        require(!st->file.isEmpty() && !m_doc->isDirty() && m_doc->scene.all_bodies().size() == 1, "a saved document with one body");
        st->a = m_doc->scene.all_bodies().front();
        st->ops = m_doc->doc.ops.size();
        st->reads = m_reads;
        run({"feature", st->file, "--kind", "cylinder", "--inputs", R"({"x":"40 mm","diameter":"10 mm","height":"30 mm"})"});
        return true;
      },
      [=, this] {
        if (st->running || m_doc->doc.ops.size() == st->ops) return false;
        require(st->exit == 0, "opad-cli feature");
        require(!m_doc->isDirty() && m_banner->state().isEmpty(), "fast-forward: clean, no banner");
        require(m_doc->undoLabel() == tr("changes from disk"), "fast-forward: one undo step");
        require(sessionIds() == fileIds(), "fast-forward: the file's log");
        require(m_reads > st->reads, "fast-forward: read on a worker");
        const auto bodies = m_doc->scene.all_bodies();
        require(bodies.size() == 2, "fast-forward: the new body");
        st->b = bodies[0] == st->a ? bodies[1] : bodies[0];
        pass(QStringLiteral("fast-forward (%1 new ops, clean, one undo step)").arg(m_doc->doc.ops.size() - st->ops));
        rename(st->a, "Mine");
        require(m_doc->isDirty(), "an unsaved change");
        st->ops = m_doc->doc.ops.size();
        run({"appearance", st->file, "--target", QString::fromStdString(st->b), "--color", "[1,0,0]"});
        return true;
      },
      [=, this] {
        if (st->running || m_banner->state() != "merge") return false;
        require(st->exit == 0, "opad-cli appearance");
        require(m_doc->doc.ops.size() == st->ops && m_doc->isDirty(), "merge banner: nothing applied by itself");
        require(m_banner->property("conflicts").toInt() == 0, "merge banner: no conflict");
        shot(".merge.png");
        st->bytes = bytes();
        trigger("file.save");
        require(bytes() == st->bytes && m_doc->isDirty() && m_banner->state() == "merge", "save guard: nothing written");
        pass("save guard");
        click("diskMerge");
        require(m_banner->state().isEmpty(), "merge: banner gone");
        require(m_doc->doc.ops.size() == st->ops + 1 && m_doc->doc.ops.back().type == "rename", "merge: the file's op, then the unsaved one");
        require(nameOf(st->a) == "Mine" && m_doc->node(st->b)->has_color, "merge: both changes");
        require(m_doc->isDirty() && m_doc->undoLabel() == "rename", "merge: the unsaved rename tops the undo stack");
        trigger("file.save");
        require(!m_doc->isDirty() && sessionIds() == fileIds(), "merge: saved");
        pass("merge");
        rename(st->a, "Mine2");
        run({"rename", st->file, "--target", QString::fromStdString(st->a), "--name", "Theirs2"});
        return true;
      },
      [=, this] {
        if (st->running || m_banner->state() != "merge") return false;
        require(m_banner->property("conflicts").toInt() == 1 && !m_banner->toolTip().isEmpty(), "conflict: counted and listed");
        shot(".conflict.png");
        click("diskMerge");
        require(nameOf(st->a) == "Mine2", "conflict: yours win");
        trigger("file.save");
        require(!m_doc->isDirty(), "conflict: saved");
        pass("conflict");
        st->ops = m_doc->doc.ops.size();
        reset();
        return true;
      },
      [=, this] {
        if (m_banner->state() != "replaced") return false;
        require(m_doc->doc.ops.size() == st->ops && !m_doc->isDirty(), "rewritten: nothing applied by itself");
        shot(".replaced.png");
        click("diskReload");
        require(m_banner->state().isEmpty() && sessionIds() == fileIds() && !m_doc->canUndo() && !m_doc->isDirty() && nameOf(st->a) == "Theirs2",
                "rewritten: reloaded");
        pass("rewritten history, reload");
        rename(st->a, "Mine3");
        reset();
        return true;
      },
      [=, this] {
        if (m_banner->state() != "replaced") return false;
        st->bytes = bytes();
        click("diskOverwrite");
        require(m_banner->state() == "confirm" && bytes() == st->bytes, "overwrite asks first");
        shot(".confirm.png");
        click("diskCancel");
        require(m_banner->state() == "replaced" && bytes() == st->bytes, "cancel keeps both");
        click("diskOverwrite");
        click("diskConfirm");
        require(m_banner->state().isEmpty() && !m_doc->isDirty() && sessionIds() == fileIds() && nameOf(st->a) == "Mine3", "overwritten");
        pass("overwrite with confirmation");
        std::string text = opad::read_text_file(path());  // what a text merge without the OPAD driver leaves
        text.insert(text.find("#bodies"), "<<<<<<< HEAD\n=======\n>>>>>>> other\n");
        opad::write_text_file(path(), text);
        rename(st->a, "Mine4");
        return true;
      },
      [=, this] {
        if (m_banner->state() != "unreadable") return false;
        shot(".unreadable.png");
        st->bytes = bytes();
        trigger("file.save");
        require(bytes() == st->bytes && m_doc->isDirty(), "unreadable: Save refused");
        click("diskOverwrite");
        click("diskConfirm");
        require(!m_doc->isDirty() && sessionIds() == fileIds(), "unreadable: overwritten");
        pass("conflict markers");
        QFile::remove(st->file);
        return true;
      },
      [=, this] {
        if (m_banner->state() != "deleted") return false;
        shot(".deleted.png");
        click("diskSave");
        require(QFileInfo::exists(st->file) && sessionIds() == fileIds() && m_banner->state().isEmpty(), "deleted: saved again");
        pass("deleted");
        QFile touched(st->file);  // only the time stamp moves (a checkout of the same content)
        require(touched.open(QIODevice::ReadWrite) && touched.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime), "touch");
        touched.close();
        rename(st->a, "Mine5");
        st->bytes = bytes();
        trigger("file.save");
        require(bytes() == st->bytes && m_doc->isDirty(), "touched: Save waits for the file to be read");
        return true;
      },
      [=, this] {
        if (m_doc->isDirty()) return false;
        require(sessionIds() == fileIds() && nameOf(st->a) == "Mine5" && m_banner->state().isEmpty(), "touched: saved once the ops were found the same");
        pass("touched file, Save goes ahead");
        st->reads = m_reads;
        st->ticks = 0;
        rename(st->a, "Mine6");
        trigger("file.save");
        require(!m_doc->isDirty(), "own save");
        return true;
      },
      [=, this] {
        if (++st->ticks < 14) return false;  // the watcher and the poll have had their turn
        require(m_reads == st->reads && m_banner->state().isEmpty() && !m_doc->isDirty(), "own writes: never read back");
        pass("own writes ignored");
        return true;
      },
  };
  auto* timer = new QTimer(this);
  timer->setInterval(150);
  connect(timer, &QTimer::timeout, this, [this, st, steps, timer] {
    try {
      if (st->step >= steps.size()) {
        timer->stop();
        QCoreApplication::exit(0);
        return;
      }
      if (steps[st->step]()) {
        ++st->step;
        st->wait = 0;
      } else if (++st->wait > 120) {
        throw opad::Error("timed out in step " + std::to_string(st->step) + " (banner: " + m_banner->state().toStdString() + ")");
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QStringLiteral("bench: external change: FAIL %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
