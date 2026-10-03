"""gui_benches cases of the viewer area (T2); the benches are in app/Viewport*Bench.cpp."""

import json
from pathlib import Path
import uuid


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


def three_parts(root, document):
    """A box, a cylinder and a sphere apart: two notes pinned to bodies and one to a face, the last part hidden and shown."""
    return document("three-parts", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "cylinder", "--inputs", '{"x":"60 mm","diameter":"12 mm","height":"20 mm"}'),
                    ("feature", "--kind", "sphere", "--inputs", '{"x":"120 mm","diameter":"10 mm"}'))


def strokes(root, document):
    """A drawing of three short strokes far apart (opened, it shows in 2D): their ends line up in empty space only."""
    path = root / "strokes.svg"
    path.write_text('<svg width="80mm" height="60mm" viewBox="0 0 80 60"><g id="Strokes" stroke="black" fill="none">'
                    '<path d="M5 50 L15 50"/><path d="M62 8 L62 16"/><path d="M34 30 L41 33"/></g></svg>', encoding="utf-8")
    return path


def boxes(root, document, notes=0):
    """1,000 boxes in ten components of 100, an editable .opad as an import of one box entry (instances): the evaluation's
    large document without the 322 MB Engine. The box comes from opad-cli; the import op is written here, and `notes`
    notes pinned to boxes of different components."""
    one = document("perf-box", ("feature", "--kind", "box", "--inputs", '{"length":"8 mm","width":"8 mm","height":"8 mm"}'))
    lines = one.read_text(encoding="utf-8").split("\n")
    at = lines.index("#bodies") + 1
    key, count = lines[at].split(" ")[1], int(lines[at].split(" ")[2])
    entry = lines[at:at + 1 + count]
    header = json.loads(lines[1])
    header["uuid"] = str(uuid.uuid4())
    layers = []
    for layer in range(10):
        children = [{"type": "body", "id": str(uuid.uuid4()), "name": f"Box {layer * 100 + i + 1}", "key": key,
                     "transform": [1, 0, 0, (i % 10) * 12, 0, 1, 0, (i // 10) * 12, 0, 0, 1, layer * 12, 0, 0, 0, 1]} for i in range(100)]
        layers.append({"type": "component", "id": str(uuid.uuid4()), "name": f"Layer {layer + 1}", "children": children})
    op = {"op": "import", "id": str(uuid.uuid4()), "ts": "2026-10-03T00:00:00Z", "by": "bench", "source": "boxes.step", "units": "mm", "nodes": layers}
    ops = [json.dumps(op)] + [json.dumps({"op": "annotation", "id": str(uuid.uuid4()), "ts": "2026-10-03T00:00:00Z", "by": "bench",
                                          "anchor": {"body": layers[i * 2]["children"][55]["id"], "kind": "body"}, "text": f"Check box {i + 1}"})
                              for i in range(notes)]
    path = root / (f"boxes-1000-{notes}-notes.opad" if notes else "boxes-1000.opad")
    path.write_text("\n".join([lines[0], json.dumps(header), "#ops"] + ops + ["#bodies"] + entry) + "\n", encoding="utf-8")
    return path


def boxes_with_notes(root, document):
    """The 1,000 boxes with five notes pinned to boxes (design note E's synthetic document; anchors measured on a worker)."""
    return boxes(root, document, notes=5)


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
    # Note anchors (UI-03): two notes pinned to bodies and one to a face are measured once on a worker; adding them, a
    # hide, undo, redo and a sync stay cheap and measure nothing again; a moved body takes its notes along. The Engine
    # case is the evaluation's (every sync took 15-18 s with two body notes). <prefix>.png.
    ("note-anchors", three_parts, {"OPAD_BENCH_NOTEANCHORS": "{prefix}"}),
    ("note-anchors-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_NOTEANCHORS": "{prefix}"}),
    # Commands on several objects (UI-02): Hide others (the fewest nodes), Lock, Opacity, the document eye and Delete of
    # two operations are each one step with one refresh, and undone in one. On as1 (editable), on three parts with a
    # design history (Delete is one plan) and on the Engine (Hide others was ~1,294 commands and ~100 s).
    ("batch", "tests/corpus/stepcode-as1-oc-214.stp", {"OPAD_BENCH_BATCH": "{prefix}"}, "[files]\nviewerMode=false\n"),
    ("batch-design", three_parts, {"OPAD_BENCH_BATCH": "{prefix}"}),
    ("batch-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_BATCH": "{prefix}"}),
    # State and selection (UI-09), from a drawing in viewer mode: Rename after "Edit unsaved copy" has the editor and the
    # typed name; Ctrl+N after viewing it leaves no 2D mode or viewer card; Ctrl+click adds and takes out; the hover text
    # follows a change; Ctrl+Shift+Z redoes; V right after a dialog closed is held back. <prefix>.png.
    ("state", strokes, {"OPAD_BENCH_STATE": "{prefix}"}),
    # Latency budgets (UI-11): hide, undo, redo (each a sync of the hidden body alone), Hide others and its undo, everything
    # hidden and shown again, select all, the Face filter and back, Properties on a face and a new document each keep every
    # UI stall under 150 ms (OPAD_BENCH_PERF_BUDGET), from the command until it has settled. On 1,000 boxes with five notes
    # and on the Engine.
    ("perf", boxes_with_notes, {"OPAD_BENCH_PERF": "{prefix}"}),
    ("perf-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_PERF": "{prefix}"}),
    # Load responsiveness (UI-40): the file opened again is watched; the strip shows the load job throughout and its
    # progress only rises (per cent while a big .opad is read), the workspace is unlocked once the document is built while
    # the bodies stream in (a view command runs, an edit waits with a toast and runs after), the display pump is the
    # load's child and one job for the stream, a few full syncs costing under 150 ms in all, no empty highlight jobs; then
    # Cancel on the strip as the bodies stream in stops the load and its pump at once.
    ("loading", boxes, {"OPAD_BENCH_LOADING": "{prefix}"}),
    ("loading-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_LOADING": "{prefix}"}),
    # Selection publishing (UI-06): nothing with agent access off; on, the selection at once with O(1) fields per ref,
    # a rubber band over every face capped at 2,000 refs and written off the UI thread; off again, the file goes.
    ("selection-publish", boxes, {"OPAD_BENCH_SELPUBLISH": "{prefix}"}),
]
