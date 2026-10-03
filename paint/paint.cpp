// Qt painting of 2D drawings (TODO 11 UI-86, UI-87): PDF and PNG of a drawing::Display through one QPainter backend.
#include "opad/drawing/paint.hpp"

#include <QFile>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPageLayout>
#include <QPageSize>
#include <QPainterPath>
#include <QPdfWriter>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "opad/drawing/sheet.hpp"
#include "opad/util.hpp"

namespace opad::drawing {
namespace {

QColor colour(uint32_t rgb) {
  if (rgb == kInk || rgb == kByLayer) return Qt::black;
  return QColor(static_cast<int>((rgb >> 16) & 255), static_cast<int>((rgb >> 8) & 255), static_cast<int>(rgb & 255));
}

QPointF pt(Vec2 v) { return {v[0], v[1]}; }
QString qpath(const std::filesystem::path& file) { return QString::fromStdU16String(file.u16string()); }
std::string name_of(const std::filesystem::path& file) { return qpath(file.filename()).toStdString(); }

void add_curve(QPainterPath& path, const Curve& c, double tol) {
  if (c.type == Curve::Type::Line || c.type == Curve::Type::Polyline) {
    if (c.pts.size() < 2) return;
    path.moveTo(pt(c.pts[0]));
    for (size_t i = 1; i < c.pts.size(); ++i) path.lineTo(pt(c.pts[i]));
    return;
  }
  const auto pieces = c.beziers(tol);
  if (pieces.empty()) return;
  path.moveTo(pt(pieces.front()[0]));
  for (const auto& b : pieces) path.cubicTo(pt(b[1]), pt(b[2]), pt(b[3]));
  if ((c.type == Curve::Type::Arc || c.type == Curve::Type::Ellipse) && c.a1 - c.a0 >= 2 * M_PI - 1e-9) path.closeSubpath();
}

// The layer's pen in drawing units; Qt counts dashes in pen widths (a dot: a dash too short to see, round capped).
QPen pen_of(const Layer& l, double pen_scale, const QColor& c) {
  const double w = std::max(l.width, 0.05) * pen_scale;
  QPen pen(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  const auto& dashes = line_type_dashes(l.line);
  if (!dashes.empty()) {
    QList<qreal> pattern;
    for (double v : dashes) pattern << std::max(std::abs(v) * pen_scale / w, 1e-3);
    pen.setDashPattern(pattern);
  }
  return pen;
}

QFont text_font() {
  QFont f(QStringLiteral("Arial"));
  f.setStyleHint(QFont::SansSerif);
  f.setPixelSize(1000);
  return f;
}

// Text lines laid out as the writers lay them (text_lines), each drawn in its own frame: the anchor in device units,
// turned by the text's angle, scaled so the font's capitals are the text height.
void paint_text(QPainter& p, const Prim& t, const QColor& c, const QTransform& fit, const QTransform& base) {
  const QFont font = text_font();
  const QFontMetricsF fm(font, p.device());
  const double cap = fm.capHeight() > 0 ? fm.capHeight() : 0.716 * fm.height();
  const double k = t.height * std::hypot(fit.m11(), fit.m12()) / cap;
  p.setFont(font);
  p.setPen(c);
  p.setBrush(Qt::NoBrush);
  for (const auto& line : text_lines(t)) {
    if (line.text.find_first_not_of(' ') == std::string::npos) continue;
    const QString s = QString::fromUtf8(line.text.data(), static_cast<qsizetype>(line.text.size()));
    const double w = fm.horizontalAdvance(s);
    const QPointF at = fit.map(pt(line.at));
    QTransform local;
    local.translate(at.x(), at.y());
    local.rotate(-t.angle * 180 / M_PI);
    local.scale(k, k);
    p.setTransform(local * base);
    p.drawText(QPointF(t.halign == 1 ? -w / 2 : t.halign == 2 ? -w : 0, 0), s);
  }
  p.setTransform(base);
}

QImage image_of(const std::string& href) {
  QImage img;
  if (href.rfind("data:", 0) == 0) {
    const size_t comma = href.find(',');
    if (comma != std::string::npos && href.substr(0, comma).find(";base64") != std::string::npos)
      img.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(href.substr(comma + 1))));
  } else if (!href.empty()) {
    img.load(qpath(path_from_utf8(href)));
  }
  return img;
}

// An image on its corners (top-left, top-right, bottom-left), its aspect kept unless fit says "none" (SVG's default
// is xMidYMid meet).
void paint_image(QPainter& p, const Prim& prim, const QTransform& fit, const QTransform& base) {
  const QImage img = image_of(prim.text);
  if (img.isNull()) return;
  QPointF o = fit.map(pt(prim.corners[0])), u = fit.map(pt(prim.corners[1])) - o, v = fit.map(pt(prim.corners[2])) - o;
  const double lu = std::hypot(u.x(), u.y()), lv = std::hypot(v.x(), v.y());
  if (lu <= 0 || lv <= 0) return;
  if (prim.fit.rfind("none", 0) != 0) {
    const double want = static_cast<double>(img.width()) / std::max(1, img.height()), have = lu / lv;
    if (have > want) {
      o += u * (1 - want / have) / 2;
      u *= want / have;
    } else {
      o += v * (1 - have / want) / 2;
      v *= have / want;
    }
  }
  p.setTransform(QTransform(u.x(), u.y(), v.x(), v.y(), o.x(), o.y()) * base);
  p.drawImage(QRectF(0, 0, 1, 1), img);
  p.setTransform(base);
}

}  // namespace

