#pragma once
// The original OPAD icon set (design_handoff_opad_desktop_ui/opad-icons.js) rendered with QPainter.
// 24 px grid, 2 px stroke, round caps and joins, currentColor. No QtSvg dependency.
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <initializer_list>
#include <utility>

namespace icons {
// An area adds its icons from its own .cpp, without editing the built-in table in Icons.cpp:
//   OPAD_ICON_TABLE(git, {"branch", R"(<circle cx="6" cy="5" r="2"/>...)"}, {"push", R"(...)"});
// Rows are inner SVG markup on the 24 px grid (path, rect, circle, ellipse), as in the built-in table. A name that is
// already taken keeps its first markup; taken with other markup, it is listed by clashes() (the seams bench fails).
struct IconTable {
  IconTable(std::initializer_list<std::pair<const char*, const char*>> rows);
};
QStringList clashes();
bool has(const QString& name);
QPixmap pixmap(const QString& name, const QColor& color, int size = 24, qreal dpr = 1.0);
QIcon icon(const QString& name, const QColor& normal, const QColor& disabled = QColor(), const QColor& selected = QColor(), int size = 24);
// fg for normal, fg3 for disabled, onsel for selected, using the current theme tokens.
QIcon themed(const QString& name, int size = 24);
// The app logo's cube mark, never the wordmark (res/opad-<n>.png, cut from the logo by tools/make_icon.py).
QIcon appIcon();
void clearCache();
// Writes the icon as a PNG under the user cache dir and returns a path usable in a stylesheet url().
QString file(const QString& name, const QColor& color, int size = 16);
// Splitter grip (handoff: centred 2x16 dotted line in fg3), as a PNG for the stylesheet; vertical = for an upright bar.
QString gripFile(const QColor& color, bool vertical);
}  // namespace icons

#define OPAD_ICON_TABLE(area, ...) [[maybe_unused]] static const icons::IconTable opad_icon_table_##area{__VA_ARGS__}
