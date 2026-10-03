// SketchEditor, typed values (TODO 11 UI-16): the boxes of the step that waits, the keys that type into them and using
// what was typed. Every numeric tool takes its numbers from the keyboard, before anything is picked too, from the view or
// from a tool panel; no digit reaches a window shortcut while a tool runs (5, 6 and 7 used to switch the display style).
#include "SketchEditor.hpp"
#include "DimensionHandle.hpp"
#include "I18n.hpp"
#include "InputKeys.hpp"
#include "opad/design/expr.hpp"
#include <QKeyEvent>
#include <algorithm>
#include <cmath>

using namespace opad::design;

namespace {
// What the pointer gives a box, to what a pixel tells apart at this zoom.
QString liveNumber(double value, double pixel) {
  const int decimals = std::clamp(int(std::ceil(-std::log10(std::max(1e-6, pixel * 2)) - 1e-9)), 0, 4);
  QString text = QString::number(value, 'f', decimals);
  if (text.startsWith('-') && text.toDouble() == 0) text.remove(0, 1);
  return text;
}
// Tools that place points: the next one can be typed (X and Y; a polyline goes on by length and angle).
const QStringList kPointTools = {"point", "line", "spline", "rect", "crect", "circle", "circle2", "circle3", "arc3", "arcc", "polygon", "polygon_outer",
                                 "slot", "cslot", "arcslot", "ellipse", "conic", "rect3", "control_spline"};
// Option tools Enter applies, once there is something picked to apply them to.
const QStringList kApplied = {"offset", "chamfer", "move", "copy", "rotate", "scale", "rect_pattern", "polar_pattern"};
}  // namespace

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
  if ((m_tool == "line" || m_tool == "spline") && !m_chain.empty())
    if (const SkPoint* p = m_sk.point(m_chain.back()))
      return {Field{"length", tr("Length"), liveNumber(std::hypot(u - p->x, v - p->y), pixel)},
              Field{"angle", tr("Angle"), QString::number(std::atan2(v - p->y, u - p->x) * 180 / M_PI, 'f', 1) + QStringLiteral("°")}};
  return {Field{"x", tr("X"), liveNumber(u, pixel)}, Field{"y", tr("Y"), liveNumber(v, pixel)}};
}

bool SketchEditor::appliesOnEnter() const { return kApplied.contains(m_tool) && !m_sel.empty() && (m_tool != "chamfer" || m_sk.point(m_sel.front())); }

// A key that types into the boxes: a value key while a tool runs (a tool without boxes drops it: it is never a window
// shortcut then), Tab and Shift+Tab (the view keeps the keyboard in a sketch).
bool SketchEditor::typingKey(const QKeyEvent* e) const {
  if (!m_active || (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) return false;
  if (e->key() == Qt::Key_Tab || e->key() == Qt::Key_Backtab) return true;
  const QString text = e->text();
  return text.size() == 1 && inputkeys::valueChar(text.front().unicode()) && m_tool != "select";
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

void SketchEditor::updateInput() {
  if (!m_input) return;
  const bool handle = m_dimensionHandle && m_dimensionHandle->isVisible();
  if (handle && m_input->typed()) m_input->used();  // the curves are picked: the box by the arrow shows the typed value
  m_input->setFields(m_active && m_visible && !handle ? inputStage() : QList<DynamicInput::Field>{});
  if (!m_input->count() || !(m_haveCursor || m_input->typed() || m_input->current() >= 0)) {
    if (m_input->isVisible()) m_input->hide();
    return;
  }
  m_input->placeNear(m_haveCursor ? m_viewport->widgetPoint(m_frame.to_world(m_cursor.u, m_cursor.v)) : QPoint(m_viewport->width() / 2, m_viewport->height() / 2));
  if (!m_input->isVisible()) {
    m_input->show();
    m_input->raise();
  }
}

bool SketchEditor::useTyped(const Snap* at) {
  const auto fields = inputStage();
  if (fields.isEmpty() || !m_input->typed()) return false;
  if (fields.front().option) {
    // Set as they were typed. Enter applies the tool when something is picked, else the value waits for the pick (the
    // fillet's and the tangent circle's picks apply it themselves).
    m_input->used();
    if (m_tool == "heal") applyTool();
    else if (appliesOnEnter()) applyTool();
    else if (kApplied.contains(m_tool)) emit status(tr("The value waits: pick what it applies to."));
    updateInput();
    emit changed();
    return true;
  }
  // The next point: the typed values win, the pointer gives the rest (a typed angle alone: as far along it as the pointer).
  const Snap cursor = at ? *at : m_cursor;
  double x = cursor.u, y = cursor.v;
  try {
    std::vector<ParamDef> defs;
    for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    const auto table = sketch_parameters(m_sk, ParamTable(defs, m_doc->scene.units));
    auto typed = [&](const char* key) { return m_input->text(key).toStdString(); };
    if (fields.front().key == "length") {
      const SkPoint* base = m_chain.empty() ? nullptr : m_sk.point(m_chain.back());
      if (!base) return false;
      const double du = cursor.u - base->x, dv = cursor.v - base->y;
      const double angle = typed("angle").empty() ? std::atan2(dv, du) : table.angle(typed("angle"));
      double length = std::hypot(du, dv);
      if (!typed("length").empty()) length = table.length(typed("length"));
      else if (!typed("angle").empty() && du * std::cos(angle) + dv * std::sin(angle) > 1e-9) length = du * std::cos(angle) + dv * std::sin(angle);
      x = base->x + length * std::cos(angle);
      y = base->y + length * std::sin(angle);
    } else {
      if (!typed("x").empty()) x = table.length(typed("x"));
      if (!typed("y").empty()) y = table.length(typed("y"));
    }
  } catch (const std::exception& e) {  // the typed values stay, to be put right
    emit status(i18n::t(QString::fromUtf8(e.what())));
    return true;
  }
  m_input->used();
  click(Snap{x, y}, Qt::AltModifier);  // exactly there: no inference, no grid
  rebuild();
  emit changed();
  return true;
}
