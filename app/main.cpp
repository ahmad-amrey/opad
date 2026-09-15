#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QSurfaceFormat>

#include "CrashLog.hpp"
#include "MainWindow.hpp"
#include "opad/core.hpp"

int main(int argc, char** argv) {
  installCrashHandler();
  opad::configure_kernel_logging();
  QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QApplication app(argc, argv);
  QApplication::setApplicationName("OPAD");
  QApplication::setOrganizationName("opad");
  QApplication::setApplicationVersion(QString::fromStdString(opad::version_string()));

  QCommandLineParser parser;
  parser.setApplicationDescription("OPAD: git-native STEP viewer");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("file", "An .opad document or a .step file to browse");
  QCommandLineOption bench("bench-select", "Select every root once the file has loaded, log the timing (OPAD_TRACE) and quit");
  bench.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(bench);
  parser.process(app);

  MainWindow win;
  win.setBenchSelect(parser.isSet(bench));
  win.show();
  QTimer::singleShot(0, &win, [&win] { win.warmUpViewport(); });  // GL init off the first-open path
  const QStringList args = parser.positionalArguments();
  if (!args.isEmpty()) win.openPath(args.first());
  return app.exec();
}
