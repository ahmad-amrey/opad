#include "FileLocation.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <filesystem>

#include "Icons.hpp"
#include "opad/drawing_io.hpp"

namespace location {
namespace {
const char* const kContext = "location";
QString tr(const char* text) { return QCoreApplication::translate(kContext, text); }

Launcher& launcher() {
  static Launcher l;
  return l;
}

Copier& copier() {
  static Copier c;
  return c;
}

void toClipboard(const QString& text) {
  if (copier()) copier()(text);
  else QApplication::clipboard()->setText(text);
}

bool launch(const Command& c) {
  QProcess p;
  p.setProgram(c.program);
#ifdef Q_OS_WIN
  p.setNativeArguments(windowsArguments(c));
#else
  p.setArguments(c.arguments);
#endif
  return p.startDetached();
}

// Right clicks on File > Recent's entries: their own menu, never choosing the entry.
class ContextMenus : public QObject {
 public:
  ContextMenus(QMenu* menu, std::function<QMenu*(const QString&, QWidget*)> build) : QObject(menu), m_menu(menu), m_build(std::move(build)) {}
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseButtonRelease && e->type() != QEvent::ContextMenu) return false;
    const QPoint pos = e->type() == QEvent::ContextMenu ? static_cast<QContextMenuEvent*>(e)->pos() : static_cast<QMouseEvent*>(e)->position().toPoint();
    if (e->type() != QEvent::ContextMenu && static_cast<QMouseEvent*>(e)->button() != Qt::RightButton) return false;
    QAction* a = m_menu->actionAt(pos);
    const QString path = a ? a->data().toString() : QString();
    if (path.isEmpty()) return false;
    if (e->type() == QEvent::MouseButtonRelease && !m_open) {  // where every platform has finished its own handling
      QMenu* m = m_build(path, m_menu);
      m->setAttribute(Qt::WA_DeleteOnClose);
      m_open = m;
      QObject::connect(m, &QMenu::triggered, m_menu, [menu = QPointer<QMenu>(m_menu)] {  // done: the whole chain closes
        for (QWidget* w = menu; qobject_cast<QMenu*>(w); w = w->parentWidget()) w->close();
      });
      m->popup(m_menu->mapToGlobal(pos));
    }
    return true;  // the press, the release and the context menu event: none chooses the entry
  }
 private:
  QMenu* m_menu;
  std::function<QMenu*(const QString&, QWidget*)> m_build;
  QPointer<QMenu> m_open;
};
}  // namespace

QString nativePath(const QString& path) { return path.isEmpty() ? QString() : QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()); }

QString nearestFolder(const QString& path) {
  QFileInfo fi(QFileInfo(path).absoluteFilePath());
  for (QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();; dir = QFileInfo(dir).absolutePath()) {
    if (QFileInfo(dir).isDir()) return dir;
    if (QDir(dir).isRoot() || QFileInfo(dir).absolutePath() == dir) return {};
  }
}

QString relativePath(const QString& path) {
  if (path.isEmpty()) return {};
  const std::filesystem::path file(QFileInfo(path).absoluteFilePath().toStdU16String());
  const std::filesystem::path top = opad::repo_top(file);
  return top.empty() ? QString() : QDir(QString::fromStdU16String(top.u16string())).relativeFilePath(QFileInfo(path).absoluteFilePath());
}

Desktop Desktop::host() {
  Desktop d;
#if defined(Q_OS_WIN)
  d.os = Windows;
  d.systemRoot = qEnvironmentVariable("SystemRoot", d.systemRoot);
#elif defined(Q_OS_MACOS)
  d.os = MacOS;
#else
  d.dbusSend = !QStandardPaths::findExecutable("dbus-send").isEmpty();
  if (const QString opener = QStandardPaths::findExecutable("xdg-open"); !opener.isEmpty()) d.xdgOpen = opener;
#endif
  return d;
}

QString windowsArguments(const Command& c) {
  return c.selects ? c.arguments.value(0) + '"' + c.arguments.value(1) + '"' : '"' + c.arguments.value(0) + '"';
}

QString fileUri(const QString& path) { return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded).replace(',', "%2C"); }

