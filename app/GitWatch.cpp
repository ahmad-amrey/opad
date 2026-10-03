#include "GitWatch.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <QTextDocumentFragment>
#include <QVBoxLayout>
#include <memory>
#include <stdexcept>

#include "Icons.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"

namespace {
class Chip : public QWidget {
 public:
  explicit Chip(QWidget* parent) : QWidget(parent) {
    setObjectName("gitChip");
    setCursor(Qt::PointingHandCursor);
  }
  std::function<void()> clicked;
 protected:
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && clicked) clicked();
  }
};

QString stateName(git::Repo::State s) {
  switch (s) {
    case git::Repo::State::None: return "reading";
    case git::Repo::State::GitMissing: return "missing";
    case git::Repo::State::NotRepo: return "none";
    case git::Repo::State::Untrusted: return "untrusted";
    case git::Repo::State::Failed: return "failed";
    case git::Repo::State::Ready: return "ready";
  }
  return {};
}
}  // namespace

GitWatch::GitWatch(JobRunner* jobs, QWidget* window) : QObject(window), m_jobs(jobs), m_window(window) {
  auto* chip = new Chip(window);
  chip->clicked = [this] {
    QMenu* m = menu(m_chip);
    m->setAttribute(Qt::WA_DeleteOnClose);
    m->popup(m_chip->mapToGlobal(QPoint(0, -m->sizeHint().height())));
  };
  m_chip = chip;
  auto* row = new QHBoxLayout(chip);
  row->setContentsMargins(4, 0, 6, 0);
  row->setSpacing(4);
  m_icon = new QLabel(chip);
  m_text = new QLabel(chip);
  m_text->setTextFormat(Qt::RichText);
  row->addWidget(m_icon);
  row->addWidget(m_text);
  chip->hide();
  m_debounce.setSingleShot(true);
  connect(&m_debounce, &QTimer::timeout, this, [this] { refresh(std::exchange(m_probe, false)); });
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
    schedule(path.endsWith("/config") || path.endsWith("/.gitattributes"));
  });
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { schedule(false); });
  // A terminal, another tool or the user's own git may have changed what the watched files do not show (a new parent
  // repository, the global config): look again when OPAD comes back to the front.
  connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
    if (s == Qt::ApplicationActive) schedule(true, 0);
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, &GitWatch::render);
}

git::Context GitWatch::context() const {
  git::Context c;
  c.program = m_program;
  c.dir = m_repo.state == git::Repo::State::Ready ? m_repo.top : QFileInfo(m_file).absolutePath();
  return c;
}

void GitWatch::setFile(const QString& file) {
  if (file == m_file) {  // saved, reloaded: the document's state in git may have moved
    schedule(false, 0);
    return;
  }
  m_file = file;
  ++m_generation;
  m_repo = {};
  m_probe = false;
  m_debounce.stop();
  if (file.isEmpty()) {
    watch();
    render();
    emit changed();
    return;
  }
  render();
  refresh(true);
}

void GitWatch::schedule(bool probe, int ms) {
  if (m_file.isEmpty()) return;
  m_probe = m_probe || probe;
  if (!m_debounce.isActive() || ms < m_debounce.remainingTime()) m_debounce.start(ms);  // at most one read per 250 ms
}

