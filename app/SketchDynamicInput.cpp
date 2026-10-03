// SketchEditor, typed values (TODO 11 UI-16): the boxes of the step that waits, the keys that type into them and using
// what was typed. Every numeric tool takes its numbers from the keyboard, before anything is picked too, from the view or
// from a tool panel; no digit reaches a window shortcut while a tool runs (5, 6 and 7 used to switch the display style).
// A point's typed values hold the rubber band as they are typed (the pointer gives what is not typed), so the preview, a
// click and Enter all put the point at the same place.
#include "SketchEditor.hpp"
#include "DimensionHandle.hpp"
#include "I18n.hpp"
#include "InputKeys.hpp"
#include "opad/design/expr.hpp"
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
QString liveAngle(double radians) {
  double degrees = std::remainder(radians * 180 / M_PI, 360.0);
  if (degrees <= -180 + 1e-9) degrees += 360;
  QString text = QString::number(degrees, 'f', 1);
  if (text == "-0.0") text = "0.0";
  return text + QStringLiteral("°");
}
// Tools that place points: the next one can be typed (X and Y; a polyline goes on by length and angle).
const QStringList kPointTools = {"point", "line", "spline", "rect", "crect", "circle", "circle2", "circle3", "arc3", "arcc", "polygon", "polygon_outer",
                                 "slot", "cslot", "arcslot", "ellipse", "conic", "rect3", "control_spline"};
// Option tools Enter applies, once there is something picked to apply them to.
const QStringList kApplied = {"offset", "chamfer", "move", "copy", "rotate", "scale", "rect_pattern", "polar_pattern"};
}  // namespace

QString SketchEditor::inputStep() const { return QStringLiteral("%1/%2/%3").arg(m_tool).arg(m_chain.size()).arg(m_clicks.size()); }

Entry SketchEditor::entry() const {
  if (m_entry && m_entryStep == inputStep()) return *m_entry;
  return (m_tool == "line" || m_tool == "spline") && !m_chain.empty() ? Entry::Polar : Entry::Absolute;
}

