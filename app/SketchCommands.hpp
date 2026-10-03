#pragma once
// The sketch's command line (TODO 11 UI-133), decided without widgets: which words run what, and how a typed coordinate
// becomes the keys the tool's value boxes take (DynamicInput, InputKeys.hpp), so the command line and the boxes beside the
// pointer read a point the same way. Generic names only: a tool's own name, its id, and the short forms drafting programs
// share (L, C, REC, TR, O, M, ...). No Qt here: tests/test_sketch_commands.cpp.
//   x,y        absolute (sent as "#x,y": in the boxes a comma after a length would make it ΔX, ΔY)
//   #x,y       absolute
//   @dx,dy     from the last point (the step's base)
//   @len<ang   from the last point by length and angle
//   len<ang    the same (as typed in the boxes); with no last point, from the sketch origin
//   value      the step's first box: a length towards the pointer, a size (a circle's diameter), a tool's value (a radius)
// A word is a command when it names one, case aside; anything else is a value (a parameter or an expression such as
// "width/2" is one).
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace sketchcommands {
constexpr const char* tr(const char* text) { return text; }

struct Command {
  const char* id;    // a sketch tool, or the editor's: undo, redo, delete, close (the polyline), fit, finish, cancel, copyclip, cut, paste
  const char* name;  // shown when the word is typed (English source text, translated where shown)
  std::vector<const char*> words;  // lower case; a tool's id is a word too
};

inline const std::vector<Command>& commands() {
  static const std::vector<Command> all = {
      {"select", tr("Select"), {"select", "sel"}},
      {"line", tr("Line"), {"line", "l"}},
      {"rect", tr("Rectangle"), {"rectangle", "rec"}},
      {"crect", tr("Centre rectangle"), {"centrerectangle", "centerrectangle"}},
      {"rect3", tr("3-point rectangle"), {"rectangle3", "3prectangle"}},
      {"circle", tr("Circle"), {"circle", "c"}},
      {"circle2", tr("2-point circle"), {"2pcircle"}},
      {"circle3", tr("3-point circle"), {"3pcircle"}},
      {"tangent_circle", tr("Tangent circle"), {"tangentcircle", "ttr"}},
      {"arc3", tr("3-point arc"), {"arc", "a"}},
      {"arcc", tr("Centre arc"), {"centrearc", "centerarc"}},
      {"tangent_arc", tr("Tangent arc"), {"tangentarc"}},
      {"ellipse", tr("Ellipse"), {"ellipse", "el"}},
      {"slot", tr("Slot"), {"slot", "sl"}},
      {"polygon", tr("Polygon"), {"polygon", "pol"}},
      {"spline", tr("Spline"), {"spline", "spl"}},
      {"point", tr("Point"), {"point", "po"}},
      {"text", tr("Text outlines"), {"text", "dt"}},
      {"trim", tr("Trim"), {"trim", "tr"}},
      {"extend", tr("Extend curve"), {"extend", "ex"}},
      {"fillet", tr("Sketch fillet"), {"fillet", "f"}},
      {"chamfer", tr("Chamfer"), {"chamfer", "cha"}},
      {"offset", tr("Offset"), {"offset", "o"}},
      {"move", tr("Move"), {"move", "m"}},
      {"copy", tr("Copy with offset"), {"copy", "co", "cp"}},
      {"rotate", tr("Rotate"), {"rotate", "ro"}},
      {"scale", tr("Scale"), {"scale", "sc"}},
      {"mirror", tr("Mirror"), {"mirror", "mi"}},
      {"split", tr("Split curve"), {"split"}},
      {"break", tr("Break at intersections"), {"breakall", "br"}},
      {"rect_pattern", tr("Rectangular pattern"), {"pattern", "array", "ar"}},
      {"polar_pattern", tr("Polar pattern"), {"polarpattern", "pp"}},
      {"dimension", tr("Dimension"), {"dimension", "dim", "d"}},
      {"project", tr("Project"), {"project"}},
      {"c:horizontal", tr("Horizontal"), {"horizontal", "hor"}},
      {"c:vertical", tr("Vertical"), {"vertical", "ver"}},
      {"c:coincident", tr("Coincident"), {"coincident", "coi"}},
      {"c:parallel", tr("Parallel"), {"parallel", "par"}},
      {"c:perpendicular", tr("Perpendicular"), {"perpendicular", "per"}},
      {"c:tangent", tr("Tangent"), {"tangent", "tan"}},
      {"c:equal", tr("Equal"), {"equal", "eq"}},
      {"c:concentric", tr("Concentric"), {"concentric", "con"}},
      {"c:midpoint", tr("Midpoint"), {"midpoint", "mid"}},
      {"c:symmetric", tr("Symmetric"), {"symmetric", "sym"}},
      {"c:collinear", tr("Collinear"), {"collinear", "col"}},
      {"c:fix", tr("Fix"), {"fix"}},
      {"undo", tr("Undo"), {"undo", "u"}},
      {"redo", tr("Redo"), {"redo"}},
      {"delete", tr("Delete"), {"delete", "erase", "e"}},
      {"close", tr("Close the polyline"), {"close", "cl"}},
      {"fit", tr("Fit"), {"fit", "zoom", "z"}},
      {"copyclip", tr("Copy to the clipboard"), {"copyclip"}},
      {"cut", tr("Cut to the clipboard"), {"cut", "cutclip"}},
      {"paste", tr("Paste from the clipboard"), {"paste", "pasteclip"}},
      {"finish", tr("Finish sketch"), {"finish", "exit"}},
      {"cancel", tr("Cancel sketch"), {"cancel"}},
  };
  return all;
}

inline std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}
inline std::string trimmed(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n"), end = text.find_last_not_of(" \t\r\n");
  return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
}

