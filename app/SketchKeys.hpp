#pragma once
// What Backspace, Enter and Esc do in the sketch right now (TODO 11 UI-20), decided in one place from the editor's state:
// the keys, the tool panel's buttons and the prompt read the same answer, so they cannot disagree.
//   Backspace, "Undo point": the last point or pick of the step in progress goes back; it never leaves the tool. With
//     nothing in progress the select tool deletes its selection (as Del does); no other tool deletes on it.
//   Enter, "Done": ends the chain (a polyline keeps its segments, a spline is made from its points), keeps the tool.
//   Esc, a ladder: ends the step in progress (a chain as Enter does, a shape not made yet is dropped), then closes the
//     tool, then clears the selection. The same from the view, the tool panel and the main window.
//   Values typed into the tool's boxes (UI-16) come first: Enter uses them, Esc drops them. With nothing typed, Enter
//     applies a tool whose curves are picked (an offset, a move, a pattern), as its Apply button does.
//   A lock that a Shift tap left on (UI-19) is the next Esc's: it lets go of the line, the step goes on. Shift taps on it
//     go through the stops along its line (where other guides or curves cross it); with none, a tap lets go too. With
//     nothing locked and the pointer on a guide or the angle ray, Shift locks onto it: the prompt says so. With nothing
//     locked and more than one object snap in reach of the pointer (an end, a midpoint, an intersection...), a Shift tap
//     shows the next of them (UI-23); held, Shift locks as before.
// No Qt and no sketch here: tests/test_sketch_keys.cpp.
#include <cstddef>
#include <string>

namespace sketchkeys {
struct State {
  std::string tool = "select";
  std::size_t chain = 0;   // points of the polyline or spline being drawn
  std::size_t clicks = 0;  // clicks of a shape made by its last click (a control-point spline: its points)
  std::size_t picks = 0;   // curves picked by a constraint, dimension, tangent, extend or mirror tool
  bool boxSelecting = false;
  bool mirrorSeeds = false;  // mirror about a picked line, its curves being chosen: Enter goes on to the line
  bool mirrorAxis = false;   // mirror, the line being picked
  bool selection = false;
  bool typed = false;    // values typed into the tool's boxes and not used yet
  bool applies = false;  // the tool's Apply would act now: an offset, move, rotate, scale or pattern with its curves picked
  bool locked = false;   // the pointer is locked onto a guide by a Shift tap (until a click or Esc)
  std::size_t stops = 0;  // locked: the stops along the line in reach besides the one the pointer is on
  bool guide = false;     // nothing locked, the pointer on a guide or the angle ray of a tool that places points
  std::size_t snaps = 0;  // nothing locked: the other object snaps in reach of the pointer besides the one shown
};
enum class Back { None, UndoPoint, UndoPick, Delete };
enum class Enter { None, UseTyped, EndChain, PickMirrorLine, Apply };
enum class Esc { None, CancelBox, DropTyped, Unlock, BackToCurves, EndChain, CancelStep, CloseTool, ClearSelection };
enum class Shift { None, Lock, NextStop, Release, NextSnap };  // what Shift does

// Tools that draw a chain until Enter: Enter is their Done button.
inline bool chainTool(const std::string& tool) { return tool == "line" || tool == "spline" || tool == "control_spline"; }
// Clicks that pick curves or loops rather than place points.
inline bool picksByClick(const std::string& tool) { return tool == "extend" || tool == "union" || tool == "subtract" || tool == "intersect"; }
inline bool inChain(const State& s) { return s.chain || (s.tool == "control_spline" && s.clicks); }

inline Back backspace(const State& s) {
  if (s.chain || (s.clicks && !picksByClick(s.tool))) return Back::UndoPoint;
  if (s.clicks || s.picks) return Back::UndoPick;
  return s.tool == "select" && s.selection ? Back::Delete : Back::None;
}

inline Enter enter(const State& s) {
  if (s.typed) return Enter::UseTyped;
  if (inChain(s)) return Enter::EndChain;
  if (s.tool == "mirror" && s.mirrorSeeds && s.selection) return Enter::PickMirrorLine;
  return s.applies ? Enter::Apply : Enter::None;
}

inline Esc escape(const State& s) {
  if (s.boxSelecting) return Esc::CancelBox;
  if (s.typed) return Esc::DropTyped;
  if (s.locked) return Esc::Unlock;
  if (s.tool == "mirror" && s.mirrorAxis && !s.picks) return Esc::BackToCurves;
  if (inChain(s)) return Esc::EndChain;
  if (s.clicks || s.picks) return Esc::CancelStep;
  if (s.tool != "select") return Esc::CloseTool;
  return s.selection ? Esc::ClearSelection : Esc::None;
}

inline Shift shift(const State& s) {
  if (!s.locked) return s.snaps ? Shift::NextSnap : s.guide ? Shift::Lock : Shift::None;
  return s.stops ? Shift::NextStop : Shift::Release;
}
}  // namespace sketchkeys