bool SketchEditor::inputBase(double& u, double& v) const {
  if (!kPointTools.contains(m_tool)) return false;
  if (m_tool == "line" || m_tool == "spline") {
    const SkPoint* p = m_chain.empty() ? nullptr : m_sk.point(m_chain.back());
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
  const SkPoint *a = m_sk.point(m_chain[m_chain.size() - 2]), *b = m_sk.point(m_chain.back());
  return a && b && std::hypot(b->x - a->x, b->y - a->y) > 1e-12 ? std::atan2(b->y - a->y, b->x - a->x) : 0;
}

QList<DynamicInput::Field> SketchEditor::inputStage() const {
  using Field = DynamicInput::Field;
  // Values the tool keeps as options (the panel's fields show the same): typed, they are set at once, also before the
  // curves are picked, and they stay for the next ones after Apply.
  auto value = [this](const char* key, const QString& label, const char* fallback) { return Field{key, label, option(key, fallback), true}; };
  if (m_tool == "offset") return {value("distance", tr("Distance"), "5 mm")};
  if (m_tool == "fillet" || m_tool == "tangent_circle") return {value("radius", tr("Radius"), "2 mm")};
  if (m_tool == "chamfer") return {value("first", tr("Distance 1"), "2 mm"), value("second", tr("Distance 2"), "2 mm")};
  if (m_tool == "move" || m_tool == "copy") return {value("dx", tr("ΔX"), "10 mm"), value("dy", tr("ΔY"), "0 mm")};
  if (m_tool == "rotate") return {value("angle", tr("Angle"), "45 deg"), value("cx", tr("Centre X"), "0 mm"), value("cy", tr("Centre Y"), "0 mm")};
  if (m_tool == "scale") return {value("scale", tr("Factor"), "2"), value("cx", tr("Centre X"), "0 mm"), value("cy", tr("Centre Y"), "0 mm")};
  if (m_tool == "rect_pattern") return {value("count", tr("Count"), "3"), value("dx", tr("Spacing X"), "10 mm"), value("rows", tr("Rows"), "1"), value("dy", tr("Spacing Y"), "10 mm")};
  if (m_tool == "polar_pattern") return {value("count", tr("Count"), "3"), value("angle", tr("Total angle"), "360 deg"), value("cx", tr("Centre X"), "0 mm"), value("cy", tr("Centre Y"), "0 mm")};
  if (m_tool == "heal") return {value("healTolerance", tr("Gap"), "0.05 mm")};
  if (!kPointTools.contains(m_tool)) return {};
  const double pixel = m_viewport->pixelSize(), u = m_haveCursor ? m_cursor.u : 0, v = m_haveCursor ? m_cursor.v : 0;
  double bu = 0, bv = 0;
  const bool base = inputBase(bu, bv);
  const QString tip = base ? tr("Typed first: # X and Y · @ ΔX and ΔY from the last point · 30<45 length and angle") : QString();
  auto field = [&](const char* key, const QString& label, const QString& live, const QString& chip = {}) { return Field{key, label, live, false, chip, tip}; };
  switch (base ? entry() : Entry::Absolute) {
    case Entry::Polar: {
      // A polyline's angle can be measured from its last segment: the switch after the box says which.
      const QString chip = m_tool == "line" && m_chain.size() >= 2 ? (m_angleRelative ? tr("∠ last line") : tr("∠ X axis")) : QString();
      return {field("length", tr("Length"), liveNumber(std::hypot(u - bu, v - bv), pixel)),
              field("angle", tr("Angle"), liveAngle(std::atan2(v - bv, u - bu) - angleReference()), chip)};
    }
    case Entry::Relative: return {field("dx", tr("ΔX"), liveNumber(u - bu, pixel)), field("dy", tr("ΔY"), liveNumber(v - bv, pixel))};
    case Entry::Absolute: break;
  }
  return {field("x", tr("X"), liveNumber(u, pixel)), field("y", tr("Y"), liveNumber(v, pixel))};
}

bool SketchEditor::appliesOnEnter() const { return kApplied.contains(m_tool) && !m_sel.empty() && (m_tool != "chamfer" || m_sk.point(m_sel.front())); }

// A key that types into the boxes: a value key while a tool runs (a tool without boxes drops it: it is never a window
// shortcut then), Tab and Shift+Tab (the view keeps the keyboard in a sketch), and a point's '@', '#' and '<'.
bool SketchEditor::typingKey(const QKeyEvent* e) const {
  if (!m_active || (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return false;
  if (e->key() == Qt::Key_Tab || e->key() == Qt::Key_Backtab) return true;
  const QString text = e->text();
  if (text.size() != 1 || m_tool == "select") return false;
  return inputkeys::valueChar(text.front().unicode()) || (inputkeys::entryChar(text.front().unicode()) && kPointTools.contains(m_tool));
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
  m_input->placeNear(m_haveCursor ? m_viewport->widgetPoint(m_frame.to_world(m_pointer.u, m_pointer.v)) : QPoint(m_viewport->width() / 2, m_viewport->height() / 2));
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
    for (int i = 0; i < m_input->count(); ++i) {
      const QString key = m_input->key(i);
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
        values[key] = key == "angle" ? table->angle(text.replace(QStringLiteral("°"), QStringLiteral(" deg")).toStdString()) : table->length(text.toStdString());
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
  Snap s;
  if (typed("length") || typed("angle")) {
    if (!base) return pointer;
    // A length alone: that far towards the pointer. An angle alone: along it as far as the pointer goes along it.
    const double du = pointer.u - bu, dv = pointer.v - bv;
    const double angle = typed("angle") ? value("angle", 0) + angleReference() : std::atan2(dv, du);
    const double along = du * std::cos(angle) + dv * std::sin(angle);
    const double length = typed("length") ? value("length", 0) : along > 1e-9 ? along : std::hypot(du, dv);
    s.u = bu + length * std::cos(angle);
    s.v = bv + length * std::sin(angle);
  } else if (typed("dx") || typed("dy")) {
    if (!base) return pointer;
    s.u = bu + value("dx", pointer.u - bu);
    s.v = bv + value("dy", pointer.v - bv);
  } else {
    s.u = value("x", pointer.u);
    s.v = value("y", pointer.v);
  }
  s.kind = Snap::Kind::Typed;
  if (std::hypot(s.u - pointer.u, s.v - pointer.v) < 1e-9) {  // where the pointer was pulled to anyway: its point and curve too
    s.point = pointer.point;
    s.entity = pointer.entity;
  }
  if (base) {  // still level with (or above) the last point: still horizontal (vertical)
    s.horizontal = pointer.horizontal && std::fabs(s.v - bv) < 1e-9;
    s.vertical = pointer.vertical && std::fabs(s.u - bu) < 1e-9;
  }
  return s;
}

void SketchEditor::forgetTyped() {
  if (m_input) m_input->used();
  m_typedValues.clear();
  m_entry.reset();
  m_cursor = m_pointer;
}

bool SketchEditor::useTyped(const Snap* at) {
  const auto fields = inputStage();
  if (fields.isEmpty() || !m_input->typed()) return false;
  if (fields.front().option) {
    // Set as they were typed. Enter applies the tool when something is picked, else the value waits for the pick (the
    // fillet's and the tangent circle's picks apply it themselves).
    forgetTyped();
    if (m_tool == "heal") applyTool();
    else if (appliesOnEnter()) applyTool();
    else if (kApplied.contains(m_tool)) emit status(tr("The value waits: pick what it applies to."));
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
  const Snap s = typedPoint(at ? *at : m_pointer);
  forgetTyped();
  click(s, Qt::AltModifier);  // exactly there: no grid, no inference beyond what still holds there
  updateInput();               // the next point's boxes (a chain's next point does not prompt anew)
  rebuild();
  emit changed();
  return true;
}
