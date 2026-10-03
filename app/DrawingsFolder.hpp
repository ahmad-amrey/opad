#pragma once
// The browser's Drawings folder (TODO 11 UI-76): the document's drawings, sheets, views and their dimensions and notes,
// as drawing::outline lists them. Rows are selected like nodes; their ids are the records' op ids (a drawing's is
// "drawing:<name>"). Drawings, sheets and views rename in place (F2); Del and the context menu tombstone what is
// selected in one step (a sheet takes its views and items, a base view the views projected from it; Ctrl+Z brings them
// back). These are definitions only: nothing here projects or measures, so every call is O(records).
#include <QString>
#include <functional>
#include <string>
#include <vector>

class AppDocument;
class QMenu;

namespace drawings {
struct Row {  // a browser row, as browser::Item has it in the folder providers
  std::string id;
  QString name, icon, tooltip;
  bool error = false;     // not drawn (a kind of a newer OPAD, a missing parent, ...): the tooltip says why
  bool editable = false;  // renamed in place
  std::vector<Row> children;
};
std::vector<Row> rows(const AppDocument& doc);       // empty: no folder
void rename(AppDocument* doc, const std::string& id, const QString& name);  // throws opad::Error
// Tombstones those of `ids` that are drawings or drawing records, in one undo step; what goes with a sheet or a view
// that goes too is left to it. Returns how many records it deleted.
int remove(AppDocument* doc, const std::vector<std::string>& ids);
// Right-click on a row: rename (startRename puts its editor up), copy its id (for the CLI and agents), export a sheet
// (exportSheet, when given: PDF, SVG, DXF, DWG or PNG of it), delete.
void contextMenu(AppDocument* doc, const std::string& id, QMenu& menu, const std::function<void()>& startRename,
                 const std::function<void()>& exportSheet = {});
}  // namespace drawings
