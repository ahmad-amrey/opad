"""gui_benches cases of the help audit's view, inspect and panel tools (TODO 11 wave 3, P7, P8 and P9): the tools do what
their guides show. The benches are in app/ViewsBench.cpp, app/InspectBench.cpp and app/PanelsBench.cpp."""


def three_boxes(root, document):
    """Three 20 x 20 x 10 mm boxes along X."""
    return document("views-boxes", ("feature", "--kind", "box", "--inputs", '{"length":"20 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "box", "--inputs", '{"x":"40 mm","length":"20 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "box", "--inputs", '{"x":"80 mm","length":"20 mm","width":"20 mm","height":"10 mm"}'))


def plan(root, document):
    """A DXF plan of a few lines (opened for viewing, it shows in 2D mode): an L-shaped outline and a door swing line."""
    out = ["0", "SECTION", "2", "ENTITIES"]
    for (x0, y0, x1, y1) in [(0, 0, 60, 0), (60, 0, 60, 20), (60, 20, 20, 20), (20, 20, 20, 40), (20, 40, 0, 40), (0, 40, 0, 0), (30, 0, 30, 8)]:
        out += ["0", "LINE", "8", "Walls", "10", str(x0), "20", str(y0), "30", "0", "11", str(x1), "21", str(y1), "31", "0"]
    out += ["0", "ENDSEC", "0", "EOF"]
    path = root / "views-plan.dxf"
    path.write_text("\n".join(out) + "\n", encoding="ascii")
    return path


def two_blocks(root, document):
    """A 10 mm cube at the origin and a 10 x 10 x 20 mm block 40 mm along X and 30 mm along Y."""
    return document("inspect-blocks", ("feature", "--kind", "box", "--inputs", '{"length":"10 mm","width":"10 mm","height":"10 mm"}'),
                    ("feature", "--kind", "box", "--inputs", '{"x":"40 mm","y":"30 mm","length":"10 mm","width":"10 mm","height":"20 mm"}'))


def thin_fin(root, document):
    """A 20 x 20 x 5 mm base with a 0.5 mm fin standing on it, 10 mm tall: a wall thinner than the print check's 0.8 mm; and
    a 10 mm cube 40 mm along Y, which prints as it is (the body the bench isolates)."""
    return document("thin-fin", ("feature", "--kind", "box", "--inputs", '{"length":"20 mm","width":"20 mm","height":"5 mm"}'),
                    ("feature", "--kind", "box", "--inputs", '{"plane":{"origin":[0,0,5],"normal":[0,0,1]},"length":"20 mm","width":"0.5 mm",'
                     '"height":"10 mm","operation":"join"}'),
                    ("feature", "--kind", "box", "--inputs", '{"y":"40 mm","length":"10 mm","width":"10 mm","height":"10 mm"}'))


GRID ="[view]\ngrid=true\n"

def browser_dim(root, document):
    """A box and an empty component "Holder" (worked in, the box is outside it)."""
    return document("browser-dim", ("feature", "--kind", "box", "--inputs", '{"length":"10 mm","width":"10 mm","height":"10 mm"}'),
                    ("component", "--name", "Holder"))


CASES = [
    # P7: the seven standard views animate to their axes; in 2D mode Isometric is off and a standard view takes the grid to
    # its plane; Isolate's card with its × ends the isolation. <prefix>.top-partway.png, <prefix>.chip.png.
    ("views", three_boxes, {"OPAD_BENCH_VIEWS": "{prefix}"}, GRID),
    # Named view 1-9 by key (Shift+Alt+1..9) and Save view's offer to keep what is hidden and shown (remembered): recalled,
    # a view turns the camera and hides what it kept hidden, shows the rest, in one step undo takes back; a camera-only view
    # leaves what is shown; the list shows each key (remapped too); read back, the view keeps what it hid.
    ("named-views", three_boxes, {"OPAD_BENCH_NAMEDVIEWS": "{prefix}"}),
    # P7 on a drawing in 2D mode: Turn 90° left twists it in an animation, the direction and the grid's plane stay.
    # <prefix>.roll-partway.png, <prefix>.roll.png.
    ("views-2d", plan, {"OPAD_BENCH_VIEWS": "{prefix}"}, GRID),
    # P8 on two blocks apart: Distance from the Body filter picks faces (bodies selected first are measured); Bounding box
    # grows with each click and Back (Esc) takes the last pick back.
    ("inspect", two_blocks, {"OPAD_BENCH_INSPECT": "{prefix}"}),
    # P8: the print check's thin walls in the error red on the model (the built-in print-check case: overhangs in amber,
    # interference: the overlap in red), following the fin's body when it moves and when the cube is isolated.
    # <prefix>.view.png, <prefix>.finding.png, <prefix>.moved.png.
    ("print-check-thin", thin_fin, {"OPAD_BENCH_CHECK": "print", "OPAD_BENCH_UISHOT": "{prefix}"}),
    # P9 on a box with four notes: a card in Annotations lights up exactly what its note is pinned to (body, face, edge,
    # point) and selects nothing; a selection, a click in the view, Esc and a note being written put it out; Reset layout
    # brings the browser and the timeline back with their commands ticked. <prefix>.face.png.
    ("panels", "box", {"OPAD_BENCH_PANELS": "{prefix}"}),
    # The collapsed browser's picture of its rows after an open and an open again in the component last worked in: as the
    # areas left the rows (the ones outside it dimmed), not as they were before (until hovered). <prefix>.collapsed.png,
    # <prefix>.browser.png.
    ("browser-dim", browser_dim, {"OPAD_BENCH_BROWSER_DIM": "{prefix}"}),
    # The start page never left where the 3D view is: the window drawn off the screen (never activated), opened again from
    # the start page: the view exposed and drawing in its place (Windows).
    ("start-cover", "box", {"OPAD_BENCH_START_COVER": "1"}),
]
