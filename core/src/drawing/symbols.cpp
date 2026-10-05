// Drafting symbols drawn as vectors (TODO 11 UI-79, UI-80, UI-81): glyphs in text, centre marks and lines, leaders, datum
// feature symbols, feature control frames, surface texture symbols and ordinate dimensions.
#include "opad/drawing/symbols.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace opad::drawing {
namespace {

Vec2 add(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }
Vec2 sub(Vec2 a, Vec2 b) { return {a[0] - b[0], a[1] - b[1]}; }
Vec2 mul(Vec2 a, double s) { return {a[0] * s, a[1] * s}; }
double dot(Vec2 a, Vec2 b) { return a[0] * b[0] + a[1] * b[1]; }
double len(Vec2 a) { return std::hypot(a[0], a[1]); }
Vec2 unit(Vec2 a, Vec2 fallback = {1, 0}) {
  const double l = len(a);
  return l > 1e-12 ? mul(a, 1 / l) : fallback;
}
Vec2 left(Vec2 a) { return {-a[1], a[0]}; }

double readable(double angle) {  // read from below or from the right
  while (angle > M_PI / 2 + 1e-9) angle -= M_PI;
  while (angle <= -M_PI / 2 + 1e-9) angle += M_PI;
  return angle;
}

// UTF-8 into code points (a bad byte stands for itself).
std::vector<char32_t> decode(const std::string& s) {
  std::vector<char32_t> out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    char32_t code = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
    i += static_cast<size_t>(n);
    out.push_back(code);
  }
  return out;
}

std::string encode(char32_t c) {
  std::string out;
  if (c < 0x80) out += static_cast<char>(c);
  else if (c < 0x800) out += static_cast<char>(0xC0 | (c >> 6)), out += static_cast<char>(0x80 | (c & 0x3F));
  else if (c < 0x10000) out += static_cast<char>(0xE0 | (c >> 12)), out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
  else
    out += static_cast<char>(0xF0 | (c >> 18)), out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)), out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)),
        out += static_cast<char>(0x80 | (c & 0x3F));
  return out;
}

// Arial's advance widths per 1000 em for ASCII 32..126 (the metrics every Helvetica-compatible face shares); its capitals
// are 0.716 em.
constexpr int kAdvance[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
                              556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778,
                              722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278,
                              278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
                              556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};
constexpr double kCap = 0.716;

double advance(char32_t c) {  // em per 1000
  if (c >= 32 && c <= 126) return kAdvance[c - 32];
  if (c == 0xB1 || c == 0xD7) return 584;  // ± ×
  if (c == 0xB0) return 400;               // °
  return 556;
}

// A glyph's strokes in its own frame: x along the baseline from its left end, y up, in cap heights.
struct Pen {
  Display& d;
  int layer;
  Vec2 at;
  double h, angle;
  uint32_t rgb;
  Vec2 p(double x, double y) const {
    const Vec2 u{std::cos(angle), std::sin(angle)}, v = left(u);
    return add(at, add(mul(u, x * h), mul(v, y * h)));
  }
  void line(double x0, double y0, double x1, double y1) const { d.line(layer, p(x0, y0), p(x1, y1), rgb); }
  void poly(std::initializer_list<std::array<double, 2>> pts, bool closed = false) const {
    std::vector<Vec2> q;
    for (const auto& [x, y] : pts) q.push_back(p(x, y));
    d.polyline(layer, q, closed, rgb);
  }
  void circle(double x, double y, double r) const { d.circle(layer, p(x, y), r * h, rgb); }
  void arc(double x, double y, double r, double a0, double a1) const { d.arc(layer, p(x, y), r * h, a0 + angle, a1 + angle, rgb); }
  void head(double x0, double y0, double x1, double y1) const {  // a filled arrowhead at (x1, y1) coming from (x0, y0)
    const Vec2 tip = p(x1, y1), dir = unit(sub(tip, p(x0, y0))), side = mul(left(dir), 0.12 * h);
    const Vec2 back = sub(tip, mul(dir, 0.4 * h));
    d.fill(layer, {{tip, add(back, side), sub(back, side)}}, rgb);
  }
};

}  // namespace

