"""gui_benches case of the sketch's Align tool (app/SketchAlignBench.cpp): a profile whose first segment is a very flat arc
(as a DXF polyline's bulge imports), held by a Rigid group, is aligned vertical by a press near the arc's end; the whole
group must turn as one."""
import json
import math


def flat_arc_profile(root, document):
    """A sketch on XY: a flat arc (62 mm radius, about 8 mm long) leaning left, then a line across and a line down, all in
    one Rigid group."""
    path = root / "sketch-align.opad"
    if path.exists():
        return path
    r, cx, cy = 62.0, -60.0, -10.0
    a0, a1 = 0.17, 0.30
    pts = [(cx, cy), (cx + r * math.cos(a0), cy + r * math.sin(a0)), (cx + r * math.cos(a1), cy + r * math.sin(a1))]
    pts += [(pts[2][0] + 7, pts[2][1] + 1), (pts[2][0] + 8, pts[1][1] - 1)]
    geometry = {
        "points": [{"id": i + 1, "x": x, "y": y} for i, (x, y) in enumerate(pts)],
        "entities": [{"id": 6, "type": "arc", "p": [1, 2, 3]}, {"id": 7, "type": "line", "p": [3, 4]},
                     {"id": 8, "type": "line", "p": [4, 5]}],
        "constraints": [{"id": 9, "type": "rigid", "refs": [6, 7, 8]}],
    }
    return document("sketch-align", ("sketch", "--name", "Profile", "--plane", '{"base":"xy"}', "--geometry", json.dumps(geometry)))


CASES = [("sketch-align", flat_arc_profile, {"OPAD_BENCH_SKETCH_ALIGN": "1"})]
