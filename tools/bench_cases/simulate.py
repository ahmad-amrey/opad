"""gui_benches cases of the Simulate workspace: joints from picked edges, the slider, dynamic and motion studies played back,
loads on picked faces, static and modal studies with their result maps, each checked against its textbook value. The
bench is app/SimulateBench.cpp."""

CASES = [
    ("simulate", "empty", {"OPAD_BENCH_SIMULATE": "{prefix}"}),
    ("simulate-rtl", "empty", {"OPAD_BENCH_SIMULATE": "{prefix}", "OPAD_LANG": "ar"}),
]
