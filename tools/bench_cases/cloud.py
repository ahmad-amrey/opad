"""gui_benches cases of point clouds (app/CloudBench.cpp): a scan-sized cloud of 20 million points, linked as a scan is,
drawn, picked as a body and as a point, zoomed into (its visible part at full density), a box of its points selected,
sliced and the slice turned into a sketch, with no UI stall over 250 ms; and Edit point cloud on it (app/CloudEditBench.cpp:
brushes dragged, points added and deleted, undo and redo, Finish). The cloud is made here: a wavy tube 80 mm across
and 120 mm tall, 5000 points round by 4000 rings, as a binary PLY of positions (240 MB)."""
import array
import math


def tube_ply(path, around=5000, rings=4000, radius=40.0, height=120.0):
    """Points on a tube whose radius ripples a little ring by ring (0.3 mm), so no slice is a perfect circle."""
    count = around * rings
    with open(path, "wb") as out:
        out.write(("ply\nformat binary_little_endian 1.0\ncomment OPAD bench tube\nelement vertex %d\n"
                   "property float x\nproperty float y\nproperty float z\nend_header\n" % count).encode("ascii"))
        cos = [math.cos(2 * math.pi * i / around) for i in range(around)]
        sin = [math.sin(2 * math.pi * i / around) for i in range(around)]
        ring = array.array("f", bytes(12 * around))
        for j in range(rings):
            r = radius + 0.3 * math.sin(j * 0.05)
            ring[0::3] = array.array("f", [r * c for c in cos])
            ring[1::3] = array.array("f", [r * s for s in sin])
            ring[2::3] = array.array("f", [height * j / rings]) * around
            out.write(ring.tobytes())
    return count


def big_cloud(root, document):
    """The tube cloud linked into a document beside it."""
    path = root / "big-cloud.opad"
    if path.exists():
        return path
    ply = root / "big-cloud.ply"
    if not ply.exists():
        tube_ply(ply)
    return document("big-cloud", ("import", "--file", str(ply), "--link", "true"))


def big_cloud_embedded(root, document):
    """The tube cloud imported into the document (a linked file's parts are read-only: Edit point cloud needs it embedded)."""
    path = root / "big-cloud-embedded.opad"
    if path.exists():
        return path
    ply = root / "big-cloud.ply"
    if not ply.exists():
        tube_ply(ply)
    return document("big-cloud-embedded", ("import", "--file", str(ply)))


CASES = [
    ("cloud-20m", big_cloud, {"OPAD_BENCH_CLOUD": "{prefix}"}),
    ("cloud-edit-20m", big_cloud_embedded, {"OPAD_BENCH_CLOUDEDIT": "{prefix}"}),
]
