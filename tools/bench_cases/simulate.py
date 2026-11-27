"""gui_benches cases of the Simulate workspace: joints from picked edges, the slider, dynamic and motion studies played back,
loads on picked faces, static and modal studies with their result maps, each checked against its textbook value, and the
beam as a printed part (the Printed part dialog), flat and standing on its end, with its failure-index map. The
bench is app/SimulateBench.cpp; the guided checks, Thermal setup and the solver install have benches of their own."""

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
    # An airflow check's openings picked as loops of edges on a duct adapter with a step: proposed past the chamfers on the
    # check's own part only, one click takes a loop in place of the last, the arrows drawn and flipped by a click on one or the
    # Flip button (app/SimulateOpeningsBench.cpp).
    ("simulate-openings", "empty", {"OPAD_BENCH_FLOWOPENINGS": "{prefix}"}),
    # Thermal setup (the cooling assistant): its example built, the pages filled, the case written (app/CoolingBench.cpp).
    ("cooling", "empty", {"OPAD_BENCH_COOLING": "{prefix}"}),
    ("cooling-rtl", "empty", {"OPAD_BENCH_COOLING": "{prefix}", "OPAD_LANG": "ar"}),
    ("cooling-walls", "empty", {"OPAD_BENCH_COOLING_WALLS": "{prefix}"}),  # a tray and its cover: an enclosure of two bodies
    ("simulate-lit", "empty", {"OPAD_BENCH_SIMLIT": "{prefix}"}),  # Thermal setup's rows in the browser as it goes; rows light their targets (app/SimulateLitBench.cpp)
    ("cooling-perf-engine", lambda root, document: engine(), {"OPAD_BENCH_COOLING_PERF": "{prefix}"}),  # opening Thermal setup never holds the UI
    # Installing OpenFOAM from the check's Run step against a fake release the bench serves on 127.0.0.1: a tampered download
    # refused, the right one downloaded, checked and unpacked (app/SimulateEnginesBench.cpp). The case's own APPDATA,
    # LOCALAPPDATA and engines folder hide any OpenFOAM of this computer and keep the user's engines folder out of it.
    ("engine-install", "empty", {"OPAD_BENCH_ENGINE_INSTALL": "{prefix}", "APPDATA": "{root}/engine-install/appdata",
                                 "LOCALAPPDATA": "{root}/engine-install/localappdata", "OPAD_ENGINES_DIR": "{root}/engine-install/engines",
                                 "OPAD_OPENFOAM": "", "OPAD_ENGINE_PACKS": ""}),
]
