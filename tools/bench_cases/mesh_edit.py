"""gui_benches case of the print-prep tools on meshes (app/MeshEditBench.cpp): scale, a mesh cut, a plane split, repair,
simplify, drop to bed and lay flat on a 1.2-million-triangle tube embedded in the document (heavy_mesh.py's), a box mesh
through its wall and a small box mesh with its top missing, floating above the bed; every step under the UI stall limit."""
import importlib.util
import struct
from pathlib import Path

_heavy = importlib.util.spec_from_file_location("opad_bench_heavy_mesh", Path(__file__).with_name("heavy_mesh.py"))
heavy_mesh = importlib.util.module_from_spec(_heavy)
_heavy.loader.exec_module(heavy_mesh)


def box_stl(path, lo, hi, open_top=False):
    """A binary STL of an axis-aligned box from lo to hi (two triangles a side, outward); open_top leaves the top out."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    corner = lambda i: (x1 if i & 1 else x0, y1 if i & 2 else y0, z1 if i & 4 else z0)
    sides = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]  # bottom, top, -y, +y, -x, +x
    tris = []
    for k, (a, b, c, d) in enumerate(sides):
        if open_top and k == 1:
            continue
        tris += [(a, b, c), (a, c, d)]
    with open(path, "wb") as out:
        out.write(b"OPAD bench box".ljust(80, b" "))
        out.write(struct.pack("<I", len(tris)))
        for t in tris:
            out.write(struct.pack("<3f", 0, 0, 0))
            for i in t:
                out.write(struct.pack("<3f", *corner(i)))
            out.write(b"\0\0")


def mesh_edit(root, document):
    path = root / "mesh-edit.opad"
    if path.exists():
        return path
    tube = root / "mesh-edit-tube.stl"
    if not tube.exists():
        heavy_mesh.tube_stl(tube)
    cutter, broken = root / "mesh-edit-cutter.stl", root / "mesh-edit-broken.stl"
    box_stl(cutter, (60, -10, 40), (90, 10, 70))  # through the tube's wall (radius 73..76)
    box_stl(broken, (120, 0, 30), (140, 20, 45), open_top=True)
    return document("mesh-edit", ("import", "--file", str(tube)), ("import", "--file", str(cutter)), ("import", "--file", str(broken)))


CASES = [
    ("mesh-edit", mesh_edit, {"OPAD_BENCH_MESHEDIT": "{prefix}"}),
]
