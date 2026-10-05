"""gui_benches cases of the hands-on test of the final build (2026-10-05): what the desktop showed, through the events the
window gets. The bench is app/HandsOnBench.cpp."""

CASES = [
    # Restored from the taskbar the view draws its whole frame; the browser stays asked for while a page hides the view; a
    # press on a chip over the view is the chip's (no rubber band after it), a press released elsewhere is dropped at the next
    # move and a button held since another window's press starts no band; 10, a click elsewhere, 45 in a value box by an
    # arrow is 45; frames back to back are no stall for the watchdog, 300 ms without one is; Shift+Alt+R reaches Open file
    # location.
    ("handson", "box", {"OPAD_BENCH_HANDSON": "{prefix}"}),
]
