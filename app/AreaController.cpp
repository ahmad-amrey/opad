#include "AreaController.hpp"

namespace {
// Function-local statics: the registrations are static initialisers in other files, in no particular order.
QMap<QString, areas::Factory>& factories() {
  static QMap<QString, areas::Factory> map;
  return map;
}
QStringList& clashList() {
  static QStringList names;
  return names;
}
}  // namespace

namespace areas {

bool add(const char* name, Factory factory) {
  const QString key = QString::fromLatin1(name);
  if (factories().contains(key)) clashList().append(key);
  else factories().insert(key, factory);
  return true;
}

QStringList names() { return factories().keys(); }
QStringList clashes() { return clashList(); }

std::vector<AreaController*> create(AreaServices& services) {
  std::vector<AreaController*> out;
  for (auto it = factories().begin(); it != factories().end(); ++it)
    if (AreaController* area = it.value()(services)) {
      area->setObjectName(it.key());
      out.push_back(area);
    }
  return out;
}

}  // namespace areas
