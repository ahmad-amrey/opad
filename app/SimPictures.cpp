#include "SimPictures.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

#include <algorithm>
#include <map>

#include "Icons.hpp"
#include "SimChecks.hpp"
#include "Theme.hpp"

namespace {

class SimPicturesText {
  Q_DECLARE_TR_FUNCTIONS(SimPicturesText)
};

struct Picture {
  const char* markup;  // inner SVG on the 300 x 104 grid, in the design's colours
  const char* alt;
};

// From the approved start screen (Simulate canvas, Main); warm-up drawn in the same style.
const std::map<QString, Picture>& pictures() {
  static const std::map<QString, Picture> m = {
      {"fitting",
       {R"(<path d="M40 30 L110 30 L170 42 L250 42 L250 72 L170 72 L110 84 L40 84 Z" fill="#a2aab4" fill-opacity="0.14" stroke="#a2aab4" stroke-width="1.6"/>
<ellipse cx="40" cy="57" rx="10" ry="27" fill="#656d77" fill-opacity="0.5" stroke="#a2aab4" stroke-width="1.6"/>
<ellipse cx="250" cy="57" rx="6" ry="15" fill="#656d77" fill-opacity="0.5" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M8 46 H34 M8 68 H34 M256 52 H290 M256 62 H290" stroke="#33c3d6" stroke-width="2.2"/>
<path d="M28 41 l7 5 -7 5 M28 63 l7 5 -7 5 M284 47 l7 5 -7 5 M284 57 l7 5 -7 5" stroke="#33c3d6" stroke-width="2.2" fill="none"/>
<path d="M118 66 q10 -8 2 -14 q-8 -4 -12 4" stroke="#e3a63a" stroke-width="1.6" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A duct that narrows from wide to small, with air arrows going through it")}},
      {"fan",
       {R"(<ellipse cx="70" cy="52" rx="16" ry="38" fill="#656d77" fill-opacity="0.45" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M70 52 q-2 -22 8 -26 M70 52 q12 10 6 24 M70 52 q-12 6 -10 -18" stroke="#dcdde0" stroke-width="1.6" fill="none"/>
<circle cx="70" cy="52" r="4" fill="#dcdde0"/>
<path d="M90 30 L240 30 L240 74 L90 74" fill="#a2aab4" fill-opacity="0.14" stroke="#a2aab4" stroke-width="1.6"/>
<ellipse cx="240" cy="52" rx="8" ry="22" fill="#656d77" fill-opacity="0.5" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M110 46 H200 M110 60 H200 M250 52 H290" stroke="#33c3d6" stroke-width="2.2"/>
<path d="M194 41 l7 5 -7 5 M194 55 l7 5 -7 5 M284 47 l7 5 -7 5" stroke="#33c3d6" stroke-width="2.2" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A round fan blowing air into a straight duct")}},
      {"overheat",
       {R"(<path d="M70 42 L150 20 L230 42 L150 64 Z" fill="#a2aab4" fill-opacity="0.08" stroke="#7f8791" stroke-width="1.4" stroke-dasharray="4 3"/>
<path d="M70 42 L70 76 L150 98 L150 64" fill="#7f8791" fill-opacity="0.18" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M150 64 L230 42 L230 76 L150 98" fill="#656d77" fill-opacity="0.2" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M95 58 L150 44 L205 58 L150 72 Z" fill="#2d9550" fill-opacity="0.45" stroke="#4fbd6f" stroke-width="1.3"/>
<path d="M138 56 L150 53 L162 56 L150 59 Z" fill="#e2584c" stroke="#e3a63a" stroke-width="1.5"/>
<path d="M144 44 q-5 -6 0 -12 q5 -6 0 -12 M156 44 q-5 -6 0 -12 q5 -6 0 -12" stroke="#e3a63a" stroke-width="1.8" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "An open box with a circuit board inside, one chip glowing hot, heat waves rising")}},
      {"heatsink",
       {R"(<path d="M100 78 L150 64 L200 78 L150 92 Z" fill="#e2584c" fill-opacity="0.7" stroke="#e3a63a" stroke-width="1.5"/>
<path d="M108 70 L108 38 M124 66 L124 32 M140 62 L140 28 M156 62 L156 28 M172 66 L172 32 M188 70 L188 38" stroke="#a2aab4" stroke-width="5" stroke-linecap="round"/>
<path d="M100 72 L150 58 L200 72" stroke="#a2aab4" stroke-width="2" fill="none"/>
<path d="M30 40 H80 M30 54 H80 M220 40 H270 M220 54 H270" stroke="#33c3d6" stroke-width="2.2"/>
<path d="M74 35 l7 5 -7 5 M74 49 l7 5 -7 5 M264 35 l7 5 -7 5 M264 49 l7 5 -7 5" stroke="#33c3d6" stroke-width="2.2" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A chip with a finned heatsink on top and cool air blowing across the fins")}},
      {"warmup",
       {R"(<path d="M64 70 L140 50 L216 70 L140 90 Z" fill="#2d9550" fill-opacity="0.45" stroke="#4fbd6f" stroke-width="1.3"/>
<path d="M124 68 L140 64 L156 68 L140 72 Z" fill="#e2584c" stroke="#e3a63a" stroke-width="1.5"/>
<path d="M134 60 q-5 -6 0 -12 q5 -6 0 -12 M146 60 q-5 -6 0 -12 q5 -6 0 -12" stroke="#e3a63a" stroke-width="1.8" fill="none"/>
<circle cx="40" cy="42" r="20" fill="#2f3136" stroke="#dcdde0" stroke-width="1.5"/>
<path d="M40 42 V28 M40 42 L51 49" stroke="#dcdde0" stroke-width="1.8" fill="none"/>
<path d="M66 22 A30 30 0 0 1 72 52" stroke="#4fbd6f" stroke-width="2.2" fill="none"/>
<path d="M65 46 l7 7 4 -9" stroke="#4fbd6f" stroke-width="2.2" fill="none"/>
<rect x="244" y="12" width="12" height="66" rx="6" fill="#2f3136" stroke="#dcdde0" stroke-width="1.5"/>
<rect x="247" y="40" width="6" height="40" rx="3" fill="#e2584c"/>
<circle cx="250" cy="84" r="10" fill="#e2584c" stroke="#dcdde0" stroke-width="1.5"/>
<path d="M260 24 H266 M260 36 H266 M260 48 H266" stroke="#9b9ea6" stroke-width="1.5"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A chip on a board warming up, a thermometer filling beside it and a clock")}},
      {"bend",
       {R"(<path d="M60 10 L60 98" stroke="#7f8791" stroke-width="3"/>
<path d="M52 14 l8 -6 M52 30 l8 -6 M52 46 l8 -6 M52 62 l8 -6 M52 78 l8 -6 M52 94 l8 -6" stroke="#7f8791" stroke-width="1.5"/>
<path d="M62 22 L76 18 L76 70 L196 38 L196 52 L76 84 L62 88 Z" fill="#a2aab4" fill-opacity="0.22" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M186 48 L186 64" stroke="#dcdde0" stroke-width="1.5"/>
<rect x="172" y="64" width="28" height="24" rx="3" fill="#656d77" stroke="#dcdde0" stroke-width="1.5"/>
<path d="M230 28 V74 M222 66 l8 8 8 -8" stroke="#9cc3ff" stroke-width="2.2" fill="none"/>
<path d="M150 54 l8 -2" stroke="#e2584c" stroke-width="3" stroke-linecap="round"/>
<path d="M78 76 l10 -3" stroke="#e2584c" stroke-width="3" stroke-linecap="round"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "An L-shaped bracket screwed to a wall with a heavy weight hanging from its end")}},
      {"print",
       {R"(<path d="M50 84 L150 62 L250 84 L150 106 Z" fill="#3a3d43" stroke="#7f8791" stroke-width="1.4"/>
<path d="M110 76 L150 67 L190 76 L190 40 L150 31 L110 40 Z" fill="#a2aab4" fill-opacity="0.2" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M110 46 L150 37 L190 46 M110 52 L150 43 L190 52 M110 58 L150 49 L190 58 M110 64 L150 55 L190 64 M110 70 L150 61 L190 70" stroke="#a2aab4" stroke-opacity="0.6" stroke-width="1"/>
<path d="M150 4 V26 M142 18 l8 8 8 -8" stroke="#9cc3ff" stroke-width="2.2" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A 3D printed part on the print bed, its layers drawn as stripes, with a load pressing on it")}},
      {"clearance",
       {R"(<path d="M70 80 L230 80 L230 92 L70 92 Z" fill="#a2aab4" fill-opacity="0.22" stroke="#a2aab4" stroke-width="1.6"/>
<circle cx="80" cy="76" r="6" fill="#656d77" stroke="#dcdde0" stroke-width="1.5"/>
<path d="M80 76 L200 26 L204 34 L84 84 Z" fill="#a2aab4" fill-opacity="0.3" stroke="#a2aab4" stroke-width="1.6"/>
<path d="M80 76 L132 14 L138 18 L86 82 Z" fill="none" stroke="#9b9ea6" stroke-width="1.3" stroke-dasharray="4 3"/>
<path d="M216 50 A120 120 0 0 0 160 8" stroke="#4fbd6f" stroke-width="2.2" fill="none"/>
<path d="M168 4 l-9 4 6 8" stroke="#4fbd6f" stroke-width="2.2" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A lid on a hinge swinging open, with a curved arrow showing the swing and a dashed outline where it could hit")}},
      {"forces",
       {R"(<rect x="56" y="58" width="44" height="34" rx="4" fill="#656d77" stroke="#dcdde0" stroke-width="1.5"/>
<circle cx="78" cy="58" r="7" fill="#2f3136" stroke="#dcdde0" stroke-width="1.5"/>
<path d="M78 58 L210 34 L212 44 L80 66 Z" fill="#a2aab4" fill-opacity="0.3" stroke="#a2aab4" stroke-width="1.6"/>
<rect x="200" y="44" width="24" height="20" rx="3" fill="#656d77" stroke="#dcdde0" stroke-width="1.5"/>
<path d="M58 40 A24 24 0 0 1 100 40" stroke="#4fbd6f" stroke-width="2.2" fill="none"/>
<path d="M94 32 l7 8 -9 3" stroke="#4fbd6f" stroke-width="2.2" fill="none"/>
<path d="M250 40 V84 M242 76 l8 8 8 -8" stroke="#9b9ea6" stroke-width="2" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A motor turning an arm that lifts a weight against gravity, with a turning arrow at the motor")}},
      {"rattle",
       {R"(<path d="M70 70 L150 48 L230 70 L150 92 Z" fill="none" stroke="#7f8791" stroke-width="1.2" stroke-dasharray="4 3"/>
<path d="M70 62 Q110 30 150 40 Q190 50 230 62 L150 84 Q110 70 70 62 Z" fill="#a2aab4" fill-opacity="0.24" stroke="#a2aab4" stroke-width="1.6"/>
<circle cx="70" cy="66" r="3.5" fill="#dcdde0"/><circle cx="230" cy="66" r="3.5" fill="#dcdde0"/>
<path d="M40 46 q6 -6 0 -12 M30 50 q10 -10 0 -20 M260 46 q-6 -6 0 -12 M270 50 q-10 -10 0 -20" stroke="#9cc3ff" stroke-width="2" fill="none"/>)",
        QT_TRANSLATE_NOOP("SimPicturesText", "A flat panel bolted at its corners, bending in a wave shape with vibration marks beside it")}},
  };
  return m;
}

QColor mix(const QColor& a, const QColor& b, double f) {
  return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * f), float(a.greenF() + (b.greenF() - a.greenF()) * f),
                          float(a.blueF() + (b.blueF() - a.blueF()) * f));
}

// The design's colours as the theme's: its model greys follow the model's face colours (darker outlines on a light
// theme), its accents the goal colours.
QColor token(const QString& hex, const Tokens& t) {
  const bool hc = t.highContrast, dark = t.dark;
  if (hex == "#a2aab4") return hc ? t.fg : dark ? t.mtop : t.medge;     // the model
  if (hex == "#7f8791") return hc ? t.fg : dark ? t.mleft : t.fg3;      // the model's shade, a wall
  if (hex == "#656d77") return hc ? t.bg2 : dark ? t.mright : t.mleft;  // a solid part's fill
  if (hex == "#dcdde0") return t.fg;
  if (hex == "#9b9ea6") return t.fg2;
  if (hex == "#33c3d6") return simchecks::goalColor(simchecks::Goal::Air, t);
  if (hex == "#9cc3ff") return simchecks::goalColor(simchecks::Goal::Strength, t);
  if (hex == "#e3a63a") return t.amber;
  if (hex == "#4fbd6f") return t.green;
  if (hex == "#2d9550") return hc ? t.green : mix(t.green, QColor(0, 0, 0), 0.25);
  if (hex == "#e2584c") return t.red;
  if (hex == "#2f3136") return t.bg3;
  if (hex == "#3a3d43") return t.bg4;
  return QColor(hex);
}

QString attr(const QString& attrs, const char* name) {
  const QString key = QLatin1String(name) + QLatin1String("=\"");
  for (qsizetype at = attrs.indexOf(key); at >= 0; at = attrs.indexOf(key, at + 1)) {
    if (at > 0 && !attrs[at - 1].isSpace()) continue;  // "opacity" is not "fill-opacity"
    const qsizetype start = at + key.size(), end = attrs.indexOf('"', start);
    return end < 0 ? QString() : attrs.mid(start, end - start);
  }
  return {};
}
double num(const QString& attrs, const char* name, double def) {
  bool ok = false;
  const double v = attr(attrs, name).toDouble(&ok);
  return ok ? v : def;
}

}  // namespace

namespace simpic {

bool has(const QString& id) { return pictures().count(id) > 0; }

QString description(const QString& id) {
  const auto it = pictures().find(id);
  return it == pictures().end() ? QString() : SimPicturesText::tr(it->second.alt);
}

void paint(QPainter& p, const QRectF& r, const QString& id, const Tokens& t) {
  const auto it = pictures().find(id);
  if (it == pictures().end() || r.width() <= 0 || r.height() <= 0) return;
  const double k = std::min(r.width() / kWidth, r.height() / kHeight);
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setClipRect(r);
  p.translate(r.center().x() - kWidth * k / 2, r.center().y() - kHeight * k / 2);
  p.scale(k, k);
  static const QRegularExpression el("<(path|rect|circle|ellipse)([^>]*?)/?>");
  for (auto m = el.globalMatch(QString::fromUtf8(it->second.markup)); m.hasNext();) {
    const auto e = m.next();
    const QString tag = e.captured(1), a = e.captured(2);
    QPainterPath path;
    if (tag == "path") path = icons::svgPath(attr(a, "d"));
    else if (tag == "rect") path.addRoundedRect(num(a, "x", 0), num(a, "y", 0), num(a, "width", 0), num(a, "height", 0), num(a, "rx", 0), num(a, "rx", 0));
    else if (tag == "ellipse") path.addEllipse(QPointF(num(a, "cx", 0), num(a, "cy", 0)), num(a, "rx", 0), num(a, "ry", 0));
    else path.addEllipse(QPointF(num(a, "cx", 0), num(a, "cy", 0)), num(a, "r", 0), num(a, "r", 0));
    const QString fill = attr(a, "fill"), stroke = attr(a, "stroke");
    if (!fill.isEmpty() && fill != "none") {
      QColor c = token(fill, t);
      // High contrast: fills stay faint so the outlines carry the shape (the hot chip, the dials stay solid).
      const bool solid = fill == "#e2584c" || fill == "#2f3136" || fill == "#dcdde0";
      c.setAlphaF(float(num(a, "fill-opacity", 1.0) * (t.highContrast && !solid ? 0.35 : 1.0)));
      p.fillPath(path, c);
    }
    if (!stroke.isEmpty() && stroke != "none") {
      QColor c = token(stroke, t);
      c.setAlphaF(float(num(a, "stroke-opacity", 1.0)));
      QPen pen(c, num(a, "stroke-width", 1.0) * (t.highContrast ? 1.3 : 1.0), Qt::SolidLine,
               attr(a, "stroke-linecap") == "round" ? Qt::RoundCap : Qt::FlatCap, Qt::RoundJoin);
      if (const QString dash = attr(a, "stroke-dasharray"); !dash.isEmpty()) {
        QVector<qreal> pattern;
        for (const QString& v : dash.split(' ', Qt::SkipEmptyParts)) pattern << v.toDouble() / std::max(0.1, pen.widthF());
        pen.setDashPattern(pattern);
      }
      p.strokePath(path, pen);
    }
  }
  p.restore();
}

}  // namespace simpic
