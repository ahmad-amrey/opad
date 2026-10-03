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
    # capacitor, four screws in a Screws component): the command plays the parts out frame by frame, level 1 moves the PCB
    # whole, the PCB activated explodes alone, level 2 splits it (the capacitor rides on the board), the PCB's browser badge
    # keeps it whole, Explode its parts on the Screws, the slider at 50 %, a click on the board selects the PCB's unit, the
    # lid's handle dragged and a value typed over the view, a hand drawing on the moved lid stored where the lid is in the
    # model, the distance tool measuring where the parts are drawn (not pinned), group and ungroup, Save as view / Collapse
    # / View > Named views / Update view, a feature edit collapsing the view. <prefix>.view.png, .panel.png, .browser.png,
    # .chips.png, .ribbon.png.
    ("explode", lambda root, document: document("explode"), {"OPAD_BENCH_EXPLODE": "{prefix}"}),
    ("explode-rtl", lambda root, document: document("explode-rtl"), {"OPAD_BENCH_EXPLODE": "{prefix}", "OPAD_LANG": "ar"}),
    # The Engine (skipped where it is not): laid out, level 2, 60 ticks from 0 to 1 each timed until every body moved, and
    # collapsed, with no event-loop gap over 250 ms.
    ("explode-engine", engine(), {"OPAD_BENCH_EXPLODE": "{prefix}"}),
]