bool is_glyph(char32_t c) {
  switch (c) {
    case 0x2300: case 0x2334: case 0x2335: case 0x21A7: case 0x25A1: case 0x23E4: case 0x23E5: case 0x25CB: case 0x232D:
    case 0x2312: case 0x2313: case 0x2220: case 0x27C2: case 0x2225: case 0x2316: case 0x25CE: case 0x232F: case 0x2197:
    case 0x2330:
      return true;
    default: return c >= 0x24B6 && c <= 0x24CF;  // circled capitals
  }
}

double glyph_advance(char32_t c, double height) {
  switch (c) {
    case 0x2300: case 0x2334: case 0x2335: case 0x25A1: return 1.0 * height;
    case 0x21A7: return 0.9 * height;
    default: return (c >= 0x24B6 && c <= 0x24CF ? 1.35 : 1.25) * height;
  }
}

double draw_glyph(Display& d, int layer, char32_t c, Vec2 at, double h, double angle, uint32_t rgb) {
  const Pen g{d, layer, at, h, angle, rgb};
  const double s60 = std::sin(M_PI / 3), c60 = 0.5;
  switch (c) {
    case 0x2300:  // ⌀ diameter
      g.circle(0.5, 0.5, 0.4);
      g.line(0.1, -0.05, 0.9, 1.05);
      break;
    case 0x2334: g.poly({{0.1, 0.95}, {0.1, 0.15}, {0.9, 0.15}, {0.9, 0.95}}); break;  // ⌴ counterbore
    case 0x2335: g.poly({{0.05, 0.95}, {0.5, 0.1}, {0.95, 0.95}}); break;              // ⌵ countersink
    case 0x21A7:                                                                        // ↧ depth
      g.line(0.1, 0, 0.8, 0);
      g.line(0.45, 1, 0.45, 0.02);
      g.poly({{0.2, 0.35}, {0.45, 0.02}, {0.7, 0.35}});
      break;
    case 0x25A1: g.poly({{0.1, 0}, {0.9, 0}, {0.9, 0.8}, {0.1, 0.8}}, true); break;  // □ square
    case 0x23E4: g.line(0.05, 0.5, 1.2, 0.5); break;                                // ⏤ straightness
    case 0x23E5: g.poly({{0.05, 0.2}, {0.85, 0.2}, {1.2, 0.8}, {0.4, 0.8}}, true); break;  // ⏥ flatness
    case 0x25CB: g.circle(0.625, 0.5, 0.45); break;                                 // ○ circularity
    case 0x232D: {                                                                  // ⌭ cylindricity
      g.circle(0.625, 0.5, 0.33);
      const double nx = -s60, ny = c60, ux = c60 * 0.62, uy = s60 * 0.62;
      for (const double side : {1.0, -1.0})
        g.line(0.625 + side * 0.33 * nx - ux, 0.5 + side * 0.33 * ny - uy, 0.625 + side * 0.33 * nx + ux, 0.5 + side * 0.33 * ny + uy);
      break;
    }
    case 0x2312: g.arc(0.625, 0.1, 0.55, M_PI / 12, M_PI - M_PI / 12); break;  // ⌒ profile of a line
    case 0x2313:                                                                // ⌓ profile of a surface
      g.arc(0.625, 0.1, 0.55, M_PI / 12, M_PI - M_PI / 12);
      g.line(0.625 - 0.55 * std::cos(M_PI / 12), 0.1 + 0.55 * std::sin(M_PI / 12), 0.625 + 0.55 * std::cos(M_PI / 12), 0.1 + 0.55 * std::sin(M_PI / 12));
      break;
    case 0x2220: g.poly({{1.2, 0.05}, {0.05, 0.05}, {1.0, 0.75}}); break;  // ∠ angularity
    case 0x27C2:                                                           // ⟂ perpendicularity
      g.line(0.05, 0.05, 1.2, 0.05);
      g.line(0.625, 0.05, 0.625, 1);
      break;
    case 0x2225:  // ∥ parallelism
      g.line(0.15, 0.05, 0.62, 0.95);
      g.line(0.62, 0.05, 1.09, 0.95);
      break;
    case 0x2316:  // ⌖ position
      g.circle(0.625, 0.5, 0.32);
      g.line(0.1, 0.5, 1.15, 0.5);
      g.line(0.625, -0.02, 0.625, 1.02);
      break;
    case 0x25CE:  // ◎ concentricity
      g.circle(0.625, 0.5, 0.45);
      g.circle(0.625, 0.5, 0.22);
      break;
    case 0x232F:  // ⌯ symmetry
      g.line(0.3, 0.2, 0.95, 0.2);
      g.line(0.05, 0.5, 1.2, 0.5);
      g.line(0.3, 0.8, 0.95, 0.8);
      break;
    case 0x2197:  // ↗ circular runout
      g.line(0.15, 0.05, 0.9, 0.8);
      g.head(0.15, 0.05, 1.05, 0.95);
      break;
    case 0x2330:  // ⌰ total runout
      g.line(0.1, 0.05, 0.6, 0.05);
      g.line(0.1, 0.05, 0.55, 0.75);
      g.line(0.6, 0.05, 1.05, 0.75);
      g.head(0.1, 0.05, 0.66, 0.92);
      g.head(0.6, 0.05, 1.16, 0.92);
      break;
    default:
      if (c >= 0x24B6 && c <= 0x24CF) {  // a circled capital: the circle drawn, the letter as text
        g.circle(0.675, 0.45, 0.52);
        d.text(layer, std::string(1, static_cast<char>('A' + (c - 0x24B6))), g.p(0.675, 0.45), 0.55 * h, angle, 1, 2, rgb);
        break;
      }
      return 0;
  }
  return glyph_advance(c, h);
}

