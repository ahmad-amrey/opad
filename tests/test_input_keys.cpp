// Typed values (app/InputKeys.hpp, TODO 11 UI-16): which keys type a value (keypad digits too, never with Ctrl, Alt or
// Meta), Tab going round the boxes, a comma moving on, '@' '#' ',' '<' switching how a point is typed, the Esc rungs of the
// boxes, Up/Down stepping a value.
#include "check.hpp"
#include "InputKeys.hpp"
using namespace inputkeys;

TEST(digits_points_commas_and_signs_type_values) {
  for (char32_t c : {U'0', U'5', U'9', U'.', U',', U'-', U'+', U'('}) {
    CHECK(valueChar(c));
    CHECK(typesValue(c, false));
    CHECK(!typesValue(c, true));  // Ctrl+5 is a shortcut
  }
  for (char32_t c : {U'a', U'L', U' ', U'@', U'#', U'/', U'*', U'\t', U'\r', U'\x1b', U'\b', U'\0'}) CHECK(!valueChar(c));
}

TEST(tab_goes_round_the_boxes_of_the_step) {
  CHECK_EQ(cycle(-1, 2, false), 0);  // nothing typed yet: Tab takes the first box, Shift+Tab the last
  CHECK_EQ(cycle(-1, 2, true), 1);
  CHECK_EQ(cycle(0, 2, false), 1);
  CHECK_EQ(cycle(1, 2, false), 0);  // round within the step
  CHECK_EQ(cycle(0, 2, true), 1);
  CHECK_EQ(cycle(0, 1, false), 0);  // one box: Tab stays on it
  CHECK_EQ(cycle(3, 4, false), 0);
  CHECK_EQ(cycle(0, 4, true), 3);
  CHECK_EQ(cycle(5, 2, false), 0);  // a box that went away
  CHECK_EQ(cycle(0, 0, false), -1);
}

TEST(a_comma_moves_on_between_boxes_and_is_a_decimal_comma_alone) {
  CHECK(comma(2) == Comma::NextBox);
  CHECK(comma(4) == Comma::NextBox);
  CHECK(comma(1) == Comma::Decimal);
}

TEST(prefixes_and_separators_switch_how_a_point_is_typed) {
  for (char32_t c : {U'@', U'#', U'<'}) CHECK(entryChar(c) && !valueChar(c));
  CHECK(!entryChar(U'5') && !entryChar(U','));
  // '#' first: absolute; '@' first: relative to the last point (none yet: dropped); either later in a box: dropped.
  auto k = entryKey(Entry::Polar, 0, true, U'#', true);
  CHECK(k.turn == Turn::Switch && k.to == Entry::Absolute && k.focus == 0);
  CHECK(entryKey(Entry::Absolute, 0, true, U'#', true).turn == Turn::Swallow);
  CHECK(entryKey(Entry::Polar, 0, false, U'#', true).turn == Turn::Swallow);
  k = entryKey(Entry::Absolute, 0, true, U'@', true);
  CHECK(k.turn == Turn::Switch && k.to == Entry::Relative && k.focus == 0);
  CHECK(entryKey(Entry::Absolute, 0, true, U'@', false).turn == Turn::Swallow);
  CHECK(entryKey(Entry::Absolute, 1, true, U'@', true).turn == Turn::Swallow);
  CHECK(entryKey(Entry::Relative, 0, true, U'@', true).turn == Turn::Swallow);
  // A comma after a length: ΔX then ΔY, the length kept as ΔX; elsewhere it is the ordinary comma.
  k = entryKey(Entry::Polar, 0, false, U',', true);
  CHECK(k.turn == Turn::Carry && k.to == Entry::Relative && k.focus == 1);
  CHECK(entryKey(Entry::Polar, 0, true, U',', true).turn == Turn::Type);
  CHECK(entryKey(Entry::Polar, 1, false, U',', true).turn == Turn::Type);
  CHECK(entryKey(Entry::Absolute, 0, false, U',', true).turn == Turn::Type);
  CHECK(entryKey(Entry::Relative, 0, false, U',', true).turn == Turn::Type);
  // '<' after a number: on to the angle (from X/Y or ΔX/ΔY the number becomes the length); nothing to measure from: dropped.
  k = entryKey(Entry::Polar, 0, false, U'<', true);
  CHECK(k.turn == Turn::Next && k.focus == 1);
  k = entryKey(Entry::Relative, 0, false, U'<', true);
  CHECK(k.turn == Turn::Carry && k.to == Entry::Polar && k.focus == 1);
  CHECK(entryKey(Entry::Absolute, 0, false, U'<', true).turn == Turn::Carry);
  CHECK(entryKey(Entry::Absolute, 0, false, U'<', false).turn == Turn::Swallow);
  CHECK(entryKey(Entry::Polar, 0, true, U'<', true).turn == Turn::Swallow);
  CHECK(entryKey(Entry::Absolute, 0, false, U'5', true).turn == Turn::Type);
  // A shape's sizes (UI-17: a rectangle's width and height): '#' and '@' switch to the point's boxes, '<' after the first
  // size goes on to the next, a comma is the ordinary one (the next box).
  k = entryKey(Entry::Shape, 0, true, U'#', true);
  CHECK(k.turn == Turn::Switch && k.to == Entry::Absolute);
  k = entryKey(Entry::Shape, 0, true, U'@', true);
  CHECK(k.turn == Turn::Switch && k.to == Entry::Relative);
  k = entryKey(Entry::Shape, 0, false, U'<', true);
  CHECK(k.turn == Turn::Next && k.to == Entry::Shape && k.focus == 1);
  CHECK(entryKey(Entry::Shape, 0, false, U',', true).turn == Turn::Type);
}

TEST(esc_undoes_the_edit_then_the_typed_values_then_is_the_tools) {
  CHECK(escape(true, true) == Esc::RevertBox);
  CHECK(escape(true, false) == Esc::RevertBox);
  CHECK(escape(false, true) == Esc::ClearTyped);
  CHECK(escape(false, false) == Esc::PassOn);
}

TEST(up_and_down_step_the_number_and_keep_the_unit) {
  std::string t = "12.5 mm";
  CHECK(nudge(t, 1, step(false, false)) && t == "13.5 mm");
  t = "10";
  CHECK(nudge(t, -1, step(true, false)) && t == "0");
  t = "10 mm";
  CHECK(nudge(t, 1, step(false, true)) && t == "10.1 mm");
  t = "-0.5";
  CHECK(nudge(t, 1, step(false, true)) && t == "-0.4");
  t = "30 deg";
  CHECK(nudge(t, -1, step(false, false)) && t == "29 deg");
  t = "w/2";
  CHECK(!nudge(t, 1, 1) && t == "w/2");  // an expression is left alone
  t = "";
  CHECK(!nudge(t, 1, 1));
  CHECK(step(true, true) == 10);
}

CHECK_MAIN()
