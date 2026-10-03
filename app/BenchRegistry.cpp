#include "BenchRegistry.hpp"

#include <QMap>
#include <QtGlobal>

namespace {
// Function-local statics: the registrations are static initialisers in other files, in no particular order.
QMap<QString, bench::Handler>& handlers() {
  static QMap<QString, bench::Handler> map;
  return map;
}
QStringList& clashList() {
  static QStringList names;
  return names;
}
}  // namespace

namespace bench {

bool add(const char* variable, Handler handler) {
  const QString name = QString::fromLatin1(variable);
  if (handlers().contains(name)) clashList().append(name);
  else handlers().insert(name, handler);
  return true;
}

QStringList variables() { return handlers().keys(); }
QStringList clashes() { return clashList(); }

QString pending() {
  for (auto it = handlers().begin(); it != handlers().end(); ++it)
    if (!qEnvironmentVariableIsEmpty(it.key().toLatin1().constData())) return it.key();
  return {};
}

bool run(MainWindow& w) {
  const QString name = pending();
  return !name.isEmpty() && handlers().value(name)(w, qEnvironmentVariable(name.toLatin1().constData()));
}

}  // namespace bench
