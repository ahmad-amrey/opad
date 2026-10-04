#pragma once
// Help for every command (UI-106). app/help/commands.json holds the English records, one per command id (the QAction's
// objectName: "design.extrude", "sketch.c.horizontal", "view.fit"); app/help/commands.<language>.json translates them
// by id, field by field. Both are embedded (help.qrc); <app dir>/help/<same name> is laid over them, like the i18n
// files. A record: title, a one-line summary, 2-4 sentences of details, search keywords, "requires" (what a disabled
// command needs; {name} placeholders are filled by the caller) and the id of its clip (UI-107; the command id when
// not given). tools/i18n_check.py and tests/test_help.cpp fail on a record without its translation.
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QAction;

struct CommandHelp {
  QString id, title, summary, details, requirement, clip;
  QStringList keywords;  // the translation's and the English ones: search finds either
  bool translated = true;  // every field the English record has is in the current language (always for English)
};

namespace help {
// The record in the current language (i18n::current(), loaded on first use), English for any field not translated.
const CommandHelp* find(const QString& id);
const QList<CommandHelp>& all();
// Reloads in `language` ("en", "ar"); `dir` replaces the built-in ":/help" (tests read the source tree). The help area's
// own tr() strings and clip texts are the fragment app/i18n/<language>/help.json, merged by i18n::install.
void load(const QString& language, const QString& dir = QString());
QString language();
// The "requires" text with {name} replaced from args (unknown names stay as written), then expand()ed.
QString requirement(const CommandHelp& h, const QVariantMap& args = {});
// Key tokens in a help text, after its translation (records, clip texts, lessons; translations keep them verbatim),
// replaced with the keys the user has now (keys::text, TODO 11 wave 3):
//   {key:<command id>}   the command's key; unassigned: an enclosing "(...)" goes with the space before it, a bare token
//                        becomes the command's title. Write "Pin ({key:inspect.pin}) keeps..." so both read well.
//   {press:<command id>} "Press <key>", unassigned "Choose <title>"
//   {fixed:<name>}       a key every tool knows (keys::fixedNames: esc, enter, ctrlEnter, ...), named for the platform
// `allBound` (optional) is cleared when a {key} or {press} names a command without a key (a clip step's captionNoKey).
QString expand(const QString& text, bool* allBound = nullptr);
// The tokens of a text, in order: "key:view.fit", "press:tools.commands", "fixed:enter" (i18n parity, tests).
QStringList tokens(const QString& text);
// A command's name: its record's title in the current language, else its action's text, else the id.
QString title(const QString& id);
// Search for the command palette: every word of the query in the title, keywords or summary, or the query is the
// command's key now ("ctrl+alt+f", as shown or as Qt writes it).
bool matches(const CommandHelp& h, const QString& query);
// Plain tooltip for an action (screen readers, Settings > tips Basic): "<qt><b>Title</b> (key now)<br>summary</qt>".
QString tooltip(const QAction* a);
// The raw JSON field names a record is checked on (title, summary, details, requires, keywords).
QStringList textFields();
// The area a command is listed under (the palette's group column, the reference's headings), translated; areas() lists
// them in the reference's order.
QString group(const QString& id);
QStringList areas();
}  // namespace help
