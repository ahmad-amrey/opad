"""gui_benches cases of the core area (T0); the benches are in app/CoreBench.cpp, app/AreaBench.cpp and app/ContractsBench.cpp."""

CASES = [
    # The extension seams (UI-119), run in Arabic so the translation fragments are looked up through tr().
    ("seams", "empty", {"OPAD_BENCH_SEAMS": "1", "OPAD_LANG": "ar"}),
    # The feature-area hooks (AreaController), browser providers and property sections through a probe area; in Arabic
    # too, since the browser paints fixed left-to-right columns in a right-to-left UI. The first starts in the probe's own
    # workspace (saved by id), the second in Design as earlier builds saved it (1).
    ("areas", "box", {"OPAD_BENCH_AREAS": "{prefix}", "OPAD_BENCH_AREAS_WORKSPACE": "probe"}, "[ui]\nworkspace=probe\n"),
    ("areas-rtl", "box", {"OPAD_BENCH_AREAS": "{prefix}", "OPAD_BENCH_AREAS_WORKSPACE": "design", "OPAD_LANG": "ar"}, "[ui]\nworkspace=1\n"),
    # The shared UI contracts (UI-120, app/ContractsBench.cpp). The command registry on a STEP file in viewer mode.
    ("commands", "screw", {"OPAD_BENCH_COMMANDS": "1"}),
]