void GitWatch::refresh(bool probe) {
  if (m_file.isEmpty()) return;
  if (m_running || m_busy) {  // one read at a time; none while a job of ours changes the repository
    m_again = true;
    m_probe = m_probe || probe;
    return;
  }
  using S = git::Repo::State;
  // A changed config or .gitattributes is not in the status: look at the repository again.
  if (m_repo.state != S::Ready || QFileInfo(m_repo.commonDir + "/config").lastModified() != m_configStamp ||
      QFileInfo(m_repo.top + "/.gitattributes").lastModified() != m_attributesStamp)
    probe = true;
  if (probe) m_program = git::findProgram();
  m_running = true;
  ++m_runs;
  git::Context c;
  c.program = m_program;
  c.dir = QFileInfo(m_file).absolutePath();
  auto out = std::make_shared<git::Repo>(m_repo);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  const QString file = m_file;
  const unsigned generation = m_generation;
  m_jobs->quiet(tr("Reading git status"), [out, c, file, probe](Progress) {
    if (probe) *out = git::probe(c, file);
    else git::readStatus(c, *out);
  }, [self = QPointer<GitWatch>(this), out, generation, probe, clock](bool ok, const QString& error) {
    if (!self) return;
    self->m_running = false;
    if (generation == self->m_generation) {
      if (!ok) {
        out->state = S::Failed;
        out->error = error;
      }
      // A git command of someone else's half done (an init, a checkout) can fail a read: look once more before saying so.
      if (out->state == S::Failed && self->m_repo.state != S::Failed && !self->m_retried) {
        self->m_retried = true;
        self->schedule(true, 1000);
        if (std::exchange(self->m_again, false)) self->refresh(std::exchange(self->m_probe, false));
        return;
      }
      self->m_retried = out->state == S::Failed;
      self->m_repo = *out;
      const git::Repo& r = self->m_repo;
      if (r.state == S::Ready) {
        self->m_configStamp = QFileInfo(r.commonDir + "/config").lastModified();
        self->m_attributesStamp = QFileInfo(r.top + "/.gitattributes").lastModified();
      }
      if (trace::enabled())
        trace::log(QStringLiteral("git: %1 %2 %3 in %4 ms").arg(probe ? "probe" : "status", stateName(r.state), r.status.branch).arg(clock->elapsed()));
      self->watch();
      self->render();
      emit self->changed();
      if (r.driverStale() && !self->m_repaired.contains(r.top)) {  // OPAD moved or was updated somewhere else: point git at this one
        self->m_repaired.insert(r.top);
        const git::Context here = self->context();
        ++self->m_busy;
        self->m_jobs->quiet(tr("Repairing the OPAD merge driver"), [here](Progress) { git::configureDriver(here, git::Install::here()); },
                            [self](bool ok, const QString& error) {
                              if (!self) return;
                              --self->m_busy;
                              self->status(ok ? tr("The OPAD merge driver of this clone points at this OPAD again.") : tr("Could not repair the OPAD merge driver: %1").arg(error));
                              self->refresh(true);
                            });
      }
    }
    if (std::exchange(self->m_again, false)) self->refresh(std::exchange(self->m_probe, false));
  });
}

void GitWatch::watch() {
  QStringList want;
  auto add = [&want](const QString& path) {
    if (!path.isEmpty() && QFileInfo::exists(path) && !want.contains(path)) want << path;
  };
  if (!m_file.isEmpty()) add(QFileInfo(m_file).absolutePath());  // saves; a `git init` here
  const git::Repo& r = m_repo;
  if (r.state == git::Repo::State::Ready) {
    // Git replaces its files by renaming a .lock over them, which the folder watches see; the file watches add the
    // content changes some systems report only on the file.
    for (const QString& p : {r.gitDir, r.gitDir + "/HEAD", r.gitDir + "/index", r.commonDir, r.commonDir + "/config", r.commonDir + "/refs/heads",
                             r.top + "/.gitattributes"})
      add(p);
    if (!r.status.branch.startsWith('(')) add(QFileInfo(r.commonDir + "/refs/heads/" + r.status.branch).absolutePath());
    if (!r.status.upstream.isEmpty()) add(QFileInfo(r.commonDir + "/refs/remotes/" + r.status.upstream).absolutePath());
  }
  QStringList have = m_watcher.files() + m_watcher.directories();
  std::sort(want.begin(), want.end());
  std::sort(have.begin(), have.end());
  if (want == have) return;
  if (!have.isEmpty()) m_watcher.removePaths(have);
  if (!want.isEmpty()) m_watcher.addPaths(want);
}

