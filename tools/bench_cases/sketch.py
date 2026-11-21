"""gui_benches cases of the sketch input area (T1); the benches are registered in app/SketchBench.cpp (SketchEditor's
Sketch*Bench.cpp) and app/LargeSketchBench.cpp."""


def segments(root, document):
    """A drawing of 30,000 separate segments (UI-29: converting it to a sketch was quadratic; UI-27: snapping over it)."""
    path = root / "segments.dxf"
    if not path.exists():
        lines = [f"0\nLINE\n8\nLines\n10\n{(i % 300) * 3}\n20\n{(i // 300) * 3}\n11\n{(i % 300) * 3 + 2}\n21\n{(i // 300) * 3 + 1}\n" for i in range(30000)]
        path.write_text("0\nSECTION\n2\nENTITIES\n" + "".join(lines) + "0\nENDSEC\n0\nEOF\n", encoding="utf-8")
    return path


def box_and_post(root, document):
    """The box fixture (30 x 20 x 10 mm on XY) and a post 12 mm across standing through the XY plane at (20, -30), from
    z -5 to 5: sketch-apply's Intersect with plane cuts it."""
    return document("box-post", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "cylinder", "--inputs",
                     '{"plane":{"origin":[0,0,-5],"normal":[0,0,1]},"x":"20 mm","y":"-30 mm","diameter":"12 mm","height":"10 mm"}'))


EDITING = "[files]\nviewerMode=false\n"  # the drawing is opened to be converted, not viewed

CASES = [
    # Each opens Sketch1 on XY in an empty document and drives the tool code as the mouse and keyboard do.
    ("sketch-grid", "empty", {"OPAD_BENCH_SKETCH_GRID": "{prefix}"}),  # grid snapping, the sketch's own grid (UI-18)
    # With grid snapping the drawing cursor jumps between the nodes: drawn there, the pointer hidden, the clicks on them.
    ("sketch-gridcursor", "empty", {"OPAD_BENCH_SKETCH_GRIDCURSOR": "{prefix}"}),
    # No trail of the value boxes over the view (app/OverlayTrailBench.cpp): every paint after a native overlay left part of
    # the view shows that part again, with a frame of its own or the last one shown again (in 3D and in a sketch).
    ("overlay-trail", "empty", {"OPAD_BENCH_OVERLAY_TRAIL": "1"}),
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
    # The Apply tools as their guides show them (TODO 11 wave 3, P4): mirror's curves then its line, Enter applies, picks
    # that preview, project's sources accumulating, break link picking, an image's frame and its calibration, intersect
    # cutting a post that stands through the plane.
    ("sketch-apply", box_and_post, {"OPAD_BENCH_SKETCH_APPLY": "{prefix}"}),
    # The pointer drives the preview and results are what the names say (TODO 11 wave 3, P5 and P6): the tangent circle on
    # the pointer's side, the arc slot the way the pointer went round, the circumscribed polygon about its circle, Smooth
    # and Curvature from a line or an arc, the Constraints page (by the panel's page switch) lighting a row's geometry, an
    # open end dragged shut, a backdrop picture dragged.
    ("sketch-pointer", "empty", {"OPAD_BENCH_SKETCH_POINTER": "{prefix}"}),
    # A tool normally used once closes after Apply or Enter, Shift keeps it (sketchkeys::afterApply): Move in a sketch
    # (Enter, Shift+Enter, Shift+click and a click on Apply; the Constraints page the user opened stays; Rigid stays), and
    # the Move feature (OK, Shift+click on OK, Shift+Enter in the view and in its panel, Enter).
    ("sketch-oneshot", "empty", {"OPAD_BENCH_SKETCH_ONESHOT": "{prefix}"}),
    ("feature-oneshot", "box", {"OPAD_BENCH_FEATURE_ONESHOT": "1"}),
    # D measures in a sketch and adds nothing (a line, a point square to it, a circle, the nearest distance), Shift+D is
    # Dimension; hovering a dimension's value names it in the status bar and a tooltip (SketchMeasureBench.cpp).
    ("sketch-measure", "empty", {"OPAD_BENCH_SKETCH_MEASURE": "{prefix}"}),
    # A constraint added through the tool (Coincident: a short line's end onto a profile's bottom edge) and a dimension
    # typed in its box move only what they name and what must follow (SolveOptions::moves, SketchMovementBench.cpp).
    ("sketch-movement", "empty", {"OPAD_BENCH_SKETCH_MOVEMENT": "{prefix}"}),
    # Sketch fillet on a drawing-like profile (SketchFilletBench.cpp): an upright line with its dimension meeting a long arc,
    # a construction line ending there, rounded by a click; two ends only held together: the click says why, in a toast too.
    ("sketch-fillet", "empty", {"OPAD_BENCH_SKETCH_FILLET": "{prefix}"}),
    # Using a tool never closes Section (it stays beside Distance's and a feature's panel, not over them); the Note
    # editor's Shift+Enter is a new line in its text, else Save and another note (never Repeat); a footer that says Enter
    # is pressed by Enter and Shift+Enter in the view, and no plain Enter there is Repeat while a tool's panel is shown.
    ("panels-stay", "box", {"OPAD_BENCH_PANELS_STAY": "1"}),
    # Tool panels hand the keyboard back to the view after a click on a button or the slider; Esc in a panel is its (UI-05).
    ("panel-focus", "box", {"OPAD_BENCH_PANEL_FOCUS": "1"}),
    # The Section panel's Pick face takes one face, then the view selects as before; Exit section is its main button.
    ("section-pick", "box", {"OPAD_BENCH_SECTION_PICK": "{prefix}"}),
    # Typed values outside the sketch: a fillet's radius, the extrude's distance and taper by the arrow (UI-122).
    ("feature-keys", "box", {"OPAD_BENCH_FEATURE_KEYS": "{prefix}"}),
    # ... the section's offset and a drawing's offset while it is placed (UI-122).
    ("tool-keys", "box", {"OPAD_BENCH_TOOL_KEYS": "{prefix}"}),
    # 30,000 segments: converted, opened, hovered, panned, snapped, selected and dragged in time (UI-27, UI-29).
    ("sketch-large", segments, {"OPAD_BENCH_LARGE": "{prefix}.json"}, EDITING),
    ("drawing-preview", segments, {"OPAD_BENCH_WIZARD": "{prefix}.png", "OPAD_BENCH_WIZARD_PREVIEW": "30000"}, EDITING),
]
