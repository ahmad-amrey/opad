"""gui_benches cases of the smart selection area (TODO 11 UI-97); the bench is app/SmartBench.cpp (OPAD_BENCH_SMART)."""
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


CASES = [
    # Select similar from a hole wall (the four through holes, again every R3 face), the area's places in the ribbon and the
    # menus, Remove faces previewed and committed as one op, undone; then the plate picked whole: its edges and faces by rule.
    ("smart", imported, {"OPAD_BENCH_SMART": "{prefix}"}),
    # The STEP itself, read-only (viewer mode): Select similar on live shapes; Remove faces is left out.
    ("smart-viewer", plate, {"OPAD_BENCH_SMART": "{prefix}"}),
]
