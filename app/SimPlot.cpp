#include "SimPlot.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "Theme.hpp"

namespace {
// Round steps for an axis: 1, 2 or 5 times a power of ten, about `count` of them over [lo, hi].
double niceStep(double lo, double hi, int count) {
  const double raw = (hi - lo) / std::max(1, count);
  if (!(raw > 0) || !std::isfinite(raw)) return 1;
  const double p = std::pow(10.0, std::floor(std::log10(raw)));
  for (double m : {1.0, 2.0, 5.0, 10.0})
    if (raw <= m * p) return m * p;
  return 10 * p;
}

QString number(double v) {
  if (v == 0) return "0";
  const double a = std::fabs(v);
  if (a >= 1e5 || a < 1e-3) return QString::number(v, 'g', 3);
  return QString::number(v, 'g', 4);
}
}  // namespace

SimPlot::SimPlot(QWidget* parent) : QWidget(parent) {
  setObjectName("simPlot");
  setMouseTracking(false);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void SimPlot::setData(std::vector<double> t, std::vector<Curve> curves) {
  m_t = std::move(t);
  m_curves = std::move(curves);
  m_cursor = std::min(m_cursor, int(m_t.size()) - 1);
  update();
}

void SimPlot::setCursor(int frame) {
  if (frame == m_cursor) return;
  m_cursor = frame;
  update();
}

QRect SimPlot::plotRect() const {
  const int lh = fontMetrics().height();
  return rect().adjusted(fontMetrics().horizontalAdvance("-00000.0") + 6, lh + 6, -8, -(lh + 8));
}

void SimPlot::pick(int x) {
  const QRect r = plotRect();
  if (m_t.size() < 2 || r.width() <= 0) return;
  const double t = m_t.front() + (m_t.back() - m_t.front()) * std::clamp(double(x - r.left()) / r.width(), 0.0, 1.0);
  const auto it = std::lower_bound(m_t.begin(), m_t.end(), t);
  int i = int(it - m_t.begin());
  if (i > 0 && (i == int(m_t.size()) || t - m_t[size_t(i - 1)] < m_t[size_t(i)] - t)) --i;
  emit framePicked(std::clamp(i, 0, int(m_t.size()) - 1));
}

void SimPlot::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) pick(int(e->position().x()));
}

void SimPlot::mouseMoveEvent(QMouseEvent* e) {
  if (e->buttons() & Qt::LeftButton) pick(int(e->position().x()));
}

void SimPlot::paintEvent(QPaintEvent*) {
  const Tokens& k = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), k.bg2);
  const QRect r = plotRect();
  const QFontMetrics fm = fontMetrics();
  if (m_t.size() < 2 || m_curves.empty() || r.width() < 20 || r.height() < 20) {
    p.setPen(k.fg3);
    p.drawText(rect(), Qt::AlignCenter, tr("Run a study to plot its results"));
    return;
  }
  double lo = INFINITY, hi = -INFINITY;
  for (const auto& c : m_curves)
    for (double v : c.v)
      if (std::isfinite(v)) lo = std::min(lo, v), hi = std::max(hi, v);
  if (!std::isfinite(lo)) lo = 0, hi = 1;
  if (hi - lo < 1e-12 * std::max(1.0, std::fabs(hi))) lo -= 1, hi += 1;
  const double pad = 0.06 * (hi - lo);
  lo -= pad, hi += pad;
  const double t0 = m_t.front(), t1 = m_t.back();
  auto X = [&](double t) { return r.left() + (t - t0) / (t1 - t0) * r.width(); };
  auto Y = [&](double v) { return r.bottom() - (v - lo) / (hi - lo) * r.height(); };
  // Grid and axis labels.
  p.setPen(QPen(k.line, 1));
  p.drawRect(r);
  const double ys = niceStep(lo, hi, 4);
  p.setFont(font());
  for (double v = std::ceil(lo / ys) * ys; v <= hi; v += ys) {
    const double y = Y(v);
    p.setPen(QPen(k.line, 1, Qt::DotLine));
    p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    p.setPen(k.fg3);
    p.drawText(QRectF(0, y - fm.height() / 2.0, r.left() - 4, fm.height()), Qt::AlignRight | Qt::AlignVCenter, number(std::fabs(v) < ys * 1e-9 ? 0 : v));
  }
  const double xs = niceStep(t0, t1, 5);
  for (double t = std::ceil(t0 / xs) * xs; t <= t1 + 1e-12; t += xs) {
    const double x = X(t);
    p.setPen(QPen(k.line, 1, Qt::DotLine));
    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    p.setPen(k.fg3);
    p.drawText(QRectF(x - 40, r.bottom() + 2, 80, fm.height()), Qt::AlignHCenter | Qt::AlignTop, number(t));
  }
  p.drawText(QRectF(r.right() - 60, r.bottom() + 2, 60, fm.height()), Qt::AlignRight | Qt::AlignTop, tr("t (s)"));
  // Curves.
  const QColor palette[] = {k.sel, k.amber, k.green, k.red, k.fg2};
  for (size_t c = 0; c < m_curves.size(); ++c) {
    QPainterPath path;
    bool started = false;
    const auto& v = m_curves[c].v;
    for (size_t i = 0; i < std::min(v.size(), m_t.size()); ++i) {
      if (!std::isfinite(v[i])) {
        started = false;
        continue;
      }
      const QPointF q(X(m_t[i]), Y(v[i]));
      if (started) path.lineTo(q);
      else path.moveTo(q), started = true;
    }
    p.setPen(QPen(palette[c % 5], 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
  }
  if (m_cursor >= 0 && size_t(m_cursor) < m_t.size()) {
    const double x = X(m_t[size_t(m_cursor)]);
    p.setPen(QPen(k.fg2, 1));
    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
  }
  // The legend above: each curve with its value at the cursor.
  int x = r.left();
  for (size_t c = 0; c < m_curves.size(); ++c) {
    p.fillRect(QRect(x, 4 + fm.height() / 2 - 2, 10, 4), palette[c % 5]);
    x += 14;
    QString text = m_curves[c].name;
    if (m_cursor >= 0 && size_t(m_cursor) < m_curves[c].v.size()) text += " " + number(m_curves[c].v[size_t(m_cursor)]);
    if (!m_curves[c].unit.isEmpty()) text += " " + m_curves[c].unit;
    p.setPen(k.fg2);
    const QString shown = fm.elidedText(text, Qt::ElideRight, std::max(40, r.right() - x));
    p.drawText(QPoint(x, 4 + fm.ascent()), shown);
    x += fm.horizontalAdvance(shown) + 12;
    if (x > r.right()) break;
  }
}