void GitWatch::render() {
  m_chip->setVisible(!m_file.isEmpty());
  if (m_file.isEmpty()) return;
  using S = git::Repo::State;
  using D = git::Repo::Doc;
  using Y = git::Repo::Sync;
  const Tokens& t = theme::current();
  m_icon->setPixmap(icons::pixmap("git", t.fg2, 14, m_chip->devicePixelRatioF()));
  auto span = [](const QColor& c, const QString& s) { return QStringLiteral("<span style='color:%1'>%2</span>").arg(c.name(), s.toHtmlEscaped()); };
  const git::Repo& r = m_repo;
  QString text, doc;
  QStringList tip;
  switch (r.state) {
    case S::None: text = span(t.fg3, QStringLiteral("…")); break;
    case S::GitMissing:
      text = span(t.fg3, tr("git not found"));
      tip << tr("OPAD keeps versions with git and did not find it. Install Git (git-scm.com), then choose Refresh in this menu.");
      if (!r.error.isEmpty()) tip << r.error;
      break;
    case S::NotRepo:
      text = span(t.fg3, tr("not in git"));
      tip << tr("This document is not in a git repository: Set up repository… makes one in its folder.");
      break;
    case S::Untrusted:
      text = span(t.amber, tr("git: folder not trusted"));
      tip << r.error;
      break;
    case S::Failed:
      text = span(t.red, tr("git error"));
      tip << r.error;
      break;
    case S::Ready: {
      const git::Status& s = r.status;
      const QString branch = s.branch == "(detached)" ? tr("detached at %1").arg(s.oid.left(7)) : s.branch;
      QStringList parts{branch.toHtmlEscaped()};
      switch (r.doc()) {
        case D::Untracked: parts << span(t.amber, tr("untracked")); doc = "untracked"; break;
        case D::Added:
        case D::Modified: parts << span(t.amber, tr("uncommitted")); doc = "uncommitted"; break;
        case D::Conflict: parts << span(t.red, tr("conflict")); doc = "conflict"; break;
        case D::Ignored: parts << span(t.fg3, tr("ignored")); doc = "ignored"; break;
        default: doc = "clean"; break;
      }
      if (r.merging) parts << span(t.amber, tr("merging"));
      else if (r.rebasing) parts << span(t.amber, tr("rebasing"));
      switch (r.sync()) {
        case Y::Ahead: parts << tr("↑%1").arg(s.ahead); break;
        case Y::Behind: parts << tr("↓%1").arg(s.behind); break;
        case Y::Diverged: parts << tr("↑%1 ↓%2").arg(s.ahead).arg(s.behind); break;
        case Y::Gone: parts << span(t.fg3, tr("upstream gone")); break;
        default: break;
      }
      if (r.needsDriver()) parts << span(t.amber, tr("set up merging"));
      text = parts.join(QStringLiteral(" · "));
      tip << tr("Repository: %1").arg(QDir::toNativeSeparators(r.top));
      if (s.oid == "(initial)") tip << tr("Branch %1, no commits yet").arg(branch);
      else if (s.upstream.isEmpty()) tip << tr("Branch %1, not pushed anywhere yet").arg(branch);
      else if (!s.tracking) tip << tr("Branch %1; its upstream %2 is gone").arg(branch, s.upstream);
      else tip << tr("Branch %1 follows %2: %3 ahead, %4 behind").arg(branch, s.upstream).arg(s.ahead).arg(s.behind);
      const int others = int(s.entries.size()) - (doc == "clean" || doc == "ignored" ? 0 : 1);
      if (others > 0) tip << tr("Other changes in the repository: %1").arg(others);
      if (r.needsDriver()) tip << tr("This clone has no OPAD merge driver: merging .opad files would write conflict markers into them.");
      break;
    }
  }
  if (r.state != S::None) tip << tr("Click for git actions.");
  m_text->setText(text);
  m_chip->setToolTip(tip.join('\n'));
  m_chip->setProperty("state", r.state == S::Ready ? QStringLiteral("ready") : stateName(r.state));
  m_chip->setProperty("doc", doc);
  m_chip->setProperty("text", QTextDocumentFragment::fromHtml(text).toPlainText());
}

