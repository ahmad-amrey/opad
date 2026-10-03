"""gui_benches cases of the viewer area (T2); the benches are in app/Viewport*Bench.cpp."""

from pathlib import Path


def beside(path):
    """A file beside the repository, also from a worktree (.claude/worktrees/<name>): the first folder above that has it."""
    def find(root, document):
        here = Path(__file__).resolve().parents[2]
        return next((folder / path for folder in here.parents if (folder / path).exists()), here.parent / path)
    return find


def bar_behind_plate(root, document):
    """A 40 mm bar whose top edges run behind a thin plate standing just past its end, the last 0.4 mm of them only."""
    return document("bar-behind-plate", ("feature", "--kind", "box", "--inputs", '{"length":"40 mm","width":"4 mm","height":"4 mm"}'),
                    ("feature", "--kind", "box", "--inputs", '{"plane":{"origin":[0,0,-5],"normal":[0,0,1]},"x":"21.5 mm",'
                     '"length":"1 mm","width":"20 mm","height":"10.4 mm"}'))


def two_blocks(root, document):
    """A 10 mm cube at the origin and a 10 x 10 x 20 mm block 40 mm along X and 30 mm along Y: corners line up in the air."""
    return document("two-blocks", ("feature", "--kind", "box", "--inputs", '{"length":"10 mm","width":"10 mm","height":"10 mm"}'),
                    ("feature", "--kind", "box", "--inputs", '{"x":"40 mm","y":"30 mm","length":"10 mm","width":"10 mm","height":"20 mm"}'))


def strokes(root, document):
    """A drawing of three short strokes far apart (opened, it shows in 2D): their ends line up in empty space only."""
    path = root / "strokes.svg"
    path.write_text('<svg width="80mm" height="60mm" viewBox="0 0 80 60"><g id="Strokes" stroke="black" fill="none">'
                    '<path d="M5 50 L15 50"/><path d="M62 8 L62 16"/><path d="M34 30 L41 33"/></g></svg>', encoding="utf-8")
    return path


CASES = [
    # Occlusion-aware tracking (UI-31) on the as1 assembly with the Distance tool: no vertex or edge behind a face is
    # hovered, acquired or boxed, the ones in sight are (after the dwell; resting again releases), guide points behind a
    # face are not offered, the other side drops the anchors it hides, and 2D mode tracks only while a tool takes points.
    # <prefix>.hidden.png, <prefix>.anchor.png, <prefix>.guide.png, <prefix>.cue.png, <prefix>.box.png.
    ("tracking", "tests/corpus/stepcode-as1-oc-214.stp", {"OPAD_BENCH_TRACKING": "{prefix}"}),
    # A line seen up to just before its end, which is behind a face: resting there acquires no anchor.
    ("tracking-ends", bar_behind_plate, {"OPAD_BENCH_TRACKING": "{prefix}", "OPAD_BENCH_TRACKING_PART": "ends"}),
    # The same on the Engine (beside the repository; skipped where it is not): the occlusion tests per hover stay cheap.
    ("tracking-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_TRACKING": "{prefix}"}),
    # Cross lock (UI-32) in the Distance tool: lock on vertex A's guide, rest on vertex B while locked (acquired), the
    # locked point snaps to where the line lines up with B (exact, both guides, an X), a click far from it takes it; Esc
    # unlocks before the tool's Esc; a held lock acquires too; an edge's line and a coordinate plane in reach take turns on
    # Shift taps. On two blocks in 3D and on a drawing in 2D mode. <prefix>.cross.png, <prefix>.held.png.
    ("crosslock", two_blocks, {"OPAD_BENCH_CROSSLOCK": "{prefix}"}),
    ("crosslock-drawing", strokes, {"OPAD_BENCH_CROSSLOCK": "{prefix}", "OPAD_BENCH_CROSSLOCK_2D": "1"}),
]
