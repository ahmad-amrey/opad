#pragma once
// Text in sketches from the built-in stroke font (TODO 10 B4): the same glyphs on every machine, so a sketch made
// from text is the same wherever it was made. Latin capitals (lower case is shown in capitals), digits and basic
// punctuation.
#include <string>
#include <utility>
#include <vector>

#include "sketch.hpp"

namespace opad::design {

// Each glyph's strokes as polylines, laid out from (0, 0) along +x on the baseline, cap height 1. Throws naming the
// first character the font does not have.
std::vector<std::vector<std::pair<double, double>>> stroke_text(const std::string& text);

struct TextOptions {
  double height = 10;          // cap height, mm
  std::string style = "outline";  // outline: closed block letters (profiles to extrude); stroke: single lines
  double weight = 0.14;        // outline stroke width as a fraction of the height
  std::string align = "left";  // left, center or right of (u, v)
  bool construction = false;
};

// Adds `text` to the sketch with its baseline at (u, v). Returns the entities made.
std::vector<int> add_text(Sketch& sketch, const std::string& text, double u, double v, const TextOptions& options);
// A point in the middle of every stroke's first segment, as add_text lays the text out: inside the letters of the
// outline style, so a profile `at` any of them picks that letter.
std::vector<std::pair<double, double>> text_stroke_points(const std::string& text, double u, double v, const TextOptions& options);

}  // namespace opad::design
