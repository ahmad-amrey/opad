"""gui_benches cases of Align (app/AlignBench.cpp): bodies and sketches aligned through the feature panel's plan and commit,
and the speed of Move and Align with many bodies picked (the Engine)."""
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


CASES = [
    ("align", align_parts, {"OPAD_BENCH_ALIGN": "{prefix}"}),
    ("align-ar", lambda root, document: align_parts(root, document), {"OPAD_BENCH_ALIGN": "{prefix}", "OPAD_LANG": "ar"}),
    ("move-perf-engine", lambda root, document: engine(), {"OPAD_BENCH_MOVEPERF": "{prefix}"}),
]
