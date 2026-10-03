// Version control as an area of the window (AreaController.hpp): the open file kept in step with the disk (DiskSync,
// UI-56) and its repository (GitWatch, UI-61 / UI-136): the status chip beside the path, File > Clone repository….
#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>

#include "AppDocument.hpp"
#include "Commands.hpp"
#include "DiskSync.hpp"
#include "GitWatch.hpp"
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
  }
  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {  // File: after Open…
    QMenu* file = menus.value("file");
    if (!file) return;
    const QList<QAction*> items = file->actions();
    const qsizetype open = items.indexOf(services().action("file.open"));
    file->insertAction(open >= 0 && open + 1 < items.size() ? items[open + 1] : nullptr, services().action("file.clone"));
  }
  void statusWidgets(QStatusBar* bar) override {  // the chip beside the document's path
    m_git = new GitWatch(services().jobs(), services().window(), services().viewport());
    connect(m_git, &GitWatch::openRequested, this, [this](const QString& file) { services().open(file); });
    bar->addWidget(m_git->chip());
  }
  void ready() override {
    AppDocument* doc = services().document();
    new DiskSync(doc, services().jobs(), services().viewport(), services().window());  // changed on disk: merged or reported, never overwritten
    connect(doc, &AppDocument::pathChanged, this, [this, doc] {
      m_git->setFile(doc->hasDocument && !doc->browse && !doc->doc.path.empty() ? doc->path() : QString());
    });
  }

 private:
  GitWatch* m_git = nullptr;
};
}  // namespace

OPAD_AREA(Vcs)
