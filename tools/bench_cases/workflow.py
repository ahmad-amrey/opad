"""gui_benches cases of the everyday workflow layer (UI-109..112): feedback (app/FeedbackBench.cpp, app/FeedbackArea.cpp),
Preferences, standard shortcuts and the CAD status bar."""


OLD_PROPERTIES_KEY = "[shortcuts]\ninspect.properties=Ctrl+P\n"  # as an earlier editor saved every row


def two_bodies(root, document):
    """A box, a cylinder beside it and a sketch "Plate" (a 20 x 10 rectangle) on XY."""
    path = root / "two-bodies.opad"
    if path.exists():
        return path
    return document("two-bodies", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'),
                    ("feature", "--kind", "cylinder", "--inputs", '{"x":"60 mm","diameter":"10 mm","height":"20 mm"}'),
                    ("sketch", "--name", "Plate", "--plane", '{"base":"xy"}', "--geometry", '{"shapes":[{"kind":"rect2","picks":[[20,0],[40,10]]}]}'))


CASES = [
    # UI-109: hints and results as toasts (a command waits for the selection it asked for), the busy cursor, "+N" in the
    # strip, the load shade's text, completion toasts; never a message box.
    ("feedback", "box", {"OPAD_BENCH_FEEDBACK": "{prefix}"}),
    ("feedback-ar", "box", {"OPAD_BENCH_FEEDBACK": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-110: Preferences (Ctrl+,): pages, search with marked rows, rows that apply at once, the trimmed gear menu.
    ("preferences", "box", {"OPAD_BENCH_PREFERENCES": "{prefix}"}),
    ("preferences-ar", "box", {"OPAD_BENCH_PREFERENCES": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-111: Select all, Invert, Repeat (also first in the context menu), Properties moved from a saved Ctrl+P to Alt+Enter,
    # Redo's second key, and an unfinished sketch finished before the action that asked goes on.
    ("standard-keys", two_bodies, {"OPAD_BENCH_STANDARDKEYS": "{prefix}"}, OLD_PROPERTIES_KEY),
    ("standard-keys-ar", two_bodies, {"OPAD_BENCH_STANDARDKEYS": "{prefix}", "OPAD_LANG": "ar"}, OLD_PROPERTIES_KEY),
    # UI-112: the status bar for CAD work: Ortho (F8) and Polar (F10) with the other drafting toggles, their right-click
    # menus, the cursor's coordinate readout (plane, model, live, sketch, 2D), Ortho in the Line tool; mirrored in Arabic.
    ("status-bar", two_bodies, {"OPAD_BENCH_STATUSBAR": "{prefix}"}),
    ("status-bar-ar", two_bodies, {"OPAD_BENCH_STATUSBAR": "{prefix}", "OPAD_LANG": "ar"}),
]
