"""gui_benches cases of the assets area (T4): KiCad boards, linked files, pictures, import colours, the viewer cache. The
benches are in app/KicadBench.cpp, app/AssetLinks.cpp and app/PictureBench.cpp. Each case makes its own files and points
its own cache (OPAD_CACHE_DIR) into the run's folder, so nothing reaches the user's."""
import json
import os
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent.parent


def step(document, name, out, kind="box", inputs='{"length":"10 mm","width":"10 mm","height":"10 mm"}'):
    """A STEP of one feature at `out`, written by opad-cli."""
    document(name, ("feature", "--kind", kind, "--inputs", inputs), ("export", "--format", "step", "--out", str(out)))
    return out


def kicad_board(root, document):
    """A board opened as a viewer: two footprints (top, and bottom turned 90) share a model that only the settings' model
    folder finds, one model is missing, one is KiCad's library's (downloaded from a local copy of the library), a mounting
    hole."""
    models, library, none = root / "kicad-models", root / "kicad-library" / "9.0.0" / "Bench.3dshapes", root / "kicad-none"
    for folder in (models, library, none):
        folder.mkdir(parents=True)
    step(document, "kicad-part", models / "part.step", inputs='{"length":"4 mm","width":"2 mm","height":"1.5 mm"}')
    step(document, "kicad-library", library / "library.step", "cylinder", '{"diameter":"3 mm","height":"4 mm"}')
    board = root / "kicad" / "board.kicad_pcb"
    board.parent.mkdir()

    def footprint(ref, at, layer, model):
        return (f'(footprint "Bench:Part" (layer "{layer}.Cu") (at {at}) (property "Reference" "{ref}")\n'
                f'  (fp_rect (start -2.5 -1.5) (end 2.5 1.5) (layer "{layer}.CrtYd"))\n'
                f'  (model "${{OPAD_BENCH_UNSET_DIR}}/Bench.3dshapes/{model}" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))\n')
    board.write_text('(kicad_pcb (version 20241229) (general (thickness 1.6))\n(gr_rect (start 100 100) (end 130 120) (layer "Edge.Cuts"))\n'
                     + footprint("U1", "108 106", "F", "part.step") + footprint("U2", "120 106 90", "B", "part.step")
                     + footprint("J1", "114 114", "F", "missing.step")
                     + footprint("D1", "104 114", "F", "library.step").replace("${OPAD_BENCH_UNSET_DIR}", "${KICAD9_3DMODEL_DIR}")
                     + '(footprint "MountingHole:MountingHole_3.2mm" (layer "F.Cu") (at 126 116) (pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2)))\n)\n',
                     encoding="utf-8")
    env = {"OPAD_KICAD_MODELS_URL": (root / "kicad-library").as_uri(), "OPAD_CACHE_DIR": str(root / "kicad-cache"), "KICAD9_3DMODEL_DIR": str(none)}
    return board, env, f"[kicad]\nmodelDirs={models.as_posix()}\ndownload=always\n"


def kicad_cli_board(root, document):
    """KiCad's own export (UI-73) through the stand-in kicad-cli built with the tests (skipped without it): a board beside a
    document, its model found through ${KIPRJMOD}; its own cache and a log of what kicad-cli was asked."""
    fake = document.app.parent / ("opad-fake-kicad-cli.exe" if os.name == "nt" else "opad-fake-kicad-cli")
    if not fake.exists():
        return root / "kicad-cli" / "no stand-in kicad-cli"
    exact = root / "kicad-cli"
    exact.mkdir()
    step(document, "kicad-cli-part", exact / "part.step", inputs='{"length":"4 mm","width":"2 mm","height":"1.5 mm"}')
    design = document("kicad-cli/design")
    (exact / "board.kicad_pcb").write_text(
        '(kicad_pcb (version 20241229) (general (thickness 1.6))\n(gr_rect (start 100 100) (end 140 120) (layer "Edge.Cuts"))\n'
        + "".join(f'(footprint "Bench:Part" (layer "{layer}.Cu") (at {at}) (property "Reference" "{ref}")\n'
                  f'  (model "${{KIPRJMOD}}/part.step" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))\n'
                  for ref, at, layer in [("R1", "108 106", "F"), ("R2", "120 106 90", "F"), ("U1", "130 114", "B")]) + ")\n", encoding="utf-8")
    return design, {"OPAD_KICAD_CLI": str(fake), "OPAD_FAKE_KICAD_LOG": str(root / "kicad-cli.log"), "OPAD_CACHE_DIR": str(root / "kicad-cli-cache")}


