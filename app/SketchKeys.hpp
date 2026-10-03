#pragma once
// What Backspace, Enter and Esc do in the sketch right now (TODO 11 UI-20), decided in one place from the editor's state:
// the keys, the tool panel's buttons and the prompt read the same answer, so they cannot disagree.
//   Backspace, "Undo point": the last point or pick of the step in progress goes back; it never leaves the tool. With
//     nothing in progress the select tool deletes its selection (as Del does); no other tool deletes on it.
//   Enter, "Done": ends the chain (a polyline keeps its segments, a spline is made from its points), keeps the tool.
//   Esc, a ladder: ends the step in progress (a chain as Enter does, a shape not made yet is dropped), then closes the
//     tool, then clears the selection. The same from the view, the tool panel and the main window.
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
};
enum class Back { None, UndoPoint, UndoPick, Delete };
enum class Enter { None, EndChain, PickMirrorLine };
enum class Esc { None, CancelBox, BackToCurves, EndChain, CancelStep, CloseTool, ClearSelection };

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
  if (inChain(s)) return Enter::EndChain;
  return s.tool == "mirror" && s.mirrorSeeds && s.selection ? Enter::PickMirrorLine : Enter::None;
}

inline Esc escape(const State& s) {
  if (s.boxSelecting) return Esc::CancelBox;
  if (s.tool == "mirror" && s.mirrorAxis && !s.picks) return Esc::BackToCurves;
  if (inChain(s)) return Esc::EndChain;
  if (s.clicks || s.picks) return Esc::CancelStep;
  if (s.tool != "select") return Esc::CloseTool;
  return s.selection ? Esc::ClearSelection : Esc::None;
}
}  // namespace sketchkeys
