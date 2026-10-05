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


def fresh(name):
    """An empty document of the case's own: the shared "empty" fixture is saved into by the benches before (design, notes)."""
    return lambda root, document: document(name)


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


def colour_boxes(root, document):
    """Three 20 x 20 x 10 mm boxes along X: grey, and two blues close to the dark and the light theme's selection colour."""
    return document("colour-boxes", ("feature", "--kind", "box", "--inputs", '{"length":"20 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "box", "--inputs", '{"x":"40 mm","length":"20 mm","width":"20 mm","height":"10 mm"}',
                     "--color", "[0.30,0.60,1.0]"),
                    ("feature", "--kind", "box", "--inputs", '{"x":"80 mm","length":"20 mm","width":"20 mm","height":"10 mm"}',
                     "--color", "[0.12,0.44,0.88]"))


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


def lines_100k(root, document):
    """A DXF with one layer of 100,000 short lines on a 5 mm grid (400 x 250, alternately along X and Y): design note E's
    big drawing layer."""
    out = ["0", "SECTION", "2", "ENTITIES"]
    for i in range(400):
        for j in range(250):
            x, y = i * 5.0, j * 5.0
            dx, dy = (3.0, 0.0) if (i + j) % 2 else (0.0, 3.0)
            out += ["0", "LINE", "8", "Grid", "10", f"{x:.3f}", "20", f"{y:.3f}", "30", "0", "11", f"{x + dx:.3f}", "21", f"{y + dy:.3f}", "31", "0"]
    out += ["0", "ENDSEC", "0", "EOF"]
    path = root / "lines-100k.dxf"
    path.write_text("\n".join(out) + "\n", encoding="ascii")
    return path


