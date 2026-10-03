"""gui_benches cases of the smart selection area (TODO 11 UI-95, UI-97); the benches are app/SmartBench.cpp (OPAD_BENCH_SMART) and
app/SmartSelectBench.cpp (OPAD_BENCH_SMARTSELECT), app/DeleteBench.cpp (OPAD_BENCH_DELETE), app/TimelineBench.cpp
(OPAD_BENCH_TIMELINE) and app/ContextMenuBench.cpp (OPAD_BENCH_CONTEXT)."""
import json


def plate(root, document):
    """A STEP plate (no history) with four 6 mm through holes and a 6 mm blind one, exported once per run."""
    step = root / "smart-plate.step"
    if not step.exists():
        holes = [("feature", "--kind", "cylinder", "--inputs", json.dumps({"plane": {"origin": [x, y, -1], "normal": [0, 0, 1]}, "diameter": "6 mm", "height": "12 mm", "operation": "cut"}))
                 for x, y in ((10, 10), (70, 10), (10, 40), (70, 40))]
        document("smart-source", ("feature", "--kind", "box", "--inputs", '{"length":"80 mm","width":"50 mm","height":"10 mm","centered":false}'), *holes,
                 ("feature", "--kind", "cylinder", "--inputs", '{"plane":{"origin":[40,25,6],"normal":[0,0,1]},"diameter":"6 mm","height":"5 mm","operation":"cut"}'),
                 ("export", "--format", "step", "--out", str(step)))
    return step


def imported(root, document):
    """The plate imported into a document: an editable body without history."""
    return document("smart", ("import", str(plate(root, document))))


def boss(root, document):
    """A 40 mm base with a 10 mm boss joined on top (the bench rounds the boss's top edges itself)."""
    return document("smartselect",
                    ("feature", "--kind", "box", "--name", "Base", "--inputs", '{"length":"40 mm","width":"40 mm","height":"10 mm","centered":false}'),
                    ("feature", "--kind", "box", "--name", "Boss", "--inputs",
                     '{"plane":{"origin":[15,15,10],"normal":[0,0,1]},"length":"10 mm","width":"10 mm","height":"10 mm","centered":false,"operation":"join"}'))


def pair(root, document):
    """Two boxes exported to STEP and imported into a document: one import op with two bodies, no history."""
    step = root / "delete-pair.step"
    if not step.exists():
        document("delete-pair-source", ("feature", "--kind", "box", "--inputs", '{"length":"20 mm","width":"20 mm","height":"10 mm","centered":false}'),
                 ("feature", "--kind", "box", "--inputs", '{"plane":{"origin":[40,0,0],"normal":[0,0,1]},"length":"20 mm","width":"20 mm","height":"10 mm","centered":false}'),
                 ("export", "--format", "step", "--out", str(step)))
    return document("delete-import", ("import", str(step)))


CASES = [
    # Select similar from a hole wall (the four through holes, again every R3 face), the area's places in the ribbon and the
    # menus, Remove faces previewed and committed as one op, undone; then the plate picked whole: its edges and faces by rule.
    ("smart", imported, {"OPAD_BENCH_SMART": "{prefix}"}),
    # The STEP itself, read-only (viewer mode): Select similar on live shapes; Remove faces is left out.
    ("smart-viewer", plate, {"OPAD_BENCH_SMART": "{prefix}"}),
    # The smart selection chip (UI-95): two boss faces -> "Boss · 5 faces", hover in amber with the timeline marker pulsing,
    # Ctrl+Up / Ctrl+Down up to the body and back, Shift+Space, double-clicks (the boss, then editing it; an edge's loop),
    # deleting the boss from its faces (Round named, previewed, both deleted, the base remains, Undo on the toast).
    ("smartselect", boss, {"OPAD_BENCH_SMARTSELECT": "{prefix}"}),
    ("smartselect-rtl", boss, {"OPAD_BENCH_SMARTSELECT": "{prefix}", "OPAD_LANG": "ar"}),
    # The timeline and the geometry (UI-99): a marker under the pointer shows what its step made (Boss's faces, Round's, the
    # renamed body), a click selects it (a feature's faces, the body Base made), Right steps on, Ctrl+C copies the op id;
    # names on the markers, the design history alone; Roll back to here, the playhead dragged to the end and back, a change
    # made while rolled back rolls forward.
    ("timeline", boss, {"OPAD_BENCH_TIMELINE": "{prefix}"}),
    # The same on the Engine (skipped where it is not): the pointer on every marker, rolled back and forward by the
    # playhead, names and the design history toggled, no event-loop gap over 250 ms.
    ("timeline-engine", "../opad_resources/bench_step_files/Engine V8-XT Turbo.opad", {"OPAD_BENCH_TIMELINEPERF": "1"}),
    # Context menus by what they are about (UI-100): nothing, a face, an edge, a vertex, the body, a component, a sketch and
    # the open sketch, each with its own entries (Repeat of the last tool first, the picks' feature to edit or find).
    ("context-menus", boss, {"OPAD_BENCH_CONTEXT": "{prefix}"}),
    ("context-menus-rtl", boss, {"OPAD_BENCH_CONTEXT": "{prefix}", "OPAD_LANG": "ar"}),
    # The chip on the imported plate (no history): a hole wall is the hole, its sizes in the tooltip and Measure's toast, its
    # Remove and Del start Remove faces.
    ("smartselect-import", imported, {"OPAD_BENCH_SMARTIMPORT": "{prefix}"}),
    # The chip on the Engine (beside the repository; skipped where it is not): its answer for two faces of the heaviest
    # body and of another, Ctrl+Up, with no event-loop gap over 250 ms.
    ("smartselect-engine", "../opad_resources/bench_step_files/Engine V8-XT Turbo.opad", {"OPAD_BENCH_SMARTPERF": "1"}),
    # Del routing (UI-04): Del takes out what the selection covers and nothing more, as one step with an Undo toast. A
    # designed box: a face opens smart selection's menu, its six faces delete the box feature, the body goes to a Remove
    # feature. An import of two bodies: one goes to a Remove feature, both tombstone the import.
    ("delete-design", "box", {"OPAD_BENCH_DELETE": "{prefix}"}),
    ("delete-import", pair, {"OPAD_BENCH_DELETE": "{prefix}"}),
    ("delete-engine", "../opad_resources/bench_step_files/Engine V8-XT Turbo.opad", {"OPAD_BENCH_DELETE": "{prefix}"}),
]