Command revealCommand(const QString& path, const Desktop& desktop) {
  Command c;
  const QFileInfo fi(path);
  const bool file = !path.isEmpty() && fi.exists() && !fi.isDir();
  c.folder = file ? fi.absolutePath() : nearestFolder(path);
  c.selects = file;
  const QString target = file ? fi.absoluteFilePath() : c.folder;
  switch (desktop.os) {
    case Desktop::Windows: {
      const QString native = QString(target).replace('/', '\\');
      c.program = QString(desktop.systemRoot).replace('/', '\\') + "\\explorer.exe";
      c.arguments = file ? QStringList{"/select,", native} : QStringList{native};
      break;
    }
    case Desktop::MacOS:
      c.program = "/usr/bin/open";
      c.arguments = file ? QStringList{"-R", target} : QStringList{target};
      break;
    case Desktop::Linux:
      // The file manager's own "show this file" (FileManager1: Nautilus, Dolphin, Nemo, Caja, Thunar), else the folder.
      if (file && desktop.dbusSend) {
        c.program = "/bin/sh";
        c.arguments = {"-c", "dbus-send --session --print-reply --dest=org.freedesktop.FileManager1 --type=method_call /org/freedesktop/FileManager1 "
                             "org.freedesktop.FileManager1.ShowItems \"array:string:$1\" string: >/dev/null 2>&1 || xdg-open \"$2\"",
                       "sh", fileUri(target), c.folder};
      } else {
        c.selects = false;
        c.program = desktop.xdgOpen;
        c.arguments = {c.folder};
      }
      break;
  }
  return c;
}

void setLauncher(Launcher l) { launcher() = std::move(l); }
void setCopier(Copier c) { copier() = std::move(c); }

bool reveal(const QString& path) {
  const Command c = revealCommand(path);
  if (c.folder.isEmpty()) return false;
  return launcher() ? launcher()(c) : launch(c);
}

void show(const QString& path, const Told& told) {
  const bool there = QFileInfo::exists(path);
  if (!reveal(path)) told(tr("Could not show %1: no folder of it is there").arg(nativePath(path)));
  else if (!there) told(tr("%1 is no longer there; its nearest folder is shown").arg(nativePath(path)));
}

void copyPath(const QString& path, const Told& told) {
  toClipboard(nativePath(path));
  told(tr("Copied %1").arg(nativePath(path)));
}

void addEntries(QMenu* menu, const QString& path, const std::function<void(const QString&)>& told, bool relative) {
  const QString native = nativePath(path);
  QAction* open = menu->addAction(icons::themed("open", 16), tr("Open file location"));
  open->setObjectName("location.reveal");
  open->setToolTip(native);
  QObject::connect(open, &QAction::triggered, menu, [path, told] { show(path, told); });
  QAction* copy = menu->addAction(icons::themed("copy", 16), tr("Copy path"));
  copy->setObjectName("location.copy");
  QObject::connect(copy, &QAction::triggered, menu, [path, told] { copyPath(path, told); });
  if (!relative) return;
  const QString rel = relativePath(path);
  if (rel.isEmpty()) return;
  QAction* inRepo = menu->addAction(icons::themed("copy", 16), tr("Copy relative path"));
  inRepo->setObjectName("location.copyRelative");
  inRepo->setToolTip(rel);
  QObject::connect(inRepo, &QAction::triggered, menu, [rel, told] {
    toClipboard(rel);
    told(tr("Copied %1").arg(rel));
  });
}

QMenu* recentMenu(const QString& path, QWidget* parent, const std::function<void(const QString&)>& told, std::function<void()> open,
                  std::function<void()> remove) {
  auto* m = new QMenu(parent);
  m->setObjectName("recentMenu");
  QAction* first = m->addAction(icons::themed("open", 16), tr("Open"));
  first->setObjectName("recent.open");
  first->setEnabled(QFileInfo::exists(path));
  QObject::connect(first, &QAction::triggered, m, std::move(open));
  m->addSeparator();
  addEntries(m, path, told);
  m->addSeparator();
  QAction* drop = m->addAction(icons::themed("delete", 16), tr("Remove from list"));
  drop->setObjectName("recent.remove");
  QObject::connect(drop, &QAction::triggered, m, std::move(remove));
  return m;
}

void addContextMenus(QMenu* menu, std::function<QMenu*(const QString&, QWidget*)> contextMenu) {
  menu->installEventFilter(new ContextMenus(menu, std::move(contextMenu)));
}

}  // namespace location