def linked_files(root, document):
    """A document linking parts/part.step beside it (changed since) and ../outside/other.step, and a third file in parts/
    for a linked import."""
    assets = root / "assets"
    parts, outside = assets / "project" / "parts", assets / "outside"
    parts.mkdir(parents=True)
    outside.mkdir()
    env = {"OPAD_CACHE_DIR": str(root / "assets-cache")}
    step(document, "asset-part", parts / "part.step")
    step(document, "asset-other", outside / "other.step", inputs='{"x":"30 mm","length":"10 mm","width":"10 mm","height":"6 mm"}')
    step(document, "asset-third", parts / "third.step", inputs='{"y":"30 mm","length":"8 mm","width":"8 mm","height":"8 mm"}')
    linked = document("assets/project/design")
    for target in (parts / "part.step", outside / "other.step"):
        subprocess.run([str(document.cli), "import", str(linked), str(target), "--link", "true"], check=True, capture_output=True, env={**os.environ, **env})
    step(document, "asset-part-2", parts / "part.step", inputs='{"length":"12 mm","width":"10 mm","height":"10 mm"}')
    return linked, env


def assembly_step(document, name, out, inputs):
    """A STEP of one box inside a component "Bracket": linked, its top node with a part under it."""
    path = document(name, ("feature", "--kind", "box", "--inputs", inputs))

    def cli(*args):
        return json.loads(subprocess.run([str(document.cli), args[0], str(path), *args[1:]], check=True, capture_output=True, text=True).stdout)
    component = cli("component", "--name", "Bracket")["id"]
    body = next(n["id"] for n in cli("tree")["roots"] if n["type"] == "body")
    cli("reparent", "--target", body, "--parent", component)
    cli("export", "--format", "step", "--out", str(out))
    return out


def asset_sync(root, document):
    """A document in a git work tree whose .gitattributes stores assets/** with LFS, linking parts/part.step and
    parts/second.step (in sync, each a component holding a box), with the next version of both and a third of the part in
    next/."""
    project = root / "asset-sync"
    parts, later = project / "parts", project / "next"
    parts.mkdir(parents=True)
    later.mkdir()
    (project / ".git").mkdir()  # a work tree as far as finding its root goes
    (project / ".gitattributes").write_text("*.opad text eol=lf\nassets/** filter=lfs diff=lfs merge=lfs -text\n", encoding="utf-8")
    env = {"OPAD_CACHE_DIR": str(root / "asset-sync-cache")}
    second = '{{"y":"30 mm","length":"8 mm","width":"8 mm","height":"{}"}}'
    assembly_step(document, "sync-part", parts / "part.step", '{"length":"10 mm","width":"10 mm","height":"10 mm"}')
    assembly_step(document, "sync-second", parts / "second.step", second.format("8 mm"))
    assembly_step(document, "sync-part-2", later / "part.step", '{"length":"12 mm","width":"10 mm","height":"10 mm"}')
    assembly_step(document, "sync-second-2", later / "second.step", second.format("12 mm"))
    assembly_step(document, "sync-part-3", later / "part-3.step", '{"length":"14 mm","width":"10 mm","height":"10 mm"}')
    design = document("asset-sync/design")
    for target in (parts / "part.step", parts / "second.step"):
        subprocess.run([str(document.cli), "import", str(design), str(target), "--link", "true"], check=True, capture_output=True, env={**os.environ, **env})
    return design, env


def colors_obj(root, document):
    """An OBJ cube, Y up: its top in a gold material of its own, the rest grey (Kd 0.439, which OCCT reads as sRGB)."""
    colors = root / "colors-obj"
    colors.mkdir()
    (colors / "cube.mtl").write_text("newmtl grey\nKd 0.439 0.439 0.439\nnewmtl gold\nKd 1 0.766 0.336\n", encoding="utf-8")
    corners = "".join(f"v {x} {y} {z}\n" for x, y, z in [(0, 0, 0), (20, 0, 0), (20, 0, 20), (0, 0, 20), (0, 20, 0), (20, 20, 0), (20, 20, 20), (0, 20, 20)])
    (colors / "cube.obj").write_text("mtllib cube.mtl\no Cube\n" + corners + "usemtl grey\nf 1 2 3 4\nf 1 5 6 2\nf 4 3 7 8\nf 1 4 8 5\nf 2 6 7 3\n"
                                     "usemtl gold\nf 5 8 7 6\n", encoding="utf-8")
    return colors / "cube.obj", {"OPAD_CACHE_DIR": str(root / "colors-cache")}


