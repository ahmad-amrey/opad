#include "CoordinateReadout.hpp"

#include <QEvent>
#include <QMouseEvent>

#include <cmath>

#include "Theme.hpp"
#include "Units.hpp"
#include "Viewport.hpp"

CoordinateReadout::CoordinateReadout(Viewport* view, std::function<bool(opad::Frame&)> sketchFrame, QWidget* parent)
    : QLabel(parent), m_view(view), m_sketchFrame(std::move(sketchFrame)) {
  setObjectName("coordinateReadout");
  setFont(theme::mono(11));
  setLayoutDirection(Qt::LeftToRight);
  setMinimumWidth(fontMetrics().horizontalAdvance("3D  X -0000.000  Y -0000.000  Z -0000.000"));  // the usual widest; longer values grow it
  setAccessibleName(tr("Cursor coordinates"));
  m_timer.setSingleShot(true);
  m_timer.setInterval(30);  // after the frame that ran the detection under the mouse
  connect(&m_timer, &QTimer::timeout, this, [this] { updateAt(m_pos); });
  connect(units::notifier(), &units::Notifier::changed, this, [this] { if (m_source != Source::None) updateAt(m_pos); });
  view->installEventFilter(this);
}

bool CoordinateReadout::eventFilter(QObject* o, QEvent* e) {
  if (o == m_view && e->type() == QEvent::MouseMove) {
    m_pos = static_cast<QMouseEvent*>(e)->position();
    if (!m_timer.isActive()) m_timer.start();
  } else if (o == m_view && e->type() == QEvent::Leave) {
    m_timer.stop();
    show(Source::None, {0, 0, 0}, QString());
  }
  return QLabel::eventFilter(o, e);
}

void CoordinateReadout::updateAt(const QPointF& pos) {
  m_pos = pos;
  double u = 0, v = 0;
  opad::Frame frame;
  if (m_sketchFrame && m_sketchFrame(frame)) {
    if (m_view->planePoint(pos, frame, u, v)) return show(Source::Sketch, {u, v, 0}, "xy");
    return show(Source::None, {0, 0, 0}, QString());
  }
  opad::Vec3 snap, hit;
  const bool snapped = m_view->shownSnap(snap);  // exact: the point an object snap shows (UI-90)
  QString name;
  if (m_drawing && m_drawing(pos, snapped ? &snap : nullptr, hit, name)) return show(Source::Drawing, hit, "xy", snapped, name);
  if (snapped) return show(Source::Model, snap, "xyz", true);
  if (!m_view->twoDimensional() && (m_view->detectedPoint(hit) || m_view->pointUnder(pos, hit))) return show(Source::Model, hit, "xyz");
  if (m_view->twoDimensional()) {  // the view plane through the origin: the two axes in it when it is a standard one
    const opad::Vec3 d = m_view->viewDirection();
    const opad::Vec3 up = std::abs(d[2]) > 0.9 ? opad::Vec3{0, 1, 0} : opad::Vec3{0, 0, 1};
    frame.x = {up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0]};
    frame.y = up;
    if (m_view->planePoint(pos, frame, u, v)) {
      const opad::Vec3 p = frame.to_world(u, v);
      const QString axes = std::abs(d[2]) > 0.999 ? "xy" : std::abs(d[1]) > 0.999 ? "xz" : std::abs(d[0]) > 0.999 ? "yz" : "xyz";
      return show(Source::View, p, axes);
    }
  } else if (m_view->planePoint(pos, opad::Frame{}, u, v)) {
    return show(Source::Plane, {u, v, 0}, "xyz");
  }
  show(Source::None, {0, 0, 0}, QString());
}

void CoordinateReadout::show(Source source, const opad::Vec3& p, const QString& axes, bool snapped, const QString& name) {
  m_source = source;
  m_point = p;
  m_snapped = snapped;
  if (source == Source::None) {
    clear();
    setToolTip(QString());
    return;
  }
  QStringList parts;
  for (int i = 0; i < 3; ++i)
    if (axes.contains(QChar('x' + i))) parts << QString("%1 %2").arg(QChar('X' + i)).arg(units::number(units::Kind::Length, p[i]));
  const QString tag = source == Source::Model    ? QString("3D")
                      : source == Source::Plane  ? QString("XY")
                      : source == Source::Sketch ? tr("Sketch")
                      : source == Source::Drawing ? tr("Drawing")
                                                  : QString("2D");
  setText(QChar(0x202A) + tag + "  " + parts.join("  ") + QChar(0x202C));  // left to right inside a right-to-left UI
  QString tip = source == Source::Model     ? tr("The cursor on the model, in model coordinates")
                : source == Source::Plane   ? tr("Nothing under the cursor: where it meets the XY plane")
                : source == Source::Sketch  ? tr("The cursor in the sketch's own X and Y")
                : source == Source::Drawing ? tr("Cursor position in the coordinates of %1, as its file has them").arg(name)
                                            : tr("The cursor on the view plane");
  if (snapped) tip += "\n" + tr("At the snapped point");
  setToolTip(tip);
}
