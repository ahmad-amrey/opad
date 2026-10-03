"""gui_benches cases of the help area (UI-106/107/108, UI-116); the benches are in app/HelpBench.cpp, HelpMenuBench.cpp and
PolishBench.cpp, the area in app/HelpArea.cpp."""


def guided(root, document):
    """A box and a sketch beside it (rectangle 20 x 10 at x 20..40): pick targets for the guide bench."""
    path = root / "guided.opad"
    if path.exists():
        return path
    return document("guided", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("sketch", "--name", "Plate", "--plane", '{"base":"xy"}', "--geometry", '{"shapes":[{"kind":"rect2","picks":[[20,0],[40,10]]}]}'))


def coach(root, document):
    """An empty document of its own (the shared "empty" one gets a body from the design bench) beside one with a box."""
    path = root / "coach.opad"
    if path.exists():
        return path
    document("coach-box", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'))
    return document("coach")


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
    # UI-107/108: Help > Tool guide (F1 at the running tool, search, steps) and the palette's preview pane.
    ("reference", "box", {"OPAD_BENCH_REFERENCE": "{prefix}"}),
    ("reference-ar", "box", {"OPAD_BENCH_REFERENCE": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-106: the command palette's recent commands, summaries and what a command not available now needs.
    ("palette", "box", {"OPAD_BENCH_PALETTE": "{prefix}"}),
    ("palette-ar", "box", {"OPAD_BENCH_PALETTE": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-108: the "?" in every tool panel's header opens the guide of the panel's command.
    ("panel-help", "box", {"OPAD_BENCH_PANELHELP": "{prefix}"}),
    ("panel-help-ar", "box", {"OPAD_BENCH_PANELHELP": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-108: the Help menu: F1, the tool guide, the shortcuts cheat sheet, Getting started, Report a problem.
    ("help-menu", "box", {"OPAD_BENCH_HELPMENU": "{prefix}"}),
    ("help-menu-ar", "box", {"OPAD_BENCH_HELPMENU": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-108: the coach card of an empty document (coach-box.opad beside it: a document with a body has none).
    ("coach", coach, {"OPAD_BENCH_COACH": "{prefix}"}),
    ("coach-ar", coach, {"OPAD_BENCH_COACH": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-116: view names, hover kinds, the section chip and the timeline's target in the UI's language; undo labels; no
    # repeated summary in the tool panel; Ctrl+Z takes a feature's picks back; the value echo's own row.
    ("polish", guided, {"OPAD_BENCH_POLISH": "{prefix}"}),
    ("polish-ar", guided, {"OPAD_BENCH_POLISH": "{prefix}", "OPAD_LANG": "ar"}),
]