double text_width(const std::string& line, double height) {
  double w = 0;
  for (char32_t c : decode(line)) w += is_glyph(c) ? glyph_advance(c, height) : advance(c) / 1000 * height / kCap;
  return w;
}

double rich_width(const std::string& s, double height) {
  double w = 0;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t end = std::min(s.find('\n', start), s.size());
    w = std::max(w, text_width(s.substr(start, end - start), height));
    start = end + 1;
  }
  return w;
}

void rich_text(Display& d, int layer, const std::string& s, Vec2 at, double h, double angle, int halign, int valign, uint32_t rgb) {
  const auto codes = decode(s);
  if (std::none_of(codes.begin(), codes.end(), is_glyph)) return d.text(layer, s, at, h, angle, halign, valign, rgb);
  Prim probe;
  probe.kind = Prim::Kind::Text;
  probe.text = s;
  probe.at = at;
  probe.height = h;
  probe.angle = angle;
  probe.valign = std::clamp(valign, 0, 3);
  const Vec2 u{std::cos(angle), std::sin(angle)};
  for (const auto& line : text_lines(probe)) {
    const double w = text_width(line.text, h);
    double x = halign == 1 ? -w / 2 : halign == 2 ? -w : 0;
    std::string run;
    double runAt = x;
    const auto flush = [&] {
      if (!run.empty()) d.text(layer, run, add(line.at, mul(u, runAt)), h, angle, 0, 0, rgb);
      run.clear();
    };
    for (char32_t c : decode(line.text)) {
      if (is_glyph(c)) {
        flush();
        x += draw_glyph(d, layer, c, add(line.at, mul(u, x)), h, angle, rgb);
        runAt = x;
      } else {
        if (run.empty()) runAt = x;
        run += encode(c);
        x += advance(c) / 1000 * h / kCap;
      }
    }
    flush();
  }
}

