// Open file location and Copy path (app/FileLocation.cpp, UI-07): the reveal command of each desktop built on this one
// (Explorer /select with the path quoted, Finder's open -R, FileManager1's ShowItems through dbus-send, else xdg-open on
// the folder), a file that is gone (its nearest folder), Linux's shell line run for real under a POSIX sh with a fake
// dbus-send and xdg-open (the call answered, and refused: the folder opened instead), and the menus' entries with what
// they copy and say. Nothing is started on the desktop and the clipboard is never taken.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <memory>

#include "FileLocation.hpp"
#include "check.hpp"
#include "opad/drawing_io.hpp"

namespace {
struct Tree {
  QTemporaryDir tmp;
  QString folder, file;  // a folder with a comma, a space and Arabic in its name; a file in it with a space
  Tree() {
    folder = tmp.path() + QString::fromUtf8("/a,b c/\xd9\x85\xd8\xac\xd9\x84\xd8\xaf");
    QDir().mkpath(folder);
    file = folder + "/x y.step";
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) throw check::Failure("cannot write " + file.toStdString());
    f.write("ISO-10303-21;");
  }
};

location::Desktop desktop(location::Desktop::Os os, bool dbus = false) {
  location::Desktop d;
  d.os = os;
  d.dbusSend = dbus;
  d.xdgOpen = "/usr/bin/xdg-open";
  d.systemRoot = "D:\\Win";
  return d;
}

QString winPath(QString p) { return p.replace('/', '\\'); }

// A POSIX sh to run Linux's line with: on PATH, else Git for Windows' or MSYS2's.
QString findSh() {
  if (const QString sh = QStandardPaths::findExecutable("sh"); !sh.isEmpty()) return sh;
  QStringList candidates{"C:/msys64/usr/bin/sh.exe", "C:/Program Files/Git/usr/bin/sh.exe", "C:/Program Files/Git/bin/sh.exe"};
  if (const QString git = QStandardPaths::findExecutable("git"); !git.isEmpty())
    candidates.prepend(QFileInfo(git).absolutePath() + "/../usr/bin/sh.exe");
  for (const QString& c : candidates)
    if (QFileInfo(c).isFile()) return QFileInfo(c).absoluteFilePath();
  return {};
}

void writeScript(const QString& path, const QByteArray& text) {
  QFile f(path);
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(text);
  f.close();
  f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
}
}  // namespace

TEST(reveal_command_per_desktop) {
  Tree t;
  const QString abs = QFileInfo(t.file).absoluteFilePath(), dir = QFileInfo(t.folder).absoluteFilePath();
  // Windows: Explorer under SystemRoot, the file selected, its path quoted whole (commas and spaces in it).
  location::Command c = location::revealCommand(t.file, desktop(location::Desktop::Windows));
  CHECK(c.program == "D:\\Win\\explorer.exe" && c.selects && c.folder == dir);
  CHECK(c.arguments == QStringList({"/select,", winPath(abs)}));
  CHECK(location::windowsArguments(c) == "/select,\"" + winPath(abs) + "\"");
  c = location::revealCommand(t.folder, desktop(location::Desktop::Windows));
  CHECK(!c.selects && c.arguments == QStringList({winPath(dir)}) && location::windowsArguments(c) == '"' + winPath(dir) + '"');
  // macOS: Finder through open -R (selected), a folder through open.
  c = location::revealCommand(t.file, desktop(location::Desktop::MacOS));
  CHECK(c.program == "/usr/bin/open" && c.selects && c.arguments == QStringList({"-R", abs}));
  c = location::revealCommand(t.folder, desktop(location::Desktop::MacOS));
  CHECK(!c.selects && c.arguments == QStringList({dir}));
  // Linux with dbus-send: FileManager1's ShowItems with the file's URI (no comma left for dbus-send to split at), the
  // folder as the fallback's argument.
  c = location::revealCommand(t.file, desktop(location::Desktop::Linux, true));
  CHECK(c.program == "/bin/sh" && c.selects && c.arguments.size() == 5 && c.arguments[0] == "-c" && c.arguments[2] == "sh");
  CHECK(c.arguments[1].contains("org.freedesktop.FileManager1.ShowItems \"array:string:$1\" string:") && c.arguments[1].contains("|| xdg-open \"$2\""));
  CHECK(c.arguments[3] == location::fileUri(abs) && c.arguments[4] == dir);
  CHECK(c.arguments[3].startsWith("file:///") && !c.arguments[3].contains(',') && !c.arguments[3].contains(' ') && c.arguments[3].contains("a%2Cb%20c"));
  CHECK(QUrl(c.arguments[3]).toLocalFile() == abs);  // the file manager decodes it back to the file
  // Linux without dbus-send, or a folder: the folder through xdg-open, nothing selected.
  c = location::revealCommand(t.file, desktop(location::Desktop::Linux, false));
  CHECK(c.program == "/usr/bin/xdg-open" && !c.selects && c.arguments == QStringList({dir}));
  c = location::revealCommand(t.folder, desktop(location::Desktop::Linux, true));
  CHECK(c.program == "/usr/bin/xdg-open" && !c.selects && c.arguments == QStringList({dir}));
  // A file that is gone: its nearest folder still there, on every desktop.
  const QString gone = t.folder + "/gone/deeper/z.step";
  CHECK(location::nearestFolder(gone) == dir);
  for (auto os : {location::Desktop::Windows, location::Desktop::MacOS, location::Desktop::Linux}) {
    c = location::revealCommand(gone, desktop(os, true));
    CHECK(!c.selects && c.folder == dir && c.arguments.size() == 1);
    CHECK(c.arguments[0] == (os == location::Desktop::Windows ? winPath(dir) : dir));
  }
  // This desktop's.
  const location::Desktop host = location::Desktop::host();
#if defined(Q_OS_WIN)
  CHECK(host.os == location::Desktop::Windows);
  CHECK(QFileInfo(location::revealCommand(t.file).program).fileName().compare("explorer.exe", Qt::CaseInsensitive) == 0);
#elif defined(Q_OS_MACOS)
  CHECK(host.os == location::Desktop::MacOS);
#else
  CHECK(host.os == location::Desktop::Linux);
#endif
}

