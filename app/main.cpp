#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileOpenEvent>
#include <QSettings>
#include <QTimer>
#include <QSurfaceFormat>

#include "CrashLog.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "opad/core.hpp"
#include "opad/drawing/paint.hpp"

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
}  // namespace

int main(int argc, char** argv) {
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
  trace::log("startup: application");

  QCommandLineParser parser;
  parser.setApplicationDescription("OPAD: CAD viewer and git-native parametric modeller");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("file", "An .opad document, or a STEP, IGES, STL, 3MF, OBJ, glTF, PLY, DXF, DWG or SVG file to view");
  QCommandLineOption bench("bench-select", "Select every root once the file has loaded, log the timing (OPAD_TRACE) and quit");
  bench.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(bench);
  parser.process(app);

  MainWindow win;
  app.installEventFilter(new FileOpenEvents(&win));
  trace::log("startup: window built");
  win.setBenchSelect(parser.isSet(bench));
  win.show();
  trace::log("startup: window shown");
  QTimer::singleShot(0, &win, [&win] { win.warmUpViewport(); });  // GL init off the first-open path
  const QStringList args = parser.positionalArguments();
  if (!args.isEmpty()) win.openPath(args.first());
  return app.exec();
}
