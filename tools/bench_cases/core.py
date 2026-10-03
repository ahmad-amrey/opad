"""gui_benches cases of the core area (T0); the benches are in app/CoreBench.cpp and app/AreaBench.cpp."""

CASES = [
    # The extension seams (UI-119), run in Arabic so the translation fragments are looked up through tr().
    ("seams", "empty", {"OPAD_BENCH_SEAMS": "1", "OPAD_LANG": "ar"}),
    # The feature-area hooks (AreaController), browser providers and property sections through a probe area; in Arabic
    # too, since the browser paints fixed left-to-right columns in a right-to-left UI.
    ("areas", "box", {"OPAD_BENCH_AREAS": "{prefix}"}),
    ("areas-rtl", "box", {"OPAD_BENCH_AREAS": "{prefix}", "OPAD_LANG": "ar"}),
]
