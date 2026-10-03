// SVG writer for drawings (TODO 11 UI-86), by hand: Qt Svg is not part of the build, and the core has no Qt. Millimetres
// throughout (width and height in mm, a mm viewBox), y flipped once by writing -y; one group per layer, which Inkscape
// shows as a named layer and OPAD's reader imports as one; exact arcs and ellipses as arc commands or their own elements.
#include <cmath>
#include <cstdio>

#include "writer_common.hpp"

namespace opad::drawing {
namespace {

using detail::num;

std::string xml(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (static_cast<unsigned char>(c) >= 0x20 || c == '\t') out += c;
  }
  return out;
}

std::string colour(uint32_t rgb) {
  if (rgb == kInk || rgb == kByLayer) return "#000000";
  char buf[8];
  std::snprintf(buf, sizeof buf, "#%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
  return buf;
}

class Writer {
 public:
  Writer(const Display& d, int decimals) : m_d(d), m_dec(decimals) {}
  std::string run();

 private:
  std::string n(double v) const { return num(v, m_dec); }
  std::string xy(Vec2 p) const { return n(p[0]) + ' ' + n(-p[1]); }
  // One curve as path commands (from its own moveto), or false when it is a full circle or ellipse (an element).
  bool path(const Curve& c, std::string& d) const;
  void element(const Curve& c, const std::string& extra);
  void text(const Prim& p);

  const Display& m_d;
  int m_dec;
  std::string m_out;
};

bool Writer::path(const Curve& c, std::string& d) const {
  switch (c.type) {
    case Curve::Type::Arc:
    case Curve::Type::Ellipse: {
      if (detail::full_turn(c)) return false;
      const double cr = std::cos(c.rot), sr = std::sin(c.rot);
      auto at = [&](double t) {
        const double x = c.r1 * std::cos(t), y = c.r2 * std::sin(t);
        return c.type == Curve::Type::Arc ? Vec2{c.c[0] + c.r1 * std::cos(t), c.c[1] + c.r1 * std::sin(t)}
                                          : Vec2{c.c[0] + x * cr - y * sr, c.c[1] + x * sr + y * cr};
      };
      const double rot = c.type == Curve::Type::Arc ? 0 : -c.rot * 180 / M_PI;
      // Counter-clockwise on the sheet is counter-clockwise on the screen too, which SVG's y-down angles call sweep 0.
      d += "M" + xy(at(c.a0)) + "A" + n(c.r1) + ' ' + n(c.type == Curve::Type::Arc ? c.r1 : c.r2) + ' ' + n(rot) + ' ' +
           (c.a1 - c.a0 > M_PI ? "1" : "0") + " 0 " + xy(at(c.a1));
      return true;
    }
    case Curve::Type::Spline: {
      const auto pieces = c.beziers(1e-3);
      if (pieces.empty()) break;
      d += "M" + xy(pieces.front()[0]);
      for (const auto& b : pieces) d += "C" + xy(b[1]) + ' ' + xy(b[2]) + ' ' + xy(b[3]);
      return true;
    }
    default: break;
  }
  const auto& pts = c.type == Curve::Type::Spline ? c.sample(1e-3) : c.pts;
  if (pts.size() < 2) return true;
  const bool closed = pts.size() > 3 && std::hypot(pts.back()[0] - pts.front()[0], pts.back()[1] - pts.front()[1]) < 1e-9;
  d += "M" + xy(pts[0]) + "L";
  for (size_t i = 1; i + (closed ? 1 : 0) < pts.size(); ++i) d += (i > 1 ? " " : "") + xy(pts[i]);
  if (closed) d += "Z";
  return true;
}

void Writer::element(const Curve& c, const std::string& extra) {
  if (c.type == Curve::Type::Arc || std::abs(c.r1 - c.r2) <= 1e-12 * c.r1) {
    m_out += "<circle cx=\"" + n(c.c[0]) + "\" cy=\"" + n(-c.c[1]) + "\" r=\"" + n(c.r1) + "\"" + extra + "/>\n";
    return;
  }
  m_out += "<ellipse cx=\"" + n(c.c[0]) + "\" cy=\"" + n(-c.c[1]) + "\" rx=\"" + n(c.r1) + "\" ry=\"" + n(c.r2) + "\"";
  if (std::abs(c.rot) > 1e-12) m_out += " transform=\"rotate(" + n(-c.rot * 180 / M_PI) + ' ' + n(c.c[0]) + ' ' + n(-c.c[1]) + ")\"";
  m_out += extra + "/>\n";
}

void Writer::text(const Prim& p) {
  // The cap height is the drawing's text height; SVG sizes the em (Arial's capitals are 0.716 of it).
  const double em = p.height / 0.716;
  const char* anchor = p.halign == 1 ? "middle" : p.halign == 2 ? "end" : "start";
  const std::string fill = p.rgb == kByLayer ? std::string() : " fill=\"" + colour(p.rgb) + "\"";
  for (const auto& line : detail::text_lines(p)) {
    if (line.text.find_first_not_of(' ') == std::string::npos) continue;
    m_out += "<text x=\"" + n(line.at[0]) + "\" y=\"" + n(-line.at[1]) + "\" font-size=\"" + n(em) + "\" text-anchor=\"" + anchor + "\"" + fill;
    if (std::abs(p.angle) > 1e-12) m_out += " transform=\"rotate(" + n(-p.angle * 180 / M_PI) + ' ' + n(line.at[0]) + ' ' + n(-line.at[1]) + ")\"";
    m_out += ">" + xml(line.text) + "</text>\n";
  }
}

std::string Writer::run() {
  const auto b = m_d.bounds();
  const double margin = 2 * m_d.pen_scale;
  const double w = std::max(b[2] - b[0], 1e-3) + 2 * margin, h = std::max(b[3] - b[1], 1e-3) + 2 * margin;
  m_out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  m_out += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" version=\"1.1\" width=\"" + n(w) +
           "mm\" height=\"" + n(h) + "mm\" viewBox=\"" + n(b[0] - margin) + ' ' + n(-b[3] - margin) + ' ' + n(w) + ' ' + n(h) +
           "\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
  if (!m_d.title.empty()) m_out += "<title>" + xml(m_d.title) + "</title>\n";
  for (size_t li = 0; li < m_d.layers.size(); ++li) {
    const Layer& l = m_d.layers[li];
    bool used = false;
    for (const auto& p : m_d.prims) used = used || p.layer == static_cast<int>(li);
    if (!used) continue;
    m_out += "<g id=\"layer" + std::to_string(li + 1) + "\" inkscape:groupmode=\"layer\" inkscape:label=\"" + xml(l.name) + "\" stroke=\"" + colour(l.rgb) +
             "\" stroke-width=\"" + n(l.width * m_d.pen_scale) + "\"";
    const auto& dashes = line_type_dashes(l.line);
    if (!dashes.empty()) {
      m_out += " stroke-dasharray=\"";
      for (size_t i = 0; i < dashes.size(); ++i) m_out += (i ? " " : "") + n(std::max(std::abs(dashes[i]), 0.0) * m_d.pen_scale);
      m_out += "\"";
    }
    m_out += " fill=\"" + colour(l.rgb) + "\">\n";
    // Curves in the layer's colour share one path; full circles and ellipses are elements; the rest one by one.
    std::string d;
    for (const auto& p : m_d.prims) {
      if (p.layer != static_cast<int>(li)) continue;
      const std::string own = p.rgb == kByLayer || p.rgb == l.rgb ? std::string() : colour(p.rgb);
      if (p.kind == Prim::Kind::Curve) {
        std::string piece;
        if (!path(p.curve, piece)) element(p.curve, own.empty() ? " fill=\"none\"" : " fill=\"none\" stroke=\"" + own + "\"");
        else if (own.empty()) d += (d.empty() ? "" : "\n") + piece;
        else m_out += "<path d=\"" + piece + "\" fill=\"none\" stroke=\"" + own + "\"/>\n";
      } else if (p.kind == Prim::Kind::Fill) {
        std::string f;
        for (const auto& loop : p.loops) {
          f += "M" + xy(loop[0]) + "L";
          for (size_t i = 1; i < loop.size(); ++i) f += (i > 1 ? " " : "") + xy(loop[i]);
          f += "Z";
        }
        m_out += "<path d=\"" + f + "\" stroke=\"none\" fill-rule=\"evenodd\"" + (own.empty() ? std::string() : " fill=\"" + own + "\"") + "/>\n";
      } else if (p.kind == Prim::Kind::Text) {
        m_out += "<g stroke=\"none\" font-family=\"Arial, Helvetica, sans-serif\">\n";
        text(p);
        m_out += "</g>\n";
      } else {
        const auto& q = p.corners;
        m_out += "<image width=\"1\" height=\"1\" preserveAspectRatio=\"" + xml(p.fit) + "\" transform=\"matrix(" + n(q[1][0] - q[0][0]) + ' ' +
                 n(-(q[1][1] - q[0][1])) + ' ' + n(q[2][0] - q[0][0]) + ' ' + n(-(q[2][1] - q[0][1])) + ' ' + n(q[0][0]) + ' ' + n(-q[0][1]) +
                 ")\" href=\"" + xml(p.text) + "\"/>\n";
      }
    }
    if (!d.empty()) m_out += "<path fill=\"none\" d=\"" + d + "\"/>\n";
    m_out += "</g>\n";
  }
  m_out += "</svg>\n";
  return std::move(m_out);
}

}  // namespace

std::string svg_text(const Display& d, int decimals) { return Writer(d, decimals).run(); }

}  // namespace opad::drawing
