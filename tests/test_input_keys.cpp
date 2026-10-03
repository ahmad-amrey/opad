// Typed values (app/InputKeys.hpp, TODO 11 UI-16): which keys type a value (keypad digits too, never with Ctrl, Alt or
// Meta), Tab going round the boxes, a comma moving on, the Esc rungs of the boxes, Up/Down stepping a value.
#include "check.hpp"
#include "InputKeys.hpp"
using namespace inputkeys;

TEST(digits_points_commas_and_signs_type_values) {
  for (char32_t c : {U'0', U'5', U'9', U'.', U',', U'-', U'+'}) {
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
