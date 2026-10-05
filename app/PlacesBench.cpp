// Where every command lives (UI-102/104): each one of the window and of every area has a place the user can find it by,
// a ribbon tab of a workspace or a menu of the menu bar, besides the context menus, the command search and its keys.
// Case in tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"

// OPAD_BENCH_PLACES=1 | <file.json> on an editable document: every command has a ribbon workspace or a menu path (the
// few that belong to one context menu or panel are named below with where they are); a file name also gets the table
// (id, label, group, menu path, workspaces, help id) written to it.
OPAD_BENCH(OPAD_BENCH_PLACES, places) {
  const CommandRegistry& registry = w.m_commands;
  // Commands whose one place is a context menu, a panel or the view: never in the ribbon or the menu bar on purpose.
  static const QHash<QString, QString> elsewhere{
      {"tools.settings", "the gear button of the tab row"},
      {"design.interference", "Inspect > Interference: Keep as check (one Interference command)"},
      {"tools.author", "Preferences > General"},
      {"tools.undodepth", "Preferences > General"},
      {"files.useOda", "Preferences > Files"},
      {"assets.kicadSettings", "Preferences > Files"},
      {"view.cubeEdgesCorners", "Preferences > Keyboard and mouse"},
  };
  QHash<QString, QString> other = elsewhere;
  for (int n = 1; n <= 9; ++n)  // keys of the list's entries: the entry itself is their place, and it shows the key
    other.insert(QString("view.named%1").arg(n), "View > Named views, the n-th entry");
  QStringList nowhere;
  QJsonArray table;
  for (const QString& id : registry.ids()) {
    const CommandInfo* c = registry.find(id);
    QAction* a = registry.action(id);
    if (c->workspaces.isEmpty() && c->menuPath.isEmpty() && !other.contains(id)) nowhere << id;
    QString label = a ? a->text() : c->label;
    QJsonObject row{{"id", id},
                    {"label", label.remove('&')},
                    {"group", c->group},
                    {"menu", c->menuPath},
                    {"workspaces", QJsonArray::fromStringList(c->workspaces)},
                    {"help", registry.helpId(id)},
                    {"edits", c->editsDocument}};
    if (a && !a->shortcut().isEmpty()) row["key"] = a->shortcut().toString(QKeySequence::PortableText);
    table.append(row);
  }
  if (value != "1") {
    QFile file(value);
    if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(table).toJson());
  }
  // OPAD_BENCH_UISHOT=<prefix>: the window in each workspace, <prefix>.<workspace>.png (the ribbon bench has every tab).
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) {
    const QString start = w.workspaceId();
    for (const QString& id : w.m_workspaceIds) {
      w.setWorkspace(id);
      QCoreApplication::processEvents();
      w.grab().save(QString("%1.%2.png").arg(shot, id));
    }
    w.setWorkspace(start);
  }
  const bool ok = nowhere.isEmpty() && registry.ids().size() > 300;
  trace::log(QString("bench: places: %1 commands, each on a ribbon tab or in a menu%2 %3")
                 .arg(registry.ids().size())
                 .arg(nowhere.isEmpty() ? QString() : ": nowhere " + nowhere.join(", "))
                 .arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
