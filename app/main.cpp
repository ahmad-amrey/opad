#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QSurfaceFormat>

#include "CompareMode.hpp"
#include <cstring>

#include "CrashLog.hpp"
#include "GitWatch.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "opad/core.hpp"
#include "opad/drawing/paint.hpp"
#include "opad/render.hpp"

namespace {
// macOS hands the files a user opens from Finder (or drops on the Dock icon) to a running app as events, not arguments.
class FileOpenEvents : public QObject {
 public:
  explicit FileOpenEvents(MainWindow* window) : QObject(window), m_window(window) {}
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() == QEvent::FileOpen) {
      const QString file = static_cast<QFileOpenEvent*>(event)->file();
      if (!file.isEmpty()) m_window->openPath(file);
      return true;
    }
    return QObject::eventFilter(watched, event);
  }
 private:
  MainWindow* m_window;
};

// git's merge driver (merge.opad.driver "opad.exe --merge-driver %O %A %B %P"): before Qt and without a window, so a
// portable or single-file install merges .opad files without Python or opad-cli.
int mergeDriver(int argc, char** argv) {
  std::vector<std::filesystem::path> files;
#ifdef _WIN32
  int n = 0;  // the paths as UTF-16: the narrow argv is in the ANSI code page
  if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n)) {
    for (int i = 2; i < n; ++i) files.emplace_back(wide[i]);
    LocalFree(wide);
    return opad::merge_driver(files);
  }
#endif
  for (int i = 2; i < argc; ++i) files.push_back(opad::path_from_utf8(argv[i]));
  return opad::merge_driver(files);
}

// git's diff driver (diff.opad.textconv "opad.exe --textconv"): the document as readable lines, like opad-cli textconv.
int textconv(int argc, char** argv) {
  if (argc < 3) return 2;
  std::filesystem::path path = opad::path_from_utf8(argv[2]);
#ifdef _WIN32
  int n = 0;
  if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n)) {
    if (n > 2) path = wide[2];
    LocalFree(wide);
  }
  _setmode(_fileno(stdout), _O_BINARY);  // LF, as git compares it
#endif
  std::string out;
  try {
    out = opad::document_outline(opad::Document::parse_index(opad::read_text_file(path), path));
  } catch (const std::exception& e) {
    try {
      out = opad::text_outline(opad::read_text_file(path), e.what());
    } catch (const std::exception&) {
      return 1;
    }
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "--merge-driver") return mergeDriver(argc, argv);
  if (argc >= 2 && std::string_view(argv[1]) == "--textconv") return textconv(argc, argv);
  // The start page's pictures where no opad-cli is beside the app (the single-file exe, UI-113): opad --thumbnail <file>
  // --out <picture.bgra|png> [--size N], in a process of its own, before any window or trace.
  if (argc > 1 && std::strcmp(argv[1], "--thumbnail") == 0) {
    QCoreApplication core(argc, argv);  // the arguments as Unicode
    opad::configure_kernel_logging();
    const QStringList a = QCoreApplication::arguments();
    if (a.size() < 5 || a[3] != "--out") return 2;
    try {
      opad::write_thumbnail(a[2].toStdString(), a[4].toStdString(), a.size() > 6 && a[5] == "--size" ? a[6].toInt() : 256);
      return 0;
    } catch (const std::exception&) {
      return 1;
    }
  }
  const bool askpass = GitWatch::isAskpass(argc, argv);  // git's GIT_ASKPASS: one dialog, no window, no file
  trace::log("startup: main");
  installCrashHandler();
  // Derived ids are for scripted builds (gap log #15): a desktop session restarted on the same document would derive
  // the same ones again.
  qunsetenv("OPAD_DETERMINISTIC");
  opad::configure_kernel_logging();
#if !defined(_WIN32) && !defined(__APPLE__)
  // The viewport hands winId() to OCCT as an X11 window (Xw_Window). Under the Wayland platform plugin that is a
  // wl_surface handle instead and the first X request fails with BadWindow, so run on X11 (XWayland) unless the
  // user chose a platform explicitly.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "xcb");
#endif
  QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QApplication app(argc, argv);
  // Workers (the jobs' threads, OCCT's pool on every logical processor) run at normal priority: above them the event loop
  // is not starved while they keep every core busy (gaps of 300-700 ms on the Engine's drawing otherwise). Measured on the
  // Engine after the t4/t5a merges, with and without it: tracking-engine's box picks held the loop 127 / 52 ms (332 / 293 ms
  // without: FAIL) in the same total time (198 s / 203 s); looks-engine and components-engine unchanged.
  QThread::currentThread()->setPriority(QThread::HighPriority);
  QApplication::setApplicationName("OPAD");
  QApplication::setOrganizationName("opad");
  QApplication::setApplicationVersion(QString::fromStdString(opad::version_string()));
  QApplication::setWindowIcon(icons::appIcon());
  opad::drawing::install_painter();  // PDF and PNG drawings (export, on workers too)
  // Portable runs keep settings and cache beside the exe instead of the registry and %LOCALAPPDATA%, so the program
  // can move between machines: always in the single-file build (<exe dir>/opad-data), and in the folder package
  // when the `opad.portable` marker sits beside the exe (<exe dir>/data).
  const QString exeDir = QApplication::applicationDirPath();
  QString dataDir;
#ifdef OPAD_SINGLE_FILE
  dataDir = exeDir + "/opad-data";
#endif
  if (QFile::exists(exeDir + "/opad.portable")) dataDir = exeDir + "/data";
  if(!qEnvironmentVariableIsEmpty("OPAD_BENCH_SETTINGS")) dataDir=qEnvironmentVariable("OPAD_BENCH_SETTINGS");
  if (!dataDir.isEmpty()) {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dataDir);
    if (qEnvironmentVariableIsEmpty("OPAD_CACHE_DIR"))
      qputenv("OPAD_CACHE_DIR", QDir::toNativeSeparators(dataDir + "/cache").toLocal8Bit());
  }
  i18n::install(app);  // before any widget exists: translator and layout direction (needs the names above for QSettings)
  if (askpass) return GitWatch::askpassDialog(argc, argv);
  trace::log("startup: application");

  QCommandLineParser parser;
  parser.setApplicationDescription("OPAD: CAD viewer and git-native parametric modeller");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("file", "An .opad document, or a STEP, IGES, STL, 3MF, OBJ, glTF, PLY, DXF, DWG or SVG file to view");
  QCommandLineOption bench("bench-select", "Select every root once the file has loaded, log the timing (OPAD_TRACE) and quit");
  bench.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(bench);
  QCommandLineOption compare("compare", "Compare a version (an .opad file, or git:REV of the file) with the file opened: opad --compare a b", "version");
  parser.addOption(compare);
  QCommandLineOption readOnly("read-only", "Open the .opad document read-only: it is not changed, edits need a copy (Save a copy)");
  parser.addOption(readOnly);
  parser.process(app);
  if (parser.isSet(compare)) CompareMode::setStartup(parser.value(compare));  // once the file is open (CompareMode.hpp)

  MainWindow win;
  app.installEventFilter(new FileOpenEvents(&win));
  trace::log("startup: window built");
  win.setBenchSelect(parser.isSet(bench));
  win.show();
  trace::log("startup: window shown");
  QTimer::singleShot(0, &win, [&win] { win.warmUpViewport(); });  // GL init off the first-open path
  const QStringList args = parser.positionalArguments();
  if (!args.isEmpty()) win.openPath(args.first(), parser.isSet(readOnly));
  return app.exec();
}
