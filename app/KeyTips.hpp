#pragma once
// KeyTips (UI-117, UI-124): tap Alt and every ribbon tab, the workspace chip and the tab row's buttons show a key; a
// tab's key opens it and shows the keys of its tools, a tool's key runs it; Esc or Backspace goes back a level, a click,
// another Alt tap or a key no tip has (arrows, Tab) leave. The menu bar keeps its own Alt+letter mnemonics.
#include <QSet>
#include <QString>
#include <QStringList>

namespace keytips {
// One key per label, in order, never one that is in `taken` or a prefix of another: the first letter of the label's words
// that is free, then its other letters, then any free letter or digit; past 36 labels the rest get two keys after a
// letter kept for them ("Z" then "ZA", "ZB" ...). Upper case, A-Z and 0-9 only (an Arabic label takes a free one).
QStringList assign(const QStringList& labels, const QSet<QString>& taken = {});
}  // namespace keytips
