"""gui_benches cases of the core area (T0); the benches are in app/CoreBench.cpp."""

CASES = [
    # The extension seams (UI-119), run in Arabic so the translation fragments are looked up through tr().
    ("seams", "empty", {"OPAD_BENCH_SEAMS": "1", "OPAD_LANG": "ar"}),
]
