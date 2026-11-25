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
    ("simulate-tabs", "empty", {"OPAD_BENCH_SIMTABS": "1"}),  # the Simulation panel's own Mechanism / Structure / Thermal bar (app/SimulateTabsBench.cpp)
    # The goal tabs and guided checks: the start page, a strength, a motion, a heat and an airflow check, the solver window
    # (app/SimulateChecksBench.cpp); the same in Arabic.
    ("simulate-checks", "empty", {"OPAD_BENCH_SIMCHECKS": "{prefix}", "OPAD_BENCH_UISHOT": "{prefix}.window.png"}),
    ("simulate-checks-rtl", "empty", {"OPAD_BENCH_SIMCHECKS": "{prefix}", "OPAD_LANG": "ar"}),
    # Thermal setup (the cooling assistant): its example built, the pages filled, the case written (app/CoolingBench.cpp).
    ("cooling", "empty", {"OPAD_BENCH_COOLING": "{prefix}"}),
    ("cooling-rtl", "empty", {"OPAD_BENCH_COOLING": "{prefix}", "OPAD_LANG": "ar"}),
    ("cooling-walls", "empty", {"OPAD_BENCH_COOLING_WALLS": "{prefix}"}),  # a tray and its cover: an enclosure of two bodies
    ("simulate-lit", "empty", {"OPAD_BENCH_SIMLIT": "{prefix}"}),  # Thermal setup's rows in the browser as it goes; rows light their targets (app/SimulateLitBench.cpp)
    ("cooling-perf-engine", lambda root, document: engine(), {"OPAD_BENCH_COOLING_PERF": "{prefix}"}),  # opening Thermal setup never holds the UI
]
