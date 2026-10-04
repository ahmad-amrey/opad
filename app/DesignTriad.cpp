// Move / copy's triad (TODO 11 P2): what its guide shows. Red, green and blue arrows along X, Y and Z and a square in the
// middle sit on the picked bodies (the middle of their cached view boxes, moved as the panel's values move them); pulling an
// arrow sets that distance, the square moves in the view's plane, each value rounded to a step that suits the zoom. With
// Rotate on, a ring goes round the move's own axis through the bodies' middle (the path they take as the angle changes);
// pulling round it sets the angle in 5 degree steps. The preview follows a pull at the pace plans come back (as the extrude's
// arrow), and the value boxes show beside the pointer during it, the pulled part's box taking the next value typed. Nothing
// here walks a shape: the boxes are the view's cached ones, an axis other than X, Y or Z comes from the preview's worker.
#include <QMouseEvent>

#include <cmath>

#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "Theme.hpp"
#include "ToolValues.hpp"
#include "TranslateTriad.hpp"
#include "opad/geometry.hpp"

using namespace opad::design;

namespace {
const char* const kAxes[3] = {"dx", "dy", "dz"};
constexpr double kRingStep = 5;  // degrees a pull round the ring snaps to
constexpr double kRingMin = 56;  // widget pixels: the ring's radius when the axis runs through the bodies

// The axis colours (the origin's X, Y and Z): red, green, blue.
QColor axisColour(int k, const Tokens& t) {
  if (k == 0) return t.red;
  if (k == 1) return t.green;
  return t.dark ? QColor("#4c8df6") : QColor("#1f5fd0");
}
}  // namespace

bool DesignController::pulling() const { return (m_distanceHandle && m_distanceHandle->dragging()) || m_pull.part >= 0; }

double DesignController::inputValue(const QString& name, Dim dim, double fallback) const {
  try {
    std::vector<ParamDef> defs;
    for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    return ParamTable(defs, m_doc->scene.units).as(dim, m_form->valueText(name).trimmed().toStdString());
  } catch (const std::exception&) {
    return fallback;
  }
}

// The axis a move with Rotate on turns about: an origin axis at once, another as the last preview's worker resolved it.
bool DesignController::moveAxis(gp_Ax1& axis) const {
  const opad::json input = m_form->inputs().value("axis", opad::json());
  if (input.is_object() && input.contains("base")) {
    const std::string base = input.value("base", "z");
    axis = gp_Ax1(gp::Origin(), base == "x" ? gp::DX() : base == "y" ? gp::DY() : gp::DZ());
    return true;
  }
  if (!m_moveAxis.valid || m_moveAxis.input != input) return false;
  axis = gp_Ax1(gp_Pnt(m_moveAxis.origin[0], m_moveAxis.origin[1], m_moveAxis.origin[2]), gp_Dir(m_moveAxis.dir[0], m_moveAxis.dir[1], m_moveAxis.dir[2]));
  return true;
}