// The command a word names (by a word, or by a tool's id), else null.
inline const Command* find(const std::string& text) {
  const std::string word = lower(trimmed(text));
  if (word.empty()) return nullptr;
  for (const Command& c : commands())
    if (word == c.id) return &c;
  for (const Command& c : commands())
    for (const char* w : c.words)
      if (word == w) return &c;
  return nullptr;
}

// Letters only (and '_', ':'): an unknown one is said to be no command when it does not evaluate as a value either.
inline bool word(const std::string& text) {
  const std::string t = trimmed(text);
  return !t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isalpha(c) || c == '_' || c == ':'; });
}

// The keys the step's boxes take for a point typed on the command line (the table above). `point`: the step places a
// point (its boxes are X and Y, ΔX and ΔY, a length and an angle, or the shape's sizes); otherwise (a tool's values: an
// offset, a move's ΔX and ΔY, a pattern's counts) the text goes in as typed, a comma moving on to the next box.
inline std::string keys(const std::string& text, bool point) {
  const std::string t = trimmed(text);
  if (!point || t.empty() || t[0] == '@' || t[0] == '#') return t;
  if (t.find(',') != std::string::npos && t.find('<') == std::string::npos) return "#" + t;
  return t;
}

// "len<ang" (no '@' or '#'): its two parts, for a step with no last point to measure from (then from the origin).
inline bool polar(const std::string& text, std::string& length, std::string& angle) {
  const std::string t = trimmed(text);
  const auto at = t.find('<');
  if (t.empty() || t[0] == '@' || t[0] == '#' || at == std::string::npos || t.find('<', at + 1) != std::string::npos) return false;
  length = trimmed(t.substr(0, at));
  angle = trimmed(t.substr(at + 1));
  return !length.empty() && !angle.empty();
}

// A bare value (one number or expression, no separator): the step's first box. With no last point and boxes for X and Y it
// says nothing about where the point goes.
inline bool bare(const std::string& text) {
  const std::string t = trimmed(text);
  return !t.empty() && t.find_first_of(",<@#") == std::string::npos;
}

// The commands a word being typed may become, best first, at most `most`: an exact word, then a word or an id it begins, then
// a name (spaces and dashes aside) it begins; the shorter word first, else the table's order. `word` is what completes it
// (a short form when that is what was typed, else the command's first word). Nothing for a value (digits, '@', ',' ...).
struct Completion {
  const Command* command;
  std::string word;
};
inline std::vector<Completion> complete(const std::string& text, size_t most = 6) {
  const std::string typed = lower(trimmed(text));
  std::vector<Completion> out;
  if (typed.empty() || !std::isalpha(static_cast<unsigned char>(typed[0])) ||
      !std::all_of(typed.begin(), typed.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == ':'; }))
    return out;
  auto begins = [&typed](const std::string& w) { return w.compare(0, typed.size(), typed) == 0; };
  std::vector<std::pair<int, Completion>> ranked;  // rank: 0 exact, 1 + length of the word begun, 1000 a name begun
  for (const Command& c : commands()) {
    int rank = -1;
    std::string word;
    for (const std::string w : c.words)
      if (begins(w) && (rank < 0 || int(w.size()) + 1 < rank)) rank = w == typed ? 0 : int(w.size()) + 1, word = w;
    const std::string id = c.id;
    if (id == typed) rank = 0, word = c.words.empty() ? id : c.words.front();
    if (rank < 0 && begins(id) && (id.find(':') == std::string::npos || typed.find(':') != std::string::npos))  // "c" is no constraint's
      rank = int(id.size()) + 1, word = c.words.empty() ? id : c.words.front();
    if (rank < 0) {
      std::string name;
      for (const char* n = c.name; *n; ++n)
        if (*n != ' ' && *n != '-') name += char(std::tolower(static_cast<unsigned char>(*n)));
      if (begins(name)) rank = 1000, word = c.words.empty() ? c.id : c.words.front();
    }
    if (rank >= 0) ranked.push_back({rank, {&c, word}});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [rank, completion] : ranked)
    if (out.size() < most) out.push_back(completion);
  return out;
}

// Up and Down through what was entered: the entry `index` steps to (history.size(): the line being typed), -1 none.
inline int recall(int index, int size, bool up) {
  if (size <= 0) return -1;
  if (up) return index <= 0 ? 0 : std::min(index, size) - 1;
  return index >= size ? size : index + 1;
}
}  // namespace sketchcommands
