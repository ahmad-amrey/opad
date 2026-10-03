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
// A key that types into a value box: a digit (top row or keypad), the decimal point, a comma, a sign. Shift may be held
// (a '+', and the digits of layouts that shift them); with Ctrl, Alt or Meta it is a shortcut.
inline bool valueChar(char32_t c) { return (c >= U'0' && c <= U'9') || c == U'.' || c == U',' || c == U'-' || c == U'+'; }
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
