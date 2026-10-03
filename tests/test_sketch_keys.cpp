// What Backspace, Enter and Esc do in the sketch (app/SketchKeys.hpp, TODO 11 UI-20): Undo point never leaves the tool,
// Done ends a chain and keeps the tool, Esc ends the step in progress, then closes the tool, then clears the selection.
#include "check.hpp"
#include "SketchKeys.hpp"
using namespace sketchkeys;

static State tool(const char* name) {
  State s;
  s.tool = name;
  return s;
}

TEST(a_polyline_takes_points_back_and_ends_on_enter_or_esc) {
  State s = tool("line");
  CHECK(backspace(s) == Back::None);  // waiting for the start: nothing to take back, Enter does nothing, Esc closes
  CHECK(enter(s) == Enter::None);
  CHECK(escape(s) == Esc::CloseTool);
  for (std::size_t n : {1, 2, 5}) {
    s.chain = n;
    CHECK(backspace(s) == Back::UndoPoint);
    CHECK(enter(s) == Enter::EndChain);
    CHECK(escape(s) == Esc::EndChain);  // the first Esc ends the chain, never the tool
  }
  s.selection = true;  // a selection never turns Backspace into Delete while a tool runs
  s.chain = 0;
  CHECK(backspace(s) == Back::None);
  CHECK(escape(s) == Esc::CloseTool);
}

TEST(splines_are_chains_also_with_control_points) {
  State s = tool("spline");
  s.chain = 3;
  CHECK(enter(s) == Enter::EndChain && escape(s) == Esc::EndChain && backspace(s) == Back::UndoPoint);
  State c = tool("control_spline");
  c.clicks = 4;
  CHECK(enter(c) == Enter::EndChain && escape(c) == Esc::EndChain && backspace(c) == Back::UndoPoint);
  CHECK(chainTool("line") && chainTool("spline") && chainTool("control_spline") && !chainTool("rect") && !chainTool("select"));
}

TEST(a_shape_made_on_its_last_click_takes_clicks_back_and_esc_drops_it) {
  for (const char* name : {"rect", "circle", "arc3", "slot", "polygon", "conic", "image_calibrate"}) {
    State s = tool(name);
    s.clicks = 1;
    CHECK(backspace(s) == Back::UndoPoint);
    CHECK(enter(s) == Enter::None);
    CHECK(escape(s) == Esc::CancelStep);
    s.clicks = 0;
    CHECK(backspace(s) == Back::None);
    CHECK(escape(s) == Esc::CloseTool);
  }
}

TEST(picks_go_back_as_picks) {
  for (const char* name : {"dimension", "c:horizontal", "tangent_circle"}) {
    State s = tool(name);
    s.picks = 1;
    CHECK(backspace(s) == Back::UndoPick);
    CHECK(escape(s) == Esc::CancelStep);
  }
  for (const char* name : {"extend", "union", "subtract", "intersect"}) {  // clicks that pick curves or loops
    State s = tool(name);
    s.clicks = 1;
    CHECK(backspace(s) == Back::UndoPick);
    CHECK(escape(s) == Esc::CancelStep);
  }
  State t = tool("tangent_arc");  // its first click picks the line end and places the start point
  t.clicks = 1;
  t.picks = 1;
  CHECK(backspace(t) == Back::UndoPoint);
}

TEST(mirror_goes_from_curves_to_the_line_and_back) {
  State s = tool("mirror");
  s.mirrorSeeds = true;
  CHECK(enter(s) == Enter::None);  // no curves chosen yet
  s.selection = true;
  CHECK(enter(s) == Enter::PickMirrorLine);
  CHECK(escape(s) == Esc::CloseTool);
  s.mirrorSeeds = false;
  s.mirrorAxis = true;
  CHECK(enter(s) == Enter::None);
  CHECK(escape(s) == Esc::BackToCurves);
  s.picks = 1;  // a line picked: Esc and Backspace take it back first
  CHECK(escape(s) == Esc::CancelStep);
  CHECK(backspace(s) == Back::UndoPick);
}

