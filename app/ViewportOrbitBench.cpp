// OPAD_BENCH_ORBITPIVOT (registered in DesignOriginBench.cpp): the orbit pivot over a big drawing (UI-51).
#include "Viewport.hpp"

#include <QElapsedTimer>

#include <cmath>

#include "Jobs.hpp"

bool Viewport::benchOrbitPivot(const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: orbit pivot: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  if (!m_initialised) return false;
  fitAll();
  m_view->Redraw();
  size_t segments = 0, runs = 0;
  for (const auto& [id, item] : m_items)
    if (const auto p = m_prs.find(item.key); p != m_prs.end()) segments += p->second->drawingSegments.size() / 2, runs += p->second->segmentRuns.size();
  require(segments >= 100000 && runs * 256 >= segments, QString("%1 segments in %2 runs").arg(segments).arg(runs));
  // What a scan of every segment finds (the press's old way), against the runs.
  auto everySegment = [this](const QPointF& cursor, double& distance) {
    distance = 1e100;
    for (const auto& [id, item] : m_items) {
      const auto p = m_prs.find(item.key);
      if (p == m_prs.end() || !m_ctx->IsDisplayed(item.ais)) continue;
      const auto& points = p->second->drawingSegments;
      for (size_t i = 0; i + 1 < points.size(); i += 2) {
        const gp_Pnt a = points[i].Transformed(item.ais->Transformation()), b = points[i + 1].Transformed(item.ais->Transformation());
        const QPointF pa = widgetPoint({a.X(), a.Y(), a.Z()}), pb = widgetPoint({b.X(), b.Y(), b.Z()}), d = pb - pa;
        const double len = QPointF::dotProduct(d, d), t = len > 0 ? std::clamp(QPointF::dotProduct(cursor - pa, d) / len, 0.0, 1.0) : 0;
        const QPointF delta = pa + d * t - cursor;
        distance = std::min(distance, QPointF::dotProduct(delta, delta));
      }
    }
  };
  double slowest = 0, scan = 0;
  for (const QPointF at : {QPointF(6, 6), QPointF(width() - 6.0, 6), QPointF(6, height() - 6.0), QPointF(width() / 2.0, 3), QPointF(width() - 3.0, height() / 2.0), QPointF(width() / 2.0, height() / 2.0)}) {
    QElapsedTimer clock;
    clock.start();
    bool found = false;
    double distance = 0, full = 0;
    nearestCurvePoint(at, found, distance);
    const double fast = clock.nsecsElapsed() / 1e6;
    clock.restart();
    everySegment(at, full);
    scan = std::max(scan, clock.nsecsElapsed() / 1e6);
    slowest = std::max(slowest, fast);
    require(found && std::abs(std::sqrt(distance) - std::sqrt(full)) < 1e-6, QString("at %1,%2 the nearest curve point is %3 px away, as a scan of every segment finds (%4 px)")
                                                                                    .arg(at.x()).arg(at.y()).arg(std::sqrt(distance), 0, 'f', 3).arg(std::sqrt(full), 0, 'f', 3));
  }
  require(slowest < 15, QString("found run by run in %1 ms at most (a scan of every segment: %2 ms)").arg(slowest, 0, 'f', 2).arg(scan, 0, 'f', 2));
  QElapsedTimer clock;
  clock.start();
  orbitPoint(devicePos(QPointF(6, 6)));
  const double press = clock.nsecsElapsed() / 1e6;
  require(press < 30, QString("an orbit press away from the drawing picks its pivot in %1 ms").arg(press, 0, 'f', 2));
  grabImage().save(prefix + ".png");
  return all;
}
