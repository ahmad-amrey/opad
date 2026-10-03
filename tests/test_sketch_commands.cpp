// The sketch's command line (app/SketchCommands.hpp, TODO 11 UI-133): words and short forms run tools, every tool word
// names a tool the steps table knows, coordinates become the keys the value boxes take (and read through InputKeys.hpp's
// rules as the boxes read them: x,y absolute, @dx,dy relative, @len<ang polar), Up and Down go through what was entered.
#include "check.hpp"
#include "InputKeys.hpp"
#include "SketchCommands.hpp"
#include "SketchSteps.hpp"
#include <set>

using namespace sketchcommands;
using inputkeys::Entry;

namespace {
// The boxes as DynamicInput fills them from typed keys (its type() and the sketch's entry hook): '@', '#' and '<' through
// inputkeys::entryKey, a comma to the next box when there are two, anything else into the box being typed.
struct Boxes {
  Entry entry;
  bool base;
  std::vector<std::string> text{"", ""};
  int current = -1;
  void type(const std::string& keys) {
    for (char c : keys) {
      if (current < 0) current = 0;
      const auto key = inputkeys::entryKey(entry, current, text[size_t(current)].empty(), char32_t(c), base);
      switch (key.turn) {
        case inputkeys::Turn::Swallow: continue;
        case inputkeys::Turn::Next: current = inputkeys::cycle(current, 2, false); continue;
        case inputkeys::Turn::Switch:
        case inputkeys::Turn::Carry: {
          const std::string carried = key.turn == inputkeys::Turn::Carry ? text[size_t(current)] : std::string();
          entry = key.to;
          text = {carried, ""};
          current = key.focus;
          continue;
        }
        case inputkeys::Turn::Type: break;
      }
      if (c == ',' && inputkeys::comma(2) == inputkeys::Comma::NextBox) { current = inputkeys::cycle(current, 2, false); continue; }
      text[size_t(current)] += c;
    }
  }
};
Boxes typed(const std::string& line, Entry entry, bool base) {
  Boxes b{entry, base};
  b.type(keys(line, true));
  return b;
}
}  // namespace

TEST(words_and_short_forms_run_tools) {
  CHECK(find("L") && std::string(find("L")->id) == "line");
  CHECK(std::string(find("line")->id) == "line");
  CHECK(std::string(find("  Rec ")->id) == "rect");
  CHECK(std::string(find("RECTANGLE")->id) == "rect");
  CHECK(std::string(find("c")->id) == "circle");
  CHECK(std::string(find("TR")->id) == "trim");
  CHECK(std::string(find("o")->id) == "offset");
  CHECK(std::string(find("M")->id) == "move");
  CHECK(std::string(find("f")->id) == "fillet");
  CHECK(std::string(find("ex")->id) == "extend");
  CHECK(std::string(find("co")->id) == "copy");
  CHECK(std::string(find("tangent_arc")->id) == "tangent_arc");  // a tool's id is a word too
  CHECK(std::string(find("c:fix")->id) == "c:fix");
  CHECK(std::string(find("u")->id) == "undo");
  CHECK(std::string(find("paste")->id) == "paste");
  CHECK(!find("") && !find("10") && !find("width") && !find("lin"));
}

TEST(every_word_is_one_command_and_every_tool_has_its_steps) {
  std::set<std::string> words;
  const std::set<std::string> actions = {"undo", "redo", "delete", "close", "fit", "copyclip", "cut", "paste", "finish", "cancel"};
  for (const Command& c : commands()) {
    CHECK(actions.count(c.id) || sketchsteps::find(c.id));  // a tool the prompt and the panel know
    CHECK(*c.name);
    for (const char* w : c.words) {
      CHECK(words.insert(w).second);  // no word runs two things
      CHECK(lower(w) == w);
      CHECK(find(w) == &c);
    }
  }
}

