"""gui_benches cases of heavy meshes (app/HeavyMeshBench.cpp): a 3D-scan-sized mesh, linked as a scan is, selected, turned
with Move / copy (its Axis box: the Edge filter), picked in every filter, sectioned and measured, with no UI stall over
250 ms. The mesh is made here: a tube of about 1.2 million triangles, wavy like a scan's surface."""
import math
import struct
from array import array


def tube_stl(path, around=1000, rings=300, outer=76.0, inner=73.0, height=120.0):
    """A binary STL of a closed tube: outer and inner walls of `around` x `rings` quads, ring caps at both ends. Its radius
    ripples a little (0.2 mm), so its sides are not one plane or cylinder and nothing in it is a perfect circle."""
    def radius(base, i, j):
        return base + 0.2 * math.sin(i * 0.37) * math.cos(j * 0.21)

    def point(base, i, j):
        a = 2 * math.pi * (i % around) / around
        r = radius(base, i % around, j)
        return (r * math.cos(a), r * math.sin(a), height * j / rings)

    facets = array("f")
    count = 0

    def tri(a, b, c):
        nonlocal count
        facets.extend((0.0, 0.0, 0.0) + a + b + c)
        count += 1

    for base, outward in ((outer, True), (inner, False)):
        grid = [[point(base, i, j) for j in range(rings + 1)] for i in range(around + 1)]
        for i in range(around):
            for j in range(rings):
                p, q, r, s = grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]
                if outward:
                    tri(p, q, r), tri(p, r, s)
                else:
                    tri(p, r, q), tri(p, s, r)
    for j, up in ((0, False), (rings, True)):
        for i in range(around):
            o1, o2, i1, i2 = point(outer, i, j), point(outer, i + 1, j), point(inner, i, j), point(inner, i + 1, j)
            if up:
                tri(o1, o2, i2), tri(o1, i2, i1)
            else:
                tri(o1, i2, o2), tri(o1, i1, i2)
    with open(path, "wb") as out:
        out.write(b"OPAD bench tube".ljust(80, b" "))
        out.write(struct.pack("<I", count))
        data = facets.tobytes()
        stride = 48
        out.write(b"".join(data[k * stride:(k + 1) * stride] + b"\0\0" for k in range(count)))
    return count


def heavy_scan(root, document):
    """The tube linked into a document beside it (as a scan is imported), with a box beside it to measure to."""
    path = root / "heavy-scan.opad"
    if path.exists():
        return path
    stl = root / "heavy-scan-tube.stl"
    if not stl.exists():
        tube_stl(stl)
    return document("heavy-scan", ("import", "--file", str(stl), "--link", "true"),
                    ("feature", "--kind", "box", "--inputs", '{"x":"120 mm","length":"20 mm","width":"20 mm","height":"20 mm"}'))


CASES = [
    ("heavy-mesh", heavy_scan, {"OPAD_BENCH_HEAVYMESH": "{prefix}"}),
]