def drawing_layers(root, document):
    """A DXF of 20 layers apart: six of 400 texts (thousands of glyph faces each), ten of walls (1,500 lines, 300 arcs and
    200 circles, under the big-layer size) and four grids of 4,000 lines (big layers)."""
    out = ["0", "SECTION", "2", "ENTITIES"]

    def entity(kind, layer, *pairs):
        out.extend(["0", kind, "8", layer])
        for code, value in pairs:
            out.extend([str(code), f"{value:.3f}" if isinstance(value, float) else str(value)])
    for t in range(6):
        for k in range(400):
            entity("TEXT", f"Text-{t + 1}", (10, t * 500.0 + (k % 20) * 20), (20, (k // 20) * 8.0), (30, 0.0), (40, 2.5), (1, f"ROOM {t}{k:03d} A"))
    for w in range(10):
        ox, oy = w * 250.0, 400.0
        for k in range(1500):
            x, y = ox + (k % 50) * 4, oy + (k // 50) * 4
            entity("LINE", f"Walls-{w + 1}", (10, x), (20, y), (30, 0.0), (11, x + 3.0), (21, y + (1.0 if k % 2 else 0.0)), (31, 0.0))
        for k in range(300):
            entity("ARC", f"Walls-{w + 1}", (10, ox + (k % 20) * 10.0), (20, oy + 130 + (k // 20) * 10.0), (30, 0.0), (40, 3.0), (50, 0.0), (51, 90.0))
        for k in range(200):
            entity("CIRCLE", f"Walls-{w + 1}", (10, ox + (k % 20) * 10.0), (20, oy + 290 + (k // 20) * 10.0), (30, 0.0), (40, 2.0))
    for g in range(4):
        ox, oy = g * 600.0, 800.0
        for k in range(4000):
            x, y = ox + (k % 80) * 6, oy + (k // 80) * 6
            entity("LINE", f"Grid-{g + 1}", (10, x), (20, y), (30, 0.0), (11, x + (4.0 if k % 2 else 0.0)), (21, y + (0.0 if k % 2 else 4.0)), (31, 0.0))
    out += ["0", "ENDSEC", "0", "EOF"]
    path = root / "drawing-layers.dxf"
    path.write_text("\n".join(out) + "\n", encoding="ascii")
    return path


def architectural_dwg(root, document):
    """The evaluation's architectural DWG (3 MB, 176 layers) where it is on this machine and the build converts DWG files
    (dwg2dxf beside the app); else a path that does not exist, which skips the case."""
    here = Path(__file__).resolve().parents[2]
    drawing = Path.home() / "Desktop" / "temp" / "المخطط المعماري .dwg"
    return drawing if drawing.exists() and (here / "build" / "windows" / "bin" / "dwg2dxf.exe").exists() else root / "no-architectural.dwg"


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
    # A big drawing layer (UI-42, UI-11): 100,000 lines open with no display step over 50 ms (the Edge filter picks them in
    # groups built on the mesh worker); hover, click, Ctrl+click, crossing and window boxes and selectRefs reach exactly
    # the right lines; the Body filter and back and closing it stay within the budget. <prefix>.png.
    ("big-drawing", lines_100k, {"OPAD_BENCH_BIGDRAWING": "{prefix}"}),
    # Every filter on a drawing of many layers (UI-42): no display step over 50 ms; the Face filter picks each layer whole
    # (never OCCT's per-face sensitives: a click on a text layer selects the layer), the Vertex filter a big layer's ends in
    # groups built on the worker (hover beside an end, click, a box, selectRefs), each filter switch within the budget.
    # On a synthetic DXF and on the architectural DWG where it is. <prefix>.vertices.png.
    ("drawing-filters", drawing_layers, {"OPAD_BENCH_DRAWINGFILTERS": "{prefix}"}),
    ("drawing-filters-dwg", architectural_dwg, {"OPAD_BENCH_DRAWINGFILTERS": "{prefix}"}),
    # Box selection's visibility test (UI-43): a crossing and a window box over as1 in the Body filter and a crossing box in
    # the Face filter settle within 3 s (a crossing box kept a 23.5 s job: every pixel of the box was picked once only hidden
    # parts were left) and take exactly what is drawn in the box (each candidate's frame against the frame without it); on
    # the Engine (shown in isolation) the three boxes settle within 15 s (bodies: 19-21 s before, faces: over a minute).
    # <prefix>.crossing.png.
    ("box-scan", "tests/corpus/stepcode-as1-oc-214.stp", {"OPAD_BENCH_BOXSCAN": "{prefix}"}),
    # Startup order (UI-44): the viewer is made after the window is shown and exposed (here: started hidden, after the wait
    # for an expose), its first frame on a later turn, the file opened after that; each step's time logged.
    ("startup", "box", {"OPAD_BENCH_STARTUP": "1"}),
    # Adaptive quality while navigating (UI-45), in Studio: one light casts shadows; an orbit drag through the view's mouse
    # handlers draws a heavy model (a full frame of 20 ms or more, timed off screen) at 1.0x resolution without shadows, faster
    # than still, and at full quality again once still; a light one (a box) is never lowered, nor anything with the setting
    # off. On the 1,000 boxes and the Engine. <prefix>.moving.png, <prefix>.still.png.
    ("orbit-fps", boxes, {"OPAD_BENCH_ORBITFPS": "{prefix}"}),
    ("orbit-fps-light", "box", {"OPAD_BENCH_ORBITFPS": "{prefix}"}),
    ("orbit-fps-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_ORBITFPS": "{prefix}"}),
    # Display styles (UI-48): every switch a sliced job with no step over 150 ms of CPU, the wireframe from the mesh worker's
    # arrays with no zoom refinement, Hidden line drawing the faces in the background's colour and outlining a cylinder
    # where it turns away (no edge there), where Shaded + edges fills it. On the cylinder and the Engine. <prefix>.<style>.png.
    ("styles", "cylinder", {"OPAD_BENCH_STYLES": "{prefix}"}),
    ("styles-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_STYLES": "{prefix}"}),
    ("box-scan-engine", beside("opad_resources/bench_step_files/Engine V8-XT Turbo.opad"), {"OPAD_BENCH_BOXSCAN": "{prefix}"}),
    # Selection publishing (UI-06): nothing with agent access off; on, the selection at once with O(1) fields per ref,
    # a rubber band over every face capped at 2,000 refs and written off the UI thread; off again, the file goes.
    ("selection-publish", boxes, {"OPAD_BENCH_SELPUBLISH": "{prefix}"}),
    # Order-independent transparency (UI-39): two translucent boxes overlap in the same colour whichever is displayed last,
    # in the Draft and Studio qualities, while unordered blending (the control) depends on the order; a looks pass that
    # finds no ghost (a component left) leaves OIT on. <prefix>.oit.png.
    ("transparency", "box", {"OPAD_BENCH_TRANSPARENCY": "{prefix}"}),
    # Hover and selection roles (UI-38), in both themes: a hovered body glows white, a selected one is hued (pixels), a
    # body in the selection's own colour is outlined, a selected edge is thicker than its hover in a halo, a face and a
    # vertex are hued, the cube's side in a top view is drawn as selected and its hover is white; a hovered silhouette edge
    # and a finished sketch's wire are white, on the light theme over a darker rim (none on the dark); in a sketch a selected
    # line is hued over a halo and a hovered one glows white (in a rim on the light theme).
    # <prefix>.<theme>.{hover,body,outline,edge,edge-hover,cube,sketch,sketch-hover,wire,wire-hover}.png.
    ("highlight", colour_boxes, {"OPAD_BENCH_HIGHLIGHT": "{prefix}"}),
    # Navigation staples (UI-47): the zoom window (a dragged rectangle comes to the centre at its width's zoom, a click
    # zooms in twice, Esc and a right click leave it), animated standard views, Home and Fit all ending where the instant
    # moves go, previous and next view, the CAD 2D preset (no orbit on any button, against Fusion's Shift+middle), the cube's
    # menu, the 2D twist; then through the window: Z and Esc, a middle double click fitting everything, CAD 2D chosen from
    # the menu and saved, the document's Home (a view op marked home: gone to exactly, undo and redo, the last one wins,
    # saved, kept off the named views, in 2D only along its plane, reset in one step), the turn buttons twisting in 2D.
    # <prefix>.zoom-band.png.
    ("navigate", colour_boxes, {"OPAD_BENCH_NAVIGATE": "{prefix}"}),
    # A wheel or a trackpad (ScrollInput.hpp; on Ubuntu every wheel panned): wheel events from a device that says TouchPad,
    # told apart as on xcb (X11, XWayland), zoom by whole notches and by a high-resolution wheel's eighths of one with the
    # direction kept and nothing panned, and pan by fractions (a notch amid them pans on, Ctrl zooms; an Xorg touchpad's
    # pixels pan by the finger's px); told apart as on Windows the same device pans as before. Preferences'
    # Scroll wheel / trackpad: Trackpad pans pans the same notch, Mouse wheel zooms zooms a touchpad's notch and a finger
    # gesture, Automatic pans the gesture again.
    ("wheel", "box", {"OPAD_BENCH_WHEEL": "1"}),
    # The measuring tools (UI-50, UI-144): a rod written as B-splines reads its radius (recognised as a cylinder) in the
    # Radius tool, a B-spline box face is explained in the panel and its pick taken back, the next pick clears it; a click's
    # XYZ is listed while the next pick is awaited; Distance in its three modes from the panel's buttons (minimum, centre to
    # centre with the centres named, maximum within its accuracy) with the measured points and the view's caption; Length and
    # area on a face (area, perimeter) and a rim (length, its loops); earlier results listed with Copy and Pin; points and Δ
    # in the axes of a turned component the first pick lies in (none offered at the root), the view's arrows along them.
    # <prefix>.radius.png, <prefix>.radius-error.png, <prefix>.modes.png, <prefix>.length.png, <prefix>.history.png,
    # <prefix>.frame.png.
    ("measure", fresh("measure-empty"), {"OPAD_BENCH_MEASURE": "{prefix}"}),
    # Select other (UI-128): a box, its twin in the same place and a pin through them, from the top. The bodies under the
    # pin are listed nearest first, Alt+click opens the list once the double-click time has passed (an Alt+double-click
    # opens none: its second click is a click, smart selection's gesture), a hovered row is hovered in the view and
    # choosing it selects it; faces behind the pin's top are listed and chosen; Tab / Shift+Tab hover the next and previous face in place and a
    # click takes it; over an edge only edges are listed (no occluder faces); in the Distance tool a row is the pick; a
    # right click's context menu offers Select other... with the same list; a plain press held still opens it too and its
    # release selects nothing, while a press that moves on, a quick click and a held press on nothing or on one thing alone
    # (a lone box beside them) open none.
    # <prefix>.menu.png, <prefix>.preview.png.
    ("select-other", fresh("select-other-empty"), {"OPAD_BENCH_SELECTOTHER": "{prefix}"}),
    # The empty design document (UI-51): the Design workspace shows the origin's axes, its three planes (picked where they
    # are) and the grid with its setting off (no echo star at the pointer), Review none of it; the XY plane picked first is
    # where New sketch draws, the origin goes while the sketch is open and comes back when it closes empty, a body ends it.
    # On that box Properties on a face and the section's Pick face measure on a worker. <prefix>.origin.png.
    ("design-origin", fresh("design-origin-empty"), {"OPAD_BENCH_DESIGNORIGIN": "{prefix}"}),
    # The orbit pivot of a press away from a drawing of 100,000 lines is found run by run in milliseconds and is the point a
    # scan of every segment finds (UI-51). <prefix>.png.
    ("orbit-pivot", lines_100k, {"OPAD_BENCH_ORBITPIVOT": "{prefix}"}),
]
