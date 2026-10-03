"""gui_benches cases of the assembly area (T2b): components, lock and exploded views. The benches are in
app/ActivationBench.cpp, app/LockBench.cpp and app/ExplodeBench.cpp."""
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


def locked_drawing(root, document):
    """A DXF whose Walls layer is locked by the file (layer flag 70 & 4) beside a plain layer, a 10 mm line on each."""
    def groups(*pairs):
        return "".join(f"{code}\n{value}\n" for code, value in pairs)
    def section(name, *pairs):
        return groups((0, "SECTION"), (2, name), *pairs, (0, "ENDSEC"))
    path = root / "locked-layer.dxf"
    path.write_text(section("TABLES", (0, "TABLE"), (2, "LAYER"), (0, "LAYER"), (2, "Walls"), (62, 1), (70, 4),
                            (0, "LAYER"), (2, "Plain"), (62, 5), (70, 0), (0, "ENDTAB")) +
                    section("ENTITIES", (0, "LINE"), (8, "Walls"), (10, 0), (20, 0), (11, 10), (21, 0),
                            (0, "LINE"), (8, "Plain"), (10, 0), (20, 5), (11, 10), (21, 5)) + "0\nEOF\n", encoding="ascii")
    return path


CASES = [
    # Components without dialogs (UI-34) on a Housing placed 30 mm up (a box in it), an empty Lid and two root boxes:
    # New component named, activated and renamed in the browser (Activate new components off: into the active one);
    # Component from selection (Ctrl+G) in one undo step where the parts are (a transform for the Housing's box only),
    # refused with a locked box; Move to component… narrowed by typing, Enter moves (keeping the place), a component is
    # not offered to itself, Esc closes; the opacity slider live while dragged and written once let go, the Opacity
    # popup written when the keys rest, Esc dropping what is pending; menu and ribbon entries. <prefix>.rename.png,
    # .picker.png, .menu.png, .popup.png, .ribbon.png.
    ("components", lambda root, document: document("components"), {"OPAD_BENCH_COMPONENTS": "{prefix}"}),
    ("components-rtl", lambda root, document: document("components-rtl"), {"OPAD_BENCH_COMPONENTS": "{prefix}", "OPAD_LANG": "ar"}),
    # The Engine (skipped where it is not): Move to component… over its components opened and narrowed letter by letter,
    # timed; the opacity slider on a component with about half of the bodies live with no event-loop gap over 250 ms,
    # then written (timed beside one appearance op).
    ("components-engine", engine(), {"OPAD_BENCH_COMPONENTS": "{prefix}"}),
    # Lock (UI-37) on a box in a Housing component and a box at the root made by the bench: Lock is one step and the
    # command says Unlock; the locked box faded to half and not picked (a click passes it), its browser row's lock
    # badge, its name in the status bar when resting on it, Unlock in the right-click menu there, picked as a reference
    # by a guided tool and not afterwards; the Housing locked holds its box (a dim badge naming it, whose click unlocks
    # the Housing; Unlock on the box frees the Housing and says so); a drop of the Housing's box and a feature moving it
    # refused in the shown language naming the Housing. <prefix>.view.png, .browser.png.
    ("lock", lambda root, document: document("lock"), {"OPAD_BENCH_LOCK": "{prefix}"}),
    ("lock-rtl", lambda root, document: document("lock-rtl"), {"OPAD_BENCH_LOCK": "{prefix}", "OPAD_LANG": "ar"}),
    # A DXF with a locked layer in viewer mode: its lines faded towards the background and not picked, the plain layer's
    # picked; the layer row's badge unlocks it (a view change). <prefix>.drawing.png.
    ("lock-drawing", locked_drawing, {"OPAD_BENCH_LOCK": "{prefix}"}),
    # The Engine (skipped where it is not): a component with about half of the bodies locked and unlocked with no
    # event-loop gap over 250 ms; a locked body's hover pick under 50 ms.
    ("lock-engine", engine(), {"OPAD_BENCH_LOCK": "{prefix}"}),
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