def colors_3mf(root, document):
    """A Bambu Studio 3MF cube printed in filament 1, its top painted in filament 2 (state 2 of the paint bitstream), placed
    as it is and stretched twice as long (a non-rigid placement)."""
    colors = root / "colors-3mf"
    colors.mkdir()
    corners = [(0, 0, 0), (20, 0, 0), (20, 20, 0), (0, 20, 0), (0, 0, 20), (20, 0, 20), (20, 20, 20), (0, 20, 20)]
    sides = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2), (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5)]
    cube = ('<model unit="millimeter"><resources><object id="1" type="model"><mesh><vertices>'
            + "".join(f'<vertex x="{x}" y="{y}" z="{z}"/>' for x, y, z in corners) + "</vertices><triangles>"
            + "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"' + (' paint_color="8"' if i in (2, 3) else "") + "/>" for i, (a, b, c) in enumerate(sides))
            + '</triangles></mesh></object></resources><build><item objectid="1"/>'
            + '<item objectid="1" transform="2 0 0 0 1 0 0 0 1 30 0 0"/></build></model>')
    with zipfile.ZipFile(colors / "painted.3mf", "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("_rels/.rels", '<Relationships><Relationship Target="/3D/3dmodel.model" Id="rel0" '
                                  'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
        z.writestr("3D/3dmodel.model", cube)
        z.writestr("Metadata/project_settings.config", '{"filament_colour": ["#3060FF", "#FF2020"]}')
    return colors / "painted.3mf", {"OPAD_CACHE_DIR": str(root / "colors-cache")}


def viewer_cache(root, document):
    """The screw read and stored once shown, its copy elsewhere opened from the cache, a drawing never stored."""
    screw = ROOT / "tests" / "corpus" / "occt-screw.step"
    cached = root / "viewer-cache"
    if not screw.exists():
        return cached / screw.name
    (cached / "copy").mkdir(parents=True)
    (cached / "screw.step").write_bytes(screw.read_bytes())
    (cached / "copy" / "screw-copy.step").write_bytes(screw.read_bytes())
    (cached / "copy" / "plan.dxf").write_text("0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nCut\n10\n0\n20\n0\n11\n40\n21\n0\n0\nENDSEC\n0\nEOF\n", encoding="utf-8")
    return cached / "screw.step", {"OPAD_BENCH_CACHE": str(cached / "copy" / "screw-copy.step"), "OPAD_BENCH_CACHE_DXF": str(cached / "copy" / "plan.dxf"),
                                   "OPAD_CACHE_DIR": str(root / "viewer-cache-dir")}


CASES = [
    # A KiCad board: the settings' model folder, boxes for missing models, a library model downloaded, the import and
    # settings dialogs (<prefix>.png, .import.png, .dialog.png).
    ("kicad", kicad_board, {"OPAD_BENCH_KICAD": "{prefix}.png"}),
    # The same board read through KiCad's own export, linked, reopened without its STEP (<prefix>.png, .dialog.png).
    ("kicad-cli", kicad_cli_board, {"OPAD_BENCH_KICAD_CLI": "{prefix}"}, "[kicad]\nreader=kicad-cli\ntracks=true\n"),
    # Linked files: changed, outside the project (asked about), saved without their bodies, synced, a linked import and a
    # linked picture (<prefix>.png, .picture.png).
    ("assets", linked_files, {"OPAD_BENCH_ASSETS": "{prefix}.png"}),
    # The linked-file UI (UI-68): badges, read-only parts, Properties, the monitor's toast and Sync all, a badge's sync, a
    # missing file located, pack (LFS badge) and embed (<prefix>.browser.png, .changed.png, .properties.png, .final.png).
    ("asset-sync", asset_sync, {"OPAD_BENCH_ASSET_SYNC": "{prefix}"}),
    # Pictures (UI-71): a JPEG canvas decoded on a worker, a sketch backdrop kept as the file has it, a move storing only
    # its fields (<prefix>.canvas.png).
    ("pictures", "empty", {"OPAD_BENCH_PICTURES": "{prefix}"}),
    # Import colours (UI-74): an OBJ material per face, recoloured (<prefix>.png, .red.png); a painted 3MF, also stretched.
    ("colors", colors_obj, {"OPAD_BENCH_COLORS": "{prefix}.png"}),
    ("colors-3mf", colors_3mf, {"OPAD_BENCH_COLORS": "{prefix}.png", "OPAD_BENCH_COLORS_PAINTED": "1"}),
    # The viewer cache (UI-75): stored after display, found by content, no drawing stored.
    ("viewer-cache", viewer_cache, {}),
]