TEST(the_select_tool_clears_and_deletes_its_selection) {
  State s = tool("select");
  CHECK(backspace(s) == Back::None && enter(s) == Enter::None && escape(s) == Esc::None);
  s.selection = true;
  CHECK(backspace(s) == Back::Delete);
  CHECK(escape(s) == Esc::ClearSelection);
  s.boxSelecting = true;  // a band being dragged goes first
  CHECK(escape(s) == Esc::CancelBox);
}

TEST(the_ladder_ends_the_step_then_closes_the_tool) {
  // A polyline with two segments: Esc ends it (state after: no chain), the next Esc closes the tool, the next clears the
  // selection, and then Esc has nothing left to do.
  State s = tool("line");
  s.chain = 3;
  s.selection = true;
  CHECK(escape(s) == Esc::EndChain);
  s.chain = 0;
  CHECK(escape(s) == Esc::CloseTool);
  s.tool = "select";
  CHECK(escape(s) == Esc::ClearSelection);
  s.selection = false;
  CHECK(escape(s) == Esc::None);
}

TEST(typed_values_come_first) {
  // UI-16: values typed into the tool's boxes are used by Enter (not the end of the chain) and dropped by the first Esc,
  // before the chain ends; Backspace still takes points back (in a box it edits the text, which the box does itself).
  State s = tool("line");
  s.chain = 2;
  s.typed = true;
  CHECK(enter(s) == Enter::UseTyped);
  CHECK(escape(s) == Esc::DropTyped);
  CHECK(backspace(s) == Back::UndoPoint);
  s.typed = false;
  CHECK(enter(s) == Enter::EndChain && escape(s) == Esc::EndChain);
  State o = tool("offset");  // a distance typed before anything is selected
  o.typed = true;
  CHECK(enter(o) == Enter::UseTyped && escape(o) == Esc::DropTyped);
  o.typed = false;
  CHECK(enter(o) == Enter::None && escape(o) == Esc::CloseTool);
  o.selection = o.applies = true;  // the curves picked: Enter applies, as the Apply button does
  CHECK(enter(o) == Enter::Apply && escape(o) == Esc::CloseTool);
  o.typed = true;  // a value typed for them is used first (and applied with it)
  CHECK(enter(o) == Enter::UseTyped);
  State b = tool("select");  // a band being dragged still goes first
  b.boxSelecting = b.typed = true;
  CHECK(escape(b) == Esc::CancelBox);
}

TEST(a_lock_left_on_by_a_shift_tap_is_the_next_esc) {
  // UI-19: Esc lets go of the line and the chain goes on; typed values still go first; Enter and Backspace are as they were.
  State s = tool("line");
  s.chain = 2;
  s.locked = true;
  CHECK(escape(s) == Esc::Unlock);
  CHECK(enter(s) == Enter::EndChain && backspace(s) == Back::UndoPoint);
  s.typed = true;
  CHECK(escape(s) == Esc::DropTyped);
  s.typed = s.locked = false;
  CHECK(escape(s) == Esc::EndChain);
  State r = tool("rect");  // before the first corner too: then the tool closes
  r.locked = true;
  CHECK(escape(r) == Esc::Unlock);
  r.locked = false;
  CHECK(escape(r) == Esc::CloseTool);
}

TEST(a_shift_tap_on_a_lock_that_stays_goes_to_its_next_stop_or_lets_go) {
  State s = tool("line");
  s.chain = 2;
  CHECK(shift(s) == Shift::None);  // not locked: a tap makes a lock
  s.locked = true;
  CHECK(shift(s) == Shift::Release);  // no stop along it (or only the one the pointer is on)
  s.stops = 2;
  CHECK(shift(s) == Shift::NextStop);
  CHECK(escape(s) == Esc::Unlock);  // Esc still lets go
}

CHECK_MAIN()
