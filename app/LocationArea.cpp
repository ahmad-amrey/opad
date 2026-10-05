// Where files are (UI-07), as an area of the window (AreaController.hpp): File > Open file location (Shift+Alt+R) and
// Copy path (Shift+Alt+C) for the open document (its file, or the viewed one), also in the File group of each workspace's
// Export tab, the same and Copy relative path in the status path's menu and the browser's document row, and an import's
// source file on its timeline marker: its menu (found where the op recorded it, opad::import_source) and its tooltip.
// FileLocation.hpp does the work; nothing here waits.
#include <QAction>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <map>

#include "AppDocument.hpp"
#include "Commands.hpp"
#include "FileLocation.hpp"
#include "Ribbon.hpp"
#include "StatusRow.hpp"
#include "TimelineWidget.hpp"
#include "Theme.hpp"
#include "opad/drawing_io.hpp"

namespace {
std::filesystem::path fsPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }

class Locations : public AreaController {
 public:
  using AreaController::AreaController;
  // The open document's file: its .opad, or the file viewed read-only; empty for an unsaved one.
  QString documentFile() const {
    const AppDocument* d = services().document();
    return !d || !d->hasDocument ? QString() : d->browse ? d->viewing : d->path();
  }
  void told(const QString& text) {
    const AppDocument* d = services().document();
    if (d && d->hasDocument) services().toast(text);
    else services().showMessage(text, 6000);  // the start page: no view to show a toast over
  }
  // An import's source as the op recorded it, found again: looked up at most every 2 s per op (tooltips ask on every move).
  std::pair<QString, bool> source(const opad::Op& op) {
    Source& s = m_sources[op.id];
    if (!s.checked.isValid() || s.checked.hasExpired(2000)) {
      const std::filesystem::path p = opad::import_source(op.data, fsPath(services().document()->path()), &s.exists);
      s.path = p.empty() ? QString() : QString::fromStdU16String(p.u16string());
      s.checked.start();
    }
    return {s.path, s.exists};
  }

  void buildActions() override {
    auto fileCommand = [this](const char* id, const QString& label, const char* icon, const char* key, const QStringList& keywords,
                              std::function<void(const QString&)> fn) {
      CommandInfo info;
      info.id = QString::fromLatin1(id);
      info.label = label;
      info.icon = QString::fromLatin1(icon);
      info.key = QKeySequence(QString::fromLatin1(key));
      info.keywords = keywords;
      info.enabledWhen = [this](const CommandContext& c) { return c.document && !documentFile().isEmpty(); };
      services().addCommand(info, [this, fn] {
        const QString file = documentFile();
        if (!file.isEmpty()) fn(file);
      });
    };
    fileCommand("file.reveal", tr("Open file location"), "open", "Shift+Alt+R", {"folder", "explorer", "finder", "show in folder", "reveal", "directory"},
                [this](const QString& file) { location::show(file, [this](const QString& text) { told(text); }); });
    fileCommand("file.copyPath", tr("Copy path"), "copy", "Shift+Alt+C", {"clipboard", "file name", "full path", "location"},
                [this](const QString& file) { location::copyPath(file, [this](const QString& text) { told(text); }); });
  }
  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {  // File: after Save screenshot…
    QMenu* file = menus.value("file");
    if (!file) return;
    const QList<QAction*> items = file->actions();
    const qsizetype at = items.indexOf(services().action("file.screenshot"));
    QAction* before = at >= 0 && at + 1 < items.size() ? items[at + 1] : nullptr;
    file->insertAction(before, services().action("file.reveal"));
    file->insertAction(before, services().action("file.copyPath"));
  }
  void ready() override {
    connect(services().pathChip(), &PathChip::menuRequested, this, [this](QMenu* menu) {
      location::addEntries(menu, services().pathChip()->file(), [this](const QString& text) { told(text); });
    });
    services().timeline()->addTipProvider([this](const opad::Op& op) -> QString {
      if (op.type != "import") return {};
      const auto [path, exists] = source(op);
      if (path.isEmpty()) return {};
      const Tokens& t = theme::current();
      return QStringLiteral("<div style='color:%1'>%2</div>")
          .arg((exists ? t.fg2 : t.amber).name(), (exists ? tr("from %1") : tr("from %1 (not found there now)")).arg(QDir::toNativeSeparators(path)).toHtmlEscaped());
    });
  }
  void contextMenu(const SelectionContext& selection, QMenu& menu) override {  // the browser's document row
    const QString file = documentFile();
    if (!selection.document || file.isEmpty()) return;
    menu.addSeparator();
    location::addEntries(&menu, file, [this](const QString& text) { told(text); });
  }
  void timelineMenu(const std::string& opId, QMenu& menu) override {  // an import: its source file
    const opad::Op* op = services().document()->doc.find_op(opId);
    if (!op || op->type != "import") return;
    const auto [path, exists] = source(*op);
    if (path.isEmpty()) return;
    menu.addSeparator();
    location::addEntries(&menu, path, [this](const QString& text) { told(text); });
    const QString name = QFileInfo(path).fileName();  // which file: these are the source's, not the document's
    for (QAction* a : menu.actions())
      if (a->objectName() == "location.reveal") a->setText(exists ? tr("Open location of %1").arg(name) : tr("Open the folder %1 was in").arg(name));
      else if (a->objectName() == "location.copy") a->setText(tr("Copy path of %1").arg(name));
      else if (a->objectName() == "location.copyRelative") a->setText(tr("Copy relative path of %1").arg(name));
  }
  void documentChanged(bool) override { m_sources.clear(); }

 private:
  struct Source {
    QString path;
    bool exists = false;
    QElapsedTimer checked;
  };
  std::map<std::string, Source> m_sources;
};
}  // namespace

OPAD_AREA(Locations)
