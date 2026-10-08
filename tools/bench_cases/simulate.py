"""gui_benches cases of the Simulate workspace: joints from picked edges, the slider, dynamic and motion studies played back,
loads on picked faces, static and modal studies with their result maps, each checked against its textbook value, and the
beam as a printed part (the Printed part dialog), flat and standing on its end, with its failure-index map. The
bench is app/SimulateBench.cpp."""

CASES = [
    ("simulate", "empty", {"OPAD_BENCH_SIMULATE": "{prefix}"}),
    ("simulate-rtl", "empty", {"OPAD_BENCH_SIMULATE": "{prefix}", "OPAD_LANG": "ar"}),
    # The Cooling assistant: its example built, the pages filled, the case written (app/CoolingBench.cpp).
    ("cooling", "empty", {"OPAD_BENCH_COOLING": "{prefix}"}),
    ("cooling-rtl", "empty", {"OPAD_BENCH_COOLING": "{prefix}", "OPAD_LANG": "ar"}),
    ("cooling-walls", "empty", {"OPAD_BENCH_COOLING_WALLS": "{prefix}"}),  # a tray and its cover: an enclosure of two bodies
]