TEST(linux_line_runs_under_sh) {
  const QString sh = findSh();
  if (sh.isEmpty()) {
    std::printf("  no POSIX sh here: Linux's line not run\n");
    return;
  }
  Tree t;
  const QString bin = t.tmp.path() + "/bin", log = t.tmp.path() + "/log.txt";
  QDir().mkpath(bin);
  writeScript(bin + "/dbus-send", "#!/bin/sh\nprintf '%s\\n' dbus-send \"$@\" >> \"$OPAD_FAKE_LOG\"\nexit \"$OPAD_FAKE_DBUS\"\n");
  writeScript(bin + "/xdg-open", "#!/bin/sh\nprintf '%s\\n' xdg-open \"$@\" >> \"$OPAD_FAKE_LOG\"\n");
  const location::Command c = location::revealCommand(t.file, desktop(location::Desktop::Linux, true));
  auto run = [&](const char* dbusExit) {
    QFile::remove(log);
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PATH", QDir::toNativeSeparators(bin) + QDir::listSeparator() + env.value("PATH"));
    env.insert("OPAD_FAKE_LOG", log);
    env.insert("OPAD_FAKE_DBUS", dbusExit);
    env.insert("LANG", "C.UTF-8");
    env.insert("MSYS2_ARG_CONV_EXCL", "*");
    p.setProcessEnvironment(env);
    p.start(sh, c.arguments);
    CHECK(p.waitForFinished(30000) && p.exitStatus() == QProcess::NormalExit);
    QFile f(log);
    if (!f.open(QIODevice::ReadOnly)) return QStringList();
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
  };
  // The file manager answers: ShowItems with the URI as one array element, nothing else started.
  QStringList seen = run("0");
  CHECK(seen == QStringList({"dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.FileManager1", "--type=method_call",
                             "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems", "array:string:" + c.arguments[3], "string:"}));
  // None answers: the folder opens, its name whole.
  seen = run("1");
  CHECK(seen.size() == 11 && seen[0] == "dbus-send" && seen[9] == "xdg-open" && seen[10] == c.folder);
}

TEST(entries_copy_and_say) {
  Tree t;
  const bool outside = opad::repo_top(std::filesystem::path(t.tmp.path().toStdU16String())).empty();
  std::vector<location::Command> launched;
  location::setLauncher([&](const location::Command& c) {
    launched.push_back(c);
    return true;
  });
  QString copied;
  location::setCopier([&](const QString& text) { copied = text; });
  QStringList told;
  auto tell = [&](const QString& text) { told << text; };
  auto names = [](QMenu& m) {
    QStringList out;
    for (QAction* a : m.actions())
      if (!a->isSeparator()) out << a->objectName();
    return out;
  };
  if (outside) {  // no work tree: no relative path
    QMenu m;
    location::addEntries(&m, t.file, tell);
    CHECK(names(m) == QStringList({"location.reveal", "location.copy"}));
  }
  QDir().mkpath(t.tmp.path() + "/.git");
  QMenu m;
  location::addEntries(&m, t.file, tell);
  CHECK(names(m) == QStringList({"location.reveal", "location.copy", "location.copyRelative"}));
  m.actions()[0]->trigger();
  CHECK(launched.size() == 1 && launched[0].selects && launched[0].folder == QFileInfo(t.folder).absoluteFilePath() && told.isEmpty());
  m.actions()[1]->trigger();
  CHECK(copied == location::nativePath(t.file) && told.size() == 1 && told[0].contains(copied));
  m.actions()[2]->trigger();
  CHECK(copied == QString::fromUtf8("a,b c/\xd9\x85\xd8\xac\xd9\x84\xd8\xaf/x y.step") && told.last().contains(copied));
  // A file that is gone: its folder shown and said so; Open in a recent file's menu greyed, Remove from list there.
  const QString gone = t.folder + "/gone.step";
  location::show(gone, tell);
  CHECK(launched.back().folder == QFileInfo(t.folder).absoluteFilePath() && !launched.back().selects && told.last().contains(location::nativePath(gone)));
  bool removed = false;
  std::unique_ptr<QMenu> recent(location::recentMenu(gone, nullptr, tell, [] {}, [&] { removed = true; }));
  CHECK(names(*recent) == QStringList({"recent.open", "location.reveal", "location.copy", "location.copyRelative", "recent.remove"}));
  CHECK(!recent->actions()[0]->isEnabled());
  recent->actions().last()->trigger();
  CHECK(removed);
  location::setLauncher(nullptr);
  location::setCopier(nullptr);
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  return check::run_all(argc, argv);
}
