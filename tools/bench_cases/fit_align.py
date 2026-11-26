"""gui_benches cases of fitted references (app/FitAlignBench.cpp): a 3D-scan-sized mesh with no exact face, aligned through
the Align panel by a plane fitted where it is clicked, and stood on the axes by Align scan to axes. The scan is made here:
a plate 120 x 80 x 6 of about 1.1 million triangles, tilted, with a scanner's noise on every vertex."""
import math
import struct
from array import array

NX, NY, NZ = 600, 400, 30
L, W, H = 120.0, 80.0, 6.0
SIGMA = 0.03


def _noise(ix, iy, iz, k):
    """A deterministic gaussian-ish value per lattice point (the same vertex of two faces moves alike)."""
    z = (ix * 73856093 ^ iy * 19349663 ^ iz * 83492791 ^ k * 2654435761) & 0xFFFFFFFFFFFFFFFF
    total = 0.0
    for _ in range(4):  # the sum of four uniforms, centred: close enough to a gaussian
        z = (z * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        total += (z >> 11) / float(1 << 53)
    return (total - 2.0) * math.sqrt(3.0)


def _tilt():
    ax = (1.0, 2.0, 3.0)
    n = math.sqrt(sum(a * a for a in ax))
    x, y, z = (a / n for a in ax)
    a = math.radians(27)
    c, s, t = math.cos(a), math.sin(a), 1 - math.cos(a)
    rot = ((t * x * x + c, t * x * y - s * z, t * x * z + s * y),
           (t * x * y + s * z, t * y * y + c, t * y * z - s * x),
           (t * x * z - s * y, t * y * z + s * x, t * z * z + c))
    return rot, (30.0, -20.0, 40.0)


def plate_stl(path):
    rot, move = _tilt()

    def point(ix, iy, iz):
        p = (L * ix / NX, W * iy / NY, H * iz / NZ)
        p = tuple(p[k] + SIGMA * _noise(ix, iy, iz, k) for k in range(3))
        return tuple(sum(rot[r][k] * p[k] for k in range(3)) + move[r] for r in range(3))

    facets = array("f")
    count = 0

    def quad(a, b, c, d):  # a b c d anticlockwise seen from outside
        nonlocal count
        facets.extend((0.0, 0.0, 0.0) + a + b + c)
        facets.extend((0.0, 0.0, 0.0) + a + c + d)
        count += 2

    # top (z = H) and bottom (z = 0)
    for iz, up in ((NZ, True), (0, False)):
        rows = [[point(i, j, iz) for j in range(NY + 1)] for i in range(NX + 1)]
        for i in range(NX):
            for j in range(NY):
                p, q, r, s = rows[i][j], rows[i + 1][j], rows[i + 1][j + 1], rows[i][j + 1]
                quad(p, q, r, s) if up else quad(p, s, r, q)
    # the four sides
    for j, out in ((0, True), (NY, False)):  # y = 0 faces -Y, y = W faces +Y
        rows = [[point(i, j, k) for k in range(NZ + 1)] for i in range(NX + 1)]
        for i in range(NX):
            for k in range(NZ):
                p, q, r, s = rows[i][k], rows[i + 1][k], rows[i + 1][k + 1], rows[i][k + 1]
                quad(p, q, r, s) if out else quad(p, s, r, q)
    for i, out in ((NX, True), (0, False)):  # x = L faces +X, x = 0 faces -X
        rows = [[point(i, j, k) for k in range(NZ + 1)] for j in range(NY + 1)]
        for j in range(NY):
            for k in range(NZ):
                p, q, r, s = rows[j][k], rows[j + 1][k], rows[j + 1][k + 1], rows[j][k + 1]
                quad(p, q, r, s) if out else quad(p, s, r, q)
    with open(path, "wb") as f:
        f.write(b"OPAD bench scan plate".ljust(80, b" "))
        f.write(struct.pack("<I", count))
        data = facets.tobytes()
        stride = 48
        f.write(b"".join(data[k * stride:(k + 1) * stride] + b"\0\0" for k in range(count)))
    return count


def scan_plate(root, document):
    """The plate linked into a document beside it, as a scan is imported."""
    path = root / "fit-scan.opad"
    if path.exists():
        return path
    stl = root / "fit-scan-plate.stl"
    if not stl.exists():
        plate_stl(stl)
    return document("fit-scan", ("import", "--file", str(stl), "--link", "true"))


CASES = [
    ("fit-align", scan_plate, {"OPAD_BENCH_FITALIGN": "{prefix}"}),
]
