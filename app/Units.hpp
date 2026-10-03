#pragma once
// Units and display precision (UI-123). OPAD keeps millimetres (mm², mm³), degrees for measured angles and grams; what
// the app shows, and what its value boxes read, follows the document's length unit (its `units` op: mm, cm, m, um, in,
// ft; a viewer-mode file may be shown in another for the session) and the display precision (settings units/decimals,
// units/angle = deg | rad, units/fraction = 0 or the denominator of fractional inches). Every length, angle, area,
// volume and mass on screen goes through format(); notifier() says when any of it changed, so views redraw their text.
#include <QObject>
#include <QString>
#include <QStringList>
#include <array>
#include <optional>
#include <string>

namespace units {
enum class Kind { Length, Angle, Area, Volume, Mass };

struct Display {
  std::string length = "mm";  // one of lengthUnits()
  int decimals = 3;
  bool radians = false;
  int fraction = 0;  // inches as whole and n/fraction (reduced); 0 = decimals
  bool operator==(const Display&) const = default;
};

class Notifier : public QObject {
  Q_OBJECT
 signals:
  void changed();
};
Notifier* notifier();

const Display& current();
void setDocumentUnit(const std::string& unit);  // the document's units op (MainWindow, on every document change)
const std::string& documentUnit();
void setSessionUnit(const std::string& unit);  // viewer mode: shown in this unit without an op; "" = the document's
const std::string& sessionUnit();
void setPrecision(int decimals, bool radians, int fraction);  // saved (QSettings units/...)
void loadSettings();

const QStringList& lengthUnits();  // as the units op takes them
double mmPer(const std::string& unit);  // 25.4 for "in"; 0 for an unknown unit
QString unitName(const std::string& unit);  // "Inches"
QString symbol(Kind kind, const Display& d = current());  // "mm", "in²", "°", "rad", "g" or "lb"
double toDisplay(Kind kind, double value, const Display& d = current());  // value in OPAD's units (mm, deg, g)
double fromDisplay(Kind kind, double shown, const Display& d = current());
// The number alone (decimals < 0: the setting; a -0 shows as 0) and with its symbol ("1.181 in", "1 3/16 in", "45.00°").
QString number(Kind kind, double value, int decimals = -1, const Display& d = current());
QString format(Kind kind, double value, int decimals = -1, const Display& d = current());
QString vector(Kind kind, const std::array<double, 3>& v, int decimals = -1, const Display& d = current());  // "(1, 2, 3) in"
// Labels on the model (sketch dimensions): the precision's decimals without trailing zeros ("1 in", "25.4 mm", "45°").
QString compact(Kind kind, double value, const Display& d = current());
// What a value box starts from: an expression the design parser reads back, decimals enough for 0.0001 mm or 0.0001°,
// no trailing zeros, the raw unit name ("1.181102 in", "250 um", "45 deg", "0.785398 rad").
QString editable(Kind kind, double value, const Display& d = current());
// A default amount offered in the shown unit, as an expression: exact when it takes two significant digits there ("4 cm"),
// else the nearest 1, 2, 2.5 or 5 times a power of ten ("0.5 in" for 10 mm). presetText: `text` itself unless it is a
// plain "<number> mm" (how the defaults are written), else its preset. Millimetres stay as written.
QString preset(double mm, const Display& d = current());
QString presetText(const QString& text, const Display& d = current());
// Decimals a value box needs to show a length step of `mm` in the shown unit (0.01 mm: 2 in mm, 4 in inches).
int decimalsFor(double mm, const Display& d = current());
// A typed value in OPAD's units. A bare number is in the display unit; "3 mm", "1/2 in", "1 1/2\"", "2'", "30°", "0.5 rad"
// and arithmetic ("20 mm + 1 in", through the design expression parser) work too. nullopt: not a value of this kind.
std::optional<double> parse(Kind kind, const QString& text, const Display& d = current());
}  // namespace units
