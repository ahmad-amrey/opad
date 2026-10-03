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
// Reloads in `language` ("en", "ar"); `dir` replaces the built-in ":/help" (tests read the source tree). Also installs
// the help area's own tr() strings (app/i18n/<language>/help.json) that no translator knows yet (until UI-119 merges).
void load(const QString& language, const QString& dir = QString());
QString language();
// The "requires" text with {name} replaced from args (unknown names stay as written).
QString requirement(const CommandHelp& h, const QVariantMap& args = {});
// Search for the command palette: every word of the query in the title, keywords or summary.
bool matches(const CommandHelp& h, const QString& query);
// Plain tooltip for an action (screen readers, Settings > tips Basic): "<qt><b>Title</b> (key)<br>summary</qt>".
QString tooltip(const QAction* a);
// The raw JSON field names a record is checked on (title, summary, details, requires, keywords).
QStringList textFields();
// The area a command is listed under (the palette's group column, the reference's headings), translated; areas() lists
// them in the reference's order.
QString group(const QString& id);
QStringList areas();
}  // namespace help
