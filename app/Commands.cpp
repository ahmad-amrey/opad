#include "Commands.hpp"

#include <QAction>
#include <QCoreApplication>

void CommandRegistry::add(const CommandInfo& info, QAction* action) {
  if (m_index.contains(info.id)) {
    m_clashes << info.id;
    return;
  }
  m_index.insert(info.id, static_cast<int>(m_infos.size()));
  m_infos << info;
  CommandInfo& stored = m_infos.last();
  if (stored.group.isEmpty()) stored.group = commands::defaultGroup(info.id);
  m_actions << action;
  if (info.scope) shortcuts::setScope(info.id, *info.scope);
  if (action) {  // the palette shows the group (opGroup) and finds the command by its keywords too
    action->setProperty("commandGroup", stored.group);
    if (!info.keywords.isEmpty()) action->setProperty("commandKeywords", info.keywords);
  }
}

const CommandInfo* CommandRegistry::find(const QString& id) const {
  const auto it = m_index.find(id);
  return it == m_index.end() ? nullptr : &m_infos[*it];
}

QAction* CommandRegistry::action(const QString& id) const {
  const auto it = m_index.find(id);
  return it == m_index.end() ? nullptr : m_actions[*it];
}

QList<QAction*> CommandRegistry::actions() const { return m_actions; }

QStringList CommandRegistry::ids() const {
  QStringList out;
  for (const CommandInfo& c : m_infos) out << c.id;
  return out;
}

QStringList CommandRegistry::groups() const {
  QStringList out;
  for (const CommandInfo& c : m_infos)
    if (!out.contains(c.group)) out << c.group;
  return out;
}

QStringList CommandRegistry::inGroup(const QString& group) const {
  QStringList out;
  for (const CommandInfo& c : m_infos)
    if (c.group == group) out << c.id;
  return out;
}

QStringList CommandRegistry::inWorkspace(const QString& workspace) const {
  QStringList out;
  for (const CommandInfo& c : m_infos)
    if (c.workspaces.contains(workspace)) out << c.id;
  return out;
}

QStringList CommandRegistry::clashes() const { return m_clashes; }

QString CommandRegistry::helpId(const QString& id) const {
  const CommandInfo* c = find(id);
  return c && !c->helpId.isEmpty() ? c->helpId : id;
}

bool CommandRegistry::editsDocument(const QString& id) const {
  const CommandInfo* c = find(id);
  return c && c->editsDocument;
}

void CommandRegistry::addWorkspace(const QString& id, const QString& workspace) {
  const auto it = m_index.find(id);
  if (it != m_index.end() && !m_infos[*it].workspaces.contains(workspace)) m_infos[*it].workspaces << workspace;
}

void CommandRegistry::setMenuPath(const QString& id, const QString& path) {
  const auto it = m_index.find(id);
  if (it != m_index.end() && m_infos[*it].menuPath.isEmpty()) m_infos[*it].menuPath = path;
}

void CommandRegistry::updateEnabled(const CommandContext& context) const {
  for (int i = 0; i < m_infos.size(); ++i)
    if (m_infos[i].enabledWhen && m_actions[i]) m_actions[i]->setEnabled(m_infos[i].enabledWhen(context));
}

namespace commands {

QString defaultGroup(const QString& id) {
  static const char* const kContext = "CommandRegistry";
  const QString area = id.section('.', 0, 0);
  static const QList<QPair<QStringList, const char*>> named = {
      {{"file"}, QT_TRANSLATE_NOOP("CommandRegistry", "File")},
      {{"edit"}, QT_TRANSLATE_NOOP("CommandRegistry", "Edit")},
      {{"view"}, QT_TRANSLATE_NOOP("CommandRegistry", "View")},
      {{"nav"}, QT_TRANSLATE_NOOP("CommandRegistry", "Navigation")},
      {{"select"}, QT_TRANSLATE_NOOP("CommandRegistry", "Selection")},
      {{"inspect"}, QT_TRANSLATE_NOOP("CommandRegistry", "Inspect")},
      {{"annotate"}, QT_TRANSLATE_NOOP("CommandRegistry", "Annotations")},
      {{"design"}, QT_TRANSLATE_NOOP("CommandRegistry", "Design")},
      {{"sketch"}, QT_TRANSLATE_NOOP("CommandRegistry", "Sketch")},
      {{"panel", "workspace"}, QT_TRANSLATE_NOOP("CommandRegistry", "Panels and workspaces")},
      {{"tools"}, QT_TRANSLATE_NOOP("CommandRegistry", "Tools")},
      {{"help"}, QT_TRANSLATE_NOOP("CommandRegistry", "Help")},
  };
  for (const auto& [areas, name] : named)
    if (areas.contains(area)) return QCoreApplication::translate(kContext, name);
  return area.isEmpty() ? QString() : area.left(1).toUpper() + area.mid(1);  // an area's commands: it names its group
}

}  // namespace commands
