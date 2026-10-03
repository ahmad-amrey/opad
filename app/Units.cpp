#include "Units.hpp"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "opad/design/expr.hpp"

namespace units {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kPound = 453.59237;  // g

struct State {
  Display display;
  std::string document = "mm", session;
};
State& state() {
  static State s;
  return s;
}
bool imperial(const Display& d) { return d.length == "in" || d.length == "ft"; }
// OPAD's units per shown unit.
double scale(Kind kind, const Display& d) {
  const double mm = mmPer(d.length) > 0 ? mmPer(d.length) : 1.0;
  switch (kind) {
    case Kind::Length: return mm;
    case Kind::Area: return mm * mm;
    case Kind::Volume: return mm * mm * mm;
    case Kind::Angle: return d.radians ? 180.0 / kPi : 1.0;
    case Kind::Mass: return imperial(d) ? kPound : 1.0;
  }
  return 1.0;
}
QString fixed(double v, int decimals) { return QString::number(std::abs(v) < 0.5 * std::pow(10.0, -decimals) ? 0.0 : v, 'f', decimals); }
QString trimmed(QString s) {
  if (s.contains('.')) {
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
  }
  return s;
}
QString labelled(Kind kind, double value, const QString& n, const Display& d) {
  if (kind == Kind::Angle && !d.radians) return n + symbol(kind, d);
  if (kind == Kind::Mass && !imperial(d) && std::abs(value) >= 1000) return n + QStringLiteral(" kg");
  return n + ' ' + symbol(kind, d);
}
QString lengthSymbol(const std::string& unit) { return unit == "um" ? QString::fromUtf8("µm") : QString::fromStdString(unit); }
// The length unit shown: the session's (viewer mode) over the document's. Tells everyone when the display changed.
void settle(const Display& before) {
  State& s = state();
  s.display.length = s.session.empty() ? s.document : s.session;
  if (!(before == s.display)) emit notifier()->changed();
}
// "1 3/16" and "3/16" as decimals, so the expression parser reads "1 3/16 in" as one length.
QString decimalFractions(QString t) {
  static const QRegularExpression fraction(R"((?<![\d.])(?:(\d+)\s+)?(\d+)\s*/\s*(\d+)(?![\d.]))");
  QString out;
  qsizetype at = 0;
  for (auto it = fraction.globalMatch(t); it.hasNext();) {
    const auto m = it.next();
    const double den = m.captured(3).toDouble();
    if (den == 0) continue;
    out += t.mid(at, m.capturedStart() - at) + QString::number(m.captured(1).toDouble() + m.captured(2).toDouble() / den, 'g', 15);
    at = m.capturedEnd();
  }
  return out + t.mid(at);
}
}  // namespace

Notifier* notifier() {
  static Notifier* n = new Notifier;
  return n;
}

const Display& current() { return state().display; }
const std::string& documentUnit() { return state().document; }
const std::string& sessionUnit() { return state().session; }

void setDocumentUnit(const std::string& unit) {
  const Display before = current();
  state().document = mmPer(unit) > 0 ? unit : "mm";
  settle(before);
}

void setSessionUnit(const std::string& unit) {
  const Display before = current();
  state().session = mmPer(unit) > 0 ? unit : std::string();
  settle(before);
}

void setPrecision(int decimals, bool radians, int fraction) {
  const Display before = current();
  Display& d = state().display;
  d.decimals = std::clamp(decimals, 0, 8);
  d.radians = radians;
  d.fraction = fraction >= 2 && fraction <= 128 && (fraction & (fraction - 1)) == 0 ? fraction : 0;
  QSettings settings;
  settings.setValue("units/decimals", d.decimals);
  settings.setValue("units/angle", d.radians ? "rad" : "deg");
  settings.setValue("units/fraction", d.fraction);
  settle(before);
}

void loadSettings() {
  const Display before = current();
  const QSettings settings;
  Display& d = state().display;
  d.decimals = std::clamp(settings.value("units/decimals", 3).toInt(), 0, 8);
  d.radians = settings.value("units/angle", "deg").toString() == "rad";
  const int fraction = settings.value("units/fraction", 0).toInt();
  d.fraction = fraction >= 2 && fraction <= 128 && (fraction & (fraction - 1)) == 0 ? fraction : 0;
  settle(before);
}

const QStringList& lengthUnits() {
  static const QStringList u = {"mm", "cm", "m", "um", "in", "ft"};
  return u;
}

double mmPer(const std::string& unit) {
  if (unit == "mm") return 1;
  if (unit == "cm") return 10;
  if (unit == "m") return 1000;
  if (unit == "um") return 0.001;
  if (unit == "in") return 25.4;
  if (unit == "ft") return 304.8;
  return 0;
}

QString unitName(const std::string& unit) {
  if (unit == "mm") return QCoreApplication::translate("units", "Millimetres");
  if (unit == "cm") return QCoreApplication::translate("units", "Centimetres");
  if (unit == "m") return QCoreApplication::translate("units", "Metres");
  if (unit == "um") return QCoreApplication::translate("units", "Micrometres");
  if (unit == "in") return QCoreApplication::translate("units", "Inches");
  if (unit == "ft") return QCoreApplication::translate("units", "Feet");
  return QString::fromStdString(unit);
}

QString symbol(Kind kind, const Display& d) {
  switch (kind) {
    case Kind::Length: return lengthSymbol(d.length);
    case Kind::Area: return lengthSymbol(d.length) + QString::fromUtf8("²");
    case Kind::Volume: return lengthSymbol(d.length) + QString::fromUtf8("³");
    case Kind::Angle: return d.radians ? QStringLiteral("rad") : QString::fromUtf8("°");
    case Kind::Mass: return imperial(d) ? QStringLiteral("lb") : QStringLiteral("g");
  }
  return {};
}

