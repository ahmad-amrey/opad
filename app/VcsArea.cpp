// Version control as an area of the window (AreaController.hpp): the open file kept in step with the disk (DiskSync,
// UI-56) and its repository (GitWatch, UI-61 / UI-136): the status chip beside the path, File > Clone repository…;
// Compare (CompareMode, UI-58): File > Compare versions…, Inspect > Versions, the git chip's Compare with the last commit.
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
#include "Ribbon.hpp"
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
  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {  // File: after Open…, Clone repository… then Compare versions…
    QMenu* file = menus.value("file");
    if (!file) return;
    const QList<QAction*> items = file->actions();
    const qsizetype open = items.indexOf(services().action("file.open"));
    QAction* before = open >= 0 && open + 1 < items.size() ? items[open + 1] : nullptr;
    file->insertAction(before, services().action("file.clone"));
    file->insertAction(before, services().action("vcs.compare"));
  }
  void ribbon(RibbonLayout& layout) override {
    layout.addGroup("review.inspect", "review.inspect.versions", tr("Versions"));
    layout.addAction("review.inspect.versions", services().action("vcs.compare"));
    layout.addAction("design.construct.history", services().action("vcs.compare"));
  }
  void statusWidgets(QStatusBar* bar) override {  // the chip beside the document's path
    m_git = new GitWatch(services().jobs(), services().window(), services().viewport());
    connect(m_git, &GitWatch::openRequested, this, [this](const QString& file) { services().open(file); });
    connect(m_git, &GitWatch::compareRequested, this, [this] { if (m_compare) m_compare->open(); });
    bar->addWidget(m_git->chip());
  }
  void ready() override {
    AppDocument* doc = services().document();
    new DiskSync(doc, services().jobs(), services().viewport(), services().window());  // changed on disk: merged or reported, never overwritten
    connect(doc, &AppDocument::pathChanged, this, [this, doc] {
      m_git->setFile(doc->hasDocument && !doc->browse && !doc->doc.path.empty() ? doc->path() : QString());
    });
    m_compare = new CompareMode(services(), m_git);
  }
  void selectionChanged(const SelectionContext& selection) override { m_compare->selectionChanged(selection); }
  void documentChanged(bool replaced) override { m_compare->documentChanged(replaced); }

 private:
  GitWatch* m_git = nullptr;
  CompareMode* m_compare = nullptr;
};
}  // namespace

OPAD_AREA(Vcs)
