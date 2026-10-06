// SketchEditor, typed values (TODO 11 UI-16): the boxes of the step that waits, the keys that type into them and using
// what was typed. Every numeric tool takes its numbers from the keyboard, before anything is picked too, from the view or
// from a tool panel; no digit reaches a window shortcut while a tool runs (5, 6 and 7 used to switch the display style).
// A point's typed values hold the rubber band as they are typed (the pointer gives what is not typed), so the preview, a
// click and Enter all put the point at the same place. A shape's later steps take its own sizes (UI-17: a rectangle's
// width and height, a circle's diameter, a slot's length and angle then width; ShapeInput.hpp), the rubber band reads
// them out where they are measured, and what they made keeps them as driving dimensions (SketchTools.cpp keepTyped).
#include "SketchEditor.hpp"
#include "SketchGeometryCache.hpp"
#include "DimensionHandle.hpp"
#include "I18n.hpp"
#include "InputKeys.hpp"
#include "Jobs.hpp"
#include "ShapeInput.hpp"
#include "SketchCommands.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/sketch_pattern.hpp"
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QSettings>
#include <algorithm>
#include <cmath>

using namespace opad::design;
using inputkeys::Entry;

namespace {
// What the pointer gives a box, to what a pixel tells apart at this zoom.
QString liveNumber(double value, double pixel) {
  const int decimals = std::clamp(int(std::ceil(-std::log10(std::max(1e-6, pixel * 2)) - 1e-9)), 0, 4);
  QString text = QString::number(value, 'f', decimals);
  if (text.startsWith('-') && text.toDouble() == 0) text.remove(0, 1);
  return text;
}
// An angle in the shown unit (UI-123): degrees to `decimals` ("12.5°"), radians to two more ("0.218 rad"); `exact` as
// typed (ten digits).
QString angleText(double radians, int decimals, bool exact = false) {
  const bool rad = units::current().radians;
  const double shown = rad ? radians : radians * 180 / M_PI;
  QString text = exact ? QString::number(shown, 'g', 10) : QString::number(shown, 'f', rad ? decimals + 2 : decimals);
  if (text.startsWith('-') && text.toDouble() == 0) text.remove(0, 1);
  return text + (rad ? QStringLiteral(" ") : QString()) + units::symbol(units::Kind::Angle);
}
QString liveAngle(double radians) {
  double degrees = std::remainder(radians * 180 / M_PI, 360.0);
  if (degrees <= -180 + 1e-9) degrees += 360;
  return angleText(degrees * M_PI / 180, 1);
}
// A typed angle for the expression parser: ° is degrees, a bare number is in the shown unit (an expression without a unit
// stays in degrees, as the parser and the kept dimensions take it).
std::string angleExpression(QString text) {
  text.replace(QStringLiteral("°"), QStringLiteral(" deg"));
  bool plain = false;
  text.trimmed().toDouble(&plain);
  if (plain) text += units::current().radians ? QStringLiteral(" rad") : QStringLiteral(" deg");
  return text.toStdString();
}
// Tools that place points: the next one can be typed (X and Y; a polyline goes on by length and angle).
const QStringList kPointTools = {"point", "line", "spline", "rect", "crect", "circle", "circle2", "circle3", "arc3", "arcc", "polygon", "polygon_outer",
                                 "slot", "cslot", "arcslot", "ellipse", "conic", "rect3", "control_spline", "tangent_arc", "text", "paste", "copybase"};
// Option tools Enter applies, once there is something picked to apply them to; and those Enter after typing applies as they
// are (a gap, an image, a trace, a tolerance).
const QStringList kApplied = {"offset", "chamfer", "move", "copy", "rotate", "scale", "rect_pattern", "polar_pattern", "node", "image_insert", "image_calibrate"};
const QStringList kAppliedNow = {"heal", "image_edit", "image_trace", "simplify", "vector_import"};
// Steps that take the shape's own sizes (UI-17): after the first click, after the second.
const QStringList kSized1 = {"rect", "crect", "circle", "circle2", "rect3", "arc3", "circle3", "slot", "cslot", "arcc", "arcslot", "ellipse", "polygon", "polygon_outer",
                             "tangent_arc"};
const QStringList kSized2 = {"rect3", "arc3", "circle3", "slot", "cslot", "arcc", "arcslot", "ellipse"};
// Option tools whose second box switches to an angle ('<' or its switch): a chamfer, a move.
const QStringList kAngled = {"chamfer", "move", "copy"};
// Boxes that take an angle; sizes that must not be zero.
bool angleKey(const QString& key) { return key == "angle" || key == "sweep"; }
const QStringList kSizes = {"length", "width", "height", "diameter", "radius", "minor"};
}  // namespace

QString SketchEditor::inputStep() const { return QStringLiteral("%1/%2/%3").arg(m_tool).arg(m_chain.size()).arg(m_clicks.size()); }

Entry SketchEditor::entry() const {
  if (m_entry && m_entryStep == inputStep()) return *m_entry;
  if ((m_tool == "line" || m_tool == "spline") && !m_chain.empty()) return Entry::Polar;
  return shaped() ? Entry::Shape : Entry::Absolute;
}

bool SketchEditor::shaped() const { return (m_clicks.size() == 1 && kSized1.contains(m_tool)) || (m_clicks.size() == 2 && kSized2.contains(m_tool)); }

double SketchEditor::unitLength() const {
  try {
    return ParamTable({}, m_doc->scene.units).length("1");
  } catch (const std::exception&) {
    return 1;
  }
}