QMenu* GitWatch::menu(QWidget* parent) {
  using S = git::Repo::State;
  auto* m = new QMenu(parent);
  m->setObjectName("gitMenu");
  auto add = [this, m](const char* id, const QString& text, std::function<void()> fn) {
    QAction* a = m->addAction(text);
    a->setObjectName(QString::fromLatin1(id));
    connect(a, &QAction::triggered, this, fn);
  };
  if (m_repo.state == S::NotRepo) add("git.setup", tr("Set up repository…"), [this] { setUp(); });
  if (m_repo.state == S::Ready) add("git.setup", tr("Set up OPAD in this repository…"), [this] { setUp(); });
  if (m_repo.needsDriver() || m_repo.driverStale()) add("git.driver", tr("Set up OPAD merging for this clone"), [this] { setUpDriver(); });
  if (!m->isEmpty()) m->addSeparator();
  add("git.refresh", tr("Refresh"), [this] {
    git::forgetTools();
    refresh(true);
  });
  return m;
}

void GitWatch::status(const QString& text) {
  if (auto* w = qobject_cast<QMainWindow*>(m_window)) w->statusBar()->showMessage(text, 8000);
  if (trace::enabled()) trace::log("git: " + text);
}

void GitWatch::failed(const QString& title, const QString& text) {
  if (trace::enabled()) trace::log("git: " + title + ": " + text);
  auto* box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Ok, m_window);
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->open();
}

Job* GitWatch::command(const QString& title, const QStringList& args, std::function<void(const git::Result&)> done, git::RunOptions o) {
  auto result = std::make_shared<git::Result>();
  const git::Context c = context();
  ++m_busy;
  return m_jobs->async(title, [result, c, args, o](Progress p) mutable {
    o.cancelled = [p] { return p.cancelled(); };
    o.progress = [p](const QString& phase, int percent) { p.setPhase(phase, percent); };
    *result = git::run(c, args, o);
  }, [self = QPointer<GitWatch>(this), result, done](bool, const QString&) {
    if (!self) return;
    --self->m_busy;
    if (done) done(*result);
    self->refresh(true);
  });
}

void GitWatch::setUp() {
  using S = git::Repo::State;
  if (m_file.isEmpty() || (m_repo.state != S::NotRepo && m_repo.state != S::Ready)) return;
  const bool fresh = m_repo.state == S::NotRepo;
  auto* d = new QDialog(m_window);
  d->setObjectName("gitSetup");
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setWindowTitle(tr("Set up version control"));
  auto* col = new QVBoxLayout(d);
  auto folder = std::make_shared<QString>(fresh ? QFileInfo(m_file).absolutePath() : m_repo.top);
  auto* where = new QLabel(d);
  where->setWordWrap(true);
  auto describe = [where, folder, fresh] {
    where->setText((fresh ? tr("A new git repository in %1, branch main.") : tr("The git repository in %1.")).arg(QDir::toNativeSeparators(*folder)));
  };
  describe();
  auto* top = new QHBoxLayout;
  top->addWidget(where, 1);
  if (fresh) {
    auto* change = new QPushButton(tr("Change…"), d);
    change->setObjectName("gitSetupFolder");
    top->addWidget(change);
    connect(change, &QPushButton::clicked, d, [this, d, folder, describe] {
      const QString f = QFileDialog::getExistingDirectory(d, tr("Repository folder"), *folder);
      if (f.isEmpty()) return;
      if (QDir(f).relativeFilePath(m_file).startsWith("..")) {
        failed(tr("Set up version control"), tr("The document must be inside the repository's folder."));
        return;
      }
      *folder = f;
      describe();
    });
  }
  col->addLayout(top);
  auto box = [d, col](const char* id, const QString& text, bool enabled) {
    auto* c = new QCheckBox(text, d);
    c->setObjectName(QString::fromLatin1(id));
    c->setChecked(enabled);
    c->setEnabled(enabled);
    col->addWidget(c);
    return c;
  };
  const bool lfs = !m_repo.lfsVersion.isEmpty();
  QCheckBox* attributes = box("attributes", tr("Merge and diff .opad files with OPAD (.gitattributes)"), true);
  QCheckBox* assets = box("lfs", lfs ? tr("Keep big files under assets/ in Git LFS") : tr("Keep big files under assets/ in Git LFS (Git LFS is not installed)"), lfs);
  QCheckBox* ignore = box("ignore", tr("Leave temporary saves, caches and recovery snapshots out (.gitignore)"), true);
  QCheckBox* driver = box("driver", tr("Use this OPAD as the merge and diff driver (this clone's settings)"), true);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, d);
  auto* run = new QPushButton(tr("Set up"), d);
  run->setObjectName("primary");  // before the box lays it out: that polishes it
  run->setProperty("action", "gitSetupRun");
  run->setDefault(true);
  buttons->addButton(run, QDialogButtonBox::AcceptRole);
  connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
  connect(run, &QPushButton::clicked, d, [this, d, folder, attributes, assets, ignore, driver] {
    git::SetupOptions o;
    o.attributes = attributes->isChecked();
    o.lfs = assets->isChecked();
    o.ignore = ignore->isChecked();
    o.driver = driver->isChecked();
    const QString chosen = *folder;
    d->accept();
    runSetUp(chosen, o);
  });
  col->addWidget(buttons);
  d->open();
}

