// Move / copy's triad (TODO 11 P2): what its guide shows. Red, green and blue arrows along X, Y and Z and a square in the
// middle sit on the picked bodies (the middle of their cached view boxes, moved as the panel's values move them); pulling an
// arrow sets that distance, the square moves in the view's plane, each value rounded to a step that suits the zoom. The
// preview follows the pull at the pace plans come back (as the extrude's arrow), and the value boxes show beside the
// pointer during it, the pulled arrow's box taking the next value typed. Nothing here walks a shape: the boxes are the
// view's cached ones.
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

// The axis colours (the origin's X, Y and Z): red, green, blue.
QColor axisColour(int k, const Tokens& t) {
  if (k == 0) return t.red;
  if (k == 1) return t.green;
  return t.dark ? QColor("#4c8df6") : QColor("#1f5fd0");
}
}  // namespace

bool DesignController::pulling() const { return (m_distanceHandle && m_distanceHandle->dragging()) || m_pull.part >= 0; }

double DesignController::lengthInput(const QString& name, double fallback) const {
  try {
    std::vector<ParamDef> defs;
    for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    return ParamTable(defs, m_doc->scene.units).as(Dim::Length, m_form->valueText(name).trimmed().toStdString());
  } catch (const std::exception&) {
    return fallback;
  }
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
  // Where the move takes it: turned first (about an origin axis; another axis leaves the triad unturned), then moved.
  const opad::json inputs = m_form->inputs();
  if (inputs.value("rotate", false) && inputs.value("axis", opad::json()).is_object() && inputs["axis"].contains("base")) {
    const std::string base = inputs["axis"].value("base", "z");
    try {
      std::vector<ParamDef> defs;
      for (const auto& p : m_doc->scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
      const double angle = ParamTable(defs, m_doc->scene.units).as(Dim::Angle, m_form->valueText("angle").trimmed().toStdString());
      gp_Trsf turn;
      turn.SetRotation(gp_Ax1(gp::Origin(), base == "x" ? gp::DX() : base == "y" ? gp::DY() : gp::DZ()), angle);
      at.Transform(turn);
    } catch (const std::exception&) {
    }
  }
  const opad::Vec3 centre{at.X() + lengthInput("dx", 0), at.Y() + lengthInput("dy", 0), at.Z() + lengthInput("dz", 0)};
  if (!m_triad) m_triad = std::make_unique<TranslateTriad>(m_viewport);
  const Tokens& t = m_viewport->tokens();
  m_triad->place(centre, {{{1, 0, 0}, axisColour(0, t)}, {{0, 1, 0}, axisColour(1, t)}, {{0, 0, 1}, axisColour(2, t)}});
}

// The viewport's mouse events, before the view picks with them: a press on an arrow or the square pulls (a press that does
// not move is a click on that arrow: its box takes what is typed next); hovering lights the part under the pointer.
bool DesignController::triadEvent(QEvent* event) {
  if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonRelease) return false;
  if (!m_triad->shown() && m_pull.part < 0) return false;
  auto* mouse = static_cast<QMouseEvent*>(event);
  const QPointF at = mouse->position();
  auto valuesNear = [this](int part, const QPointF& where) {
    if (part >= 1 && part <= 3) m_values->showNear(where.toPoint(), kAxes[part - 1]);
    else if (part == 0) m_values->showNear(where.toPoint(), QString());
  };
  switch (event->type()) {
    case QEvent::MouseButtonPress: {
      if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier) return false;
      const int part = m_triad->partAt(at);
      if (part < 0) return false;
      m_pull = {};
      m_pull.part = part;
      for (int k = 0; k < 3; ++k) m_pull.start[k] = lengthInput(kAxes[k], 0);
      m_triad->begin(part, at);
      return true;
    }
    case QEvent::MouseMove: {
      if (m_pull.part < 0) {
        if (mouse->buttons() == Qt::NoButton) m_triad->setHover(m_triad->partAt(at));
        return false;
      }
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
      valuesNear(m_pull.part, at);
      return true;
    }
    case QEvent::MouseButtonRelease: {
      if (mouse->button() != Qt::LeftButton || m_pull.part < 0) return false;
      const int part = m_pull.part;
      m_pull = {};
      m_triad->end();
      valuesNear(part, at);  // a click on an arrow too: its box takes the next value typed
      return true;
    }
    default: return false;
  }
}
