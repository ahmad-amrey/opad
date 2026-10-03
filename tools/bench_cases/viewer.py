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
]