void GitWatch::runSetUp(const QString& folder, const git::SetupOptions& o) {
  auto done = std::make_shared<QStringList>();
  const git::Context c = context();
  const git::Install in = git::Install::here();
  ++m_busy;
  m_jobs->async(tr("Setting up the repository"), [done, c, folder, in, o](Progress p) {
    git::RunOptions ro;
    ro.cancelled = [p] { return p.cancelled(); };
    ro.progress = [p](const QString& phase, int percent) { p.setPhase(phase, percent); };
    *done = git::setUp(c, folder, in, o, ro);
  }, [self = QPointer<GitWatch>(this), done, folder](bool ok, const QString& error) {
    if (!self) return;
    --self->m_busy;
    if (trace::enabled()) trace::log("git: set up: " + done->join(" | "));
    if (ok) self->status(tr("Version control is set up in %1").arg(QDir::toNativeSeparators(folder)));
    else self->failed(tr("Could not set up version control"), error);
    self->refresh(true);
  });
}

void GitWatch::setUpDriver() {
  const git::Context c = context();
  const bool lfs = !m_repo.lfsVersion.isEmpty() && !m_repo.lfsHooks;
  const QString top = m_repo.top;
  ++m_busy;
  m_jobs->async(tr("Setting up the OPAD merge driver"), [c, lfs, top](Progress) {
    git::configureDriver(c, git::Install::here());
    QFile attributes(top + "/.gitattributes");
    if (lfs && attributes.open(QIODevice::ReadOnly) && attributes.readAll().contains("filter=lfs")) git::check(c, {"lfs", "install", "--local"});
  }, [self = QPointer<GitWatch>(this)](bool ok, const QString& error) {
    if (!self) return;
    --self->m_busy;
    if (ok) self->status(tr("This clone merges and diffs .opad files with this OPAD."));
    else self->failed(tr("Could not set up the OPAD merge driver"), error);
    self->refresh(true);
  });
}