TEST(a_word_is_letters_a_value_is_anything_else) {
  CHECK(word("trim") && word("c:fix") && word(" L "));
  CHECK(!word("10") && !word("@10,0") && !word("w/2") && !word("") && !word("10 mm"));
}

TEST(coordinates_read_as_on_a_drafting_command_line) {
  CHECK(keys("10,20", true) == "#10,20");  // absolute, not the boxes' ΔX after a length
  CHECK(keys(" 10 , 20 ", true) == "#10 , 20");
  CHECK(keys("@10,20", true) == "@10,20");
  CHECK(keys("#10,20", true) == "#10,20");
  CHECK(keys("@30<45", true) == "@30<45");
  CHECK(keys("30<45", true) == "30<45");
  CHECK(keys("25", true) == "25");
  CHECK(keys("3,10", false) == "3,10");  // a tool's values (a pattern's count and spacing) as typed
  CHECK(bare("25") && bare("width / 2") && !bare("1,2") && !bare("@3") && !bare("3<4") && !bare(" "));
  std::string length, angle;
  CHECK(polar("30<45", length, angle) && length == "30" && angle == "45");
  CHECK(polar(" 2*w < 90 deg ", length, angle) && length == "2*w" && angle == "90 deg");
  CHECK(!polar("@30<45", length, angle) && !polar("30", length, angle) && !polar("<45", length, angle) && !polar("1<2<3", length, angle));
}

TEST(the_keys_fill_the_boxes_as_typed_over_the_view) {
  // The next point of a polyline (polar boxes): x,y goes absolute, @dx,dy relative, @len<ang and len<ang polar, a bare
  // value is the length.
  auto b = typed("40,30", Entry::Polar, true);
  CHECK(b.entry == Entry::Absolute && b.text[0] == "40" && b.text[1] == "30");
  b = typed("@40,-30", Entry::Polar, true);
  CHECK(b.entry == Entry::Relative && b.text[0] == "40" && b.text[1] == "-30");
  b = typed("@40<30", Entry::Polar, true);
  CHECK(b.entry == Entry::Polar && b.text[0] == "40" && b.text[1] == "30");
  b = typed("40<30", Entry::Polar, true);
  CHECK(b.entry == Entry::Polar && b.text[0] == "40" && b.text[1] == "30");
  b = typed("25", Entry::Polar, true);
  CHECK(b.entry == Entry::Polar && b.text[0] == "25" && b.text[1].empty());
  // A rectangle's opposite corner (the shape's width and height): x,y is the corner itself, @dx,dy from the first.
  b = typed("40,30", Entry::Shape, true);
  CHECK(b.entry == Entry::Absolute && b.text[0] == "40" && b.text[1] == "30");
  b = typed("@40,30", Entry::Shape, true);
  CHECK(b.entry == Entry::Relative && b.text[0] == "40" && b.text[1] == "30");
  // The first point (X and Y, nothing to measure from): '@' has no base and is dropped, so @x,y is x,y from the origin.
  b = typed("5,6", Entry::Absolute, false);
  CHECK(b.entry == Entry::Absolute && b.text[0] == "5" && b.text[1] == "6");
  b = typed("@5,6", Entry::Absolute, false);
  CHECK(b.entry == Entry::Absolute && b.text[0] == "5" && b.text[1] == "6");
}

TEST(up_and_down_go_through_what_was_entered) {
  // Three entries; 3 is the line being typed.
  CHECK_EQ(recall(3, 3, true), 2);
  CHECK_EQ(recall(2, 3, true), 1);
  CHECK_EQ(recall(0, 3, true), 0);  // the oldest stays
  CHECK_EQ(recall(0, 3, false), 1);
  CHECK_EQ(recall(2, 3, false), 3);  // back to the line being typed
  CHECK_EQ(recall(3, 3, false), 3);
  CHECK_EQ(recall(7, 3, true), 2);
  CHECK_EQ(recall(0, 0, true), -1);
}

CHECK_MAIN()
