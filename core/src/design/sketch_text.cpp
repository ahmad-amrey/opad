// Text from the built-in stroke font (TODO 10 B4; the glyphs moved here from the desktop so the CLI and MCP get them).
#include "opad/design/sketch_text.hpp"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <sstream>

#include "opad/design/sketch_modify.hpp"

namespace opad::design {
namespace {

// A small, explicit single-stroke alphabet. Coordinates are in units of the cap height; each glyph is polylines
// separated by '|', points "x,y" separated by spaces, x spanning 0..1 (drawn 0.65 wide).
const std::map<char, const char*>& glyphs() {
  static const std::map<char, const char*> table = {
      {'A', "0,0 .5,1 1,0|.2,.4 .8,.4"}, {'B', "0,0 0,1 .6,1 1,.8 .6,.5 0,.5|.6,.5 1,.3 .6,0 0,0"},
      {'C', "1,.85 .75,1 .25,1 0,.75 0,.25 .25,0 .75,0 1,.15"}, {'D', "0,0 0,1 .5,1 1,.75 1,.25 .5,0 0,0"},
      {'E', "1,1 0,1 0,0 1,0|0,.5 .8,.5"}, {'F', "0,0 0,1 1,1|0,.5 .8,.5"},
      {'G', "1,.8 .75,1 .25,1 0,.75 0,.25 .25,0 .75,0 1,.25 1,.5 .55,.5"}, {'H', "0,0 0,1|1,0 1,1|0,.5 1,.5"},
      {'I', "0,1 1,1|.5,1 .5,0|0,0 1,0"}, {'J', "0,.25 .25,0 .75,0 1,.25 1,1 .25,1"},
      {'K', "0,0 0,1|1,1 0,.45 1,0"}, {'L', "0,1 0,0 1,0"}, {'M', "0,0 0,1 .5,.4 1,1 1,0"},
      {'N', "0,0 0,1 1,0 1,1"}, {'O', ".25,0 0,.25 0,.75 .25,1 .75,1 1,.75 1,.25 .75,0 .25,0"},
      {'P', "0,0 0,1 .75,1 1,.75 .75,.5 0,.5"}, {'Q', ".25,0 0,.25 0,.75 .25,1 .75,1 1,.75 1,.25 .75,0 .25,0|.6,.3 1,-.1"},
      {'R', "0,0 0,1 .75,1 1,.75 .75,.5 0,.5|.5,.5 1,0"}, {'S', "1,.85 .75,1 .25,1 0,.75 .25,.5 .75,.5 1,.25 .75,0 .25,0 0,.15"},
      {'T', "0,1 1,1|.5,1 .5,0"}, {'U', "0,1 0,.25 .25,0 .75,0 1,.25 1,1"}, {'V', "0,1 .5,0 1,1"},
      {'W', "0,1 .2,0 .5,.6 .8,0 1,1"}, {'X', "0,0 1,1|0,1 1,0"}, {'Y', "0,1 .5,.5 1,1|.5,.5 .5,0"}, {'Z', "0,1 1,1 0,0 1,0"},
      {'0', ".25,0 0,.25 0,.75 .25,1 .75,1 1,.75 1,.25 .75,0 .25,0|.15,.15 .85,.85"},
      {'1', ".2,.8 .5,1 .5,0|.2,0 .8,0"}, {'2', "0,.8 .25,1 .75,1 1,.8 1,.65 0,0 1,0"},
      {'3', "0,1 1,1 .5,.5 1,.25 .75,0 0,0"}, {'4', ".75,0 .75,1 0,.3 1,.3"}, {'5', "1,1 0,1 0,.55 .75,.55 1,.3 .75,0 0,0"},
      {'6', "1,1 .25,1 0,.7 0,.2 .25,0 .75,0 1,.2 1,.45 .75,.6 0,.6"},
      {'7', "0,1 1,1 .3,0"}, {'8', ".25,.5 0,.75 .25,1 .75,1 1,.75 .75,.5 .25,.5 0,.25 .25,0 .75,0 1,.25 .75,.5"},
      {'9', "0,0 .75,0 1,.3 1,.8 .75,1 .25,1 0,.8 0,.55 .25,.4 1,.4"},
      {'-', "0,.5 1,.5"}, {'_', "0,0 1,0"}, {'+', "0,.5 1,.5|.5,0 .5,1"}, {'/', "0,0 1,1"},
      {'.', ".45,0 .55,0"}, {',', ".5,.1 .35,-.15"}, {':', ".45,.25 .55,.25|.45,.75 .55,.75"},
      {'(', ".75,1 .25,.75 .25,.25 .75,0"}, {')', ".25,1 .75,.75 .75,.25 .25,0"},
      {'!', ".5,1 .5,.25|.45,0 .55,0"}, {'?', "0,.8 .25,1 .75,1 1,.75 .5,.5 .5,.25|.45,0 .55,0"}};
  return table;
}

constexpr double kWidth = 0.65;    // a glyph's drawn width
constexpr double kAdvance = 0.9;   // from one glyph to the next
constexpr double kSpace = 0.55;    // a space

// Polylines of one glyph at `advance`.
std::vector<std::vector<std::pair<double, double>>> glyph_lines(char c, double advance) {
  std::vector<std::vector<std::pair<double, double>>> out;
  std::stringstream strokes(glyphs().at(c));
  std::string stroke;
  while (std::getline(strokes, stroke, '|')) {
    std::vector<std::pair<double, double>> line;
    std::stringstream points(stroke);
    std::string pair;
    while (points >> pair) {
      // from_chars ignores the C locale (a Qt process in an Arabic locale must still read ".5").
      const size_t comma = pair.find(',');
      double x = 0, y = 0;
      std::from_chars(pair.data(), pair.data() + comma, x);
      std::from_chars(pair.data() + comma + 1, pair.data() + pair.size(), y);
      line.push_back({advance + x * kWidth, y});
    }
    out.push_back(std::move(line));
  }
  return out;
}

std::vector<std::vector<std::vector<std::pair<double, double>>>> layout(const std::string& text) {
  std::vector<std::vector<std::vector<std::pair<double, double>>>> glyphs_out;
  double advance = 0;
  for (unsigned char raw : text) {
    if (std::isspace(raw)) {
      advance += kSpace;
      continue;
    }
    const char c = static_cast<char>(std::toupper(raw));
    if (!glyphs().count(c))
      throw Error(std::string("the built-in font has no '") + static_cast<char>(raw) + "' (it has Latin letters, digits and - _ + / . , : ( ) ! ?)");
    glyphs_out.push_back(glyph_lines(c, advance));
    advance += kAdvance;
  }
  return glyphs_out;
}

// A stroke segment as a rectangle `width` wide, square ends reaching past the points by half the width, so the
// segments of a stroke overlap at every corner.
TopoDS_Shape segment_face(gp_Pnt a, gp_Pnt b, double width) {
  const double h = width / 2, dx = b.X() - a.X(), dy = b.Y() - a.Y(), len = std::hypot(dx, dy);
  const double ux = len > 1e-12 ? dx / len : 1, uy = len > 1e-12 ? dy / len : 0, nx = -uy, ny = ux;
  auto corner = [&](const gp_Pnt& p, double along, double side) { return gp_Pnt(p.X() + ux * along + nx * side, p.Y() + uy * along + ny * side, 0); };
  BRepBuilderAPI_MakePolygon poly(corner(a, -h, -h), corner(b, h, -h), corner(b, h, h), corner(a, -h, h), Standard_True);
  return BRepBuilderAPI_MakeFace(poly.Wire(), Standard_True).Shape();
}

// Where the text starts for its alignment.
double aligned(const std::string& text, double u, const TextOptions& options) {
  if (options.align == "left") return u;
  if (options.align != "center" && options.align != "right") throw Error("text align is left, center or right");
  double width = 0;
  for (const auto& glyph : layout(text))
    for (const auto& line : glyph)
      for (const auto& p : line) width = std::max(width, p.first);
  return u - (options.align == "center" ? width / 2 : width) * options.height;
}

}  // namespace

std::vector<std::vector<std::pair<double, double>>> stroke_text(const std::string& text) {
  std::vector<std::vector<std::pair<double, double>>> out;
  for (auto& glyph : layout(text))
    for (auto& line : glyph) out.push_back(std::move(line));
  return out;
}

std::vector<int> add_text(Sketch& sketch, const std::string& text, double u, double v, const TextOptions& options) {
  if (text.empty() || text.size() > 512) throw Error("enter between 1 and 512 text characters");
  if (!(options.height > 0) || !std::isfinite(options.height)) throw Error("text height must be positive");
  if (options.style != "outline" && options.style != "stroke") throw Error("text style is outline or stroke");
  if (!(options.weight > 0.02 && options.weight < 0.4)) throw Error("text weight is a fraction of the height between 0.02 and 0.4");
  Sketch sk = sketch;  // atomic
  const double h = options.height;
  u = aligned(text, u, options);
  std::vector<int> made;
  for (const auto& glyph : layout(text)) {
    if (options.style == "stroke") {
      std::map<std::pair<long long, long long>, int> points;  // strokes meeting at a point share it
      auto point = [&](double x, double y) {
        const auto key = std::make_pair(std::llround(x * 1e6), std::llround(y * 1e6));
        auto it = points.find(key);
        return it != points.end() ? it->second : points[key] = sk.add_point(u + x * h, v + y * h);
      };
      for (const auto& line : glyph) {
        int last = 0;
        for (const auto& [x, y] : line) {
          const int p = point(x, y);
          if (last && last != p) made.push_back(sk.add_line(last, p, options.construction));
          last = p;
        }
      }
      continue;
    }
    // Outline: every stroke thickened and the glyph's pieces fused into closed block letters.
    TopTools_ListOfShape pieces;
    for (const auto& line : glyph)
      for (size_t i = 0; i + 1 < line.size(); ++i)
        pieces.Append(segment_face(gp_Pnt(u + line[i].first * h, v + line[i].second * h, 0), gp_Pnt(u + line[i + 1].first * h, v + line[i + 1].second * h, 0),
                                   options.weight * h));
    TopoDS_Shape letter = pieces.First();
    if (pieces.Extent() > 1) {
      TopTools_ListOfShape arguments, tools;
      arguments.Append(pieces.First());
      for (auto it = std::next(pieces.begin()); it != pieces.end(); ++it) tools.Append(*it);
      BRepAlgoAPI_Fuse fuse;
      fuse.SetArguments(arguments);
      fuse.SetTools(tools);
      fuse.Build();
      if (!fuse.IsDone()) throw Error("the text outline could not be built");
      letter = fuse.Shape();
    }
    ShapeUpgrade_UnifySameDomain unify(letter, Standard_True, Standard_True, Standard_False);
    unify.Build();
    for (int id : append_sketch_shape(sk, unify.Shape(), Frame{}, options.construction)) made.push_back(id);
  }
  sk.validate();
  sketch = std::move(sk);
  return made;
}

std::vector<std::pair<double, double>> text_stroke_points(const std::string& text, double u, double v, const TextOptions& options) {
  u = aligned(text, u, options);
  std::vector<std::pair<double, double>> out;
  for (const auto& glyph : layout(text))
    for (const auto& line : glyph)
      if (line.size() >= 2)
        out.push_back({u + (line[0].first + line[1].first) / 2 * options.height, v + (line[0].second + line[1].second) / 2 * options.height});
  return out;
}

}  // namespace opad::design
