"""gui_benches case of Deviation (app/DeviationArea.cpp): a scan-sized mesh measured against a solid, the colour map drawn on
it, the stats the generated data must give, a probe, pins, a recolour, a best fit, then a point cloud measured against the
same solid (its points coloured by the cloud display), and no UI stall over 250 ms. The mesh is the heavy-mesh case's tube
(about 1.2 million triangles, its outer wall rippling 0.2 mm about radius 76, its bore at radius 73); the reference is a
solid cylinder of radius 76: the outer wall lies within tolerance and the bore 3 mm inside it (material missing). The cloud
is 100000 points on radius 76.15 (0.15 mm outside the solid's side), away from its caps."""
import array
import importlib.util
import math
from pathlib import Path

_spec = importlib.util.spec_from_file_location("bench_cases_heavy_mesh_for_deviation", Path(__file__).with_name("heavy_mesh.py"))
_heavy = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_heavy)


def ring_cloud_ply(path, around=500, rings=200, radius=76.15, bottom=5.0, top=115.0):
    """A binary PLY of positions only (no faces: a point cloud), rings of points on a cylinder."""
    with open(path, "wb") as out:
        out.write(("ply\nformat binary_little_endian 1.0\ncomment OPAD bench deviation cloud\nelement vertex %d\n"
                   "property float x\nproperty float y\nproperty float z\nend_header\n" % (around * rings)).encode("ascii"))
        for j in range(rings):
            z = bottom + (top - bottom) * j / (rings - 1)
            ring = array.array("f")
            for i in range(around):
                a = 2 * math.pi * (i + 0.5 * (j % 2)) / around
                ring.extend((radius * math.cos(a), radius * math.sin(a), z))
            out.write(ring.tobytes())


def scan_and_reference(root, document):
    path = root / "deviation-scan.opad"
    if path.exists():
        return path
    stl = root / "heavy-scan-tube.stl"  # the heavy-mesh case's, made once per run
    if not stl.exists():
        _heavy.tube_stl(stl)
    cloud = root / "deviation-cloud.ply"
    if not cloud.exists():
        ring_cloud_ply(cloud)
    return document("deviation-scan", ("import", "--file", str(stl), "--link", "true"),
                    ("import", "--file", str(cloud)),
                    ("feature", "--kind", "cylinder", "--inputs", '{"diameter":"152 mm","height":"120 mm"}'))


CASES = [
    ("deviation", scan_and_reference, {"OPAD_BENCH_DEVIATION": "{prefix}"}),
]
