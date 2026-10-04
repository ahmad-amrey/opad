#pragma once
// The one key formatter (TODO 11 wave 3, help audit §4.3): every place that shows a command's key asks here for the
// key the user has bound now (the shortcut editor's, held while a sketch is open too), never for a default or a literal.
// Key caps are built from QKeyCombination (never by splitting text on '+'), with one naming table: Return and Enter ->
// Enter, Escape -> Esc, Delete -> Del, PageDown -> PgDn, arrows -> ↑↓←→, modifiers in Qt's order; the macOS style
// names them with glyphs (⌃⌥⇧⌘ ↩ ⌫ ⌦), and a Style parameter lets the tests check that table on any system. Inline key
// text is wrapped in LRI..PDI so it keeps its order inside right-to-left sentences. The fixed keys every tool knows
// (Esc, Enter, Tab, Shift, ...) are named here too, so macOS shows them its way. notifier() says when a binding changed
// (the shortcut editor applied): surfaces that keep key text refresh on it.
//   keys::text("view.fit")        -> "⁦Ctrl+Alt+F⁩" after a remap, "" when unassigned
//   keys::caps(QKeySequence("Ctrl+Shift+F"), keys::Style::Mac) -> ⇧, ⌘, F
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QAction;

namespace keys {
enum class Style { Native, Pc, Mac };  // Native: Mac on macOS, Pc elsewhere

class Notifier : public QObject {
  Q_OBJECT
 signals:
  void changed();  // a command's key changed: key text shown anywhere may be stale
};
Notifier* notifier();
void announce();  // emits changed(): the shortcut editor applied, shortcuts::bind

// The window's commands by id (MainWindow::action), installed by the help area as RichTip's lookup is.
void setLookup(std::function<QAction*(const QString&)> lookup);
bool hasLookup();
QAction* action(const QString& id);  // null: no lookup or no such command
QKeySequence binding(const QString& id);  // shortcuts::binding (held during a sketch too); empty: unassigned or unknown
QKeySequence binding(const QAction* a);
QList<QKeySequence> bindings(const QAction* a);  // the key, then its alternates (Redo's Ctrl+Shift+Z)
// A key someone can press: not empty and no Key_Exit (Qt's standard Quit on Windows), which no keyboard has; the
// bindings above leave such keys out, so Quit shows no key rather than "Exit".
bool pressable(const QKeySequence& key);

inline const QString kThen = QStringLiteral(",");  // between the chords of a multi-chord sequence in caps()
// One cap per modifier and key, every chord in order with kThen between chords; empty for the empty sequence.
QStringList caps(const QKeySequence& key, Style style = Style::Native);
QString joined(const QStringList& caps, Style style = Style::Native);  // '+' between caps (none on macOS), ", " at kThen
QString plain(const QKeySequence& key, Style style = Style::Native);   // joined(caps(key)): search, comparisons
QString isolate(const QString& text);                                 // LRI text PDI; empty stays empty
QString text(const QKeySequence& key, Style style = Style::Native);   // isolate(plain(key))
QString text(const QString& id, Style style = Style::Native);         // the command's key now; "" when unassigned
// The keys every tool knows, by name: esc, enter, ctrlEnter, tab, shiftTab, shift, alt, ctrl, del, shiftDel, backspace,
// space, f2 (the browser's and the timeline's own rename key), undo, redo, copy (the last three: the platform's standard
// key). Empty for an unknown name.
QStringList fixedNames();
QStringList fixedCaps(const QString& name, Style style = Style::Native);
QString fixedText(const QString& name, Style style = Style::Native);  // isolated
// "<key> <verb>" for a hint list ("P pin"), "" when the command has no key (the list leaves the entry out).
QString hint(const QString& id, const QString& verb);
// Several commands' keys as one entry of a hint ("1–4 filter"): "1–4" when they are single consecutive characters, else
// the bound ones joined by "/" ("1/Ctrl+Alt+2/3/4"); isolated; "" when none has a key.
QString span(const QStringList& ids);
// A key given as text, a fixed key's name ("esc", fixedNames) or a command id ("inspect.pin"): what it reads now, without
// isolates (a label of its own: PanelFooter). A command without a key: "".
QString spec(const QString& keyOrId);
bool isCommandId(const QString& text);  // "inspect.pin", not "Alt+Left" or "Esc"
}  // namespace keys
