// DXF R2000 (AC1015) writer for drawings (TODO 11 UI-86), from Autodesk's public DXF reference: handles throughout, the
// tables a reader looks things up in (line types, layers with colour, line type and line weight, the Standard text
// style, the model and paper space block records), the model and paper space blocks, the entities, and the objects
// AutoCAD expects of an R2000 file (the root dictionary, groups, the Model and Layout1 layouts, plot style names).
// Everything lies in model space at 1:1 mm. R2000 has no true colours: the nearest indexed colour is written. A sheet's
// dimensions are DIMENSION entities, each over an anonymous block (*D1, ...) of what it draws.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "../drawing_common.hpp"
#include "writer_common.hpp"

namespace opad::drawing {
namespace {

using detail::num;

std::string hex(unsigned h) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%X", h);
  return buf;
}

// Text as an R2000 file carries it: ASCII as is, anything else as \U+XXXX (outside the basic plane: '?'); control
// characters dropped.
std::string escaped(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    uint32_t code = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
    i += static_cast<size_t>(n);
    if (code < 0x20 || code == 0x7F) continue;
    if (code < 0x80) out += static_cast<char>(code);
    else if (code > 0xFFFF || (n == 1 && c >= 0x80)) out += '?';
    else {
      char buf[16];
      std::snprintf(buf, sizeof buf, "\\U+%04X", static_cast<unsigned>(code));
      out += buf;
    }
  }
  return out;
}

