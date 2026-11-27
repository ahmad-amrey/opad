"""gui_benches cases of Align (app/AlignBench.cpp): bodies and sketches aligned through the feature panel's plan and commit,
and the speed of Move and Align with many bodies picked (the Engine); point pairs (app/AlignPairsBench.cpp): a scan-like
mesh put onto its solid by matching points picked in turn."""
import math
import struct
from pathlib import Path


def engine():
    """The Engine .opad beside the repository (as tools/bench_cases/design.py finds it); else a path that does not exist (the
    case is skipped)."""
    name = Path("opad_resources") / "bench_step_files" / "Engine V8-XT Turbo.opad"
    for folder in Path(__file__).resolve().parents:
        if (folder / name).exists():
            return str(folder / name)
    return str(Path("..") / name)


def align_parts(root, document):
    """A base 40 x 30 x 10 from the origin, a 10 mm block at x 60..70 and a sketch "Plate" on XY (a 12 x 8 rectangle at
    -40..-28, -40..-32), which the bench extrudes."""
    path = root / "align-parts.opad"
    if path.exists():
        return path
    box = lambda name, x, size, h: ("feature", "--kind", "box", "--name", name, "--inputs",
                                     '{"x":"%s mm","length":"%s mm","width":"%s mm","height":"%s mm","centered":false}' % (x, size[0], size[1], h))
    return document("align-parts", box("Base", 0, (40, 30), 10), box("Block", 60, (10, 10), 10),
                    ("sketch", "--name", "Plate", "--plane", '{"base":"xy"}', "--geometry", '{"shapes":[{"kind":"rect2","picks":[[-40,-40],[-28,-32]]}]}'))


# The scan of a 100 x 60 x 40 block: its faces in 2 mm cells (12,400 triangles), every node off by a scanner's noise, moved
# away by a known move (25 degrees about (1, 1, 2), then (180, 30, 20)); the block itself as a solid at the origin.
PAIRS_SIZE = (100.0, 60.0, 40.0)
PAIRS_CELL = 2.0
PAIRS_SIGMA = 0.03


def _pairs_move():
    ax = (1.0, 1.0, 2.0)
    n = math.sqrt(sum(a * a for a in ax))
    x, y, z = (a / n for a in ax)
    a = math.radians(25)
    c, s, t = math.cos(a), math.sin(a), 1 - math.cos(a)
    rot = ((t * x * x + c, t * x * y - s * z, t * x * z + s * y),
           (t * x * y + s * z, t * y * y + c, t * y * z - s * x),
           (t * x * z - s * y, t * y * z + s * x, t * z * z + c))
    return rot, (180.0, 30.0, 20.0)


def _pairs_noise(key, k):
    z = (key * 2654435761 + k * 40503 + 12345) & 0xFFFFFFFFFFFFFFFF
    total = 0.0
    for _ in range(4):
        z = (z * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        total += (z >> 11) / float(1 << 53)
    return (total - 2.0) * math.sqrt(3.0)


def pairs_scan_stl(path):
    rot, move = _pairs_move()
    L, W, H = PAIRS_SIZE
    nx, ny, nz = int(L / PAIRS_CELL), int(W / PAIRS_CELL), int(H / PAIRS_CELL)

    def point(i, j, k):  # a lattice node, the same on every face that has it
        p = (L * i / nx, W * j / ny, H * k / nz)
        key = (i * 1000 + j) * 1000 + k
        p = tuple(p[q] + PAIRS_SIGMA * _pairs_noise(key, q) for q in range(3))
        return tuple(sum(rot[r][q] * p[q] for q in range(3)) + move[r] for r in range(3))

    facets = []

    def quad(a, b, c, d):
        facets.append((a, b, c))
        facets.append((a, c, d))

    for k, up in ((nz, True), (0, False)):
        for i in range(nx):
            for j in range(ny):
                p, q, r, s_ = point(i, j, k), point(i + 1, j, k), point(i + 1, j + 1, k), point(i, j + 1, k)
                quad(p, q, r, s_) if up else quad(p, s_, r, q)
    for j, out in ((0, True), (ny, False)):
        for i in range(nx):
            for k in range(nz):
                p, q, r, s_ = point(i, j, k), point(i + 1, j, k), point(i + 1, j, k + 1), point(i, j, k + 1)
                quad(p, q, r, s_) if out else quad(p, s_, r, q)
    for i, out in ((nx, True), (0, False)):
        for j in range(ny):
            for k in range(nz):
                p, q, r, s_ = point(i, j, k), point(i, j + 1, k), point(i, j + 1, k + 1), point(i, j, k + 1)
                quad(p, q, r, s_) if out else quad(p, s_, r, q)
    with open(path, "wb") as f:
        f.write(b"OPAD bench point pairs scan".ljust(80, b" "))
        f.write(struct.pack("<I", len(facets)))
        for a, b, c in facets:
            f.write(struct.pack("<12fH", 0.0, 0.0, 0.0, *a, *b, *c, 0))


def pairs_parts(root, document):
    """The block as a solid ("Block", from the origin) and its scan as a mesh, moved away."""
    path = root / "align-pairs.opad"
    if path.exists():
        return path
    stl = root / "align-pairs-scan.stl"
    if not stl.exists():
        pairs_scan_stl(stl)
    L, W, H = PAIRS_SIZE
    block = ("feature", "--kind", "box", "--name", "Block", "--inputs",
             '{"length":"%s mm","width":"%s mm","height":"%s mm","centered":false}' % (L, W, H))
    return document("align-pairs", block, ("import", "--file", str(stl)))


CASES = [
    ("align-pairs", pairs_parts, {"OPAD_BENCH_ALIGN_PAIRS": "{prefix}"}),
    ("align-steps", pairs_parts, {"OPAD_BENCH_ALIGN_STEPS": "{prefix}"}),
    ("align", align_parts, {"OPAD_BENCH_ALIGN": "{prefix}"}),
    ("align-ar", lambda root, document: align_parts(root, document), {"OPAD_BENCH_ALIGN": "{prefix}", "OPAD_LANG": "ar"}),
    ("move-perf-engine", lambda root, document: engine(), {"OPAD_BENCH_MOVEPERF": "{prefix}"}),
]
