#pragma once
// Design tokens from design_handoff_opad_desktop_ui/README.md, applied as QPalette + a generated stylesheet.
#include <QColor>
#include <QFont>
#include <QList>
#include <QObject>
#include <QString>

struct Tokens {
  bool dark = true;
  bool highContrast = false;  // the system's high-contrast colours (theme::highContrastTokens): selected text in onsel
  QColor bg, bg2, bg3, bg4, line, fg, fg2, fg3, vp, sel, selbg, hov, amber, green, red, mtop, mleft, mright, medge, cap, onsel;
  // Semantic roles (UI-120 e): what a colour means, the same in 3D, sketch and 2D, so new panels and overlays take a role
  // instead of a colour. Interaction: selected3d (hued, the selection in the view), hover (white glow), candidate (amber:
  // what a click would pick or a rule would add), ghost (translucent: inactive or deactivated). States: locked, error,
  // warning; a version diff (added, removed, modified, moved); a linked asset (in sync, stale, missing). A state is never
  // told by colour alone: theme::cue gives each its mark and label, and the diff and asset colours stay apart under
  // deuteranopia and protanopia (tests/test_ui_contracts).
  QColor selected3d, hover, candidate, ghost, locked, error, warning;
  QColor diffAdded, diffRemoved, diffModified, diffMoved, assetLinked, assetStale, assetMissing;
  // What a creation feature's preview does (an extrusion's operation): a new body (neutral), material joined to a body, the
  // volume a cut removes (drawn through the bodies it cuts), what an intersect keeps. Apart under deuteranopia too.
  QColor previewNew, previewJoin, previewCut, previewIntersect;
};

namespace theme {

// Emits changed() after apply(); widgets that bake token colours at construction re-apply them on it. refreshRequested():
// a setting apply() reads changed (contrast, text size); the window applies the theme again.
class Notifier : public QObject {
  Q_OBJECT
 signals:
  void changed();
  void refreshRequested();
};
Notifier* notifier();
void refresh();  // emits refreshRequested
const Tokens& current();
Tokens tokens(bool dark);
void apply(bool dark);  // sets the global tokens (high contrast when highContrast()), text size, palette, fonts and stylesheet

// High contrast (UI-124): the setting ui/contrast (0 as the system, 1 on, 2 off); the system's is Windows' High contrast
// (Qt's contrast preference). Its tokens are the system colours where the platform gives them (Windows: GetSysColor),
// else black, white and yellow.
bool systemHighContrast();
bool highContrast();
Tokens highContrastTokens();

// Text size (UI-124): the setting ui/textScale (percent, 0 or none: the system's, Windows' Accessibility › Text size),
// 1.0 to 2.25, read by apply(). The theme's fonts, the stylesheet's font sizes and the heights of its boxes that hold text
// grow with it, and so do the custom-painted rows (browser, timeline, ribbon tabs) through px().
double systemTextScale();
double textScale();
int px(int base);  // base logical pixels at the text size
QFont ui(int px = 13, int weight = QFont::Normal);  // at the text size
QFont mono(int px = 12);
QString css(const QColor& c);  // "#rrggbb" or "rgba(r,g,b,a)"
QString stylesheet(const Tokens& t);  // at textScale()
QString scaledSheet(const QString& sheet, double scale);  // font sizes and the heights of boxes holding text, times scale

// The non-colour cue of a state token: a mark drawn beside or on it ("+", "!") and its name (source text, shown through
// i18n::t). States: diffAdded, diffRemoved, diffModified, diffMoved, assetLinked, assetStale, assetMissing, locked, error,
// warning; null for anything else.
struct Cue {
  const char* state;
  QColor Tokens::* colour;
  const char* mark;   // UTF-8, one character
  const char* label;  // QT_TRANSLATE_NOOP("theme", ...)
};
const Cue* cue(const QString& state);
const QList<Cue>& cues();

// Colour vision deficiency, simulated (Machado, Oliveira and Fernandes 2009, severity 1), for checks of the tokens.
enum class Vision { Normal, Protanopia, Deuteranopia };
QColor simulate(const QColor& c, Vision v);
double deltaE(const QColor& a, const QColor& b);  // CIE76 in L*a*b* (D65): about 2 is just noticeable, 20 clearly apart
double contrast(const QColor& a, const QColor& b);  // WCAG 2 contrast ratio, 1 to 21 (text wants 4.5, high contrast 7)
}  // namespace theme
