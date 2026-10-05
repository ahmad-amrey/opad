"""gui_benches cases of the Design workspace's tools as the help guides show them (TODO 11 wave 3): primitives placed by the
pointer (app/PrimitivePlaceBench.cpp); clicks in the view reach the input the guide's animation clicks
(app/PickRoutingBench.cpp); the value arrows and Move's triad the guides pull (app/HandlesBench.cpp)."""
from pathlib import Path


def engine():
    """The Engine .opad beside the repository (as tools/bench_cases/assembly.py finds it); else a path that does not exist (the
    case is skipped)."""
    name = Path("opad_resources") / "bench_step_files" / "Engine V8-XT Turbo.opad"
    for folder in Path(__file__).resolve().parents:
        if (folder / name).exists():
            return str(folder / name)
    return str(Path("..") / name)


def routing_parts(root, document):
    """A box away from the origin (x -55..-25), a cylinder on the other side (x 32..48) and a sketch "Ring" on XZ
    (x 15..22, z 0..10) to revolve about Z: the origin planes and axes stay clear of the bodies."""
    path = root / "routing-parts.opad"
    if path.exists():
        return path
    return document("routing-parts", ("feature", "--kind", "box", "--inputs", '{"x":"-40 mm","length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "cylinder", "--inputs", '{"x":"40 mm","diameter":"16 mm","height":"20 mm"}'),
                    ("sketch", "--name", "Ring", "--plane", '{"base":"xz"}', "--geometry", '{"shapes":[{"kind":"rect2","picks":[[15,0],[22,10]]}]}'))


def handle_parts(root, document):
    """A 30 x 20 x 10 box centred on the origin: pressed down, filleted, moved and arrowed by the handles case."""
    path = root / "handle-parts.opad"
    if path.exists():
        return path
    return document("handle-parts", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'))


def primitive_parts(root, document):
    """A 30 x 20 x 10 block at x -55..-25: a face to place a sphere on, and the origin clear for the primitives on XY; a sketch
    "Marks" on XY with a circle of 5 mm about (25, 18), to snap to."""
    path = root / "primitive-parts.opad"
    if path.exists():
        return path
    return document("primitive-parts", ("feature", "--kind", "box", "--inputs", '{"x":"-40 mm","length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("sketch", "--name", "Marks", "--plane", '{"base":"xy"}', "--geometry", '{"shapes":[{"kind":"circle","picks":[[25,18],[30,18]]}]}'))


CASES = [
    # P1: a cone, a box (grid snapping, Esc back from the footprint and the height), a sphere (pressed on a face and dragged),
    # a torus (ring, then section) placed by the pointer, each preview checked while the pointer moves or the arrow is held,
    # the committed inputs against the values shown; a cylinder by Enter alone; snapping as a sketch's points (a sketch
    # circle's centre and quadrant, the block's corner and an edge's midpoint, Alt free), small rings and coils never refused,
    # the sketch origin snapped the same way.
    ("primitive-place", primitive_parts, {"OPAD_BENCH_PRIMITIVES": "{prefix}"}),
    # P1 on the Engine: every mouse event of placing and sizing a cylinder handled in under 100 ms (picks, the face's frame and
    # the previews on workers).
    ("primitive-place-engine", engine(), {"OPAD_BENCH_PRIMITIVES_PERF": "1"}),
    # P3: the region then the Z axis (revolve), the body then the YZ plane (mirror), target then tool (combine), a face then
    # the XY plane and the Neutral plane box not taking that face (draft), axes for a circular pattern, a round face for a
    # construction axis, a face and an origin plane for a construction plane, the XY plane and Enter for a new sketch.
    ("pick-routing", routing_parts, {"OPAD_BENCH_PICKROUTING": "{prefix}"}),
    # P2: the press pull's arrow pulled below its face (negative, the preview following while held), the fillet's on its first
    # edge pulled out and typed into, Move's X arrow and ring pulled and typed into, and the chamfer, thicken, plane and box
    # arrows pulled out, each preview checked while the button is held.
    ("handles", handle_parts, {"OPAD_BENCH_HANDLES": "{prefix}"}),
    # Ctrl shows the original to pick more (the report): a fillet's preview stands in for the box, whose other edges cannot be
    # picked; Ctrl held shows the box as it is with the picked edge selected, the prompt and the panel's hint say so; a
    # Ctrl+click adds an edge (its preview waits), the release brings the preview of both back; a Ctrl+click takes one back.
    ("preview-peek", handle_parts, {"OPAD_BENCH_PREVIEWPEEK": "{prefix}"}),
]
