#pragma once
// Typed values (TODO 11 UI-16), decided without widgets: which keys type a value, where Tab and a comma go, what Esc does
// while values are typed, how the arrow keys step a value. The value boxes (DynamicInput), the offset's handle and the
// sketch read the same answers. No Qt here: tests/test_input_keys.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace inputkeys {
// A key that types into a value box: a digit (top row or keypad), the decimal point, a comma, a sign, a bracket that opens
// an expression. Shift may be held (a '+', and the digits of layouts that shift them); with Ctrl, Alt or Meta it is a
// shortcut.
inline bool valueChar(char32_t c) { return (c >= U'0' && c <= U'9') || c == U'.' || c == U',' || c == U'-' || c == U'+' || c == U'('; }
inline bool typesValue(char32_t c, bool command) { return !command && valueChar(c); }

// The box Tab (back: Shift+Tab) goes to among `count`: round within the step; from none, the first (back: the last).
inline int cycle(int current, int count, bool back) {
  if (count <= 0) return -1;
  if (current < 0 || current >= count) return back ? count - 1 : 0;
  return ((back ? current - 1 : current + 1) % count + count) % count;
}

// A comma between boxes moves on (40,30 is X then Y, as typed in AutoCAD); with one box it is the decimal comma.
enum class Comma { NextBox, Decimal };
inline Comma comma(int count) { return count > 1 ? Comma::NextBox : Comma::Decimal; }

// How the next point is typed: X and Y in the sketch (absolute), ΔX and ΔY from the last point (relative), a length and
// an angle from it (polar, how a polyline goes on), or the shape's own sizes (UI-17: a rectangle's width and height, a
// circle's diameter, a slot's width). As typed in AutoCAD a prefix or a separator switches: '#' first is absolute, '@'
// first relative, a comma after a length makes it ΔX (then ΔY), '<' after a number goes on to an angle (the shape's next
// size). What a key does to the entry (`box` the box it is typed into, `empty` that box, `base` a last point to measure
// from):
//   Type: an ordinary key (a comma: the rule above), Swallow: dropped (never part of a value), Next: the next box,
//   Switch: to the entry `to` with empty boxes, Carry: to `to` with the first box's text kept; then box `focus` is typed.
enum class Entry { Absolute, Relative, Polar, Shape };
enum class Turn { Type, Swallow, Next, Switch, Carry };
struct EntryKey { Turn turn = Turn::Type; Entry to = Entry::Absolute; int focus = 0; };
inline bool entryChar(char32_t c) { return c == U'@' || c == U'#' || c == U'<'; }
inline EntryKey entryKey(Entry entry, int box, bool empty, char32_t c, bool base) {
  const bool first = box <= 0;
  if (c == U'#') return first && empty && entry != Entry::Absolute ? EntryKey{Turn::Switch, Entry::Absolute, 0} : EntryKey{Turn::Swallow};
  if (c == U'@') return first && empty && base && entry != Entry::Relative ? EntryKey{Turn::Switch, Entry::Relative, 0} : EntryKey{Turn::Swallow};
  if (c == U'<') {
    if (!first || empty || !base) return {Turn::Swallow};
    return entry == Entry::Polar || entry == Entry::Shape ? EntryKey{Turn::Next, entry, 1} : EntryKey{Turn::Carry, Entry::Polar, 1};
  }
  if (c == U',' && entry == Entry::Polar && first && !empty) return {Turn::Carry, Entry::Relative, 1};
  return {};
}

// Esc while values are typed: the edit of the box that has the keyboard is undone first, then every typed value is
// dropped, then Esc is the tool's (its own ladder: end the step, close the tool, clear the selection).
enum class Esc { RevertBox, ClearTyped, PassOn };
inline Esc escape(bool boxEdited, bool anyTyped) { return boxEdited ? Esc::RevertBox : anyTyped ? Esc::ClearTyped : Esc::PassOn; }

// Up/Down (or the wheel) step the number a value starts with, by 1 (Shift 10, Ctrl 0.1), keeping its decimals and what
// follows it (a unit). False when the text does not start with a number (an expression is left alone).
inline double step(bool shift, bool control) { return shift ? 10 : control ? 0.1 : 1; }
inline bool nudge(std::string& text, double steps, double size) {
  const char* begin = text.c_str();
  while (*begin == ' ') ++begin;
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  if (end == begin || !std::isfinite(value)) return false;
  int decimals = size < 1 ? 1 : 0;
  if (const char* dot = static_cast<const char*>(std::memchr(begin, '.', static_cast<std::size_t>(end - begin))))
    decimals = std::max(decimals, static_cast<int>(end - dot - 1));
  const double next = value + steps * size;
  char number[64];
  std::snprintf(number, sizeof number, "%.*f", decimals, std::fabs(next) < 1e-9 ? 0.0 : next);
  text = number + std::string(end);
  return true;
}
}  // namespace inputkeys