// The indexed colour nearest an RGB (by squared distance); the foreground and pure black or white are 7.
int aci(uint32_t rgb) {
  if (rgb == kInk || rgb == kByLayer || (rgb & 0xFFFFFFu) == 0 || (rgb & 0xFFFFFFu) == 0xFFFFFFu) return 7;
  int best = 7;
  long bestD = -1;
  for (int i = 1; i <= 255; ++i) {
    if (i == 7) continue;
    const uint32_t c = opad::detail::aci_rgb(i);
    const long dr = long((c >> 16) & 255) - long((rgb >> 16) & 255), dg = long((c >> 8) & 255) - long((rgb >> 8) & 255), db = long(c & 255) - long(rgb & 255);
    const long d = dr * dr + dg * dg + db * db;
    if (bestD < 0 || d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}

// Line weights a DXF may name (1/100 mm).
int lineweight(double mm) {
  static const int weights[] = {0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
  int best = 25;
  double bestD = 1e300;
  for (int w : weights)
    if (std::abs(w - mm * 100) < bestD) {
      bestD = std::abs(w - mm * 100);
      best = w;
    }
  return best;
}

double degrees(double radians) {
  double d = std::fmod(radians * 180 / M_PI, 360.0);
  if (d < 0) d += 360;
  return d >= 360 - 1e-10 ? 0 : d;
}

double turn(double radians) {  // into [0, 2 pi)
  double r = std::fmod(radians, 2 * M_PI);
  if (r < 0) r += 2 * M_PI;
  return r >= 2 * M_PI - 1e-12 ? 0 : r;
}

bool convex(const std::vector<Vec2>& p) {
  int sign = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    const Vec2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
    const double cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
    if (std::abs(cross) < 1e-12) continue;
    const int s = cross > 0 ? 1 : -1;
    if (sign && s != sign) return false;
    sign = s;
  }
  return sign != 0;
}

// A dimension's text as a DIMENSION carries it: ⌀ ± ° as their %% codes, lines split by \P, the rest as escaped() writes it.
std::string dimension_text(const std::string& s) {
  static const std::pair<std::string, std::string> codes[] = {{"\xE2\x8C\x80", "%%c"}, {"\xC2\xB1", "%%p"}, {"\xC2\xB0", "%%d"}, {"\n", "\\P"}};
  std::string out, run;
  for (size_t i = 0; i < s.size();) {
    const auto code = std::find_if(std::begin(codes), std::end(codes), [&](const auto& c) { return s.compare(i, c.first.size(), c.first) == 0; });
    if (code == std::end(codes)) {
      run += s[i++];
      continue;
    }
    out += escaped(run) + code->second;
    run.clear();
    i += code->first.size();
  }
  return out + escaped(run);
}

class Writer {
 public:
  Writer(const Display& d, int decimals, bool mtext, bool dimensions) : m_d(d), m_dec(decimals), m_mtext(mtext) {
    if (!dimensions) return;
    std::map<std::string, size_t> records;  // a source's dimension record (the first)
    for (size_t i = d.dimensions.size(); i-- > 0;)
      if (!d.dimensions[i].source.empty()) records[d.dimensions[i].source] = i;
    for (const auto& p : d.prims)  // a block each, in the order they are drawn
      if (const auto it = records.find(p.source); !p.source.empty() && it != records.end() && !m_blockOf.count(p.source)) {
        m_blockOf[p.source] = m_blocks.size();
        m_blocks.push_back({it->second, 0, "*D" + std::to_string(m_blocks.size() + 1), p.layer});
      }
  }
  std::string run();

 private:
  void g(int code, const std::string& v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%3d\n", code);
    m_out += buf;
    m_out += v;
    m_out += '\n';
  }
  void g(int code, double v) { g(code, num(v, m_dec)); }
  void gi(int code, long v) { g(code, std::to_string(v)); }
  void point(int code, Vec2 p) {
    g(code, p[0]);
    g(code + 10, p[1]);
    g(code + 20, std::string("0"));
  }
  unsigned handle() { return m_next++; }
  void table(const std::string& name, unsigned h, int count) {
    g(0, "TABLE");
    g(2, name);
    g(5, hex(h));
    g(330, "0");
    g(100, "AcDbSymbolTable");
    gi(70, count);
  }
  void record(const std::string& type, unsigned owner, const char* subclass) {
    g(0, type);
    g(5, hex(handle()));
    g(330, hex(owner));
    g(100, "AcDbSymbolTableRecord");
    g(100, subclass);
  }
  void entity(const char* type, const Prim& p, const char* subclass) {
    g(0, type);
    g(5, hex(handle()));
    g(330, hex(m_owner));
    g(100, "AcDbEntity");
    g(8, layerName(p.layer));
    if (p.rgb != kByLayer && p.layer >= 0 && p.layer < static_cast<int>(m_d.layers.size()) && aci(p.rgb) != aci(m_d.layers[static_cast<size_t>(p.layer)].rgb))
      gi(62, aci(p.rgb));
    g(100, subclass);
  }
  const std::string& layerName(int layer) const { return m_layerNames[static_cast<size_t>(std::clamp(layer, 0, static_cast<int>(m_layerNames.size()) - 1))]; }
  void header();
  void tables();
  void blocks();
  void entities();
  void objects();
  void prim(const Prim& p);
  void curve(const Prim& p);
  void fill(const Prim& p);
  void text(const Prim& p);
  struct Block {
    size_t record;     // Display::dimensions
    unsigned handle;   // its BLOCK_RECORD
    std::string name;  // *D1, *D2, ...
    int layer;         // the DIMENSION's: its first primitive's
  };
  void dimension(const Block& b);

  const Display& m_d;
  int m_dec;
  bool m_mtext;
  std::string m_out;
  unsigned m_next = 0x20;
  std::vector<std::string> m_layerNames;
  unsigned m_modelRecord = 0, m_paperRecord = 0, m_root = 0, m_groups = 0, m_layouts = 0, m_plotStyles = 0, m_placeholder = 0, m_modelLayout = 0,
           m_paperLayout = 0, m_style = 0, m_owner = 0;
  std::vector<Block> m_blocks;              // a DIMENSION's each
  std::map<std::string, size_t> m_blockOf;  // source -> m_blocks
};

void Writer::header() {
  const auto b = m_d.bounds();
  g(0, "SECTION");
  g(2, "HEADER");
  g(9, "$ACADVER");
  g(1, "AC1015");
  g(9, "$ACADMAINTVER");
  gi(70, 6);
  g(9, "$DWGCODEPAGE");
  g(3, "ANSI_1252");
  g(9, "$INSBASE");
  point(10, {0, 0});
  g(9, "$EXTMIN");
  point(10, {b[0], b[1]});
  g(9, "$EXTMAX");
  point(10, {b[2], b[3]});
  g(9, "$LIMMIN");
  g(10, b[0]);
  g(20, b[1]);
  g(9, "$LIMMAX");
  g(10, b[2]);
  g(20, b[3]);
  g(9, "$LTSCALE");
  g(40, m_d.pen_scale);
  g(9, "$TEXTSTYLE");
  g(7, "Standard");
  g(9, "$CLAYER");
  g(8, "0");
  g(9, "$CELTYPE");
  g(6, "ByLayer");
  g(9, "$CECOLOR");
  gi(62, 256);
  g(9, "$HANDSEED");
  g(5, "@HANDSEED@");  // filled in last
  g(9, "$MEASUREMENT");
  gi(70, 1);
  g(9, "$LWDISPLAY");
  gi(290, 1);
  g(9, "$INSUNITS");
  gi(70, 4);
  g(9, "$TILEMODE");
  gi(70, 1);
  g(0, "ENDSEC");
  g(0, "SECTION");
  g(2, "CLASSES");
  for (const auto& [name, cls] : {std::pair{"ACDBDICTIONARYWDFLT", "AcDbDictionaryWithDefault"}, std::pair{"ACDBPLACEHOLDER", "AcDbPlaceHolder"},
                                 std::pair{"LAYOUT", "AcDbLayout"}}) {
    g(0, "CLASS");
    g(1, name);
    g(2, cls);
    g(3, "ObjectDBX Classes");
    gi(90, 0);
    gi(280, 0);
    gi(281, 0);
  }
  g(0, "ENDSEC");
}

void Writer::tables() {
  const auto b = m_d.bounds();
  g(0, "SECTION");
  g(2, "TABLES");
  // The view a reader opens with: the drawing's extents.
  unsigned t = handle();
  table("VPORT", t, 1);
  record("VPORT", t, "AcDbViewportTableRecord");
  g(2, "*Active");
  gi(70, 0);
  g(10, 0.0);
  g(20, 0.0);
  g(11, 1.0);
  g(21, 1.0);
  g(12, (b[0] + b[2]) / 2);
  g(22, (b[1] + b[3]) / 2);
  g(13, 0.0);
  g(23, 0.0);
  g(14, 10.0);
  g(24, 10.0);
  g(15, 10.0);
  g(25, 10.0);
  g(16, 0.0);
  g(26, 0.0);
  g(36, 1.0);
  g(17, 0.0);
  g(27, 0.0);
  g(37, 0.0);
  g(40, std::max({(b[3] - b[1]) * 1.1, (b[2] - b[0]) * 1.1 / 1.5, 1.0}));
  g(41, 1.5);
  g(42, 50.0);
  g(43, 0.0);
  g(44, 0.0);
  g(50, 0.0);
  g(51, 0.0);
  gi(71, 0);
  gi(72, 1000);
  gi(73, 1);
  gi(74, 3);
  gi(75, 0);
  gi(76, 0);
  gi(77, 0);
  gi(78, 0);
  gi(281, 0);
  gi(65, 1);
  g(146, 0.0);
  g(0, "ENDTAB");

  t = handle();
  const LineType types[] = {LineType::Hidden, LineType::Center, LineType::Phantom, LineType::Dotted};
  table("LTYPE", t, 3 + 4);
  for (const char* name : {"ByBlock", "ByLayer", "Continuous"}) {
    record("LTYPE", t, "AcDbLinetypeTableRecord");
    g(2, name);
    gi(70, 0);
    g(3, std::string(name) == "Continuous" ? "Solid line" : "");
    gi(72, 65);
    gi(73, 0);
    g(40, 0.0);
  }
  for (LineType type : types) {
    const auto& dashes = line_type_dashes(type);
    double total = 0;
    std::string look;
    for (double d : dashes) {
      total += std::abs(d);
      look += d > 0 ? std::string(static_cast<size_t>(std::max(1.0, d / 1.5)), '_') : d == 0 ? "." : " ";
    }
    record("LTYPE", t, "AcDbLinetypeTableRecord");
    g(2, line_type_name(type));
    gi(70, 0);
    g(3, look + look + look);
    gi(72, 65);
    gi(73, static_cast<long>(dashes.size()));
    g(40, total);
    for (double d : dashes) {
      g(49, d);
      gi(74, 0);
    }
  }
  g(0, "ENDTAB");

  // Layer names: what DXF allows, unique without regard to case; "0" always there.
  std::set<std::string> taken;
  auto upper = [](std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  };
  for (const auto& l : m_d.layers) {
    std::string name;
    for (char c : l.name) name += std::string("<>/\\\":;?*|=`").find(c) == std::string::npos && static_cast<unsigned char>(c) >= 0x20 ? c : '_';
    name = escaped(name);
    if (name.empty()) name = "0";
    if (name.size() > 255) name.resize(255);
    std::string unique = name;
    for (int n = 2; taken.count(upper(unique)); ++n) unique = name + " (" + std::to_string(n) + ")";
    taken.insert(upper(unique));
    m_layerNames.push_back(unique);
  }
  if (m_layerNames.empty()) m_layerNames.push_back("0");
  t = handle();
  table("LAYER", t, static_cast<int>(m_d.layers.size()) + (taken.count("0") ? 0 : 1));
  auto layer = [&](const std::string& name, const Layer& l) {
    record("LAYER", t, "AcDbLayerTableRecord");
    g(2, name);
    gi(70, 0);
    gi(62, aci(l.rgb));
    g(6, line_type_name(l.line));
    gi(370, lineweight(l.width));
    g(390, hex(m_placeholder));
  };
  if (!taken.count("0")) layer("0", Layer{"0", kInk, LineType::Continuous, 0.25});
  for (size_t i = 0; i < m_d.layers.size(); ++i) layer(m_layerNames[i], m_d.layers[i]);
  g(0, "ENDTAB");

  t = handle();
  table("STYLE", t, 1);
  record("STYLE", t, "AcDbTextStyleTableRecord");
  m_style = m_next - 1;
  g(2, "Standard");
  gi(70, 0);
  g(40, 0.0);
  g(41, 1.0);
  g(50, 0.0);
  gi(71, 0);
  g(42, 3.5);
  g(3, "arial.ttf");
  g(4, "");
  g(0, "ENDTAB");

  for (const char* name : {"VIEW", "UCS"}) {
    table(name, handle(), 0);
    g(0, "ENDTAB");
  }

  t = handle();
  table("APPID", t, 1);
  record("APPID", t, "AcDbRegAppTableRecord");
  g(2, "ACAD");
  gi(70, 0);
  g(0, "ENDTAB");

  t = handle();
  table("DIMSTYLE", t, 1);
  g(100, "AcDbDimStyleTable");
  g(0, "DIMSTYLE");
  g(105, hex(handle()));
  g(330, hex(t));
  g(100, "AcDbSymbolTableRecord");
  g(100, "AcDbDimStyleTableRecord");
  g(2, "Standard");
  gi(70, 0);
  g(41, 3.0);    // arrow size
  g(42, 1.0);    // extension line offset
  g(44, 2.0);    // extension past the dimension line
  g(140, 3.5);   // text height
  g(147, 1.0);   // gap around the text
  gi(77, 1);     // text above the line
  gi(78, 8);     // no trailing zeros
  gi(271, 2);
  g(340, hex(m_style));
  g(0, "ENDTAB");

  t = handle();
  table("BLOCK_RECORD", t, 2 + static_cast<int>(m_blocks.size()));
  for (const auto& [h, name, layout] : {std::tuple{m_modelRecord, "*Model_Space", m_modelLayout}, std::tuple{m_paperRecord, "*Paper_Space", m_paperLayout}}) {
    g(0, "BLOCK_RECORD");
    g(5, hex(h));
    g(330, hex(t));
    g(100, "AcDbSymbolTableRecord");
    g(100, "AcDbBlockTableRecord");
    g(2, name);
    g(340, hex(layout));
  }
  for (const auto& b : m_blocks) {
    g(0, "BLOCK_RECORD");
    g(5, hex(b.handle));
    g(330, hex(t));
    g(100, "AcDbSymbolTableRecord");
    g(100, "AcDbBlockTableRecord");
    g(2, b.name);
    g(340, "0");
  }
  g(0, "ENDTAB");
  g(0, "ENDSEC");
}

void Writer::blocks() {
  g(0, "SECTION");
  g(2, "BLOCKS");
  for (const auto& [record, name, paper] : {std::tuple{m_modelRecord, "*Model_Space", false}, std::tuple{m_paperRecord, "*Paper_Space", true}}) {
    g(0, "BLOCK");
    g(5, hex(handle()));
    g(330, hex(record));
    g(100, "AcDbEntity");
    if (paper) gi(67, 1);
    g(8, "0");
    g(100, "AcDbBlockBegin");
    g(2, name);
    gi(70, 0);
    point(10, {0, 0});
    g(3, name);
    g(1, "");
    g(0, "ENDBLK");
    g(5, hex(handle()));
    g(330, hex(record));
    g(100, "AcDbEntity");
    if (paper) gi(67, 1);
    g(8, "0");
    g(100, "AcDbBlockEnd");
  }
  // A dimension's block: what it draws, at the drawing's own coordinates (base point 0, 0), owned by its record.
  for (const auto& b : m_blocks) {
    g(0, "BLOCK");
    g(5, hex(handle()));
    g(330, hex(b.handle));
    g(100, "AcDbEntity");
    g(8, "0");
    g(100, "AcDbBlockBegin");
    g(2, b.name);
    gi(70, 1);  // anonymous
    point(10, {0, 0});
    g(3, b.name);
    g(1, "");
    m_owner = b.handle;
    const std::string& source = m_d.dimensions[b.record].source;
    for (const auto& p : m_d.prims)
      if (p.source == source) prim(p);
    m_owner = m_modelRecord;
    g(0, "ENDBLK");
    g(5, hex(handle()));
    g(330, hex(b.handle));
    g(100, "AcDbEntity");
    g(8, "0");
    g(100, "AcDbBlockEnd");
  }
  g(0, "ENDSEC");
}

void Writer::curve(const Prim& p) {
  const Curve& c = p.curve;
  switch (c.type) {
    case Curve::Type::Line:
      if (c.pts.size() < 2) return;
      entity("LINE", p, "AcDbLine");
      point(10, c.pts.front());
      point(11, c.pts.back());
      return;
    case Curve::Type::Arc:
      if (detail::full_turn(c)) {
        entity("CIRCLE", p, "AcDbCircle");
        point(10, c.c);
        g(40, c.r1);
        return;
      }
      entity("ARC", p, "AcDbCircle");
      point(10, c.c);
      g(40, c.r1);
      g(100, "AcDbArc");
      g(50, degrees(c.a0));
      g(51, degrees(c.a1));
      return;
    case Curve::Type::Ellipse: {
      entity("ELLIPSE", p, "AcDbEllipse");
      point(10, c.c);
      point(11, {c.r1 * std::cos(c.rot), c.r1 * std::sin(c.rot)});
      g(210, 0.0);
      g(220, 0.0);
      g(230, 1.0);
      g(40, c.r1 > 0 ? std::min(1.0, c.r2 / c.r1) : 1.0);
      const bool full = detail::full_turn(c);
      g(41, full ? 0.0 : turn(c.a0));
      g(42, full ? 2 * M_PI : turn(c.a1) == 0 ? 2 * M_PI : turn(c.a1));
      return;
    }
    case Curve::Type::Spline: {
      if (c.pts.size() < 2 || c.knots.size() != c.pts.size() + static_cast<size_t>(c.degree) + 1) break;
      const bool rational = c.weights.size() == c.pts.size();
      entity("SPLINE", p, "AcDbSpline");
      g(210, 0.0);
      g(220, 0.0);
      g(230, 1.0);
      gi(70, 8 + (rational ? 4 : 0));
      gi(71, c.degree);
      gi(72, static_cast<long>(c.knots.size()));
      gi(73, static_cast<long>(c.pts.size()));
      gi(74, 0);
      g(42, std::string("0.0000001"));
      g(43, std::string("0.0000001"));
      for (double k : c.knots) g(40, num(k, 12));
      if (rational)
        for (double w : c.weights) g(41, num(w, 12));
      for (const auto& q : c.pts) point(10, q);
      return;
    }
    default: break;
  }
  // Polylines (and a spline that cannot be written as one): LWPOLYLINE, closed when its ends meet.
  std::vector<Vec2> pts = c.type == Curve::Type::Polyline ? c.pts : c.sample(1e-3);
  if (pts.size() < 2) return;
  const bool closed = pts.size() > 3 && std::hypot(pts.back()[0] - pts.front()[0], pts.back()[1] - pts.front()[1]) < 1e-9;
  if (closed) pts.pop_back();
  entity("LWPOLYLINE", p, "AcDbPolyline");
  gi(90, static_cast<long>(pts.size()));
  gi(70, closed ? 1 : 0);
  g(43, 0.0);
  for (const auto& q : pts) {
    g(10, q[0]);
    g(20, q[1]);
  }
}

void Writer::fill(const Prim& p) {
  if (p.loops.size() == 1 && (p.loops[0].size() == 3 || p.loops[0].size() == 4) && convex(p.loops[0])) {
    // SOLID: corners in the order 1 2 4 3 (its second pair is crossed).
    const auto& q = p.loops[0];
    entity("SOLID", p, "AcDbTrace");
    point(10, q[0]);
    point(11, q[1]);
    point(12, q.size() == 4 ? q[3] : q[2]);
    point(13, q[2]);
    return;
  }
  entity("HATCH", p, "AcDbHatch");
  point(10, {0, 0});
  g(210, 0.0);
  g(220, 0.0);
  g(230, 1.0);
  g(2, "SOLID");
  gi(70, 1);
  gi(71, 0);
  gi(91, static_cast<long>(p.loops.size()));
  for (size_t i = 0; i < p.loops.size(); ++i) {
    gi(92, i == 0 ? 3 : 2);  // polyline; the first one external
    gi(72, 0);
    gi(73, 1);
    gi(93, static_cast<long>(p.loops[i].size()));
    for (const auto& q : p.loops[i]) {
      g(10, q[0]);
      g(20, q[1]);
    }
    gi(97, 0);
  }
  gi(75, 0);  // odd parity: a loop inside another is a hole
  gi(76, 1);
  gi(98, 0);
}

void Writer::text(const Prim& p) {
  if (p.text.find('\n') != std::string::npos && !m_mtext) {  // one TEXT a line (for readers that lose MTEXT heights)
    for (const auto& line : detail::text_lines(p)) {
      Prim one = p;
      one.text = line.text;
      one.at = line.at;
      one.valign = 0;
      if (line.text.find_first_not_of(' ') != std::string::npos) text(one);
    }
    return;
  }
  const Vec2 u{std::cos(p.angle), std::sin(p.angle)}, v{-u[1], u[0]};
  if (p.text.find('\n') == std::string::npos) {
    std::string s = escaped(p.text);
    if (s.find("%%") != std::string::npos) {
      std::string t;
      for (char ch : s) t += ch == '%' ? std::string("%%%") : std::string(1, ch);
      s = t;
    }
    // Aligned text: 11 is where it is aligned, 10 its baseline's start, as AutoCAD writes them (it works the start out
    // again; a converter that finds both equal drops the alignment point).
    Vec2 start = p.at;
    if (p.halign || p.valign) {
      size_t chars = 0;
      for (char ch : p.text) chars += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
      const double w = 0.7 * p.height * static_cast<double>(chars) * p.halign / 2;
      const double up = p.valign == 3 ? -p.height : p.valign == 2 ? -p.height / 2 : p.valign == 1 ? 0.3 * p.height : 0;
      start = {p.at[0] - u[0] * w + v[0] * up, p.at[1] - u[1] * w + v[1] * up};
    }
    entity("TEXT", p, "AcDbText");
    point(10, start);
    g(40, p.height);
    g(1, s);
    g(50, degrees(p.angle));
    g(7, "Standard");
    if (p.halign || p.valign) {
      gi(72, p.halign);
      point(11, p.at);
    }
    g(100, "AcDbText");
    if (p.valign) gi(73, p.valign);
    return;
  }
  // MTEXT: lines by \P, backslashes and braces escaped; the attachment from the alignment (on the baseline: the first
  // line's top, a height above); chunks of 250 characters (an escape never split).
  std::string s, line;
  for (size_t i = 0; i <= p.text.size(); ++i) {
    const char ch = i < p.text.size() ? p.text[i] : '\n';
    if (ch != '\n') {
      line += ch == '\\' || ch == '{' || ch == '}' ? std::string("\\") + ch : std::string(1, ch);
      continue;
    }
    s += escaped(line) + (i < p.text.size() ? "\\P" : "");
    line.clear();
  }
  entity("MTEXT", p, "AcDbMText");
  point(10, p.valign ? p.at : Vec2{p.at[0] + v[0] * p.height, p.at[1] + v[1] * p.height});
  g(40, p.height);
  g(41, 0.0);
  const int row = p.valign == 3 || p.valign == 0 ? 0 : p.valign == 2 ? 1 : 2;
  gi(71, row * 3 + p.halign + 1);
  gi(72, 1);
  size_t at = 0;
  while (s.size() - at > 250) {
    size_t cut = at + 250;
    for (size_t back = 1; back < 8 && cut - back > at; ++back)
      if (s[cut - back] == '\\') { cut -= back; break; }
    g(3, s.substr(at, cut - at));
    at = cut;
  }
  g(1, s.substr(at));
  g(7, "Standard");
  point(11, u);
}

void Writer::entities() {
  g(0, "SECTION");
  g(2, "ENTITIES");
  std::set<size_t> written;
  for (const auto& p : m_d.prims) {
    const auto it = p.source.empty() ? m_blockOf.end() : m_blockOf.find(p.source);
    if (it == m_blockOf.end()) prim(p);
    else if (written.insert(it->second).second) dimension(m_blocks[it->second]);  // where it is first drawn: its DIMENSION
  }
  g(0, "ENDSEC");
}

void Writer::prim(const Prim& p) {
  switch (p.kind) {
    case Prim::Kind::Curve: curve(p); break;
    case Prim::Kind::Fill: fill(p); break;
    case Prim::Kind::Text: text(p); break;
    case Prim::Kind::Image: throw Error("Raster images require SVG export; DXF raster references are not supported");
  }
}

// A DIMENSION over its block: the definition points, its type (with 32: the block is its own; 128: its text stands where
// it was drawn), the measurement and the text as drawn; a linear scale (DIMLFAC) among the Standard style's overrides, so
// a program that measures it again on paper finds the model's value.
void Writer::dimension(const Block& b) {
  const DimensionRecord& r = m_d.dimensions[b.record];
  g(0, "DIMENSION");
  g(5, hex(handle()));
  g(330, hex(m_modelRecord));
  g(100, "AcDbEntity");
  g(8, layerName(b.layer));
  g(100, "AcDbDimension");
  g(2, b.name);
  point(10, r.p10);
  point(11, r.p11);
  gi(70, r.type | 32 | 128);
  g(1, dimension_text(r.text));
  g(42, r.value);
  g(3, "Standard");
  const auto dist = [](Vec2 a, Vec2 c) { return std::hypot(c[0] - a[0], c[1] - a[1]); };
  double paper = 0;  // what it spans on paper
  if (r.type == 0 || r.type == 1) {
    g(100, "AcDbAlignedDimension");
    point(13, r.p13);
    point(14, r.p14);
    if (r.type == 0) {
      g(50, degrees(r.angle));
      g(100, "AcDbRotatedDimension");
      paper = std::fabs((r.p14[0] - r.p13[0]) * std::cos(r.angle) + (r.p14[1] - r.p13[1]) * std::sin(r.angle));
    } else {
      paper = dist(r.p13, r.p14);
    }
  } else if (r.type == 2) {
    g(100, "AcDb2LineAngularDimension");
    point(13, r.p13);
    point(14, r.p14);
    point(15, r.p15);
    point(16, r.p16);
  } else {
    g(100, r.type == 3 ? "AcDbDiametricDimension" : "AcDbRadialDimension");
    point(15, r.p15);
    g(40, r.leader);
    paper = dist(r.p10, r.p15);
  }
  if (paper > 1e-9 && r.type != 2 && std::fabs(r.value / paper - 1) > 1e-9) {
    g(1001, "ACAD");
    g(1000, "DSTYLE");
    g(1002, "{");
    gi(1070, 144);
    g(1040, num(r.value / paper, 12));
    g(1002, "}");
  }
}

void Writer::objects() {
  g(0, "SECTION");
  g(2, "OBJECTS");
  g(0, "DICTIONARY");
  g(5, hex(m_root));
  g(330, "0");
  g(100, "AcDbDictionary");
  gi(281, 1);
  for (const auto& [name, h] : {std::pair{"ACAD_GROUP", m_groups}, std::pair{"ACAD_LAYOUT", m_layouts}, std::pair{"ACAD_PLOTSTYLENAME", m_plotStyles}}) {
    g(3, name);
    g(350, hex(h));
  }
  g(0, "DICTIONARY");
  g(5, hex(m_groups));
  g(330, hex(m_root));
  g(100, "AcDbDictionary");
  gi(281, 1);
  g(0, "DICTIONARY");
  g(5, hex(m_layouts));
  g(330, hex(m_root));
  g(100, "AcDbDictionary");
  gi(281, 1);
  g(3, "Layout1");
  g(350, hex(m_paperLayout));
  g(3, "Model");
  g(350, hex(m_modelLayout));
  g(0, "ACDBDICTIONARYWDFLT");
  g(5, hex(m_plotStyles));
  g(330, hex(m_root));
  g(100, "AcDbDictionary");
  gi(281, 1);
  g(3, "Normal");
  g(350, hex(m_placeholder));
  g(100, "AcDbDictionaryWithDefault");
  g(340, hex(m_placeholder));
  g(0, "ACDBPLACEHOLDER");
  g(5, hex(m_placeholder));
  g(330, hex(m_plotStyles));
  const auto b = m_d.bounds();
  for (const auto& [h, name, record, tab] : {std::tuple{m_modelLayout, "Model", m_modelRecord, 0}, std::tuple{m_paperLayout, "Layout1", m_paperRecord, 1}}) {
    g(0, "LAYOUT");
    g(5, hex(h));
    g(330, hex(m_layouts));
    g(100, "AcDbPlotSettings");
    g(1, "");
    g(2, "none_device");
    g(4, "ISO_A3_(420.00_x_297.00_MM)");
    g(6, "");
    for (int code : {40, 41, 42, 43}) g(code, 7.5);
    g(44, 420.0);
    g(45, 297.0);
    for (int code : {46, 47, 48, 49, 140, 141}) g(code, 0.0);
    g(142, 1.0);
    g(143, 1.0);
    gi(70, tab ? 0 : 1024);
    gi(72, 1);
    gi(73, 0);
    gi(74, 5);
    g(7, "");
    gi(75, 16);
    g(147, 1.0);
    g(148, 0.0);
    g(149, 0.0);
    g(100, "AcDbLayout");
    g(1, name);
    gi(70, 1);
    gi(71, tab);
    g(10, 0.0);
    g(20, 0.0);
    g(11, 420.0);
    g(21, 297.0);
    point(12, {0, 0});
    point(14, tab ? Vec2{0, 0} : Vec2{b[0], b[1]});
    point(15, tab ? Vec2{0, 0} : Vec2{b[2], b[3]});
    g(146, 0.0);
    point(13, {0, 0});
    point(16, {1, 0});
    point(17, {0, 1});
    gi(76, 1);
    g(330, hex(record));
  }
  g(0, "ENDSEC");
}

std::string Writer::run() {
  // Handles that are named before they are written.
  m_modelRecord = handle();
  m_paperRecord = handle();
  m_root = handle();
  m_groups = handle();
  m_layouts = handle();
  m_plotStyles = handle();
  m_placeholder = handle();
  m_modelLayout = handle();
  m_paperLayout = handle();
  m_owner = m_modelRecord;
  for (auto& b : m_blocks) b.handle = handle();
  header();
  tables();
  blocks();
  entities();
  objects();
  g(0, "EOF");
  m_out.replace(m_out.find("@HANDSEED@"), 10, hex(m_next));
  return std::move(m_out);
}

}  // namespace

std::string dxf_text(const Display& d, int decimals, bool mtext, bool dimensions) { return Writer(d, decimals, mtext, dimensions).run(); }

}  // namespace opad::drawing
