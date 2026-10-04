#pragma once
// What each sketch tool asks for, in one place (TODO 11 UI-25): its steps in order, read by the prompt bar, the tool
// panel's step list and the status line alike (the status line said one thing and the steps another, and about forty
// tools had no prompt at all). SketchEditor::toolSteps() says which steps are done and what each took ("12, 4", "Line 7",
// "2 mm") where they said "Ready"; the status line is "<tool>: <the step that waits>", then the tool's note.
// Style, checked by tests/test_sketch_steps.cpp: a step is one short imperative phrase, no full stop, that starts with
// what the hand does:
//   Click   a place in the view (a point is made there, or a size set)
//   Pick    one existing point or curve
//   Select  several curves
//   Set     a value (typed over the view, in a box or in the panel)
//   Choose  an option or a file
//   Press   a key
//   Apply   the tool's button (Enter)
//   Drag    the pointer (the select tool's second step)
// A note says what else the tool does: lower case, parts joined by " · ".
// The strings are English source text: tr() marks them for tools/i18n_check.py, i18n::t translates them where shown.
// No Qt here.
#include <cstring>
#include <string>
#include <vector>

namespace sketchsteps {
constexpr const char* tr(const char* text) { return text; }

struct Tool {
  const char* id;
  const char* name;
  std::vector<const char*> steps;
  const char* note = "";
};

inline const std::vector<Tool>& tools() {
  static const std::vector<Tool> all = {
      {"select", tr("Select"), {tr("Select points, curves or dimensions"), tr("Drag, constrain or change the selection")}, tr("double-click a dimension to change it · Del deletes · X construction")},
      // Create
      {"line", tr("Line"), {tr("Click the start point"), tr("Click the next point"), tr("Click more points or press Enter to end")}, tr("a double-click or Esc ends the chain too")},
      {"rect", tr("Rectangle"), {tr("Click the first corner"), tr("Click the opposite corner")}},
      {"crect", tr("Centre rectangle"), {tr("Click the centre"), tr("Click a corner")}},
      {"rect3", tr("3-point rectangle"), {tr("Click the first corner"), tr("Click the end of the base"), tr("Click to set the height")}},
      {"circle", tr("Circle"), {tr("Click the centre"), tr("Click a point on the circle")}},
      {"circle2", tr("2-point circle"), {tr("Click one end of a diameter"), tr("Click the other end")}},
      {"circle3", tr("3-point circle"), {tr("Click the first point"), tr("Click the second point"), tr("Click the third point")}},
      {"tangent_circle", tr("Tangent circle"), {tr("Pick the first line"), tr("Pick the second line"), tr("Click the side for the circle")}},
      {"arc3", tr("3-point arc"), {tr("Click the start"), tr("Click the end"), tr("Click a point on the arc")}},
      {"arcc", tr("Centre arc"), {tr("Click the centre"), tr("Click the start"), tr("Click the end")}},
      {"tangent_arc", tr("Tangent arc"), {tr("Pick the end of a line"), tr("Click the end of the arc")}},
      {"ellipse", tr("Ellipse"), {tr("Click the centre"), tr("Click the end of the first axis"), tr("Click to set the second axis")}},
      {"slot", tr("Slot"), {tr("Click the first centre"), tr("Click the second centre"), tr("Click to set the width")}},
      {"cslot", tr("Centre slot"), {tr("Click the slot centre"), tr("Click a cap centre"), tr("Click to set the width")}},
      {"arcslot", tr("Arc slot"), {tr("Click the arc centre"), tr("Click the start"), tr("Click the end")}, tr("the width is in the panel or typed")},
      {"polygon", tr("Polygon"), {tr("Click the centre"), tr("Click a corner")}, tr("sides in the panel or typed")},
      {"polygon_outer", tr("Circumscribed polygon"), {tr("Click the centre"), tr("Click the middle of a side")}, tr("sides in the panel or typed")},
      {"spline", tr("Spline"), {tr("Click the first node"), tr("Click the next node"), tr("Click more nodes or press Enter to finish")},
       tr("Alt-click a finished spline to insert a node · double-click a node to edit its weights")},
      {"control_spline", tr("Control-point spline"), {tr("Click the control points"), tr("Click more points or press Enter to finish")}},
      {"conic", tr("Conic"), {tr("Click the start point"), tr("Click where the end tangents meet"), tr("Click the end point")}, tr("rho in the panel or typed")},
      {"point", tr("Point"), {tr("Click to place a point")}, tr("holes are drilled at sketch points")},
      {"text", tr("Text outlines"), {tr("Set the text, font and height"), tr("Click the insertion point")}},
      // Modify
      {"trim", tr("Trim"), {tr("Pick the piece of a curve to remove")}, tr("it lights red before you click · drag a fence across pieces to remove them all")},
      {"fillet", tr("Sketch fillet"), {tr("Set the radius"), tr("Pick a corner where two curves meet")}, tr("lines and arcs · the arc shows on the hovered corner")},
      {"chamfer", tr("Chamfer"), {tr("Pick a corner"), tr("Set the distances and apply")}},
      {"mirror", tr("Mirror"), {tr("Select the curves to mirror"), tr("Pick the mirror line"), tr("Apply to make the mirror image")}},
      {"offset", tr("Offset"), {tr("Select a connected chain"), tr("Set the distance and apply")}, tr("drag the arrow or type the distance")},
      {"move", tr("Move"), {tr("Select the curves to move"), tr("Set the offset and apply")}},
      {"rotate", tr("Rotate"), {tr("Select the curves to rotate"), tr("Set the angle and centre, then apply")}},
      {"scale", tr("Scale"), {tr("Select the curves to scale"), tr("Set the factor and centre, then apply")}},
      {"copy", tr("Copy with offset"), {tr("Select the curves to copy"), tr("Set the offset and copies, then apply")}},
      {"rect_pattern", tr("Rectangular pattern"), {tr("Select the curves to repeat"), tr("Set counts and spacing, then apply")}},
      {"polar_pattern", tr("Polar pattern"), {tr("Select the curves to repeat"), tr("Set the count, angle and centre, then apply")}},
      {"split", tr("Split curve"), {tr("Pick inside a curve where it splits")}},
      {"extend", tr("Extend curve"), {tr("Pick a line or an arc near the end to extend")}, tr("it runs on to the nearest curve it meets, shown dashed before you click")},
      {"break", tr("Break at intersections"), {tr("Select the curves to break"), tr("Apply to split them where they cross")}},
      {"union", tr("Region union"), {tr("Click inside the first loop"), tr("Click inside the second loop"), tr("Apply to combine the loops")}},
      {"subtract", tr("Region subtract"), {tr("Click inside the loop to keep"), tr("Click inside the loop to take away"), tr("Apply to combine the loops")}},
      {"intersect", tr("Region intersection"), {tr("Click inside the first loop"), tr("Click inside the second loop"), tr("Apply to combine the loops")}},
      {"heal", tr("Heal endpoints"), {tr("Set the gap tolerance"), tr("Apply to merge nearby endpoints")}},
      {"explode", tr("Explode pattern"), {tr("Select a pattern instance"), tr("Apply to make its copies editable")}},
      {"node", tr("Spline node weights"), {tr("Pick a spline node"), tr("Set the weights and apply")}},
      {"copybase", tr("Copy with base point"), {tr("Select the curves to copy"), tr("Click the base point")}, tr("then Ctrl+V pastes them by it")},
      {"paste", tr("Paste"), {tr("Click where the base point goes")}, tr("typed: X and Y, or @ΔX,ΔY from where they were copied")},
      // Constrain
      {"dimension", tr("Dimension"), {tr("Pick a line, a circle, an arc or two points"), tr("Click where the value sits"), tr("Set the value and apply")}},
      {"c:horizontal", tr("Horizontal"), {tr("Pick a line or a point"), tr("Pick a second point")}},
      {"c:vertical", tr("Vertical"), {tr("Pick a line or a point"), tr("Pick a second point")}},
      {"c:coincident", tr("Coincident"), {tr("Pick a point"), tr("Pick a point or a curve")}},
      {"c:collinear", tr("Collinear"), {tr("Pick the first line"), tr("Pick the second line")}},
      {"c:parallel", tr("Parallel"), {tr("Pick the first line"), tr("Pick the second line")}},
      {"c:perpendicular", tr("Perpendicular"), {tr("Pick the first line"), tr("Pick the second line")}},
      {"c:tangent", tr("Tangent"), {tr("Pick a line, circle, arc or spline"), tr("Pick the curve it touches")}},
      {"c:equal", tr("Equal"), {tr("Pick a line, circle or arc"), tr("Pick one of the same kind")}},
      {"c:smooth", tr("Smooth spline join (G2)"), {tr("Pick a line, circle, arc or spline"), tr("Pick the control-point spline it joins")}},
      {"c:curvature", tr("Equal endpoint curvature"), {tr("Pick a line, circle, arc or spline"), tr("Pick the control-point spline it joins")}},
      {"c:concentric", tr("Concentric"), {tr("Pick a circle, arc or ellipse"), tr("Pick the second one")}},
      {"c:midpoint", tr("Midpoint"), {tr("Pick a point"), tr("Pick the line")}},
      {"c:symmetric", tr("Symmetric"), {tr("Pick the first point"), tr("Pick the second point"), tr("Pick the line of symmetry")}},
      {"c:fix", tr("Fix"), {tr("Pick the point or curve to fix")}},
      // Reference
      {"project", tr("Project"), {tr("Pick the source edges, faces or bodies"), tr("Choose the link and apply")}, tr("they become fixed reference curves")},
      {"intersect_body", tr("Body-plane intersection"), {tr("Pick the source bodies"), tr("Choose the link and apply")}},
      {"silhouette", tr("Silhouette"), {tr("Pick the source bodies"), tr("Choose the link and apply")}},
      {"include3d", tr("Include reference curves"), {tr("Pick the source curves"), tr("Choose the link and apply")}},
      {"break_link", tr("Break projection link"), {tr("Select projected curves"), tr("Apply to make them editable")}},
      // Images and files
      {"image_insert", tr("Insert image"), {tr("Choose an image file"), tr("Click where it goes"), tr("Set its size and apply")}},
      {"image_edit", tr("Transform image"), {tr("Choose the backdrop image"), tr("Set its place, size and opacity, then apply")}, tr("drag the picture in the view to move it")},
      {"image_calibrate", tr("Calibrate image"), {tr("Click the first known point"), tr("Click the second known point"), tr("Set the known distance and apply")}},
      {"image_trace", tr("Trace image"), {tr("Choose the backdrop image"), tr("Set the tracing values"), tr("Apply to create editable curves")}},
      {"image_remove", tr("Remove image"), {tr("Choose the backdrop image"), tr("Apply to remove it")}},
      {"simplify", tr("Simplify curves"), {tr("Set the curve tolerance"), tr("Apply to simplify the curves")}},
      {"vector_import", tr("Import SVG / DXF"), {tr("Choose an SVG or DXF file"), tr("Apply to add its curves")}},
      {"vector_export", tr("Export SVG / DXF"), {tr("Choose where to save the file"), tr("Apply to export the sketch")}},
  };
  return all;
}

inline const Tool* find(const std::string& id) {
  for (const Tool& t : tools())
    if (id == t.id) return &t;
  return nullptr;
}

// The style above: a verb from the list, then more, no full stop at the end.
inline bool styled(const char* step) {
  static const char* const verbs[] = {"Click ", "Pick ", "Select ", "Set ", "Choose ", "Press ", "Apply ", "Drag, "};
  const std::size_t n = std::strlen(step);
  if (!n || step[n - 1] == '.' || step[n - 1] == ' ') return false;
  for (const char* v : verbs)
    if (std::strncmp(step, v, std::strlen(v)) == 0 && n > std::strlen(v)) return true;
  return false;
}
}  // namespace sketchsteps