void paint(QPainter& p, const Display& d, const std::array<double, 4>& window, const QRectF& target) {
  const double ww = std::max(window[2] - window[0], 1e-9), wh = std::max(window[3] - window[1], 1e-9);
  const double s = std::min(target.width() / ww, target.height() / wh);
  const double ox = target.center().x() - s * (window[0] + window[2]) / 2, oy = target.center().y() + s * (window[1] + window[3]) / 2;
  const QTransform fit(s, 0, 0, -s, ox, oy), base = p.transform();
  const double ps = d.pen_scale > 0 ? d.pen_scale : 1, tol = 1e-3 * ps;
  p.save();
  p.setLayoutDirection(Qt::LayoutDirectionAuto);
  for (size_t li = 0; li < d.layers.size(); ++li) {
    const Layer& layer = d.layers[li];
    const int index = static_cast<int>(li);
    // A path a curve: a path a layer makes no smaller a PDF, and the raster engine takes minutes over one of a hundred
    // thousand dashed curves (the Engine's front view: 265 s as PNG, 10 s a curve at a time). Fills one by one too:
    // their loops are even-odd among themselves, not with other fills.
    std::map<uint32_t, std::vector<const Curve*>> curves;
    std::vector<std::pair<uint32_t, QPainterPath>> fills;
    std::vector<const Prim*> texts;
    for (const auto& prim : d.prims) {
      if (prim.layer != index) continue;
      const uint32_t rgb = prim.rgb == kByLayer ? layer.rgb : prim.rgb;
      if (prim.kind == Prim::Kind::Curve) {
        curves[rgb].push_back(&prim.curve);
      } else if (prim.kind == Prim::Kind::Text) {
        texts.push_back(&prim);
      } else if (prim.kind == Prim::Kind::Image) {
        paint_image(p, prim, fit, base);
      } else {
        QPainterPath f;
        f.setFillRule(Qt::OddEvenFill);
        for (const auto& loop : prim.loops) {
          f.moveTo(pt(loop[0]));
          for (size_t i = 1; i < loop.size(); ++i) f.lineTo(pt(loop[i]));
          f.closeSubpath();
        }
        fills.emplace_back(rgb, std::move(f));
      }
    }
    p.setTransform(fit * base);
    p.setPen(Qt::NoPen);
    for (const auto& [rgb, path] : fills) {
      p.setBrush(colour(rgb));
      p.drawPath(path);
    }
    p.setBrush(Qt::NoBrush);
    for (const auto& [rgb, list] : curves) {
      p.setPen(pen_of(layer, ps, colour(rgb)));
      for (const Curve* c : list) {
        QPainterPath path;
        add_curve(path, *c, tol);
        p.drawPath(path);
      }
    }
    p.setTransform(base);
    for (const Prim* t : texts) paint_text(p, *t, colour(t->rgb == kByLayer ? layer.rgb : t->rgb), fit, base);
  }
  p.restore();
}

Page page_for(const Display& d, double margin, bool standard) {
  const double ps = d.pen_scale > 0 ? d.pen_scale : 1;
  Page page;
  if (d.has_paper()) {  // its own sheet, named when it is a standard one
    page.w = (d.paper[2] - d.paper[0]) / ps, page.h = (d.paper[3] - d.paper[1]) / ps;
    page.window = d.paper;
    for (const auto& paper : paper_sizes())
      if (std::abs(std::min(page.w, page.h) - paper.w) < 0.5 && std::abs(std::max(page.w, page.h) - paper.h) < 0.5) page.paper = paper.name;
    return page;
  }
  const auto b = d.bounds();
  page.w = (b[2] - b[0]) / ps + 2 * margin;
  page.h = (b[3] - b[1]) / ps + 2 * margin;
  if (standard)
    for (const auto& paper : paper_sizes()) {
      if (std::string(paper.name).rfind("ANSI", 0) == 0) continue;  // ISO, smallest first
      const bool landscape = page.w > page.h;
      const double w = landscape ? paper.h : paper.w, h = landscape ? paper.w : paper.h;
      if (page.w <= w + 1e-9 && page.h <= h + 1e-9) {
        page.w = w, page.h = h, page.paper = paper.name;
        break;
      }
    }
  const double cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2;
  page.window = {cx - page.w * ps / 2, cy - page.h * ps / 2, cx + page.w * ps / 2, cy + page.h * ps / 2};
  return page;
}

