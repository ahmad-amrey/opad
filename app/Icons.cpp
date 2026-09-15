#include "Icons.hpp"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <cmath>

#include "Theme.hpp"
#include "opad/cache.hpp"

namespace {

// Inner SVG markup per icon, copied verbatim from opad-icons.js.
const QHash<QString, QString>& table() {
  static const QHash<QString, QString> t = {
      {"open", R"(<path d="M3 6h6l2 2h10v12H3z"/><path d="M3 11h18"/>)"},
      {"import", R"(<path d="M12 3v11M8 10l4 4 4-4M4 17v3h16v-3"/>)"},
      {"save", R"(<path d="M5 3h11l3 3v15H5z"/><path d="M8 3v5h7V3M8 21v-6h8v6"/>)"},
      {"export", R"(<path d="M12 14V3M8 7l4-4 4 4M4 17v3h16v-3"/>)"},
      {"fit", R"(<path d="M3 8V3h5M16 3h5v5M21 16v5h-5M8 21H3v-5"/><rect x="8" y="8" width="8" height="8"/>)"},
      {"home", R"(<path d="M3 11l9-8 9 8M5 10v10h14V10"/>)"},
      {"ortho", R"(<path d="M4 8h12v12H4zM4 8l4-4h12v12l-4 4M16 8l4-4"/>)"},
      {"shaded", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".3"/>)"},
      {"shadedEdges", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".3"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
      {"wireframe", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4zM4 8l8 4 8-4M12 12v8"/><path d="M4 16l8-4 8 4M12 4v8" opacity=".45"/>)"},
      {"grid", R"(<rect x="3" y="3" width="18" height="18"/><path d="M3 9h18M3 15h18M9 3v18M15 3v18"/>)"},
      {"section", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M2 15L22 5" stroke-dasharray="3 2"/>)"},
      {"isolate", R"(<circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3"/>)"},
      {"showAll", R"(<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>)"},
      {"distance", R"(<path d="M3 12h18M3 8v8M21 8v8M8 10l-3 2 3 2M16 10l3 2-3 2"/>)"},
      {"angle", R"(<path d="M4 20L18 6M4 20h16"/><path d="M13 20a9 9 0 0 0-2.6-6.4"/>)"},
      {"radius", R"(<circle cx="12" cy="12" r="9"/><path d="M12 12l6.4-6.4"/><circle cx="12" cy="12" r="1.2" fill="currentColor"/>)"},
      {"bbox", R"(<rect x="4" y="8" width="12" height="12" stroke-dasharray="3 2"/><path d="M4 8l4-4h12v12l-4 4M16 8l4-4"/>)"},
      {"annotate", R"(<path d="M4 4h16v11H10l-4 4v-4H4z"/><path d="M8 9h8M8 12h5"/>)"},
      {"pin", R"(<path d="M9 3h6v6l3 3H6l3-3zM12 12v9"/>)"},
      {"rename", R"(<path d="M12 4v16M9 4h6M9 20h6M4 9h4v6H4M20 9h-4v6h4"/>)"},
      {"hide", R"(<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="3"/><path d="M4 4l16 16"/>)"},
      {"eye", R"(<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>)"},
      {"lock", R"(<rect x="5" y="11" width="14" height="10"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>)"},
      {"delete", R"(<path d="M4 7h16M9 7V4h6v3M6 7l1 14h10l1-14M10 11v6M14 11v6"/>)"},
      {"restore", R"(<path d="M3 12a9 9 0 1 0 3-6.7M3 4v5h5"/>)"},
      {"search", R"(<circle cx="11" cy="11" r="6"/><path d="M20 20l-4.5-4.5"/>)"},
      {"settings", R"(<circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="3"/><path d="M12 3v2M12 19v2M3 12h2M19 12h2M5.6 5.6l1.4 1.4M18.4 5.6l-1.4 1.4M18.4 18.4l-1.4-1.4M5.6 18.4l1.4-1.4"/>)"},
      {"git", R"(<circle cx="6" cy="5" r="2"/><circle cx="6" cy="19" r="2"/><circle cx="18" cy="8" r="2"/><path d="M6 7v10M18 10c0 5-12 3-12 7"/>)"},
      {"move", R"(<path d="M12 2v20M2 12h20M9 5l3-3 3 3M9 19l3 3 3-3M5 9l-3 3 3 3M19 9l3 3-3 3"/>)"},
      {"reparent", R"(<path d="M4 4h6v5H4zM14 15h6v5h-6zM7 9v9h7"/>)"},
      {"doc", R"(<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/>)"},
      {"component", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
      {"body", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".35"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
      {"chevronRight", R"(<path d="M9 6l6 6-6 6"/>)"},
      {"chevronDown", R"(<path d="M6 9l6 6 6-6"/>)"},
      {"chevronUp", R"(<path d="M6 15l6-6 6 6"/>)"},
      {"close", R"(<path d="M6 6l12 12M18 6L6 18"/>)"},
      {"float", R"(<rect x="3" y="7" width="14" height="14"/><path d="M7 3h14v14"/>)"},
      {"min", R"(<path d="M5 12h14"/>)"},
      {"max", R"(<rect x="5" y="5" width="14" height="14"/>)"},
      {"flip", R"(<path d="M12 3v18M9 8L4 12l5 4zM15 8l5 4-5 4z"/>)"},
      {"warning", R"(<path d="M12 3l10 18H2z"/><path d="M12 10v4M12 18h.01"/>)"},
      {"check", R"(<path d="M5 12l5 5L20 7"/>)"},
      {"dot", R"(<circle cx="12" cy="12" r="3" fill="currentColor"/>)"},
      {"recent", R"(<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 3"/>)"},
      {"step", R"(<path d="M6 4h12M6 20h12M12 4v16"/><path d="M8 8l4 4-4 4M16 8l-4 4 4 4" opacity=".5"/>)"},
      {"plus", R"(<path d="M12 5v14M5 12h14"/>)"},
      {"more", R"(<circle cx="5" cy="12" r="1.5" fill="currentColor"/><circle cx="12" cy="12" r="1.5" fill="currentColor"/><circle cx="19" cy="12" r="1.5" fill="currentColor"/>)"},
      {"commit", R"(<circle cx="12" cy="12" r="4"/><path d="M2 12h6M16 12h6"/>)"},
      {"browse", R"(<path d="M3 6h6l2 2h10v12H3z"/><circle cx="14" cy="14" r="2.5"/><path d="M16 16l2 2"/>)"},
      {"filterBodies", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".35"/>)"},
      {"filterFaces", R"(<path d="M4 8l8-4 8 4-8 4z" fill="currentColor" fill-opacity=".5"/><path d="M4 8v8l8 4 8-4V8M12 12v8" opacity=".4"/>)"},
      {"filterEdges", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" opacity=".35"/><path d="M4 8l8 4"/>)"},
      {"filterVertices", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4zM4 8l8 4 8-4M12 12v8" opacity=".35"/><circle cx="12" cy="12" r="2.5" fill="currentColor"/>)"},
  };
  return t;
}

// ---------------------------------------------------------------- SVG path data -> QPainterPath
struct PathParser {
  QString d;
  int i = 0;
  QPainterPath path;
  QPointF cur, start, lastCtrl;
  QChar lastCmd;

  void skip() {
    while (i < d.size() && (d[i].isSpace() || d[i] == ',')) ++i;
  }
  bool number(double& out) {
    skip();
    int s = i;
    if (i < d.size() && (d[i] == '-' || d[i] == '+')) ++i;
    bool digits = false;
    while (i < d.size() && (d[i].isDigit() || d[i] == '.')) { ++i; digits = true; }
    if (i < d.size() && (d[i] == 'e' || d[i] == 'E')) {
      ++i;
      if (i < d.size() && (d[i] == '-' || d[i] == '+')) ++i;
      while (i < d.size() && d[i].isDigit()) ++i;
    }
    if (!digits) { i = s; return false; }
    out = d.mid(s, i - s).toDouble();
    return true;
  }
  bool flag(int& out) {
    skip();
    if (i < d.size() && (d[i] == '0' || d[i] == '1')) { out = d[i] == '1'; ++i; return true; }
    return false;
  }

  void arc(double rx, double ry, double rot, int large, int sweep, QPointF to) {
    // SVG endpoint parameterisation -> centre parameterisation (W3C implementation notes F.6.5).
    if (rx == 0 || ry == 0 || to == cur) { path.lineTo(to); cur = to; return; }
    double phi = rot * M_PI / 180.0, cph = std::cos(phi), sph = std::sin(phi);
    double dx = (cur.x() - to.x()) / 2, dy = (cur.y() - to.y()) / 2;
    double x1 = cph * dx + sph * dy, y1 = -sph * dx + cph * dy;
    rx = std::fabs(rx); ry = std::fabs(ry);
    double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) { rx *= std::sqrt(lambda); ry *= std::sqrt(lambda); }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coef = (large != sweep ? 1 : -1) * std::sqrt(std::max(0.0, num / den));
    double cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
    double cx = cph * cx1 - sph * cy1 + (cur.x() + to.x()) / 2, cy = sph * cx1 + cph * cy1 + (cur.y() + to.y()) / 2;
    auto ang = [](double ux, double uy, double vx, double vy) {
      double dot = ux * vx + uy * vy, len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
      double a = std::acos(std::clamp(dot / len, -1.0, 1.0));
      return (ux * vy - uy * vx) < 0 ? -a : a;
    };
    double theta1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double dtheta = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && dtheta > 0) dtheta -= 2 * M_PI;
    else if (sweep && dtheta < 0) dtheta += 2 * M_PI;
    // Flatten into cubic segments in the rotated frame.
    int segs = std::max(1, static_cast<int>(std::ceil(std::fabs(dtheta) / (M_PI / 2))));
    double delta = dtheta / segs;
    double t = 4.0 / 3.0 * std::tan(delta / 4);
    double th = theta1;
    for (int s = 0; s < segs; ++s) {
      double c1 = std::cos(th), s1 = std::sin(th), c2 = std::cos(th + delta), s2 = std::sin(th + delta);
      QPointF p1(c1 - t * s1, s1 + t * c1), p2(c2 + t * s2, s2 - t * c2), p3(c2, s2);
      auto map = [&](QPointF p) { double x = p.x() * rx, y = p.y() * ry; return QPointF(cph * x - sph * y + cx, sph * x + cph * y + cy); };
      path.cubicTo(map(p1), map(p2), map(p3));
      th += delta;
    }
    cur = to;
  }

  void parse() {
    while (true) {
      skip();
      if (i >= d.size()) break;
      QChar c = d[i];
      if (c.isLetter()) { lastCmd = c; ++i; }
      else if (lastCmd.isNull()) break;
      QChar cmd = lastCmd;
      bool rel = cmd.isLower();
      QChar u = cmd.toUpper();
      auto pt = [&](double x, double y) { return rel ? cur + QPointF(x, y) : QPointF(x, y); };
      double a, b, c2, d2, e, f;
      if (u == 'M') {
        if (!number(a) || !number(b)) break;
        cur = pt(a, b); start = cur; path.moveTo(cur); lastCmd = rel ? 'l' : 'L';
      } else if (u == 'L') {
        if (!number(a) || !number(b)) break;
        cur = pt(a, b); path.lineTo(cur);
      } else if (u == 'H') {
        if (!number(a)) break;
        cur = QPointF(rel ? cur.x() + a : a, cur.y()); path.lineTo(cur);
      } else if (u == 'V') {
        if (!number(a)) break;
        cur = QPointF(cur.x(), rel ? cur.y() + a : a); path.lineTo(cur);
      } else if (u == 'C') {
        if (!number(a) || !number(b) || !number(c2) || !number(d2) || !number(e) || !number(f)) break;
        QPointF p1 = pt(a, b), p2 = pt(c2, d2), p3 = pt(e, f);
        path.cubicTo(p1, p2, p3); lastCtrl = p2; cur = p3;
      } else if (u == 'S') {
        if (!number(a) || !number(b) || !number(c2) || !number(d2)) break;
        QPointF p1 = (lastCmd.toUpper() == 'S' || lastCmd.toUpper() == 'C') ? cur * 2 - lastCtrl : cur;
        QPointF p2 = pt(a, b), p3 = pt(c2, d2);
        path.cubicTo(p1, p2, p3); lastCtrl = p2; cur = p3;
      } else if (u == 'A') {
        int large, sweep;
        if (!number(a) || !number(b) || !number(c2) || !flag(large) || !flag(sweep) || !number(e) || !number(f)) break;
        arc(a, b, c2, large, sweep, pt(e, f));
      } else if (u == 'Z') {
        path.closeSubpath(); cur = start;
      } else {
        break;
      }
      if (u != 'C' && u != 'S') lastCtrl = cur;
      if (u == 'Z') lastCmd = QChar();
    }
  }
};

double attr_num(const QString& attrs, const char* name, double def) {
  QRegularExpression re(QString("\\b%1=\"([^\"]*)\"").arg(name));
  auto m = re.match(attrs);
  return m.hasMatch() ? m.captured(1).toDouble() : def;
}
QString attr_str(const QString& attrs, const char* name) {
  QRegularExpression re(QString("\\b%1=\"([^\"]*)\"").arg(name));
  auto m = re.match(attrs);
  return m.hasMatch() ? m.captured(1) : QString();
}

void render(QPainter& p, const QString& markup, const QColor& color) {
  static const QRegularExpression el("<(path|rect|circle)([^>]*?)/?>");
  auto it = el.globalMatch(markup);
  while (it.hasNext()) {
    auto m = it.next();
    QString tag = m.captured(1), attrs = m.captured(2);
    QPainterPath path;
    if (tag == "path") {
      PathParser pp{attr_str(attrs, "d")};
      pp.parse();
      path = pp.path;
    } else if (tag == "rect") {
      path.addRect(attr_num(attrs, "x", 0), attr_num(attrs, "y", 0), attr_num(attrs, "width", 0), attr_num(attrs, "height", 0));
    } else {
      double r = attr_num(attrs, "r", 0);
      path.addEllipse(QPointF(attr_num(attrs, "cx", 0), attr_num(attrs, "cy", 0)), r, r);
    }
    p.save();
    p.setOpacity(attr_num(attrs, "opacity", 1.0));
    QString fill = attr_str(attrs, "fill");
    if (fill == "currentColor") {
      QColor fc = color;
      fc.setAlphaF(attr_num(attrs, "fill-opacity", 1.0));
      p.fillPath(path, fc);
    }
    QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QString dash = attr_str(attrs, "stroke-dasharray");
    if (!dash.isEmpty()) {
      QVector<qreal> pattern;
      for (const QString& v : dash.split(' ', Qt::SkipEmptyParts)) pattern << v.toDouble() / 2.0;
      pen.setDashPattern(pattern);
    }
    p.strokePath(path, pen);
    p.restore();
  }
}

QHash<QString, QPixmap> g_cache;

}  // namespace

namespace icons {

bool has(const QString& name) { return table().contains(name); }

QPixmap pixmap(const QString& name, const QColor& color, int size, qreal dpr) {
  QString key = QString("%1|%2|%3|%4").arg(name, color.name(QColor::HexArgb)).arg(size).arg(dpr);
  auto it = g_cache.find(key);
  if (it != g_cache.end()) return *it;
  QPixmap pm(static_cast<int>(size * dpr), static_cast<int>(size * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  if (table().contains(name)) {
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    render(p, table()[name], color);
  }
  g_cache.insert(key, pm);
  return pm;
}

QIcon icon(const QString& name, const QColor& normal, const QColor& disabled, const QColor& selected, int size) {
  QIcon ic;
  for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
    ic.addPixmap(pixmap(name, normal, size, dpr), QIcon::Normal);
    ic.addPixmap(pixmap(name, disabled.isValid() ? disabled : normal, size, dpr), QIcon::Disabled);
    ic.addPixmap(pixmap(name, selected.isValid() ? selected : normal, size, dpr), QIcon::Selected);
    ic.addPixmap(pixmap(name, normal, size, dpr), QIcon::Active);
  }
  return ic;
}

QIcon themed(const QString& name, int size) {
  const Tokens& t = theme::current();
  return icon(name, t.fg, t.fg3, t.onsel, size);
}

void clearCache() { g_cache.clear(); }

QString file(const QString& name, const QColor& color, int size) {
  QString dir = QString::fromStdString((opad::cache_dir() / "ui-icons").string());
  QDir().mkpath(dir);
  QString path = QString("%1/%2-%3-%4.png").arg(dir, name, color.name(QColor::HexArgb).mid(1)).arg(size);
  if (!QFile::exists(path)) pixmap(name, color, size, 2.0).save(path);
  return path.replace('\\', '/');
}

}  // namespace icons
