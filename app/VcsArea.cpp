// Version control as an area of the window (AreaController.hpp): the open file kept in step with the disk (DiskSync,
// UI-56) and its repository (GitWatch, UI-61 / UI-136): the status chip beside the path, File > Clone repository…;
// Compare (CompareMode, UI-58): File > Compare versions…, Inspect > Versions, the git chip's Compare with the last commit,
// the Recovery offer's Compare…; Show unsaved changes (UI-59), also the unsaved-changes question's Review changes…;
// the Version control panel and its commands (VersionControl, UI-62): File > Version control, Alt+4, the chip's menu.
#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>
#include <tuple>

#include "AppDocument.hpp"
#include "Commands.hpp"
#include "CompareMode.hpp"
#include "DiskSync.hpp"
#include "GitWatch.hpp"
#include "RecoveryManager.hpp"
#include "Ribbon.hpp"
#include "VersionControl.hpp"
#include "Viewport.hpp"

namespace {
class Vcs : public AreaController {
 public:
  using AreaController::AreaController;
  void buildActions() override {
    CommandInfo clone;
    clone.id = "file.clone";
    clone.label = tr("Clone repository…");
    clone.icon = "git";
    clone.keywords = {"git"};
    services().addCommand(clone, [this] { if (m_git) m_git->cloneRepository(); });
    const QString group = tr("Version control");
    CommandInfo compare;
    compare.id = "vcs.compare";
    compare.label = tr("Compare versions…");
    compare.icon = "compare";
    compare.group = group;
    compare.keywords = {"diff", "changes", "git", "history", "version", "revision"};
    compare.enabledWhen = [](const CommandContext& c) { return c.document && !c.viewer && !c.sketching; };
    services().addCommand(compare, [this] { if (m_compare) m_compare->open(); });
    CommandInfo unsaved;  // UI-59: also the unsaved-changes question's Review changes…
    unsaved.id = "vcs.unsavedChanges";
    unsaved.label = tr("Show unsaved changes");
    unsaved.icon = "compare";
    unsaved.group = group;
    unsaved.keywords = {"diff", "changes", "unsaved", "modified", "review"};
    unsaved.enabledWhen = [this](const CommandContext& c) {
      const AppDocument* d = services().document();
      return c.document && !c.viewer && !c.sketching && d && d->isDirty() && !d->doc.path.empty();
    };
    services().addCommand(unsaved, [this] { if (m_compare) m_compare->showUnsaved(); });
    // Version control (UI-62): the panel and its commands, for a saved OPAD document; they say why when git cannot.
    auto saved = [this](const CommandContext& c) {
      const AppDocument* d = services().document();
      return c.document && !c.viewer && d && !d->doc.path.empty();
    };
    auto command = [&](const char* id, const QString& label, const char* icon, const QStringList& keywords, std::function<void(VersionControl*)> fn,
                       const QKeySequence& key = {}) {
      CommandInfo info;
      info.id = QString::fromLatin1(id);
      info.label = label;
      info.icon = QString::fromLatin1(icon);
      info.group = group;
      info.key = key;
      info.keywords = keywords;
      info.enabledWhen = saved;
      services().addCommand(info, [this, fn] { if (m_version) fn(m_version); });
    };
    command("vcs.panel", tr("Version control"), "git", {"git", "history", "branch", "commit", "push", "pull", "merge", "versions"},
            [](VersionControl* v) { v->openPanel(); }, QKeySequence(QStringLiteral("Alt+4")));
    command("vcs.commit", tr("Commit…"), "commit", {"git", "save", "version", "check in"}, [](VersionControl* v) { v->commit(); });
    command("vcs.push", tr("Push"), "push", {"git", "upload", "share", "remote"}, [](VersionControl* v) { v->push(); });
    command("vcs.pull", tr("Pull"), "pull", {"git", "update", "download", "remote", "merge"}, [](VersionControl* v) { v->pull(); });
    command("vcs.fetch", tr("Fetch"), "pull", {"git", "remote", "update"}, [](VersionControl* v) { v->fetch(); });
    command("vcs.newBranch", tr("New branch…"), "branch", {"git", "branch", "create"}, [](VersionControl* v) { v->newBranch(); });
    command("vcs.history", tr("History"), "history", {"git", "log", "commits", "restore", "older"}, [](VersionControl* v) { v->openPanel(VersionControl::History); });
    command("vcs.branches", tr("Branches"), "branch", {"git", "switch", "checkout", "merge"}, [](VersionControl* v) { v->openPanel(VersionControl::Branches); });
    command("vcs.pack", tr("Pack the repository"), "git", {"git", "gc", "maintenance", "size", "compress"}, [](VersionControl* v) { v->pack(); });
    // ] and [ step through the changes while Compare is open; elsewhere they do nothing.
    for (const auto& [id, label, key, delta] : {std::tuple{"vcs.nextChange", tr("Next change"), "]", 1}, std::tuple{"vcs.previousChange", tr("Previous change"), "[", -1}}) {
      CommandInfo step;
      step.id = id;
      step.label = label;
      step.key = QKeySequence(QString::fromLatin1(key));
      step.scope = shortcuts::OutsideSketch;
      step.group = group;
      step.keywords = {"compare", "diff"};
      step.enabledWhen = [this](const CommandContext&) { return m_compare && m_compare->active(); };
      services().addCommand(step, [this, delta = delta] { if (m_compare) m_compare->step(delta); });
    }
  }
  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {  // File: after Open…, Clone repository…, Compare versions…, Show unsaved changes
    QMenu* file = menus.value("file");
    if (!file) return;
    const QList<QAction*> items = file->actions();
    const qsizetype open = items.indexOf(services().action("file.open"));
    QAction* before = open >= 0 && open + 1 < items.size() ? items[open + 1] : nullptr;
    file->insertAction(before, services().action("file.clone"));
    file->insertAction(before, services().action("vcs.compare"));
    file->insertAction(before, services().action("vcs.unsavedChanges"));
    auto* version = new QMenu(tr("Version control"), file);
    version->setObjectName("versionMenu");
    for (const char* id : {"vcs.panel", "vcs.commit", "vcs.pull", "vcs.push", "vcs.fetch", "-", "vcs.history", "vcs.branches", "vcs.newBranch", "-", "vcs.pack"})
      if (QString::fromLatin1(id) == "-") version->addSeparator();
      else version->addAction(services().action(QString::fromLatin1(id)));
    file->insertMenu(before, version);
  }
  void ribbon(RibbonLayout& layout) override {
    layout.addGroup("review.inspect", "review.inspect.versions", tr("Versions"));
    layout.addAction("review.inspect.versions", services().action("vcs.compare"));
    layout.addAction("design.construct.history", services().action("vcs.compare"));
    for (const char* group : {"review.inspect.versions", "design.construct.history"}) {
      layout.addAction(QString::fromLatin1(group), services().action("vcs.panel"));
      layout.addAction(QString::fromLatin1(group), services().action("vcs.commit"));
    }
  }
  void statusWidgets(QStatusBar* bar) override {  // the chip beside the document's path
    m_git = new GitWatch(services().jobs(), services().window(), services().viewport());
    connect(m_git, &GitWatch::openRequested, this, [this](const QString& file) { services().open(file); });
    connect(m_git, &GitWatch::compareRequested, this, [this] { if (m_compare) m_compare->open(); });
    bar->addWidget(m_git->chip());
  }
  void ready() override {
    AppDocument* doc = services().document();
    auto* disk = new DiskSync(doc, services().jobs(), services().viewport(), services().window());  // changed on disk: merged or reported, never overwritten
    connect(doc, &AppDocument::pathChanged, this, [this, doc] {
      m_git->setFile(doc->hasDocument && !doc->browse && !doc->doc.path.empty() ? doc->path() : QString());
    });
    m_compare = new CompareMode(services(), m_git);
    m_version = new VersionControl(services(), m_git, m_compare, disk);
    if (auto* recovery = services().window()->findChild<RecoveryManager*>())  // the Recovery offer's Compare…: the file, then the snapshot
      connect(recovery, &RecoveryManager::compareRequested, this, [this](const QString& source, const QString& snapshot, const QString& time) {
        m_compare->compareIn(source, CompareMode::savedVersion(source), CompareMode::recoveryVersion(snapshot, time));
      });
  }
  void selectionChanged(const SelectionContext& selection) override { m_compare->selectionChanged(selection); }
  void documentChanged(bool replaced) override { m_compare->documentChanged(replaced); }

 private:
  GitWatch* m_git = nullptr;
  CompareMode* m_compare = nullptr;
  VersionControl* m_version = nullptr;
};
}  // namespace

OPAD_AREA(Vcs)
