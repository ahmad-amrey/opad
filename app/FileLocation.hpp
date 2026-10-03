#pragma once
// Open file location and Copy path (UI-07) for any file the window names: the open document (File, the status path, the
// browser's document row), a recent file (the start page, File > Recent) and an import's source (the timeline). Nothing
// here waits: the file manager starts detached, per OS (Explorer /select, Finder open -R, the freedesktop FileManager1
// ShowItems call through dbus-send, else the folder through the desktop's opener); a file that is gone shows its nearest
// folder that is still there. A copy puts the native path on the clipboard and says so (the caller's `told`: a toast).
#include <QString>
#include <QStringList>
#include <functional>

class QAction;
class QMenu;
class QWidget;

namespace location {

struct Command {
  QString program;      // started detached with the arguments; empty: open `folder` with the desktop's opener
  QStringList arguments;
  QString folder;       // the folder shown (the file's, or the nearest one there when the file is gone)
  bool selects = false;  // the file manager shows the file itself selected
};
Command revealCommand(const QString& path);  // what reveal() runs (benches check it)
using Launcher = std::function<bool(const Command&)>;
void setLauncher(Launcher launcher);  // benches: record instead of opening a window on the desktop; null: the real one
using Copier = std::function<void(const QString& text)>;
void setCopier(Copier copier);  // benches: record instead of taking the desktop's clipboard (other programs write it too)
bool reveal(const QString& path);  // false: nothing could be started
QString nearestFolder(const QString& path);  // the folder itself, else the nearest existing folder above the path
QString nativePath(const QString& path);     // absolute, with the OS's separators
QString relativePath(const QString& path);   // inside its git work tree, '/' separated as git names it; empty: none

using Told = std::function<void(const QString&)>;
void show(const QString& path, const Told& told);      // reveal(), and told when the file is gone or nothing could start
void copyPath(const QString& path, const Told& told);  // the native path on the clipboard, told so

// Open file location, Copy path and (in a git work tree, `relative`) Copy relative path for `path`, named location.reveal,
// location.copy and location.copyRelative; `told` says what happened (the path copied, a missing file's folder shown).
void addEntries(QMenu* menu, const QString& path, const std::function<void(const QString&)>& told, bool relative = true);
// A recent file's menu (the start page, File > Recent): Open, the entries above, Remove from list (recent.open, recent.remove).
QMenu* recentMenu(const QString& path, QWidget* parent, const std::function<void(const QString&)>& told, std::function<void()> open,
                  std::function<void()> remove);
// A right click on an entry of `menu` that carries a path (QAction::data, File > Recent) opens contextMenu(path) instead
// of choosing it; contextMenu builds that menu (benches call it).
void addContextMenus(QMenu* menu, std::function<QMenu*(const QString& path, QWidget* parent)> contextMenu);

}  // namespace location