json write_pdf(const std::vector<const Display*>& pages, const std::filesystem::path& file) {
  if (pages.empty()) throw Error("nothing to write");
  QFile out(qpath(file));
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw Error("cannot write " + name_of(file));
  json report = json::array();
  {
    QPdfWriter pdf(&out);
    static const std::map<std::string, QPageSize::PageSizeId> ids = {
        {"A4", QPageSize::A4},         {"A3", QPageSize::A3},         {"A2", QPageSize::A2},         {"A1", QPageSize::A1},
        {"A0", QPageSize::A0},         {"ANSI-A", QPageSize::AnsiA}, {"ANSI-B", QPageSize::AnsiB}, {"ANSI-C", QPageSize::AnsiC},
        {"ANSI-D", QPageSize::AnsiD}, {"ANSI-E", QPageSize::AnsiE}};
    const auto layout = [&](const Page& page) {
      const auto id = ids.find(page.paper);
      const QPageSize size = id != ids.end() ? QPageSize(id->second)
                                             : QPageSize(QSizeF(std::min(page.w, page.h), std::max(page.w, page.h)), QPageSize::Millimeter, QString(), QPageSize::ExactMatch);
      return QPageLayout(size, page.w > page.h ? QPageLayout::Landscape : QPageLayout::Portrait, QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
    };
    pdf.setResolution(1200);
    pdf.setTitle(QString::fromStdString(pages[0]->title));
    pdf.setCreator(QStringLiteral("OPAD ") + QString::fromStdString(version_string()));
    QPainter p;
    for (size_t i = 0; i < pages.size(); ++i) {
      const Display& d = *pages[i];
      const Page page = page_for(d);
      pdf.setPageLayout(layout(page));  // before the page it is for: the first before begin(), the others before newPage()
      if (i == 0 ? !p.begin(&pdf) : !pdf.newPage()) throw Error("cannot write " + name_of(file));
      p.setRenderHint(QPainter::Antialiasing);
      const double dots = 1200 / 25.4;
      paint(p, d, page.window, QRectF(0, 0, page.w * dots, page.h * dots));
      report.push_back({{"page", {page.w, page.h}}, {"paper", page.paper}, {"scale", scale_text(1 / (d.pen_scale > 0 ? d.pen_scale : 1))}});
    }
    p.end();
  }
  out.close();
  if (out.error() != QFileDevice::NoError) throw Error("cannot write " + name_of(file));
  if (report.size() == 1) return report[0];
  return {{"pages", report}};
}

QImage paint_image(const Display& d, double dpi) {
  const Page page = page_for(d, 2, false);
  double per_mm = std::max(dpi, 1.0) / 25.4;
  per_mm = std::min({per_mm, 16384 / std::max(page.w, 1e-9), 16384 / std::max(page.h, 1e-9), std::sqrt(50e6 / std::max(page.w * page.h, 1e-9))});
  QImage img(std::max(1, static_cast<int>(std::lround(page.w * per_mm))), std::max(1, static_cast<int>(std::lround(page.h * per_mm))), QImage::Format_ARGB32_Premultiplied);
  if (img.isNull()) throw Error("not enough memory for the picture");
  img.fill(Qt::white);
  img.setDotsPerMeterX(static_cast<int>(std::lround(per_mm * 1000)));
  img.setDotsPerMeterY(static_cast<int>(std::lround(per_mm * 1000)));
  QPainter p(&img);
  p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  paint(p, d, page.window, QRectF(0, 0, img.width(), img.height()));
  return img;
}

json write_png(const Display& d, const std::filesystem::path& file, double dpi) {
  const QImage img = paint_image(d, dpi);
  if (!img.save(qpath(file), "PNG")) throw Error("cannot write " + name_of(file));
  return {{"pixels", {img.width(), img.height()}}, {"dpi", std::lround(img.dotsPerMeterX() * 0.0254)}};
}

void install_painter() {
  set_paint_writer([](const std::vector<const Display*>& pages, const std::filesystem::path& file, const std::string& format, const json& options) {
    {
      static std::mutex mu;
      std::lock_guard<std::mutex> lock(mu);
      if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        if (QCoreApplication::instance()) throw Error("PDF and PNG need a Qt GUI application");
        static int argc = 1;
        static char name[] = "opad";
        static char* argv[] = {name, nullptr};
        // Offscreen: no display needed; Windows falls back to its own plugin where only that one is deployed.
#ifdef _WIN32
        if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen;windows");
#else
        if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
        new QGuiApplication(argc, argv);  // for the rest of the process
      }
    }
    if (format == "pdf") return write_pdf(pages, file);
    if (format == "png" && pages.size() == 1) return write_png(*pages[0], file, options.value("dpi", 300.0));
    throw Error("the painter writes pdf (pages) and png (one), not " + format);
  });
}

}  // namespace opad::drawing
