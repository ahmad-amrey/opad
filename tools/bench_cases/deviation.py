"""gui_benches case of Deviation (app/DeviationArea.cpp): a scan-sized mesh measured against a solid, the colour map drawn on
it, the stats the generated data must give, a probe, pins, a recolour, and no UI stall over 250 ms. The mesh is the heavy-mesh
case's tube (about 1.2 million triangles, its outer wall rippling 0.2 mm about radius 76, its bore at radius 73); the reference
is a solid cylinder of radius 76: the outer wall lies within tolerance and the bore 3 mm inside it (material missing)."""
import importlib.util
from pathlib import Path

_spec = importlib.util.spec_from_file_location("bench_cases_heavy_mesh_for_deviation", Path(__file__).with_name("heavy_mesh.py"))
_heavy = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_heavy)


def scan_and_reference(root, document):
    path = root / "deviation-scan.opad"
    if path.exists():
        return path
    stl = root / "heavy-scan-tube.stl"  # the heavy-mesh case's, made once per run
    if not stl.exists():
        _heavy.tube_stl(stl)
    return document("deviation-scan", ("import", "--file", str(stl), "--link", "true"),
                    ("feature", "--kind", "cylinder", "--inputs", '{"diameter":"152 mm","height":"120 mm"}'))


CASES = [
    ("deviation", scan_and_reference, {"OPAD_BENCH_DEVIATION": "{prefix}"}),
]
