"""gui_benches case of Mesh to solid (app/MeshSolidBench.cpp): an STL of a faceted cylinder rebuilt as a sketch and an extrusion."""
import math


def faceted_cylinder(root, document):
    """An ASCII STL of a cylinder of radius 10 and height 25 in 36 sides, as a CAD program exports it."""
    path = root / "faceted-cylinder.stl"
    if path.exists():
        return path
    n, r, h = 36, 10.0, 25.0
    ring = [(r * math.cos(2 * math.pi * i / n), r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    facets = []
    for i in range(n):
        (x0, y0), (x1, y1) = ring[i], ring[(i + 1) % n]
        facets += [((0, 0, 0), (x1, y1, 0), (x0, y0, 0)), ((0, 0, h), (x0, y0, h), (x1, y1, h)),
                   ((x0, y0, 0), (x1, y1, 0), (x1, y1, h)), ((x0, y0, 0), (x1, y1, h), (x0, y0, h))]
    lines = ["solid cylinder"]
    for tri in facets:
        lines += ["facet normal 0 0 0", "outer loop"] + [f"vertex {x:.9f} {y:.9f} {z:.9f}" for x, y, z in tri] + ["endloop", "endfacet"]
    path.write_text("\n".join(lines + ["endsolid cylinder", ""]), encoding="ascii")
    return path


EDITING = "[files]\nviewerMode=false\n"

CASES = [
    ("mesh-to-solid", faceted_cylinder, {"OPAD_BENCH_MESHSOLID": "{prefix}"}, EDITING),
]
