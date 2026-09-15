#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>

#include "MainWindow.hpp"
#include "opad/core.hpp"

int main(int argc, char** argv) {
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
  parser.process(app);

  MainWindow win;
  win.show();
  const QStringList args = parser.positionalArguments();
  if (!args.isEmpty()) win.openPath(args.first());
  return app.exec();
}