// The boxes of a shape's step (Tab order), the pointer's sizes shown grey: what a click there would make.
QList<DynamicInput::Field> SketchEditor::shapeFields() const {
  using Field = DynamicInput::Field;
  if (!shaped()) return {};
  const double unit = unitLength(), pixel = m_viewport->pixelSize() / unit, u = m_cursor.u, v = m_cursor.v;
  auto number = [&](double mm) { return liveNumber(mm / unit, pixel); };
  const QString tip = tr("Typed first: # X and Y · @ ΔX and ΔY from the last point");
  auto field = [&](const char* key, const QString& label, const QString& live, const QString& chip = {}) { return Field{key, label, live, false, chip, tip}; };
  const Snap& a = m_clicks[0];
  const double du = u - a.u, dv = v - a.v, reach = std::hypot(du, dv);
  if (m_clicks.size() == 1) {
    if (m_tool == "rect") return {field("width", tr("Width"), number(std::fabs(du))), field("height", tr("Height"), number(std::fabs(dv)))};
    if (m_tool == "crect") return {field("width", tr("Width"), number(2 * std::fabs(du))), field("height", tr("Height"), number(2 * std::fabs(dv)))};
    if (m_tool == "tangent_arc") {  // the circle through the pointer, tangent at the line's end: its radius and sweep
      double tu = 1, tv = 0, au = a.u, av = a.v;
      tangentStart(au, av, tu, tv);
      const double off = (u - au) * -tv + (v - av) * tu, r = std::fabs(off) > 1e-12 ? ((u - au) * (u - au) + (v - av) * (v - av)) / (2 * std::fabs(off)) : 0;
      const shapeinput::P end = shapeinput::tangentArc({au, av}, {tu, tv}, {u, v}, nullptr, nullptr);
      const double cu = au - (off < 0 ? -1 : 1) * tv * r, cv = av + (off < 0 ? -1 : 1) * tu * r;
      const double turn = shapeinput::turned(std::atan2(av - cv, au - cu), std::atan2(end.v - cv, end.u - cu), off < 0 ? -1 : 1);
      return {field("radius", tr("Radius"), number(r)), field("sweep", tr("Sweep angle"), angleText(turn, 1))};
    }
    if (m_tool == "circle")  // the switch after the box: diameter or radius (saved)
      return {m_circleRadius ? field("radius", tr("Radius"), number(reach), tr("R")) : field("diameter", tr("Diameter"), number(2 * reach), tr("Ø"))};
    QList<Field> out;
    if (m_tool == "circle2") out << field("diameter", tr("Diameter"), number(reach));
    else if (m_tool == "arcc" || m_tool == "arcslot") out << field("radius", tr("Radius"), number(reach));
    else if (m_tool == "ellipse") out << field("radius", tr("Major radius"), number(reach));
    else if (m_tool == "polygon" || m_tool == "polygon_outer")  // the switch: corners on the circle, or sides touching it
      out << field("diameter", m_tool == "polygon" ? tr("Diameter") : tr("Across flats"), number(2 * reach), m_tool == "polygon" ? tr("Inscribed") : tr("Circumscribed"));
    else out << field("length", tr("Length"), number(m_tool == "cslot" ? 2 * reach : reach));
    out << field("angle", m_tool == "arcc" || m_tool == "arcslot" ? tr("Start angle") : tr("Angle"), liveAngle(std::atan2(dv, du)));
    if (m_tool == "polygon" || m_tool == "polygon_outer") out << Field{"sides", tr("Sides"), option("sides", "6"), true};
    return out;
  }
  const Snap& b = m_clicks[1];
  const double bu = b.u - a.u, bv = b.v - a.v, chord = std::max(1e-12, std::hypot(bu, bv));
  const double off = std::fabs((dv * bu - du * bv) / chord);  // the pointer's distance from the line through the clicks
  if (m_tool == "rect3") return {field("height", tr("Height"), number(off))};
  if (m_tool == "slot" || m_tool == "cslot") return {field("width", tr("Width"), number(2 * off))};
  if (m_tool == "ellipse") return {field("minor", tr("Minor radius"), number(off))};
  if (m_tool == "arc3" || m_tool == "circle3") {
    const double d = 2 * (a.u * (b.v - v) + b.u * (v - a.v) + u * (a.v - b.v));
    double r = 0;
    if (std::fabs(d) > 1e-12) {
      const double ux = ((a.u * a.u + a.v * a.v) * (b.v - v) + (b.u * b.u + b.v * b.v) * (v - a.v) + (u * u + v * v) * (a.v - b.v)) / d;
      const double uy = ((a.u * a.u + a.v * a.v) * (u - b.u) + (b.u * b.u + b.v * b.v) * (a.u - u) + (u * u + v * v) * (b.u - a.u)) / d;
      r = std::hypot(a.u - ux, a.v - uy);
    }
    return {field("radius", tr("Radius"), number(r))};
  }
  // The centre arc's or arc slot's sweep from its start (signed, counter-clockwise positive), the way the pointer went round
  // (P5), past half a turn too.
  const double sweep = m_tool == "arcslot" || m_tool == "arcc" ? slotSweep(u, v) : std::remainder(std::atan2(dv, du) - std::atan2(bv, bu), 2 * M_PI);
  QList<Field> out{field("sweep", tr("Sweep angle"), angleText(sweep, 1))};
  if (m_tool == "arcslot") out << Field{"width", tr("Width"), option("width", "2 mm"), true};
  return out;
}

bool SketchEditor::inputBase(double& u, double& v) const {
  if (!kPointTools.contains(m_tool)) return false;
  if (m_tool == "paste") {  // @dx,dy: from where the curves were copied
    if (m_clip) u = m_clip->bu, v = m_clip->bv;
    return m_clip != nullptr;
  }
  if (m_tool == "line" || m_tool == "spline") {
    const SkPoint* p = m_chain.empty() ? nullptr : pointOf(m_chain.back());
    if (p) u = p->x, v = p->y;
    return p;
  }
  if (m_clicks.empty()) return false;
  u = m_clicks.back().u;
  v = m_clicks.back().v;
  return true;
}

double SketchEditor::angleReference() const {
  if (!m_angleRelative || m_tool != "line" || m_chain.size() < 2) return 0;
  const SkPoint *a = pointOf(m_chain[m_chain.size() - 2]), *b = pointOf(m_chain.back());
  return a && b && std::hypot(b->x - a->x, b->y - a->y) > 1e-12 ? std::atan2(b->y - a->y, b->x - a->x) : 0;
}

