#pragma once
// The command registry (UI-120 a): one record per command, beside the QAction the window makes from it. Every command of
// the window is registered as it is made (MainWindow::addCommand; the old addAction(id, text, ...) fills a record from the
// id), so menus, ribbon, palette, shortcut editor and help can ask one table instead of keeping their own lists. A feature
// area registers its commands the same way, with the whole record (AreaServices::addCommand): its help id, workspaces and
// editsDocument come with it, so a new edit command asks to save a viewed file first without a list to extend.
//   CommandInfo info{"assets.sync", label, "regen"};  // label, group: translated text
//   info.group = group; info.editsDocument = true; info.keywords = {"reload", "update"};
//   info.enabledWhen = [](const CommandContext& c) { return c.document && !c.selection.empty(); };
//   services().addCommand(info, [this] { sync(); });
// What the table fills in itself: the menu path when the menus are built, and the workspaces of the ribbon tabs the
// command is placed on.
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "AreaController.hpp"
#include "ShortcutEditor.hpp"

class QAction;

// What enabledWhen sees: the window's state when commands are re-checked (document open or replaced, selection moved,
// workspace or sketch mode changed).
struct CommandContext {
  bool document = false;   // a document is open
  bool viewer = false;     // it is viewed read-only (viewer mode)
  bool sketching = false;  // a sketch is open
  QString workspace;       // the one shown: "review", "design", "sketch" or an area's
  SelectionContext selection;
};

struct CommandInfo {
  QString id;     // "<area>.<name>": the QAction's objectName and the key of its shortcut setting, help record and palette entry
  QString label;  // menu text (tr), '&' marks a mnemonic
  QString icon;   // icons:: name; empty: none
  QKeySequence key;  // the default shortcut; the user's own binding (setting shortcuts/<id>) wins
  std::optional<shortcuts::Scope> scope;  // where the key works; unset: from the id (shortcuts::scope)
  QString group;       // palette and shortcut-editor group, translated; empty: from the id's area (commands::defaultGroup)
  QString menuPath;    // the menu bar's path to it by menu ids, "design/create"; empty: filled in when the menus are built
  QStringList keywords;    // more words the palette finds it by: "push pull", "offset face"
  QString helpId;          // its help record (UI-106); empty: the id
  QStringList workspaces;  // the ribbon workspaces it belongs to; the ones it is placed in are added as the ribbon is built
  bool checkable = false;
  bool fixedKey = false;  // its key is not the user's to change (Esc): the shortcut editor lists it as reserved
  bool editsDocument = false;  // changes the document: in viewer mode it asks to save as OPAD first, then runs (requireEditable)
  std::function<bool(const CommandContext&)> enabledWhen;  // null: the window enables it (most built-in commands)
};

class CommandRegistry {
 public:
  // Records a command and its action. An id that is there already keeps its first record (clashes() names it).
  void add(const CommandInfo& info, QAction* action);
  const CommandInfo* find(const QString& id) const;
  QAction* action(const QString& id) const;  // null: no such command
  QList<QAction*> actions() const;           // in the order they were added (the palette's order)
  QStringList ids() const;
  QStringList groups() const;                         // in the order of their first command
  QStringList inGroup(const QString& group) const;    // the ids of a group, in order
  QStringList inWorkspace(const QString& workspace) const;  // the ids that belong to a workspace, in order
  QStringList clashes() const;                        // ids added twice
  QString helpId(const QString& id) const;            // its help record: helpId, else the id
  bool editsDocument(const QString& id) const;
  void addWorkspace(const QString& id, const QString& workspace);  // placed on a tab of that workspace
  void setMenuPath(const QString& id, const QString& path);       // where the menu bar shows it (the first place wins)
  // Enables the commands that have an enabledWhen by it; the others are left as the window set them.
  void updateEnabled(const CommandContext& context) const;

 private:
  QList<CommandInfo> m_infos;
  QList<QAction*> m_actions;
  QHash<QString, int> m_index;
  QStringList m_clashes;
};

namespace commands {
QString defaultGroup(const QString& id);  // by the id's area: "design.extrude" -> "Design" (translated)
}  // namespace commands