double toDisplay(Kind kind, double value, const Display& d) { return value / scale(kind, d); }
double fromDisplay(Kind kind, double shown, const Display& d) { return shown * scale(kind, d); }

QString number(Kind kind, double value, int decimals, const Display& d) {
  if (decimals < 0) decimals = d.decimals;
  if (kind == Kind::Length && d.length == "in" && d.fraction > 0) {  // to the nearest 1/fraction, reduced
    const long long n = std::llround(std::abs(value) / 25.4 * d.fraction);
    long long whole = n / d.fraction, rest = n % d.fraction, den = d.fraction;
    const long long g = std::gcd(rest, den);
    if (g > 1) rest /= g, den /= g;
    QString s = value < 0 && n ? QStringLiteral("-") : QString();
    if (whole || !rest) s += QString::number(whole);
    if (rest) s += (whole ? QStringLiteral(" ") : QString()) + QString("%1/%2").arg(rest).arg(den);
    return s;
  }
  double shown = toDisplay(kind, value, d);
  if (kind == Kind::Mass && !imperial(d) && std::abs(shown) >= 1000) shown /= 1000;
  return fixed(shown, decimals);
}

QString format(Kind kind, double value, int decimals, const Display& d) { return labelled(kind, value, number(kind, value, decimals, d), d); }

QString vector(Kind kind, const std::array<double, 3>& v, int decimals, const Display& d) {
  return QString("(%1, %2, %3) %4").arg(number(kind, v[0], decimals, d), number(kind, v[1], decimals, d), number(kind, v[2], decimals, d), symbol(kind, d));
}

QString compact(Kind kind, double value, const Display& d) { return labelled(kind, value, trimmed(number(kind, value, -1, d)), d); }

QString editable(Kind kind, double value, const Display& d) {
  if (kind == Kind::Length) return trimmed(fixed(toDisplay(kind, value, d), decimalsFor(1e-4, d))) + ' ' + QString::fromStdString(mmPer(d.length) > 0 ? d.length : "mm");
  if (kind == Kind::Angle) return d.radians ? trimmed(fixed(value * kPi / 180, 6)) + QStringLiteral(" rad") : trimmed(fixed(value, 4)) + QStringLiteral(" deg");
  return labelled(kind, value, trimmed(number(kind, value, 6, d)), d);
}

QString preset(double mm, const Display& d) {
  const std::string unit = mmPer(d.length) > 0 ? d.length : "mm";
  if (unit == "mm") return QString::number(mm, 'g', 12) + QStringLiteral(" mm");
  double v = mm / mmPer(unit);
  int power = 0;
  if (v != 0) {
    power = int(std::floor(std::log10(std::abs(v))));
    const double tenths = std::abs(v) / std::pow(10.0, power - 1);  // two significant digits: a whole number of these
    if (std::abs(tenths - std::round(tenths)) > 1e-6 * tenths) {
      const double mantissa = std::abs(v) / std::pow(10.0, power);
      double best = 1;
      for (const double step : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (std::abs(std::log(mantissa / step)) < std::abs(std::log(mantissa / best))) best = step;
      v = std::copysign(best * std::pow(10.0, power), v);
    }
  }
  return trimmed(fixed(v, std::max(0, 2 - power))) + ' ' + QString::fromStdString(unit);
}

QString presetText(const QString& text, const Display& d) {
  static const QRegularExpression plain(R"(^\s*([-+]?(?:\d+\.?\d*|\.\d+))\s*mm\s*$)");
  if (d.length == "mm") return text;
  const auto m = plain.match(text);
  return m.hasMatch() ? preset(m.captured(1).toDouble(), d) : text;
}

int decimalsFor(double mm, const Display& d) {
  return std::clamp(int(std::ceil(-std::log10(toDisplay(Kind::Length, mm, d)) - 1e-9)), 0, 8);
}

std::optional<double> parse(Kind kind, const QString& text, const Display& d) {
  QString t = text.trimmed();
  if (t.isEmpty()) return std::nullopt;
  t.replace(QString::fromUtf8("µ"), "u").replace(QString::fromUtf8("μ"), "u");
  t.replace(QString::fromUtf8("″"), " in").replace('"', " in").replace(QString::fromUtf8("′"), " ft").replace('\'', " ft");
  t.replace(QString::fromUtf8("°"), " deg");
  t = decimalFractions(t);
  try {
    if (kind == Kind::Length) return opad::design::ParamTable({}, d.length).length(t.toStdString());
    if (kind == Kind::Angle) {
      static const QRegularExpression letters("[A-Za-z]");
      if (d.radians && !t.contains(letters)) t += " rad";
      return opad::design::ParamTable({}, d.length).angle(t.toStdString()) * 180.0 / kPi;
    }
    // Area, volume, mass: a number, with this display's symbol or none.
    double factor = 1;
    for (const QString& suffix : {QStringLiteral("kg"), symbol(kind, d), QString::fromStdString(d.length) + (kind == Kind::Area ? "^2" : "^3")}) {
      if (!t.endsWith(suffix)) continue;
      if (suffix == "kg") {
        if (kind != Kind::Mass || imperial(d)) return std::nullopt;
        factor = 1000;
      }
      t.chop(suffix.size());
      break;
    }
    bool ok = false;
    const double v = t.trimmed().toDouble(&ok);
    if (!ok) return std::nullopt;
    return fromDisplay(kind, v * factor, d);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
}  // namespace units