// ---------------------------------------------------------------- GD&T names
const std::vector<std::string>& characteristics() {
  static const std::vector<std::string> names = {"straightness", "flatness",     "circularity",      "cylindricity",   "line_profile",
                                                 "surface_profile", "angularity", "perpendicularity", "parallelism",   "position",
                                                 "concentricity", "symmetry",     "circular_runout",  "total_runout"};
  return names;
}

std::string characteristic_glyph(const std::string& name) {
  static const std::map<std::string, char32_t> glyphs = {
      {"straightness", 0x23E4}, {"flatness", 0x23E5},       {"circularity", 0x25CB},      {"cylindricity", 0x232D},
      {"line_profile", 0x2312}, {"surface_profile", 0x2313}, {"angularity", 0x2220},      {"perpendicularity", 0x27C2},
      {"parallelism", 0x2225},  {"position", 0x2316},        {"concentricity", 0x25CE},   {"symmetry", 0x232F},
      {"circular_runout", 0x2197}, {"total_runout", 0x2330}};
  const auto it = glyphs.find(name);
  return it == glyphs.end() ? std::string() : encode(it->second);
}

std::string modifier_glyph(const std::string& letter) {
  if (letter.size() != 1 || letter[0] < 'A' || letter[0] > 'Z') return {};
  return encode(0x24B6 + static_cast<char32_t>(letter[0] - 'A'));
}

// ---------------------------------------------------------------- marks
void centre_line(Display& d, int layer, Vec2 a, Vec2 b, const DimStyle& s) {
  const double L = len(sub(b, a)), k = s.scale;
  const double dash = 5 * k, gap = 1 * k, dotLen = 0.5 * k, step = dash + 2 * gap + dotLen;
  const int n = static_cast<int>(std::floor((L - dash) / (2 * step)));
  if (n < 1) return d.line(layer, a, b);
  const Vec2 u = unit(sub(b, a)), m = mul(add(a, b), 0.5);
  const auto piece = [&](double t0, double t1) { d.line(layer, add(m, mul(u, t0)), add(m, mul(u, t1))); };
  piece(-dash / 2, dash / 2);
  for (const double side : {1.0, -1.0}) {
    double at = dash / 2;
    for (int i = 1; i <= n; ++i) {
      piece(side * (at + gap), side * (at + gap + dotLen));
      const double end = i == n ? L / 2 : at + 2 * gap + dotLen + dash;  // the last dash reaches the end
      piece(side * (at + 2 * gap + dotLen), side * end);
      at += step;
    }
  }
}

void centre_mark(Display& d, int layer, Vec2 c, double r, double extend, Vec2 axis, const DimStyle& s) {
  const Vec2 u = unit(axis), v = left(u);
  const double k = s.scale, reach = r + extend, cross = 1.5 * k, gap = 1 * k;
  for (const Vec2 dir : {u, v}) {
    if (r < 3 * k || reach < cross + gap + 1 * k) {
      d.line(layer, sub(c, mul(dir, reach)), add(c, mul(dir, reach)));
      continue;
    }
    d.line(layer, sub(c, mul(dir, cross)), add(c, mul(dir, cross)));
    d.line(layer, add(c, mul(dir, cross + gap)), add(c, mul(dir, reach)));
    d.line(layer, sub(c, mul(dir, cross + gap)), sub(c, mul(dir, reach)));
  }
}

Vec2 leader(Display& d, int layer, Vec2 tip, Vec2 text_at, const std::string& text, const DimStyle& s, bool dotted) {
  const double k = s.scale, shoulder = 3 * k, h = s.text * k;
  const double side = text_at[0] >= tip[0] ? 1 : -1;
  const Vec2 knee{text_at[0] - side * shoulder, text_at[1]};
  d.line(layer, tip, knee);
  d.line(layer, knee, text_at);
  const Vec2 dir = unit(sub(tip, knee), {side, 0});
  if (dotted) {
    d.fill(layer, {[&] {
             std::vector<Vec2> dotPts;
             for (int i = 0; i < 12; ++i) dotPts.push_back(add(tip, mul({std::cos(i * M_PI / 6), std::sin(i * M_PI / 6)}, 0.6 * k)));
             return dotPts;
           }()});
  } else {
    const double l = s.arrow * k, w = l / 6;
    const Vec2 back = sub(tip, mul(dir, l)), across = mul(left(dir), w);
    d.fill(layer, {{tip, add(back, across), sub(back, across)}});
  }
  const Vec2 anchor{text_at[0] + side * s.gap * k, text_at[1]};
  rich_text(d, layer, text, anchor, h, 0, side > 0 ? 0 : 2, 2);
  return anchor;
}

