"""gui_benches cases of the Simulate workspace: joints from picked edges, the slider, dynamic and motion studies played back,
loads on picked faces, static and modal studies with their result maps, each checked against its textbook value, and the
beam as a printed part (the Printed part dialog), flat and standing on its end, with its failure-index map. The
bench is app/SimulateBench.cpp."""

from pathlib import Path


def engine():
    """The Engine .opad beside the repository; else a path that does not exist (the case is skipped)."""
    name = Path("opad_resources") / "bench_step_files" / "Engine V8-XT Turbo.opad"
    for folder in Path(__file__).resolve().parents:
        if (folder / name).exists():
            return str(folder / name)
    return str(Path("..") / name)


CASES = [
    ("simulate", "empty", {"OPAD_BENCH_SIMULATE": "{prefix}"}),
    ("simulate-rtl", "empty", {"OPAD_BENCH_SIMULATE": "{prefix}", "OPAD_LANG": "ar"}),
    # Thermal setup (the cooling assistant): its example built, the pages filled, the case written (app/CoolingBench.cpp).
    ("cooling", "empty", {"OPAD_BENCH_COOLING": "{prefix}"}),
    ("cooling-rtl", "empty", {"OPAD_BENCH_COOLING": "{prefix}", "OPAD_LANG": "ar"}),
    ("cooling-walls", "empty", {"OPAD_BENCH_COOLING_WALLS": "{prefix}"}),  # a tray and its cover: an enclosure of two bodies
    ("cooling-perf-engine", lambda root, document: engine(), {"OPAD_BENCH_COOLING_PERF": "{prefix}"}),  # opening Thermal setup never holds the UI
]
