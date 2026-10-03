"""gui_benches cases of the help area (UI-106/107); the benches are in app/HelpBench.cpp, the area in app/HelpArea.cpp."""


def guided(root, document):
    """A box and a sketch beside it (rectangle 20 x 10 at x 20..40): pick targets for the guide bench."""
    path = root / "guided.opad"
    if path.exists():
        return path
    return document("guided", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("sketch", "--name", "Plate", "--plane", '{"base":"xy"}', "--geometry", '{"shapes":[{"kind":"rect2","picks":[[20,0],[40,10]]}]}'))


CASES = [
    # UI-106: help for every command, the rich hover card on ribbon buttons (English, then Arabic right to left).
    ("richtip", "box", {"OPAD_BENCH_RICHTIP": "{prefix}"}),
    ("richtip-ar", "box", {"OPAD_BENCH_RICHTIP": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-107: every animated help clip loads, moves and renders in budget (contact sheets in <output>/clips), the
    # player runs only while visible, holds still with reduced motion and plays in the rich card; Arabic mirrored.
    ("clips", "empty", {"OPAD_BENCH_CLIPS": "{prefix}"}),
    ("clips-ar", "empty", {"OPAD_BENCH_CLIPS": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-107: the tool, feature and sketch panels play the running command's clip at the step it waits for.
    ("tool-guide", guided, {"OPAD_BENCH_GUIDE": "{prefix}"}),
    ("tool-guide-ar", guided, {"OPAD_BENCH_GUIDE": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-107: Help > Command reference (F1 at the running tool, search, steps) and the palette's preview pane.
    ("reference", "box", {"OPAD_BENCH_REFERENCE": "{prefix}"}),
    ("reference-ar", "box", {"OPAD_BENCH_REFERENCE": "{prefix}", "OPAD_LANG": "ar"}),
]
