#pragma once
// Plotting 2D drawings (UI-88), without Qt so tests/test_drawing2d covers it: what a plot draws (the visible bodies of
// drawings on plotted layers, in their colours, lineweights and linetypes, and their raster images), in which plane, and
// where it lands on paper (the plot area, fit or 1:N, centred, within the margins). PlotDialog paints it with QPainter for the preview, a PDF and
// a printer, so all three are the same picture.
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "Drawing2D.hpp"
#include "opad/document.hpp"
#include "opad/scene.hpp"

namespace plot {
using Pt = std::array<double, 2>;  // in the plot plane, drawing millimetres

struct Style {
  drawing2d::Rgb color{0, 0, 0};
  bool ink = false;              // no colour of its own (DXF colour 7): black on paper
  double weight = -1;            // the layer's lineweight (paper mm); < 0: the default
  std::vector<double> dashes;    // drawing2d::dashes of the layer's linetype; empty: continuous
  bool operator==(const Style&) const = default;
};
struct Item {
  int style = 0;
  bool fill = false;                  // a fill (hatch, solid, text): its rings filled even-odd; else one polyline per ring
  bool dot = false;                   // a point of its own
  std::vector<std::vector<Pt>> rings;
};
// A raster image of a drawing (an SVG's embedded picture): its data and where its corners land in the plot plane.
struct Image {
  std::string href;    // data:image/...;base64,...
  std::string aspect;  // the SVG's preserveAspectRatio ("none": stretched to its frame)
  Pt origin, right, down;  // its top left, top right and bottom left corners
};
struct Sheet {
  opad::Frame plane;  // where the plot looks at the drawings from
  std::vector<Style> styles;
  std::vector<Item> items;
  std::vector<Image> images;  // painted under the lines
  double x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // the extents (x0 > x1: nothing to plot)
  int bodies = 0;
  bool empty() const { return items.empty() && images.empty(); }
};

// The drawings' own plane when they all lie in one (the first drawing's), else `fallback` (the view's).
opad::Frame plane(const opad::Document& doc, const opad::Scene& scene, const opad::Frame& fallback);
// Every visible drawing body on a plotted layer, its curves sampled finely enough for paper (worker thread).
Sheet collect(const opad::Document& doc, const opad::Scene& scene, const opad::Frame& plane, const std::function<bool()>& cancelled = {});

struct Area {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double width() const { return x1 - x0; }
  double height() const { return y1 - y0; }
};
enum class Region { Extents, Display, Window };
struct Settings {
  Region region = Region::Extents;
  Area display, window;      // the view's and a picked window, in the plot plane
  double paperWidth = 297, paperHeight = 210;  // as it lies (landscape: the long side across)
  double margin = 10;        // mm on every side
  bool fit = true;           // else paper mm per drawing mm is `scale` (1:N = 1 / N)
  double scale = 1;
  bool monochrome = false;   // every colour black
  bool lineweights = true;   // the layers' lineweights; off: every line kThinnest
  std::string stamp;         // the plot stamp (file, date, paper, scale) along the bottom margin; empty: none
};
constexpr double kDefaultWeight = 0.25, kThinnest = 0.13;  // paper mm
// How dashes come out on paper: as on screen (Drawing2D's kPatternPixelsPerMm at 96 dpi), mm on paper per pattern mm.
constexpr double kPatternPaper = drawing2d::kPatternPixelsPerMm * 25.4 / 96;

// Where the plot area lands: paper = (u - area.x0) * scale + x, (area.y1 - v) * scale + y (paper y down), centred in the
// printable rectangle; clipped when it does not fit at the scale asked for.
struct Placement {
  Area area;
  double scale = 1, x = 0, y = 0;
  bool clipped = false;
  bool valid() const { return area.width() > 0 && area.height() > 0 && scale > 0; }
};
Area areaOf(const Sheet& sheet, const Settings& settings);
Placement place(const Sheet& sheet, const Settings& settings);
// A style as it goes to paper: its colour (black in monochrome, black for the ink) and width.
drawing2d::Rgb paperColor(const Style& style, const Settings& settings);
double paperWeight(const Style& style, const Settings& settings);
// The paper scale as people say it: "1:50", "2:1", "1:37.42" (fit).
std::string scaleText(double scale);
}  // namespace plot
