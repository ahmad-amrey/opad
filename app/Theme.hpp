#pragma once
// Design tokens from design_handoff_opad_desktop_ui/README.md, applied as QPalette + a generated stylesheet.
#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>

struct Tokens {
  bool dark = true;
  QColor bg, bg2, bg3, bg4, line, fg, fg2, fg3, vp, sel, selbg, hov, amber, green, red, mtop, mleft, mright, medge, cap, onsel;
};

namespace theme {

// Emits changed() after apply(); widgets that bake token colours at construction re-apply them on it.
class Notifier : public QObject {
  Q_OBJECT
 signals:
  void changed();
};
Notifier* notifier();
const Tokens& current();
Tokens tokens(bool dark);
void apply(bool dark);  // sets the global tokens, palette, fonts and stylesheet
QFont ui(int px = 13, int weight = QFont::Normal);
QFont mono(int px = 12);
QString css(const QColor& c);  // "#rrggbb" or "rgba(r,g,b,a)"
QString stylesheet(const Tokens& t);
}  // namespace theme