QList<DynamicInput::Field> SketchEditor::inputStage() const {
  using Field = DynamicInput::Field;
  // Values the tool keeps as options (the panel's fields show the same): typed, they are set at once, also before the
  // curves are picked, and they stay for the next ones after Apply.
  auto value = [this](const char* key, const QString& label, const char* fallback) { return Field{key, label, option(key, fallback), true}; };
  if (m_tool == "offset") return {value("distance", tr("Distance"), "5 mm")};
  if (m_tool == "fillet" || m_tool == "tangent_circle") return {value("radius", tr("Radius"), "2 mm")};
  if (m_tool == "chamfer") {  // its second distance, or (the box's switch, or '<' as in 4<30) its angle to the first line
    if (option("chamferMode", "distance") == "angle") return {value("first", tr("Distance"), "2 mm"), Field{"chamferAngle", tr("Angle"), option("chamferAngle", "45 deg"), true, tr("or distance")}};
    return {value("first", tr("Distance 1"), "2 mm"), Field{"second", tr("Distance 2"), option("second", "2 mm"), true, tr("or angle")}};
  }
  if (m_tool == "move" || m_tool == "copy") {  // ΔX and ΔY, or (the switch, or '<' as in 10<30) a distance and an angle; a copy's count
    QList<Field> out = option("moveMode", "xy") == "polar"
                           ? QList<Field>{value("moveDistance", tr("Distance"), "10 mm"), Field{"moveAngle", tr("Angle"), option("moveAngle", "0 deg"), true, tr("or ΔX ΔY")}}
                           : QList<Field>{value("dx", tr("ΔX"), "10 mm"), Field{"dy", tr("ΔY"), option("dy", "0 mm"), true, tr("or angle")}};
    if (m_tool == "copy") out << value("copies", tr("Copies"), "1");
    return out;
  }
  if (m_tool == "rotate") return {value("angle", tr("Angle"), "45 deg"), value("cx", tr("Centre X"), "0 mm"), value("cy", tr("Centre Y"), "0 mm")};
  if (m_tool == "scale") return {value("scale", tr("Factor"), "2"), value("cx", tr("Centre X"), "0 mm"), value("cy", tr("Centre Y"), "0 mm")};
  if (m_tool == "rect_pattern") return {value("count", tr("Count"), "3"), value("dx", tr("Spacing X"), "10 mm"), value("rows", tr("Rows"), "1"), value("dy", tr("Spacing Y"), "10 mm")};
  if (m_tool == "polar_pattern") return {value("count", tr("Count"), "3"), value("angle", tr("Total angle"), "360 deg"), value("cx", tr("Centre X"), "0 mm"), value("cy", tr("Centre Y"), "0 mm")};
  if (m_tool == "heal") return {value("healTolerance", tr("Gap"), "0.05 mm")};
  if (m_tool == "image_insert") return {value("imageWidth", tr("Image width"), "100 mm")};
  if (m_tool == "image_calibrate") return {value("knownDistance", tr("Known distance"), "10 mm")};
  // The values the panel holds for these (a backdrop's place and look, a trace's settings, a tolerance, a node's weights).
  if (m_tool == "image_edit")
    return {value("imageX", tr("X position"), "0 mm"), value("imageY", tr("Y position"), "0 mm"), value("imageWidth", tr("Image width"), "100 mm"),
            value("imageAngle", tr("Rotation"), "0 deg"), value("imageOpacity", tr("Opacity (0 to 1)"), "0.5")};
  if (m_tool == "image_trace")
    return {value("threshold", tr("Threshold (0 to 255)"), "128"), value("smoothing", tr("Smoothing (0 to 10 pixels)"), "1"), value("noise", tr("Minimum area in pixels"), "8"),
            value("traceTolerance", tr("Trace tolerance in pixels"), "0.75"), value("cornerAngle", tr("Preserve corners above (degrees)"), "60")};
  if (m_tool == "simplify" || m_tool == "vector_import") return {value("curveTolerance", tr("Curve tolerance"), "0.01 mm")};
  if (m_tool == "node") return {value("weight", tr("Node weight"), "1"), value("incoming", tr("Incoming handle weight"), "1"), value("outgoing", tr("Outgoing handle weight"), "1")};
  if (!kPointTools.contains(m_tool)) return {};
  // The text tool's words first (every printable key typed goes there), its height, then where it goes.
  QList<Field> out;
  if (m_tool == "text") out = {Field{"text", tr("Text"), option("text", "OPAD"), true, {}, {}, false, true}, value("height", tr("Text height"), "10 mm")};
  const double unit = unitLength(), pixel = m_viewport->pixelSize() / unit, u = m_haveCursor ? m_cursor.u : 0, v = m_haveCursor ? m_cursor.v : 0;
  auto number = [&](double mm) { return liveNumber(mm / unit, pixel); };
  double bu = 0, bv = 0;
  const bool base = inputBase(bu, bv);
  const QString tip = base ? tr("Typed first: # X and Y · @ ΔX and ΔY from the last point · 30<45 length and angle") : QString();
  auto field = [&](const char* key, const QString& label, const QString& live, const QString& chip = {}) { return Field{key, label, live, false, chip, tip}; };
  switch (base ? entry() : Entry::Absolute) {
    case Entry::Shape: out << shapeFields(); break;
    case Entry::Polar: {
      // A polyline's angle can be measured from its last segment: the switch after the box says which.
      const QString chip = m_tool == "line" && m_chain.size() >= 2 ? (m_angleRelative ? tr("∠ last line") : tr("∠ X axis")) : QString();
      out << field("length", tr("Length"), number(std::hypot(u - bu, v - bv))) << field("angle", tr("Angle"), liveAngle(std::atan2(v - bv, u - bu) - angleReference()), chip);
      break;
    }
    case Entry::Relative: out << field("dx", tr("ΔX"), number(u - bu)) << field("dy", tr("ΔY"), number(v - bv)); break;
    case Entry::Absolute: out << field("x", tr("X"), number(u)) << field("y", tr("Y"), number(v)); break;
  }
  if (m_tool == "conic") out << value("rho", tr("Rho"), "0.5");  // in every step: how full the curve is
  return out;
}

// Every tool with an Apply button takes Enter once its picks are complete (TODO 11 wave 3, P4: the guides press Enter):
// what each one's Apply needs, so Enter never applies a tool that would only say what is missing.
bool SketchEditor::appliesOnEnter() const {
  const QString& t = m_tool;
  if (!sketchkeys::entersApply(t.toStdString())) return false;
  auto curves = [this] { return std::count_if(m_sel.begin(), m_sel.end(), [this](int id) { return m_sk.entity(id) != nullptr; }); };
  if (t == "chamfer") return m_sel.size() == 1 && m_sk.point(m_sel.front());
  if (t == "node") return m_sel.size() == 1;
  if (t == "mirror") return curves() > 0 && (option("mirrorAxis", "picked") != "picked" || (option("mirrorStage", "seed") == "axis" && !m_picked.empty()));
  if (t == "break") return curves() >= 2;
  if (t == "union" || t == "subtract" || t == "intersect") return m_clicks.size() == 2;
  if (t == "explode") return std::any_of(m_sel.begin(), m_sel.end(), [this](int id) { return pattern_of(m_sk, id, true) != 0; });
  if (t == "heal" || t == "simplify") return true;
  if (sketchkeys::referenceTool(t.toStdString())) return !m_sources.isEmpty();
  if (t == "break_link") return std::any_of(m_sel.begin(), m_sel.end(), [this](int id) { const auto* e = m_sk.entity(id); return e && !e->source.is_null(); });
  if (t == "image_insert") return !option("imageFile").isEmpty() && m_clicks.size() == 1;
  if (t == "image_calibrate") return !m_sk.images.empty() && m_clicks.size() == 2;
  if (t == "image_edit" || t == "image_trace" || t == "image_remove") return !m_sk.images.empty();
  if (t == "vector_import" || t == "vector_export") return !option("vectorFile").isEmpty();
  return curves() > 0;  // offset, move, copy, rotate, scale and the patterns: their curves
}

