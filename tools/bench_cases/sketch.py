"""gui_benches cases of the sketch input area (T1); the benches are registered in app/SketchBench.cpp (SketchEditor's
Sketch*Bench.cpp) and app/LargeSketchBench.cpp."""


def segments(root, document):
    """A drawing of 30,000 separate segments (UI-29: converting it to a sketch was quadratic; UI-27: snapping over it)."""
    path = root / "segments.dxf"
    if not path.exists():
        lines = [f"0\nLINE\n8\nLines\n10\n{(i % 300) * 3}\n20\n{(i // 300) * 3}\n11\n{(i % 300) * 3 + 2}\n21\n{(i // 300) * 3 + 1}\n" for i in range(30000)]
        path.write_text("0\nSECTION\n2\nENTITIES\n" + "".join(lines) + "0\nENDSEC\n0\nEOF\n", encoding="utf-8")
    return path


EDITING = "[files]\nviewerMode=false\n"  # the drawing is opened to be converted, not viewed

CASES = [
    # Each opens Sketch1 on XY in an empty document and drives the tool code as the mouse and keyboard do.
    ("sketch-grid", "empty", {"OPAD_BENCH_SKETCH_GRID": "{prefix}"}),  # grid snapping, the sketch's own grid (UI-18)
    # With grid snapping the drawing cursor jumps between the nodes: drawn there, the pointer hidden, the clicks on them.
    ("sketch-gridcursor", "empty", {"OPAD_BENCH_SKETCH_GRIDCURSOR": "{prefix}"}),
    ("sketch-ladder", "empty", {"OPAD_BENCH_SKETCH_LADDER": "{prefix}"}),  # Backspace / Enter / Esc (UI-20)
    ("sketch-keys", "empty", {"OPAD_BENCH_SKETCH_KEYS": "{prefix}"}),  # typed values beside the pointer (UI-16)
    ("sketch-shapes", "empty", {"OPAD_BENCH_SKETCH_SHAPES": "{prefix}"}),  # a shape's own sizes typed (UI-17)
    ("sketch-crosslock", "empty", {"OPAD_BENCH_SKETCH_CROSSLOCK": "{prefix}"}),  # tracking guides and the Shift lock (UI-19)
    ("sketch-snaps", "empty", {"OPAD_BENCH_SKETCH_SNAPS": "{prefix}"}),  # snap markers, snaps that constrain (UI-21, UI-23)
    ("sketch-steps", "empty", {"OPAD_BENCH_SKETCH_STEPS": "{prefix}"}),  # prompts and steps from one source (UI-25)
    ("sketch-constraints", "empty", {"OPAD_BENCH_SKETCH_CONSTRAINTS": "{prefix}"}),  # constraint badges (UI-24)
    ("sketch-commandline", "empty", {"OPAD_BENCH_SKETCH_COMMANDLINE": "{prefix}"}),  # drafting by the keyboard (UI-133)
    ("sketch-clipboard", "empty", {"OPAD_BENCH_SKETCH_CLIPBOARD": "{prefix}"}),  # copy, cut, paste; the timeline's op id (UI-129)
    ("clipboard-bodies", "box", {"OPAD_BENCH_CLIPBOARD_BODIES": "{prefix}"}),  # bodies: new ones, linked instances, from another document (UI-129)
    ("sketch-edits", "empty", {"OPAD_BENCH_SKETCH_EDITS": "{prefix}"}),  # line-arc fillet, fence trim, one-click extend, drag merge (UI-28)
    # Tool panels hand the keyboard back to the view after a click on a button or the slider; Esc in a panel is its (UI-05).
    ("panel-focus", "box", {"OPAD_BENCH_PANEL_FOCUS": "1"}),
    # Typed values outside the sketch: a fillet's radius, the extrude's distance and taper by the arrow (UI-122).
    ("feature-keys", "box", {"OPAD_BENCH_FEATURE_KEYS": "{prefix}"}),
    # ... the section's offset and a drawing's offset while it is placed (UI-122).
    ("tool-keys", "box", {"OPAD_BENCH_TOOL_KEYS": "{prefix}"}),
    # 30,000 segments: converted, opened, hovered, panned, snapped, selected and dragged in time (UI-27, UI-29).
    ("sketch-large", segments, {"OPAD_BENCH_LARGE": "{prefix}.json"}, EDITING),
    ("drawing-preview", segments, {"OPAD_BENCH_WIZARD": "{prefix}.png", "OPAD_BENCH_WIZARD_PREVIEW": "30000"}, EDITING),
]