void DesignController::placeMoveTriad() {
  const bool on = m_featureOn && !m_pickPlane && !m_sketch->active() && m_form->spec() && m_form->spec()->kind == "move";
  const opad::json picks = on ? m_form->picks("bodies") : opad::json();
  if (!on || !picks.is_array() || picks.empty()) {
    if (m_triad && m_pull.part < 0) m_triad->hide();
    return;
  }
  // The middle of the picked bodies as the view has them, before the move (the timeline is rolled back while editing).
  Bnd_Box box;
  for (const auto& pick : picks) try {
      const Bnd_Box b = opad::node_world_bbox(m_doc->doc, m_doc->scene, pick.value("body", ""));  // cached per key: O(1)
      if (!b.IsVoid()) box.Add(b);
    } catch (const std::exception&) {
    }
  if (box.IsVoid()) {
    if (m_triad && m_pull.part < 0) m_triad->hide();
    return;
  }
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  gp_Pnt at((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
  // Where the move takes it: turned first, about its axis, then moved.
  const gp_Vec shift(inputValue("dx", Dim::Length, 0), inputValue("dy", Dim::Length, 0), inputValue("dz", Dim::Length, 0));
  gp_Ax1 axis;
  const bool turning = m_form->inputs().value("rotate", false) && moveAxis(axis);
  if (turning) {
    gp_Trsf turn;
    turn.SetRotation(axis, inputValue("angle", Dim::Angle, 0));
    at.Transform(turn);
  }
  at.Translate(shift);
  if (!m_triad) m_triad = std::make_unique<TranslateTriad>(m_viewport);
  // The ring the middle travels along as the angle changes: round the moved axis, through the middle (a small ring round
  // the axis when it runs through the middle: the bodies turn in place).
  if (turning) {
    const gp_Pnt onAxis = axis.Location().Translated(shift);
    const gp_Vec d(axis.Direction());
    const gp_Pnt centre = onAxis.Translated(d * gp_Vec(onAxis, at).Dot(d));
    const double radius = std::max(centre.Distance(at), kRingMin * std::max(1e-9, m_viewport->pixelSize()));
    m_triad->setRing(true, {centre.X(), centre.Y(), centre.Z()}, {d.X(), d.Y(), d.Z()}, radius);
  } else {
    m_triad->setRing(false);
  }
  const Tokens& t = m_viewport->tokens();
  m_triad->place({at.X(), at.Y(), at.Z()}, {{{1, 0, 0}, axisColour(0, t)}, {{0, 1, 0}, axisColour(1, t)}, {{0, 0, 1}, axisColour(2, t)}});
}

// The viewport's mouse events, before the view picks with them: a press on an arrow, the square or the ring pulls (a press
// that does not move is a click on it: its box takes what is typed next); hovering lights the part under the pointer.
bool DesignController::triadEvent(QEvent* event) {
  if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonRelease) return false;
  if (!m_triad->shown() && m_pull.part < 0) return false;
  auto* mouse = static_cast<QMouseEvent*>(event);
  const QPointF at = mouse->position();
  auto valuesNear = [this](int part, const QPointF& where) {
    m_values->showNear(where.toPoint(), part >= 1 && part <= 3 ? QString(kAxes[part - 1]) : part == TranslateTriad::kRing ? QString("angle") : QString());
  };
  switch (event->type()) {
    case QEvent::MouseButtonPress: {
      if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier) return false;
      const int part = m_triad->partAt(at);
      if (part < 0) return false;
      m_pull = {};
      m_pull.part = part;
      for (int k = 0; k < 3; ++k) m_pull.start[k] = inputValue(kAxes[k], Dim::Length, 0);
      m_pull.angle = inputValue("angle", Dim::Angle, 0);
      m_triad->begin(part, at);
      return true;
    }
    case QEvent::MouseMove: {
      if (m_pull.part < 0) {
        if (mouse->buttons() == Qt::NoButton) m_triad->setHover(m_triad->partAt(at));
        return false;
      }
      if (m_pull.part == TranslateTriad::kRing) {
        const double degrees = std::round((m_pull.angle + m_triad->turn(at)) * 180 / M_PI / kRingStep) * kRingStep;
        const QString text = QString::number(degrees == 0 ? 0.0 : degrees, 'g', 12) + " deg";
        if (text != m_form->valueText("angle")) m_form->setValue("angle", text.toStdString());  // the preview follows
      } else {
        const double step = DimensionHandle::pullStep(m_viewport->pixelSize());
        opad::Vec3 move{0, 0, 0};
        if (m_pull.part > 0) move[m_pull.part - 1] = m_triad->along(at);
        else move = m_triad->inPlane(at);
        for (int k = 0; k < 3; ++k) {
          if (m_pull.part > 0 && k != m_pull.part - 1) continue;
          const double value = std::round((m_pull.start[k] + move[k]) / step) * step;
          const QString text = DimensionHandle::pulledText(value, step);
          if (text != m_form->valueText(kAxes[k])) m_form->setValue(kAxes[k], text.toStdString());  // the preview follows (schedulePreview)
        }
      }
      valuesNear(m_pull.part, at);
      return true;
    }
    case QEvent::MouseButtonRelease: {
      if (mouse->button() != Qt::LeftButton || m_pull.part < 0) return false;
      const int part = m_pull.part;
      m_pull = {};
      m_triad->end();
      valuesNear(part, at);  // a click on an arrow or the ring too: its box takes the next value typed
      placeMoveTriad();      // the ring again round where the bodies are now
      return true;
    }
    default: return false;
  }
}

// What the preview's worker found for the move's axis (feature_handles' ring entry): for an axis that is an edge, a face or a
// construction axis, so the triad turns with the bodies and the ring goes round the right line.
void DesignController::moveAxisResolved(const opad::json& handles, const opad::json& inputs) {
  if (!m_form->spec() || m_form->spec()->kind != "move") return;
  for (const auto& h : handles)
    if (h.value("ring", false)) {
      m_moveAxis = {true, inputs.value("axis", opad::json()), h.at("origin").get<opad::Vec3>(), h.at("axis").get<opad::Vec3>()};
      if (m_pull.part < 0) placeMoveTriad();
      return;
    }
}