// A key that types into the boxes: a value key while a tool runs (a tool without boxes drops it: it is never a window
// shortcut then), Tab and Shift+Tab (the view keeps the keyboard in a sketch), and a point's '@', '#' and '<'.
bool SketchEditor::typingKey(const QKeyEvent* e) const {
  if (!m_active || (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return false;
  if (e->key() == Qt::Key_Tab || e->key() == Qt::Key_Backtab) return true;
  const QString text = e->text();
  if (text.size() != 1 || m_tool == "select") return false;
  if (m_tool == "text" && text.front().isPrint()) return true;  // the text box takes every printable key
  // A value being typed goes on with the operators of an expression ("10/4"): '/' is the window's Hover highlight key, which
  // must not take the key from a value typed over the view (its box without the keyboard).
  if ((text == QLatin1String("/") || text == QLatin1String("*") || text == QLatin1String(")")) && m_input && m_input->typed()) return true;
  return inputkeys::valueChar(text.front().unicode()) || (inputkeys::entryChar(text.front().unicode()) && (kPointTools.contains(m_tool) || (kAngled.contains(m_tool) && text == "<")));
}

bool SketchEditor::sketchType(QKeyEvent* e) {
  if (!typingKey(e)) return false;
  const bool tab = e->key() == Qt::Key_Tab || e->key() == Qt::Key_Backtab;
  const bool back = e->key() == Qt::Key_Backtab || e->modifiers().testFlag(Qt::ShiftModifier);
  if (m_dimensionHandle->isVisible()) {  // the offset's curves are picked: the box by its arrow takes the value
    if (tab) m_dimensionHandle->focusValue();
    else m_dimensionHandle->type(e->text());
    return true;
  }
  updateInput();
  if (!m_input->count()) return true;  // a tool without values (trim, a constraint): the key goes nowhere
  if (tab) m_input->cycle(back);
  else m_input->type(e->text());
  updateInput();  // shown now also when the pointer has not been over the view
  return true;
}

// A prefix or a separator that switches how the point is typed (InputKeys.hpp): the boxes of the other entry, the number
// typed so far carried over to the first one where it is the same distance.
bool SketchEditor::entryKey(int box, QChar c) {
  if (m_input && kAngled.contains(m_tool) && inputkeys::entryChar(c.unicode())) {  // 4<30: the distance, then the angle; '@' and '#' are nothing here
    if (c == '<') {
      const QString typed = m_input->text(m_input->key(0));
      setAngled(true);
      if (!typed.isEmpty()) m_options[m_input->key(0)] = typed;  // a move's ΔX typed: its distance now
      updateInput();
      m_input->select(1);
    }
    return true;
  }
  if (!m_input || !kPointTools.contains(m_tool)) return false;
  double bu = 0, bv = 0;
  const bool base = inputBase(bu, bv);
  const QLineEdit* edit = m_input->box(box);
  const auto key = inputkeys::entryKey(entry(), box, !edit || edit->text().isEmpty(), c.unicode(), base);
  using inputkeys::Turn;
  switch (key.turn) {
    case Turn::Type: return false;
    case Turn::Swallow: return true;
    case Turn::Next: m_input->cycle(false); return true;
    case Turn::Switch:
    case Turn::Carry: {
      const QString carried = key.turn == Turn::Carry && edit ? edit->text() : QString();
      m_entry = key.to;
      m_entryStep = inputStep();
      updateInput();
      if (!carried.isEmpty()) m_input->setText(0, carried);
      m_input->select(key.focus);
      updateInput();
      retype();
      emit changed();
      return true;
    }
  }
  return false;
}

// The second box's switch: a chamfer's second distance or its angle to the first line, a move's ΔX and ΔY or its distance
// and angle (the offset stays what it was).
void SketchEditor::setAngled(bool angled) {
  if (m_tool == "chamfer") {
    if ((option("chamferMode", "distance") == "angle") == angled) return;
    m_options["chamferMode"] = angled ? "angle" : "distance";
  } else if (kAngled.contains(m_tool)) {
    if ((option("moveMode", "xy") == "polar") == angled) return;
    m_options["moveMode"] = angled ? "polar" : "xy";
    try {
      const ParamTable table({}, m_doc->scene.units);
      // In the document's unit (UI-26: an inch sketch's boxes read "254 mm" after the switch).
      const double unit = unitLength();
      const auto mm = [&](double value) { return QString::number(std::fabs(value) < 1e-12 ? 0 : value / unit, 'g', 12) + " " + QString::fromStdString(m_doc->scene.units); };
      if (angled) {
        const double dx = table.length(option("dx", "10 mm").toStdString()), dy = table.length(option("dy", "0 mm").toStdString());
        m_options["moveDistance"] = mm(std::hypot(dx, dy));
        m_options["moveAngle"] = QString::number(std::atan2(dy, dx) * 180 / M_PI, 'g', 12) + " deg";
      } else {
        const double d = table.length(option("moveDistance", "10 mm").toStdString()), a = table.angle(option("moveAngle", "0 deg").toStdString());
        m_options["dx"] = mm(d * std::cos(a));
        m_options["dy"] = mm(d * std::sin(a));
      }
    } catch (const std::exception&) {  // an expression with parameters: the other boxes keep what they had
    }
  } else {
    return;
  }
  m_panelFieldsDirty = true;
  updateInput();
  scheduleToolPreview();
  emit changed();
}

void SketchEditor::updateInput() {
  if (!m_input) return;
  const bool handle = m_dimensionHandle && m_dimensionHandle->isVisible();
  if (handle && m_input->typed()) m_input->used();  // the curves are picked: the box by the arrow shows the typed value
  m_input->setFields(m_active && m_visible && !handle ? inputStage() : QList<DynamicInput::Field>{});
  if (!m_input->typed() && !m_typedValues.empty()) {  // the boxes went with their step (another tool): their values too
    m_typedValues.clear();
    m_cursor = m_pointer;
  }
  if (!m_input->count() || !(m_haveCursor || m_input->typed() || m_input->current() >= 0)) {
    if (m_input->isVisible()) m_input->hide();
    return;
  }
  // A box whose value the rubber band reads out sits there, on what it measures (UI-17); the others beside the pointer.
  QList<DynamicInput::Anchor> anchors;
  const auto marks = readouts();
  const double px = m_viewport->pixelSize();
  for (int i = 0; i < m_input->count() && !marks.empty(); ++i) {
    DynamicInput::Anchor anchor;
    for (const auto& r : marks)
      if (r.key == m_input->key(i) && boxed(r.key)) {
        const QPoint at = m_viewport->widgetPoint(m_frame.to_world(r.bu, r.bv)), to = m_viewport->widgetPoint(m_frame.to_world(r.bu + r.bx * 50 * px, r.bv + r.by * 50 * px));
        if (m_viewport->rect().adjusted(20, 20, -20, -20).contains(at)) anchor = {true, QPointF(at), QPointF(to - at)};  // off the view: beside the pointer
      }
    anchors << anchor;
  }
  // Beside the cursor: with the drawing cursor on a grid node the hidden pointer is up to half a step off it, so the row
  // keeps 20 px off both (it never comes under the pointer, which would take its moves).
  int gap = 20;
  if (m_viewport->ownCursor() && m_viewport->gridSnap() && px > 0) gap += int(std::ceil(m_viewport->gridStep() / 2 / px));
  m_input->placeNear(m_haveCursor ? m_viewport->widgetPoint(m_frame.to_world(m_pointer.u, m_pointer.v)) : QPoint(m_viewport->width() / 2, m_viewport->height() / 2), anchors, std::min(gap, 160));
  if (!m_input->isVisible()) {
    m_input->show();
    m_input->raise();
  }
}

// The point's typed values evaluated (expressions, parameters and the sketch's own dimension names work); one that does not
// evaluate turns its box red and the pointer gives that value meanwhile. The rubber band follows at once.
void SketchEditor::retype() {
  if (!kPointTools.contains(m_tool) && m_typedValues.empty()) return;  // an option tool: its values are its options
  std::map<QString, double> values;
  if (m_input && m_input->typed() && kPointTools.contains(m_tool)) {
    std::optional<ParamTable> table;
    for (const auto& field : inputStage()) {
      if (field.option) continue;  // set as it is typed (an option of the tool)
      const QString key = field.key;
      QString text = m_input->text(key);
      if (text.isEmpty()) {
        m_input->setProblem(key, {});
        continue;
      }
      try {
        if (!table) {
          std::vector<ParamDef> defs;
          for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
          table = sketch_parameters(m_sk, ParamTable(defs, m_doc->scene.units));
        }
        const double value = angleKey(key) ? table->angle(angleExpression(text)) : table->length(text.toStdString());
        if (kSizes.contains(key) && std::fabs(value) < 1e-9) throw opad::Error("a size must not be zero");
        if (key == "sweep" && m_tool == "tangent_arc" && (value <= 1e-9 || value >= 2 * M_PI - 1e-9)) throw opad::Error("the sweep must be above 0 and under a full turn");
        if (key == "radius" && (m_tool == "arc3" || m_tool == "circle3") && m_clicks.size() == 2 &&
            std::fabs(value) < std::hypot(m_clicks[1].u - m_clicks[0].u, m_clicks[1].v - m_clicks[0].v) / 2 - 1e-9)
          throw opad::Error("the radius is less than half the distance between the ends");
        values[key] = value;
        m_input->setProblem(key, {});
      } catch (const std::exception& e) {
        m_input->setProblem(key, i18n::t(QString::fromUtf8(e.what())));
      }
    }
  }
  m_typedValues = std::move(values);
  m_cursor = typedPoint(m_pointer);
  updateTransient();
  updateInput();
}

SketchEditor::Snap SketchEditor::typedPoint(const Snap& pointer) const {
  if (m_typedValues.empty()) return pointer;
  auto typed = [this](const char* key) { return m_typedValues.count(key) > 0; };
  auto value = [this](const char* key, double otherwise) { const auto it = m_typedValues.find(key); return it == m_typedValues.end() ? otherwise : it->second; };
  double bu = 0, bv = 0;
  const bool base = inputBase(bu, bv);
  // Drawn by the keyboard alone (the pointer never over the view): the last point stands in for the pointer, so a length
  // alone goes along X, a size to the positive side, X alone keeps the last point's Y.
  Snap at = pointer;
  if (!m_haveCursor && base) at.u = bu, at.v = bv;
  Snap s;
  if (base && entry() == Entry::Shape) {
    if (!shapePoint(at, s.u, s.v)) return pointer;
  } else if (typed("length") || typed("angle")) {
    if (!base) return pointer;
    // A length alone: that far towards the pointer. An angle alone: along it as far as the pointer goes along it.
    const double angle = value("angle", 0) + angleReference();
    const auto p = shapeinput::polar({bu, bv}, {at.u, at.v}, typed("length") ? &m_typedValues.at("length") : nullptr, typed("angle") ? &angle : nullptr);
    s.u = p.u;
    s.v = p.v;
  } else if (typed("dx") || typed("dy")) {
    if (!base) return pointer;
    s.u = bu + value("dx", at.u - bu);
    s.v = bv + value("dy", at.v - bv);
  } else {
    s.u = value("x", at.u);
    s.v = value("y", at.v);
  }
  s.kind = Snap::Kind::Typed;
  if (std::hypot(s.u - pointer.u, s.v - pointer.v) < 1e-9) {  // where the pointer was pulled to anyway: its point, curve and holds too
    s.point = pointer.point;
    s.entity = pointer.entity;
    s.holds = pointer.holds;
    s.segment = pointer.segment;
  }
  double fu = 0, fv = 0;
  int from = 0;
  if (fromPoint(fu, fv, from)) {  // still level with (or above) the step's last point: still horizontal (vertical)
    s.horizontal = pointer.horizontal && std::fabs(s.v - fv) < 1e-9;
    s.vertical = pointer.vertical && std::fabs(s.u - fu) < 1e-9;
  }
  return s;
}

// Where the shape's typed sizes put the click its step waits for; the pointer gives the sizes that are not typed.
bool SketchEditor::shapePoint(const Snap& pointer, double& u, double& v) const {
  using shapeinput::P;
  auto get = [this](const char* key) -> const double* { const auto it = m_typedValues.find(key); return it == m_typedValues.end() ? nullptr : &it->second; };
  double scaled = 0;
  auto times = [&](const double* value, double scale) -> const double* {
    if (!value) return nullptr;
    scaled = *value * scale;
    return &scaled;
  };
  const P a{m_clicks[0].u, m_clicks[0].v}, at{pointer.u, pointer.v};
  P out = at;
  if (m_clicks.size() == 1 && m_tool == "tangent_arc") {
    double au = a.u, av = a.v, tu = 1, tv = 0;
    if (!tangentStart(au, av, tu, tv)) return false;
    out = shapeinput::tangentArc({au, av}, {tu, tv}, m_haveCursor ? at : P{au, av}, get("radius"), get("sweep"));
  } else if (m_clicks.size() == 1) {
    if (m_tool == "rect" || m_tool == "crect") out = shapeinput::corner(a, at, get("width"), get("height"), m_haveCursor, m_tool == "crect" ? 0.5 : 1);
    else if (m_tool == "circle") out = shapeinput::polar(a, at, get("radius") ? get("radius") : times(get("diameter"), 0.5), nullptr);
    else {
      const double* size = m_tool == "circle2" ? get("diameter")
                           : m_tool == "polygon" || m_tool == "polygon_outer" ? times(get("diameter"), 0.5)
                           : m_tool == "arcc" || m_tool == "arcslot" || m_tool == "ellipse" ? get("radius")
                           : times(get("length"), m_tool == "cslot" ? 0.5 : 1);
      out = shapeinput::polar(a, at, size, get("angle"));
    }
  } else {
    const P b{m_clicks[1].u, m_clicks[1].v};
    if (const double* h = get("height"); h && m_tool == "rect3") out = shapeinput::across(a, b, at, *h);
    else if (const double* w = get("width"); w && (m_tool == "slot" || m_tool == "cslot")) out = shapeinput::across(a, b, at, *w / 2);
    else if (const double* minor = get("minor"); minor && m_tool == "ellipse") out = shapeinput::across(a, b, at, *minor);
    else if (const double* r = get("radius"); r && (m_tool == "arc3" || m_tool == "circle3")) {
      if (!shapeinput::bulge(a, b, at, *r, out)) return false;
    } else if (const double* sweep = get("sweep"); sweep && (m_tool == "arcc" || m_tool == "arcslot")) {
      const double r = std::hypot(b.u - a.u, b.v - a.v), to = std::atan2(b.v - a.v, b.u - a.u) + *sweep;
      out = {a.u + r * std::cos(to), a.v + r * std::sin(to)};
    }
  }
  u = out.u;
  v = out.v;
  return true;
}

// The end of the picked line a tangent arc starts at (the one nearer its first click), and the way out of the line there.
bool SketchEditor::tangentStart(double& u, double& v, double& tu, double& tv) const {
  const SkEntity* line = m_picked.empty() || m_clicks.empty() ? nullptr : m_sk.entity(m_picked.front());
  if (!line || line->type != SkEntity::Type::Line) return false;
  const SkPoint *p = m_sk.point(line->p[0]), *q = m_sk.point(line->p[1]);
  const Snap& at = m_clicks.front();
  if (std::hypot(q->x - at.u, q->y - at.v) < std::hypot(p->x - at.u, p->y - at.v)) std::swap(p, q);  // p: the end
  const double length = std::hypot(p->x - q->x, p->y - q->y);
  if (length < 1e-12) return false;
  u = p->x, v = p->y, tu = (p->x - q->x) / length, tv = (p->y - q->y) / length;
  return true;
}

int SketchEditor::pointAt(double u, double v) const {
  if (!m_geometry || m_geometryJob) return 0;
  constexpr double exact = 1e-9;
  for (size_t index : m_geometry->query(u - exact, v - exact, u + exact, v + exact).points)
    if (index < m_sk.points.size() && std::hypot(m_sk.points[index].x - u, m_sk.points[index].y - v) < exact) return m_sk.points[index].id;
  return 0;
}

void SketchEditor::forgetTyped() {
  if (m_input) m_input->used();
  m_typedValues.clear();
  m_entry.reset();
  m_cursor = m_pointer;
}

bool SketchEditor::pointTyped() const {
  if (!m_input || !m_input->typed()) return false;
  const auto fields = inputStage();
  return std::any_of(fields.begin(), fields.end(), [this](const DynamicInput::Field& f) { return !f.option && !m_input->text(f.key).isEmpty(); });
}

bool SketchEditor::useTyped(const Snap* at) {
  const auto fields = inputStage();
  if (fields.isEmpty() || !m_input->typed()) return false;
  if (!pointTyped()) {  // options only (a polygon's sides, a conic's rho beside the point's boxes): set, the point waits
    // Set as they were typed. Enter applies the tool when something is picked, else the value waits for the pick (the
    // fillet's and the tangent circle's picks apply it themselves).
    forgetTyped();
    if (kAppliedNow.contains(m_tool) || appliesOnEnter()) applyTool();
    else if (kApplied.contains(m_tool)) emit status(tr("The value waits: pick what it applies to."));
    else if (m_tool == "text") emit status(tr("Click where the text goes, or Tab to its X and Y and Enter."));
    updateInput();
    emit changed();
    return true;
  }
  // The next point: where the rubber band is held, the typed values winning over the pointer.
  retype();
  for (const auto& field : fields)
    if (const QString problem = m_input->problem(field.key); !problem.isEmpty()) {  // the typed values stay, to be put right
      emit status(problem);
      return true;
    }
  Snap s = typedPoint(at ? *at : m_pointer);
  for (const auto& [key, value] : m_typedValues) s.typed[key] = {value, m_input->text(key)};  // what the click made keeps them
  for (const auto& field : fields)  // a size typed into an option box of the step (an arc slot's width) too
    if (const QString text = m_input->text(field.key); field.option && kSizes.contains(field.key) && !text.isEmpty()) try {
        std::vector<ParamDef> defs;
        for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
        s.typed[field.key] = {sketch_parameters(m_sk, ParamTable(defs, m_doc->scene.units)).length(text.toStdString()), text};
      } catch (const std::exception&) {  // set as typed, it is the tool's option; not kept
      }
  if (!s.point)
    if (const int id = pointAt(s.u, s.v)) {  // typed onto a point (the origin, where the chain started): that point
      s.point = id;
      s.entity = 0;
      s.holds.clear();
    }
  forgetTyped();
  click(s, Qt::AltModifier);  // exactly there: no grid, no inference beyond what still holds there
  updateInput();               // the next point's boxes (a chain's next point does not prompt anew)
  rebuild();
  emit changed();
  return true;
}

// The command line's entry (UI-133): the keys SketchCommands.hpp makes of it typed into the step's boxes one by one, as keys
// over the view are (the boxes, the entry hook and InputKeys.hpp read them), then Enter. A point step takes x,y (absolute),
// @dx,dy, @len<ang and len<ang, and a bare value in its first box unless that is X; a tool's values go into its boxes in
// order. A value that does not evaluate is said and dropped, so the step waits as it did.
QString SketchEditor::enter(const QString& text) {
  namespace sc = sketchcommands;
  const QString line = text.trimmed();
  if (!m_active || line.isEmpty()) return {};
  dropPreviewJob();
  if (m_editJob) return tr("The sketch is busy; try again");
  if (m_tool == "select") return tr("Choose a tool first: type its name (L, C, REC, ...)");
  if (m_dimensionHandle->isVisible()) {  // the offset's curves are picked: its distance, then it applies
    m_options["distance"] = line;
    m_panelFieldsDirty = true;
    scheduleToolPreview();
    emit workflowChanged();
    applyTool();
    return {};
  }
  forgetTyped();
  updateInput();
  const auto fields = inputStage();
  const auto boxes = std::find_if(fields.begin(), fields.end(), [](const DynamicInput::Field& f) { return !f.option; });
  const bool point = kPointTools.contains(m_tool) && boxes != fields.end();
  if (fields.isEmpty() || !m_input->count()) return tr("This tool takes no values: pick in the view");
  std::string keys = sc::keys(line.toStdString(), point);
  double bu = 0, bv = 0;
  if (point && !inputBase(bu, bv)) {
    std::string length, angle;
    if (sc::polar(line.toStdString(), length, angle)) try {  // from the origin: no last point to measure from
        std::vector<ParamDef> defs;
        for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
        const auto table = sketch_parameters(m_sk, ParamTable(defs, m_doc->scene.units));
        const double r = table.length(length), a = table.angle(angleExpression(QString::fromStdString(angle)));
        keys = QStringLiteral("#%1 mm,%2 mm").arg(r * std::cos(a), 0, 'g', 15).arg(r * std::sin(a), 0, 'g', 15).toStdString();
      } catch (const std::exception& e) {
        return i18n::t(QString::fromUtf8(e.what()));
      }
  }
  if (point && sc::bare(line.toStdString()) && boxes->key == "x") return tr("A point needs X and Y: type x,y (or @dx,dy, @length<angle)");
  m_input->select(point ? int(boxes - fields.begin()) : 0);  // past the text tool's words to its X
  for (const QChar c : QString::fromStdString(keys)) m_input->type(QString(c));
  if (point) retype();
  for (const auto& field : inputStage())
    if (const QString problem = m_input->problem(field.key); !problem.isEmpty()) {
      m_input->dropTyped();
      forgetTyped();
      updateInput();
      return problem;
    }
  if (!m_input->typed()) return tr("Nothing to type there");
  done();
  return {};
}

bool SketchEditor::boxed(const QString& key) const {
  if (!m_input || (m_dimensionHandle && m_dimensionHandle->isVisible())) return false;
  const auto fields = inputStage();
  return std::any_of(fields.begin(), fields.end(), [&](const DynamicInput::Field& f) { return f.key == key && !f.option; });
}

// What the rubber band reads out as it goes (UI-17): the step's sizes where they are measured (a segment's length beside
// its middle, its angle on an arc from what it is measured from, a rectangle's width under it and height beside it), a
// typed one held (drawn locked).
std::vector<SketchEditor::Readout> SketchEditor::readouts() const {
  std::vector<Readout> out;
  const bool chain = (m_tool == "line" || m_tool == "spline") && !m_chain.empty();
  if (!chain && !shaped()) return out;
  const double unit = unitLength(), px = m_viewport->pixelSize(), cu = m_cursor.u, cv = m_cursor.v;
  const QString units = QString::fromStdString(m_doc->scene.units);
  auto typed = [this](const char* key) { return m_typedValues.count(key) > 0; };
  auto length = [&](double mm, bool locked, const QString& prefix) {
    const double value = std::fabs(mm) / unit;
    return prefix + (locked ? QString::number(value, 'g', 10) : liveNumber(value, px / unit)) + " " + units;
  };
  // A label `gap` pixels off (x, y) towards (ox, oy), clear of it by its own size that way (the overlay's 13 pixel font).
  const QFontMetricsF metrics(theme::ui());
  auto place = [&](Readout& r, double x, double y, double ox, double oy, double gap) {
    r.ox = ox, r.oy = oy;
    r.bu = x + ox * (gap - 6) * px;  // a box sits 6 pixels past what it measures
    r.bv = y + oy * (gap - 6) * px;
    r.bx = ox, r.by = oy;
    r.ext = (std::fabs(ox) * metrics.horizontalAdvance(r.text) / 2 + std::fabs(oy) * metrics.height() / 2) * px;
    r.u = x + ox * (gap * px + r.ext);
    r.v = y + oy * (gap * px + r.ext);
  };
  // A size along a -> b (times `scale`: a diameter from its radius), beside its middle: on the upper side, or on the
  // `side` given (+1 the left of a -> b, -1 its right).
  auto along = [&](const char* key, double au, double av, double bu, double bv, const QString& prefix = {}, double scale = 1, bool leader = false, int side = 0) {
    const double dx = bu - au, dy = bv - av, l = std::hypot(dx, dy);
    if (l < 1e-12) return;
    double nx = -dy / l, ny = dx / l;
    if (side < 0 || (!side && (ny < -1e-12 || (std::fabs(ny) <= 1e-12 && nx < 0)))) nx = -nx, ny = -ny;
    Readout r;
    r.key = key;
    r.locked = typed(key);
    r.text = length(l * scale, r.locked, prefix);
    r.leader = leader;
    r.fu = au, r.fv = av, r.tu = bu, r.tv = bv;
    place(r, (au + bu) / 2, (av + bv) / 2, nx, ny, 6);
    out.push_back(r);
  };
  // An angle at (x, y) from the direction `from` through `sweep` (radians), on an arc 40 pixels out, the value past it.
  auto angle = [&](const char* key, double x, double y, double from, double sweep) {
    Readout r;
    r.cu = x, r.cv = y, r.r = 40 * px, r.from = from, r.sweep = sweep;
    r.key = key;
    r.locked = typed(key);
    r.text = angleText(sweep, 1, r.locked);
    place(r, x, y, std::cos(from + sweep / 2), std::sin(from + sweep / 2), 46);
    if (std::fabs(sweep) < M_PI / 3) {  // too narrow for a box: beside the arc's start, outside the angle (over no line)
      const double way = from - (sweep < 0 ? -1 : 1) * M_PI / 7;
      r.bx = std::cos(way), r.by = std::sin(way), r.bu = x + r.bx * 40 * px, r.bv = y + r.by * 40 * px;
    }
    out.push_back(r);
  };
  // A length and its angle from `from` at (x, y): the length on the other side of the line from the angle's arc.
  auto polar = [&](const char* key, double x, double y, double from, const QString& prefix = {}, double scale = 1, bool leader = false) {
    const double heading = std::atan2(cv - y, cu - x), sweep = std::remainder(heading - from, 2 * M_PI), side = std::sin(from + sweep / 2 - heading);
    along(key, x, y, cu, cv, prefix, scale, leader, std::fabs(sweep) < 1e-9 ? 0 : side > 0 ? -1 : 1);
    angle("angle", x, y, from, sweep);
  };
  auto direction = [](double au, double av, double bu, double bv) { return std::atan2(bv - av, bu - au); };
  if (chain) {
    const SkPoint* p = pointOf(m_chain.back());
    if (p && std::hypot(cu - p->x, cv - p->y) > 1e-12) polar("length", p->x, p->y, angleReference());
    return out;
  }
  const Snap& a = m_clicks[0];
  if (m_clicks.size() == 1) {
    if (std::hypot(cu - a.u, cv - a.v) < 1e-12) return out;
    if (m_tool == "rect" || m_tool == "crect") {  // the width under the rectangle, the height beside it
      const double w = std::fabs(cu - a.u), h = std::fabs(cv - a.v);
      const double x0 = m_tool == "crect" ? a.u - w : std::min(a.u, cu), x1 = m_tool == "crect" ? a.u + w : std::max(a.u, cu);
      const double y0 = m_tool == "crect" ? a.v - h : std::min(a.v, cv), y1 = m_tool == "crect" ? a.v + h : std::max(a.v, cv);
      Readout width, height;
      width.key = "width", width.locked = typed("width"), width.text = length(x1 - x0, width.locked, {});
      height.key = "height", height.locked = typed("height"), height.text = length(y1 - y0, height.locked, {});
      place(width, (x0 + x1) / 2, y0, 0, -1, 6);
      place(height, x1, (y0 + y1) / 2, 1, 0, 6);
      out.push_back(width);
      out.push_back(height);
      return out;
    }
    if (m_tool == "tangent_arc") {  // its radius from the centre, its sweep about it
      double au = a.u, av = a.v, tu = 1, tv = 0;
      if (!tangentStart(au, av, tu, tv)) return out;
      const double off = (cu - au) * -tv + (cv - av) * tu, side = off < 0 ? -1 : 1, d2 = (cu - au) * (cu - au) + (cv - av) * (cv - av);
      const auto r = m_typedValues.find("radius");
      const double radius = r != m_typedValues.end() ? std::fabs(r->second) : std::fabs(off) > 1e-12 ? d2 / (2 * std::fabs(off)) : 0;
      if (radius < 1e-12) return out;
      const double ou = au - side * tv * radius, ov = av + side * tu * radius, from = direction(ou, ov, au, av);
      along("radius", ou, ov, cu, cv, QStringLiteral("R "), 1, true);
      angle("sweep", ou, ov, from, side * shapeinput::turned(from, direction(ou, ov, cu, cv), side));  // on from the line: past half a turn too
      return out;
    }
    if (m_tool == "circle") {
      along(m_circleRadius ? "radius" : "diameter", a.u, a.v, cu, cv, m_circleRadius ? QStringLiteral("R ") : QStringLiteral("Ø "), m_circleRadius ? 1 : 2, true);
      return out;
    }
    if (m_tool == "circle2") polar("diameter", a.u, a.v, 0, QStringLiteral("Ø "), 1, true);
    else if (m_tool == "polygon" || m_tool == "polygon_outer") polar("diameter", a.u, a.v, 0, m_tool == "polygon" ? QStringLiteral("Ø ") : QString(), 2, true);
    else if (m_tool == "arcc" || m_tool == "arcslot" || m_tool == "ellipse") polar("radius", a.u, a.v, 0, QStringLiteral("R "), 1, true);
    else if (m_tool == "cslot") {
      along("length", 2 * a.u - cu, 2 * a.v - cv, cu, cv, {}, 1, true);
      angle("angle", a.u, a.v, 0, std::remainder(direction(a.u, a.v, cu, cv), 2 * M_PI));
    } else polar("length", a.u, a.v, 0, {}, 1, true);
    return out;
  }
  const Snap& b = m_clicks[1];
  const double bx = b.u - a.u, by = b.v - a.v, l = std::hypot(bx, by);
  if (l < 1e-12) return out;
  const double nx = -by / l, ny = bx / l, off = (cu - a.u) * nx + (cv - a.v) * ny;  // the pointer off the line through the clicks
  if (m_tool == "rect3") along("height", b.u, b.v, b.u + nx * off, b.v + ny * off, {}, 1, true);
  else if (m_tool == "ellipse") along("minor", a.u, a.v, a.u + nx * off, a.v + ny * off, QStringLiteral("R "), 1, true);
  else if (m_tool == "slot" || m_tool == "cslot") {  // across the second cap's centre, the value past the cap
    along("width", b.u - nx * off, b.v - ny * off, b.u + nx * off, b.v + ny * off, {}, 1, true);
    if (!out.empty()) place(out.back(), b.u + bx / l * std::fabs(off), b.v + by / l * std::fabs(off), bx / l, by / l, 6);
  }
  else if (m_tool == "arc3" || m_tool == "circle3") {
    const double d = 2 * (a.u * (b.v - cv) + b.u * (cv - a.v) + cu * (a.v - b.v));
    if (std::fabs(d) < 1e-12) return out;
    const double ux = ((a.u * a.u + a.v * a.v) * (b.v - cv) + (b.u * b.u + b.v * b.v) * (cv - a.v) + (cu * cu + cv * cv) * (a.v - b.v)) / d;
    const double uy = ((a.u * a.u + a.v * a.v) * (cu - b.u) + (b.u * b.u + b.v * b.v) * (a.u - cu) + (cu * cu + cv * cv) * (b.u - a.u)) / d;
    along("radius", ux, uy, cu, cv, QStringLiteral("R "), 1, true);
  } else if (m_tool == "arcc" || m_tool == "arcslot") {  // the sweep from the start, the way the pointer went (or typed): past half a turn too
    const auto it = m_typedValues.find("sweep");
    const double sweep = slotSweep(cu, cv);
    angle("sweep", a.u, a.v, direction(a.u, a.v, b.u, b.v), it != m_typedValues.end() ? it->second : sweep);
  }
  return out;
}