void datum_symbol(Display& d, int layer, Vec2 foot, Vec2 out, Vec2 frame, const std::string& letter, const DimStyle& s) {
  const double k = s.scale, h = s.text * k, half = 1.5 * k, tall = 2.6 * k, side = 2 * h;
  const Vec2 o = unit(out, {0, 1}), along = left(o);
  const Vec2 apex = add(foot, mul(o, tall));
  d.fill(layer, {{add(foot, mul(along, half)), apex, sub(foot, mul(along, half))}});
  // The line from the triangle to the frame's side that faces it.
  const Vec2 to = sub(apex, frame);
  const double t = std::max(std::fabs(to[0]), std::fabs(to[1]));
  const Vec2 edge = t > 1e-12 ? add(frame, mul(to, side / 2 / t)) : frame;
  if (len(sub(edge, apex)) > 1e-9 && len(to) > side / 2) d.line(layer, apex, edge);
  d.polyline(layer, {{frame[0] - side / 2, frame[1] - side / 2}, {frame[0] + side / 2, frame[1] - side / 2}, {frame[0] + side / 2, frame[1] + side / 2},
                     {frame[0] - side / 2, frame[1] + side / 2}},
             true);
  rich_text(d, layer, letter, frame, h, 0, 1, 2);
}

namespace {
std::vector<double> frame_widths(const FrameCells& c, const DimStyle& s) {
  const double h = s.text * s.scale, tall = 2 * h, pad = 0.6 * h;
  std::vector<double> w{tall, std::max(tall, rich_width(c.tolerance, h) + 2 * pad)};
  for (const auto& datum : c.datums) w.push_back(std::max(tall, rich_width(datum, h) + 2 * pad));
  return w;
}
}  // namespace

double feature_frame_width(const FrameCells& cells, const DimStyle& s) {
  const auto w = frame_widths(cells, s);
  return std::accumulate(w.begin(), w.end(), 0.0);
}

double feature_frame(Display& d, int layer, Vec2 at, const FrameCells& c, const DimStyle& s) {
  const double h = s.text * s.scale, tall = 2 * h;
  const auto widths = frame_widths(c, s);
  const double total = std::accumulate(widths.begin(), widths.end(), 0.0);
  const double y0 = at[1] - tall / 2, y1 = at[1] + tall / 2;
  d.polyline(layer, {{at[0], y0}, {at[0] + total, y0}, {at[0] + total, y1}, {at[0], y1}}, true);
  double x = at[0];
  for (size_t i = 0; i < widths.size(); ++i) {
    const std::string text = i == 0 ? c.characteristic : i == 1 ? c.tolerance : c.datums[i - 2];
    if (i == 0) {  // the characteristic's glyph, centred in its square
      const auto codes = decode(text);
      if (codes.size() == 1 && is_glyph(codes[0])) draw_glyph(d, layer, codes[0], {x + (widths[0] - glyph_advance(codes[0], h)) / 2, at[1] - h / 2}, h);
      else rich_text(d, layer, text, {x + widths[0] / 2, at[1]}, h, 0, 1, 2);
    } else {
      rich_text(d, layer, text, {x + widths[i] / 2, at[1]}, h, 0, 1, 2);
    }
    x += widths[i];
    if (i + 1 < widths.size()) d.line(layer, {x, y0}, {x, y1});
  }
  return total;
}

