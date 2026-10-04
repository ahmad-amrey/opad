// Exploded views (UI-36): moving a part by hand. The selected part's DimensionHandle arrow is its first axis (its own
// direction, or X/Y/Z as the panel says) and takes typed values; this triad adds the two axes square to it and a square
// in the middle that moves the part in the view's plane, and a press on the part itself drags it the same way (a press
// that does not move is a click, handed to the view). Every move only changes the spec's manual offset of the part's
// unit (nothing is written); the part follows at once through the Explode look layer.
#include <QApplication>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "ExplodeArea.hpp"
#include "ToolPanel.hpp"
#include "TranslateTriad.hpp"
#include "Viewport.hpp"

namespace {
double dot(const opad::Vec3& a, const opad::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
opad::Vec3 cross(const opad::Vec3& a, const opad::Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
opad::Vec3 scaled(const opad::Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
opad::Vec3 plus(const opad::Vec3& a, const opad::Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
}  // namespace

// The two axes square to the first one: the world axes least along it, made square (X and Y for a part going up), each
// pointing the positive way of its largest component.
std::array<opad::Vec3, 2> Explode::crossAxes(const opad::Vec3& axis) {
  const double n = std::sqrt(dot(axis, axis));
  const opad::Vec3 a = n > 0 ? scaled(axis, 1 / n) : opad::Vec3{0, 0, 1};
  size_t k = 0;
  for (size_t i = 1; i < 3; ++i)
    if (std::abs(a[i]) < std::abs(a[k]) - 1e-9) k = i;
  opad::Vec3 e{0, 0, 0};
  e[k] = 1;
  opad::Vec3 u = plus(e, scaled(a, -dot(e, a)));
  u = scaled(u, 1 / std::sqrt(dot(u, u)));
  opad::Vec3 v = cross(a, u);
  size_t big = 0;
  for (size_t i = 1; i < 3; ++i)
    if (std::abs(v[i]) > std::abs(v[big]) + 1e-9) big = i;
  if (v[big] < 0) v = scaled(v, -1);
  return {u, v};
}

bool Explode::triadShown() const { return m_triad && m_triad->shown(); }

void Explode::placeTriad(bool shown, const opad::Vec3& at, const opad::Vec3& axis) {
  if (!m_triad) m_triad = std::make_unique<TranslateTriad>(services().viewport());
  if (!shown) {
    if (m_drag.part >= 0) return;  // the part being dragged is never dropped mid-way
    m_triad->hide();
    return;
  }
  m_triadAxes = crossAxes(axis);
  m_triad->place(at, {{m_triadAxes[0], {}}, {m_triadAxes[1], {}}});
}

int Explode::triadPart(const QPointF& at) const { return m_triad ? m_triad->partAt(at) : -1; }

QPointF Explode::triadPoint(int part) const { return m_triad ? m_triad->partPoint(part) : QPointF(); }

void Explode::beginDrag(int part, const QPointF& at) {
  if (m_dragUnit < 0 || m_dragUnit >= static_cast<int>(m_units.size()) || !m_triad) return;
  const opad::ExplodeUnit& u = m_units[static_cast<size_t>(m_dragUnit)];
  m_drag = {};
  m_drag.part = part;
  m_drag.unit = u.id;
  m_drag.scale = std::max(0.2, opad::explode_progress(u, m_t));  // the part moves this much of its offset at t
  if (const auto m = m_spec.offsets.find(u.id); m != m_spec.offsets.end()) m_drag.manual = m->second;
  if (part > 0) m_drag.axis = m_triadAxes[static_cast<size_t>(part - 1)];  // along an arrow, else in the view's plane through the part
  m_triad->begin(part, at);
  hideHint(true);
  placeHandle();
}

void Explode::dragTo(const QPointF& at) {
  if (m_drag.part < 0) return;
  const auto unit = std::find_if(m_units.begin(), m_units.end(), [&](const opad::ExplodeUnit& u) { return u.id == m_drag.unit; });
  if (unit == m_units.end()) return endDrag();  // laid out again without it
  const opad::Vec3 move = m_drag.part > 0 ? scaled(m_drag.axis, m_triad->along(at)) : m_triad->inPlane(at);
  const opad::Vec3 manual = plus(m_drag.manual, scaled(move, 1 / m_drag.scale));
  if (dot(manual, manual) < 1e-18) m_spec.offsets.erase(m_drag.unit);
  else m_spec.offsets[m_drag.unit] = manual;
  apply();
}

void Explode::endDrag() {
  if (m_drag.part < 0) return;
  m_drag = {};
  if (m_triad) m_triad->end();
  opad::explode_stage(m_units, m_spec);  // one after another: its turn now that it is let go
  apply();
  services().browser()->refreshDecorations();
}

// The viewport's mouse events, before the view and the DimensionHandle see them (an application filter installed after
// the handle's).
bool Explode::dragEvent(QEvent* event) {
  Viewport* view = services().viewport();
  if (m_replaying) return false;
  auto* mouse = static_cast<QMouseEvent*>(event);
  const QPointF at = mouse->position();
  switch (event->type()) {
    case QEvent::MouseButtonPress: {
      if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier || !triadShown() || view->ghostsPickable()) return false;
      if (const int part = triadPart(at); part >= 0) {
        beginDrag(part, at);
        return true;
      }
      if (m_handle->grips(at)) return false;  // its arrow: the handle's
      // On the selected part itself: a drag moves it in the view's plane, a press that does not move is a click.
      opad::Ref ref;
      if (!view->referenceAt(at, ref) || ref.body.empty() || unitOf(ref.body) != m_dragUnit) return false;
      m_pressAt = at;
      m_pressGlobal = mouse->globalPosition();
      m_bodyArmed = true;
      return true;
    }
    case QEvent::MouseMove:
      if (m_drag.part >= 0) {
        dragTo(at);
        return true;
      }
      if (m_bodyArmed) {
        if ((at - m_pressAt).manhattanLength() >= QApplication::startDragDistance()) {
          m_bodyArmed = false;
          beginDrag(0, m_pressAt);
          dragTo(at);
        }
        return true;
      }
      if (mouse->buttons() == Qt::NoButton && triadShown()) m_triad->setHover(triadPart(at));
      return false;
    case QEvent::MouseButtonRelease:
      if (mouse->button() != Qt::LeftButton) return false;
      if (m_drag.part >= 0) {
        endDrag();
        return true;
      }
      if (m_bodyArmed) {  // a click after all: the view gets it as it came
        m_bodyArmed = false;
        QMouseEvent press(QEvent::MouseButtonPress, m_pressAt, m_pressGlobal, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, m_pressAt, m_pressGlobal, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        m_replaying = true;
        QCoreApplication::sendEvent(view, &press);
        QCoreApplication::sendEvent(view, &release);
        m_replaying = false;
        return true;
      }
      return false;
    default: return false;
  }
}
