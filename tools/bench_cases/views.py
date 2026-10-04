"""gui_benches cases of the help audit's view and inspect tools (TODO 11 wave 3, P7 and P8): the tools do what their guides
show. The benches are in app/ViewsBench.cpp."""


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


GRID = "[view]\ngrid=true\n"

CASES = [
    # P7: the seven standard views animate to their axes; in 2D mode Isometric is off and a standard view takes the grid to
    # its plane. <prefix>.top-partway.png.
    ("views", three_boxes, {"OPAD_BENCH_VIEWS": "{prefix}"}, GRID),
    # P7 on a drawing in 2D mode: Turn 90° left twists it in an animation, the direction and the grid's plane stay.
    # <prefix>.roll-partway.png, <prefix>.roll.png.
    ("views-2d", plan, {"OPAD_BENCH_VIEWS": "{prefix}"}, GRID),
]