void surface_symbol(Display& d, int layer, Vec2 foot, Vec2 out, const std::string& process, const std::string& text, const DimStyle& s) {
  const double h = s.text * s.scale, h1 = 1.4 * h, h2 = 3 * h, t60 = std::tan(M_PI / 3);
  const Vec2 up = unit(out, {0, 1});
  // Readable: the symbol stands on the surface's outside, its text horizontal unless the surface is steep.
  const double angle = readable(std::atan2(up[1], up[0]) - M_PI / 2);
  const Vec2 x{std::cos(angle), std::sin(angle)}, y = up;
  const auto p = [&](double a, double b) { return add(foot, add(mul(x, a), mul(y, b))); };
  const Vec2 shortEnd = p(-h1 / t60, h1), longEnd = p(h2 / t60, h2);
  d.polyline(layer, {shortEnd, foot, longEnd});
  if (process == "removal") d.line(layer, shortEnd, p(h1 / t60, h1));
  if (process == "no_removal") {
    const double rc = 0.38 * h;  // touching both legs
    d.circle(layer, p(0, rc / std::sin(M_PI / 6)), rc);
  }
  if (text.empty()) return;
  const double w = rich_width(text, h);
  d.line(layer, longEnd, add(longEnd, mul(x, w + 0.8 * h)));
  rich_text(d, layer, text, add(longEnd, add(mul(x, 0.4 * h), mul(y, -1.4 * h))), h, angle, 0, 0);
}

void ordinate_dimensions(Display& d, int layer, const std::vector<Vec2>& points, const std::vector<std::string>& texts, Vec2 axis, double level,
                         const DimStyle& s) {
  const Vec2 u = unit(axis), n = left(u);
  const double k = s.scale, h = s.text * k, gap = s.gap * k, room = 1.6 * h;
  std::vector<size_t> order(points.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return dot(points[a], u) < dot(points[b], u); });
  // Where each value stands along the axis: values closer than `room` form a group spread `room` apart around the middle
  // of its points; groups that then meet join.
  struct Group {
    size_t first, count;
    double sum, start;
  };
  std::vector<Group> groups;
  for (size_t j = 0; j < order.size(); ++j) {
    const double want = dot(points[order[j]], u);
    groups.push_back({j, 1, want, want});
    while (groups.size() > 1) {
      Group& b = groups.back();
      const Group& a = groups[groups.size() - 2];
      if (b.start >= a.start + room * static_cast<double>(a.count) - 1e-9) break;
      Group m{a.first, a.count + b.count, a.sum + b.sum, 0};
      m.start = m.sum / static_cast<double>(m.count) - room * static_cast<double>(m.count - 1) / 2;
      groups.pop_back();
      groups.back() = m;
    }
  }
  std::vector<double> at(points.size());
  for (const auto& g : groups)
    for (size_t k = 0; k < g.count; ++k) at[order[g.first + k]] = g.start + room * static_cast<double>(k);
  for (size_t i = 0; i < points.size(); ++i) {
    const Vec2 p = points[i];
    const double t = dot(p, u), q = at[i], pn = dot(p, n), side = level >= pn ? 1 : -1;
    const auto P = [&](double a, double b) { return add(mul(u, a), mul(n, b)); };
    const double from = pn + side * gap;
    if (std::fabs(q - t) < 1e-9 || std::fabs(level - pn) < 4 * k) {
      d.line(layer, P(t, from), P(q, level));
    } else {  // jogged: out from the feature, across to its value, on to the level
      d.polyline(layer, {P(t, from), P(t, level - side * 3 * k), P(q, level - side * 1.5 * k), P(q, level)});
    }
    const double angle = std::atan2(n[1] * side, n[0] * side), shown = readable(angle);
    rich_text(d, layer, i < texts.size() ? texts[i] : std::string(), P(q, level + side * gap), h, shown, std::fabs(shown - angle) < 1e-9 ? 0 : 2, 2);
  }
}

}  // namespace opad::drawing
