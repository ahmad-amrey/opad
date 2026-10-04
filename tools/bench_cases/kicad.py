"""gui_benches cases of the KiCad area (KicadArea: UI-72 UI, UI-134): Insert KiCad PCB, the sync preview and an incremental
sync, projecting a board into a sketch that follows a sync, small parts hidden while navigating. The benches are in
app/KicadAreaBench.cpp. Each case writes its own synthetic board (never a user's board) and STEP models through opad-cli,
with its own cache (OPAD_CACHE_DIR) in the run's folder."""
import os
import subprocess
from pathlib import Path

HOLE = ('(footprint "MountingHole:MountingHole_3.2mm" (layer "F.Cu") (uuid "cccccccc-0000-0000-0000-0000000000{n:02d}") (at {at})\n'
        '  (property "Reference" "{ref}")\n  (pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2)))\n')
PART = ('(footprint "Bench:{name}" (layer "{layer}.Cu") (uuid "cccccccc-0000-0000-0000-0000000001{n:02d}") (at {at})\n'
        '  (property "Reference" "{ref}")\n  (fp_rect (start -1 -0.5) (end 1 0.5) (layer "{layer}.CrtYd"))\n'
        '  (model "${{KIPRJMOD}}/{model}" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))\n')


def board_text(outline, holes, parts):
    """A .kicad_pcb: `outline` (Edge.Cuts records), holes [(ref, "x y")], parts [(ref, "x y [deg]", model, side)]."""
    text = "(kicad_pcb (version 20241229) (general (thickness 1.6))\n" + outline
    text += "".join(HOLE.format(n=i, ref=ref, at=at) for i, (ref, at) in enumerate(holes))
    text += "".join(PART.format(n=i, name=model.split(".")[0], ref=ref, at=at, model=model, layer=side) for i, (ref, at, model, side) in enumerate(parts))
    return text + ")\n"


RECT = '(gr_rect (start 100 100) (end 160 140) (layer "Edge.Cuts"))\n'
NOTCHED = ('(gr_poly (pts (xy 100 100) (xy 160 100) (xy 160 140) (xy 140 140) (xy 140 134) (xy 120 134) (xy 120 140) (xy 100 140)) '
           '(layer "Edge.Cuts"))\n')


def models(document, folder):
    """The models: a 2 x 1 x 0.6 mm chip, a 9 x 7 x 3.5 mm connector and another connector 12 mm wide."""
    def step(name, inputs):
        document(f"{folder.name}-{name}", ("feature", "--kind", "box", "--inputs", inputs), ("export", "--format", "step", "--out", str(folder / f"{name}.step")))
    step("chip", '{"length":"2 mm","width":"1 mm","height":"0.6 mm"}')
    step("conn", '{"length":"9 mm","width":"7 mm","height":"3.5 mm"}')
    step("conn2", '{"length":"12 mm","width":"7 mm","height":"3.5 mm"}')


def kicad_area(root, document):
    """An empty document beside a board (R1..R3 sharing the chip model, J1 the connector, H1 and H2 mounting holes) and the
    board's next version in next.kicad_pcb (R2 moved, H1 moved, J1 on the other connector, R4 added)."""
    folder = root / "kicad-area"
    folder.mkdir()
    models(document, folder)
    chips = [("R1", "110 110", "chip.step", "F"), ("R2", "115 110 90", "chip.step", "F"), ("R3", "120 110", "chip.step", "B")]
    (folder / "board.kicad_pcb").write_text(board_text(RECT, [("H1", "104 104"), ("H2", "156 136")], chips + [("J1", "140 120", "conn.step", "F")]), encoding="utf-8")
    moved = [("R1", "110 110", "chip.step", "F"), ("R2", "117 112 90", "chip.step", "F"), ("R3", "120 110", "chip.step", "B")]
    (folder / "next.kicad_pcb").write_text(board_text(RECT, [("H1", "106 104"), ("H2", "156 136")],
                                                      moved + [("J1", "140 120", "conn2.step", "F"), ("R4", "125 110", "chip.step", "F")]), encoding="utf-8")
    design = document("kicad-area/design")
    return design, {"OPAD_CACHE_DIR": str(root / "kicad-area-cache")}


def kicad_project(root, document):
    """A document linking a board (J1, H1, H2) beside it, and the board's next version (a notch in the outline, H1 moved, J1
    turned) in next.kicad_pcb."""
    folder = root / "kicad-project"
    folder.mkdir()
    models(document, folder)
    (folder / "board.kicad_pcb").write_text(board_text(RECT, [("H1", "104 104"), ("H2", "156 136")], [("J1", "130 125", "conn.step", "F")]), encoding="utf-8")
    (folder / "next.kicad_pcb").write_text(board_text(NOTCHED, [("H1", "108 106"), ("H2", "156 136")], [("J1", "130 125 90", "conn.step", "F")]), encoding="utf-8")
    design = document("kicad-project/design")
    env = {"OPAD_CACHE_DIR": str(root / "kicad-project-cache")}
    subprocess.run([str(document.cli), "import", str(design), str(folder / "board.kicad_pcb"), "--link", "true"], check=True, capture_output=True, env={**os.environ, **env})
    return design, env


def small_parts(root, document):
    """A document linking a board crowded with 0402-sized chips (12 of them) around one connector."""
    folder = root / "small-parts"
    folder.mkdir()
    models(document, folder)
    chips = [(f"R{i + 1}", f"{105 + 4 * (i % 6)} {108 + 6 * (i // 6)}", "chip.step", "F") for i in range(12)]
    (folder / "board.kicad_pcb").write_text(board_text(RECT, [("H1", "104 104")], chips + [("J1", "145 125", "conn.step", "F")]), encoding="utf-8")
    design = document("small-parts/design")
    env = {"OPAD_CACHE_DIR": str(root / "small-parts-cache")}
    subprocess.run([str(document.cli), "import", str(design), str(folder / "board.kicad_pcb"), "--link", "true"], check=True, capture_output=True, env={**os.environ, **env})
    return design, env


CASES = [
    # Insert KiCad PCB (the board's dialog answered, linked), repeated models meshed once, the changed board's toast opening the
    # sync preview (moved, model changed, added, holes; tinted), Sync from its footer re-meshing only the changed shapes and
    # relocating the moved part (<prefix>.png, .preview.png, .tinted.png).
    ("kicad-area", kicad_area, {"OPAD_BENCH_KICAD_AREA": "{prefix}"}),
    # UI-134: the board's outline, its mounting holes and J1 projected into a sketch from the dialog and the outline extruded;
    # the sync preview lists the design the change affects (the sketch, the extrude), Sync commits what it previewed, and the
    # sketch follows the notched outline, the moved hole and the turned J1 (<prefix>.png, .dialog.png, .preview.png, .synced.png).
    ("kicad-project", kicad_project, {"OPAD_BENCH_KICAD_PROJECT": "{prefix}"}),
    # Small parts hidden while the view moves, the selected one kept, all back once still (<prefix>.moving.png, .still.png).
    ("small-parts", small_parts, {"OPAD_BENCH_SMALL_PARTS": "{prefix}"}),
]