// OPAD_BENCH_GIT=<prefix>, on a saved document in a folder of its own outside any repository (gui_benches gives git an
// empty global config): set up from the chip's dialog, then git run from outside as a terminal would (commit, edit,
// push to a bare remote, commit again, switch branch, break and drop the driver config) and the chip following each
// change by itself, with no read while nothing changes.
bool GitWatch::bench() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_GIT");
  if (prefix.isEmpty()) return false;
  QString cli = qEnvironmentVariable("OPAD_BENCH_CLI");
  if (cli.isEmpty()) cli = git::Install::here().cli;
  struct State {
    size_t step = 0;
    int wait = 0, ticks = 0, runs = 0, exit = 0;
    bool running = false;
    QString file, dir, error;
    std::vector<QStringList> pending;
    std::function<void()> next;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  st->file = m_file;
  st->dir = QFileInfo(m_file).absolutePath();
  using S = git::Repo::State;
  using D = git::Repo::Doc;
  using Y = git::Repo::Sync;
  const git::Install here = git::Install::here();
  auto require = [](bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
  };
  auto pass = [](const QString& what) { trace::log("bench: git: " + what + " PASS"); };
  auto settled = [this] { return !m_running && !m_busy && !m_debounce.isActive() && m_repo.state != S::None; };
  auto text = [this] { return m_chip->property("text").toString(); };
  auto shot = [prefix](QWidget* w, const QString& suffix) { w->grab().save(prefix + suffix); };
  auto gitArgs = [this](const QStringList& args) {
    return QStringList{m_program, "-c", "user.name=OPAD Bench", "-c", "user.email=bench@example.com", "-c", "commit.gpgsign=false"} + args;
  };
  auto box = [cli, st](int x) {
    return QStringList{cli, "feature", st->file, "--kind", "box", "--inputs",
                       QStringLiteral(R"({"x":"%1 mm","length":"5 mm","width":"5 mm","height":"5 mm"})").arg(x)};
  };
  // Commands one after another, as typed in a terminal; the first failure stops the rest.
  auto external = [this, st](std::vector<QStringList> commands) {
    st->pending = std::move(commands);
    st->running = true;
    st->exit = 0;
    st->next = [this, st] {
      if (st->pending.empty() || st->exit != 0) {
        st->running = false;
        return;
      }
      QStringList c = st->pending.front();
      st->pending.erase(st->pending.begin());
      auto* p = new QProcess(this);
      p->setWorkingDirectory(st->dir);
      connect(p, &QProcess::finished, this, [st, p, c](int code) {
        st->exit = code;
        if (code) st->error = c.join(' ') + ": " + QString::fromUtf8(p->readAllStandardError());
        p->deleteLater();
        st->next();
      });
      connect(p, &QProcess::errorOccurred, this, [st, p, c](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        st->exit = -1;
        st->error = c.join(' ') + ": " + p->errorString();
        p->deleteLater();
        st->next();
      });
      const QString program = c.front();
      p->start(program, c.mid(1));
    };
    st->next();
  };
  auto finished = [st] {
    if (st->running) return false;
    if (st->exit != 0) throw std::runtime_error(st->error.toStdString());
    return true;
  };
  const std::vector<std::function<bool()>> steps = {
      [=, this] {
        if (!settled()) return false;
        require(!st->file.isEmpty(), "a saved document");
        require(m_repo.state == S::NotRepo && m_chip->property("state") == "none" && text() == tr("not in git"), "outside git: not in git");
        std::unique_ptr<QMenu> m(menu(m_window));
        QAction* setup = m->findChild<QAction*>("git.setup");
        require(setup && setup->text() == tr("Set up repository…"), "the chip offers to set up a repository");
        pass("not in a repository");
        setup->trigger();
        auto* d = m_window->findChild<QDialog*>("gitSetup");
        require(d && d->isVisible(), "the set-up dialog");
        auto* lfs = d->findChild<QCheckBox*>("lfs");
        require(lfs && lfs->isEnabled() == !m_repo.lfsVersion.isEmpty() && lfs->isChecked() == lfs->isEnabled(), "Git LFS offered when installed");
        shot(d, ".setup.png");
        for (QPushButton* b : d->findChildren<QPushButton*>())
          if (b->property("action") == "gitSetupRun") b->click();
        return true;
      },
      [=, this] {
        if (!settled() || m_repo.state != S::Ready) return false;
        QFile attributes(m_repo.top + "/.gitattributes"), ignore(m_repo.top + "/.gitignore");
        require(attributes.open(QIODevice::ReadOnly) && attributes.readAll().contains("*.opad text eol=lf merge=opad diff=opad"), ".gitattributes");
        require(ignore.open(QIODevice::ReadOnly) && ignore.readAll().contains("*.opad.tmp"), ".gitignore");
        require(m_repo.managed && m_repo.driver == here.mergeDriver() && m_repo.textconv == here.textconv() && m_repo.wantsDriver, "the managed driver config");
        require(m_repo.lfsVersion.isEmpty() || m_repo.lfsHooks, "Git LFS hooks in the clone");
        require(m_repo.status.branch == "main" && m_repo.status.oid == "(initial)", "a fresh repository on main");
        require(m_repo.doc() == D::Untracked && text() == "main · " + tr("untracked"), "the chip: main · untracked");
        shot(m_chip, ".chip-untracked.png");
        pass("set up: a fresh repository on main, the document untracked");
        st->runs = m_runs;
        st->clock.start();
        external({gitArgs({"add", "-A"}), gitArgs({"commit", "-q", "-m", "first"})});
        return true;
      },
      [=, this] {
        if (!finished() || !settled() || m_repo.status.oid == "(initial)" || m_repo.doc() != D::Clean) return false;
        require(m_runs > st->runs && text() == "main", "the commit read by itself");
        pass(QStringLiteral("commit from outside seen in %1 ms (%2 reads)").arg(st->clock.elapsed()).arg(m_runs - st->runs));
        st->runs = m_runs;
        st->ticks = 0;
        return true;
      },
      [=, this] {
        if (++st->ticks < 14) return false;  // 2 s
        require(m_runs == st->runs, "nothing reads git while nothing changes");
        pass("idle: git not run");
        external({box(40)});
        return true;
      },
      [=, this] {
        if (!finished() || !settled() || m_repo.doc() != D::Modified) return false;
        require(text() == "main · " + tr("uncommitted"), "the chip: main · uncommitted");
        shot(m_chip, ".chip-uncommitted.png");
        pass("edited: uncommitted");
        const QString remote = QDir(st->dir).absoluteFilePath("../git-bench-remote.git");
        external({gitArgs({"init", "-q", "--bare", remote}), gitArgs({"remote", "add", "origin", remote}), gitArgs({"commit", "-q", "-am", "second"}),
                  gitArgs({"push", "-q", "-u", "origin", "main"})});
        return true;
      },
      [=, this] {
        if (!finished() || !settled() || m_repo.sync() != Y::Synced || m_repo.doc() != D::Clean) return false;
        require(m_repo.status.upstream == "origin/main" && text() == "main", "following origin/main");
        pass("pushed: in step with origin/main");
        external({box(60), gitArgs({"commit", "-q", "-am", "third"})});
        return true;
      },
      [=, this] {
        if (!finished() || !settled() || m_repo.sync() != Y::Ahead || m_repo.doc() != D::Clean) return false;
        require(m_repo.status.ahead == 1 && text() == "main · " + tr("↑%1").arg(1), "the chip: main · ↑1");
        shot(m_chip, ".chip-ahead.png");
        pass("ahead of origin/main");
        external({gitArgs({"switch", "-q", "-c", "feature/x"})});
        return true;
      },
      [=, this] {
        if (!finished() || !settled() || m_repo.status.branch != "feature/x") return false;
        require(text() == "feature/x", "the chip names the branch");
        pass("branch switch seen");
        external({gitArgs({"config", "merge.opad.driver", "\"C:/nowhere/opad-cli.exe\" merge-driver %O %A %B %P"})});
        return true;
      },
      [=, this] {
        if (!finished() || !settled() || !m_repaired.contains(m_repo.top) || m_repo.driver != here.mergeDriver()) return false;
        pass("managed driver config repaired");
        external({gitArgs({"config", "--unset", "merge.opad.driver"}), gitArgs({"config", "--unset", "diff.opad.textconv"}),
                  gitArgs({"config", "--unset", "opad.managed"})});
        return true;
      },
      [=, this] {
        if (!finished() || !settled() || !m_repo.needsDriver()) return false;
        require(text() == "feature/x · " + tr("set up merging"), "the chip: set up merging");
        std::unique_ptr<QMenu> m(menu(m_window));
        QAction* driver = m->findChild<QAction*>("git.driver");
        require(driver, "the chip offers to set up merging");
        shot(m_chip, ".chip-driver.png");
        driver->trigger();
        return true;
      },
      [=, this] {
        if (!settled() || m_repo.needsDriver() || !m_repo.managed) return false;
        require(m_repo.driver == here.mergeDriver() && m_repo.textconv == here.textconv() && text() == "feature/x", "the driver set up again");
        pass("a clone without the driver, set up from the chip");
        return true;
      },
  };
  auto* timer = new QTimer(this);
  timer->setInterval(150);
  connect(timer, &QTimer::timeout, this, [st, steps, timer] {
    try {
      if (st->step >= steps.size()) {
        timer->stop();
        QCoreApplication::exit(0);
        return;
      }
      if (steps[st->step]()) {
        ++st->step;
        st->wait = 0;
      } else if (++st->wait > 400) {
        throw std::runtime_error("timed out in step " + std::to_string(st->step));
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QStringLiteral("bench: git: FAIL %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
