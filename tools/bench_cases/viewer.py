"""gui_benches cases of the viewer area (T2); the benches are in app/Viewport*Bench.cpp."""

from pathlib import Path


def beside(path):
    """A file beside the repository, also from a worktree (.claude/worktrees/<name>): the first folder above that has it."""
    def find(root, document):
        here = Path(__file__).resolve().parents[2]
        return next((folder / path for folder in here.parents if (folder / path).exists()), here.parent / path)
    return find


CASES = [
    # Occlusion-aware tracking (UI-31) on the as1 assembly with the Distance tool: no vertex or edge behind a face is
    # hovered or acquired, the ones in sight are (after the dwell; resting again releases), guide points behind a face
    # are not offered, the other side drops the anchors it hides, and 2D mode tracks only while a tool takes points.
    # <prefix>.hidden.png, <prefix>.anchor.png, <prefix>.guide.png.
    ("tracking", "tests/corpus/stepcode-as1-oc-214.stp", {"OPAD_BENCH_TRACKING": "{prefix}"}),
    # The same on the Engine (beside the repository; skipped where it is not): the occlusion tests per hover stay cheap.
    ("tracking-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_TRACKING": "{prefix}"}),
]
