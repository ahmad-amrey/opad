"""gui_benches cases of the assembly area (T2b): components and exploded views. The benches are in app/ActivationBench.cpp
and app/ExplodeBench.cpp."""
from pathlib import Path


def engine():
    """The Engine .opad beside the repository (opad_resources), also from a worktree under .claude/worktrees; else a path
    that does not exist (the case is skipped)."""
    name = Path("opad_resources") / "bench_step_files" / "Engine V8-XT Turbo.opad"
    for folder in Path(__file__).resolve().parents:
        if (folder / name).exists():
            return str(folder / name)
    return str(Path("..") / name)


def viewed_step(root, document):
    """Two boxes side by side as a STEP file, made by opad-cli (a new document, two features, an export)."""
    step = root / "explode-viewer.step"
    document("explode-viewer", ("feature", "--kind", "box", "--inputs", '{"length":"20 mm","width":"20 mm","height":"10 mm"}'),
             ("feature", "--kind", "box", "--inputs", '{"x":"30 mm","length":"10 mm","width":"10 mm","height":"10 mm"}'),
             ("export", "--out", str(step)))
    return step


CASES = [
    # Activate component (UI-33) on two components made by the bench (Housing and Lid, a box made in each) and a sketch at
    # the root: the Lid activated by its browser radio, Alt+click on rows, the breadcrumb; the Housing ghosted and refused
    # by a click, picked by a guided tool; chip, browser pill, timeline (dimmed, or only the Lid's ops), live_state;
    # visibility off and inactive opacity; a ghost's hover hint, right-click and double click; F; a sketch (in the Lid's
    # Sketches folder), a box and an import made in the Lid; context menu; undo falls back to the root; the chip's click
    # activates the root. <prefix>.ghost.png, .browser.png, .chips.png, .timeline.png, .history.png, .sketches.png.
    ("activate", lambda root, document: document("activate"), {"OPAD_BENCH_ACTIVATE": "{prefix}"}),  # its own: benches save "empty"
    ("activate-rtl", lambda root, document: document("activate-rtl"), {"OPAD_BENCH_ACTIVATE": "{prefix}", "OPAD_LANG": "ar"}),
    # The same on the Engine (skipped where it is not): a component with about half of the bodies activated and the root
    # again, no event-loop gap over 250 ms; a ghost's hover pick under 50 ms.
    ("activate-engine", engine(), {"OPAD_BENCH_ACTIVATE": "{prefix}"}),
    # Exploded view (UI-36) on an enclosure made by the bench (shell, lid, a PCB subassembly with a board, a chip and a
    # capacitor, four screws in a Screws component): the command plays the parts out frame by frame and the camera
    # glides to frame them, the first-use hint at the top centre goes for good once a badge is used, the units laid out
    # again from the measured tight boxes are opad-cli explode's, the screws leave down along their axis
    # (.fasteners.png), level 1 moves the PCB whole, the PCB activated explodes alone, level 2 splits it (the capacitor
    # rides on the board), the PCB's browser badge keeps it whole, Explode its parts on the Screws, the slider at 50 %,
    # a click on the board selects the PCB's unit, the lid's handle dragged and a value typed over the view, One after
    # another staged again without a layout when the shell is typed out of its place and back, the lid's triad (X and Y
    # square to its way up: the X arrow dragged, the square moving it under the mouse in the view's plane, the lid
    # itself dragged, a click on it still a click; .triad.png), a hand drawing on the moved lid stored where the lid is
    # in the model, the distance tool measuring where the parts are drawn (not pinned), group and ungroup, Save as view
    # / Collapse / View > Named views / Update view, a feature edit collapsing the view. <prefix>.view.png, .panel.png,
    # .browser.png, .chips.png, .ribbon.png.
    ("explode", lambda root, document: document("explode"), {"OPAD_BENCH_EXPLODE": "{prefix}"}),
    ("explode-rtl", lambda root, document: document("explode-rtl"), {"OPAD_BENCH_EXPLODE": "{prefix}", "OPAD_LANG": "ar"}),
    # A STEP file (two boxes exported by opad-cli) in viewer mode: exploded and collapsed; Save as view writes nothing.
    ("explode-viewer", viewed_step, {"OPAD_BENCH_EXPLODE": "{prefix}"}),
    # The Engine (skipped where it is not): laid out, level 2, the tight boxes measured on a worker and laid out again, 60
    # ticks from 0 to 1 each timed until every body moved, and collapsed, with no event-loop gap over 250 ms.
    ("explode-engine", engine(), {"OPAD_BENCH_EXPLODE": "{prefix}"}),
]
