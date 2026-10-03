"""gui_benches cases of the everyday workflow layer (UI-109..112): feedback (app/FeedbackBench.cpp, app/FeedbackArea.cpp),
Preferences, standard shortcuts and the CAD status bar."""

CASES = [
    # UI-109: hints and results as toasts (a command waits for the selection it asked for), the busy cursor, "+N" in the
    # strip, the load shade's text, completion toasts; never a message box.
    ("feedback", "box", {"OPAD_BENCH_FEEDBACK": "{prefix}"}),
    ("feedback-ar", "box", {"OPAD_BENCH_FEEDBACK": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-110: Preferences (Ctrl+,): pages, search with marked rows, rows that apply at once, the trimmed gear menu.
    ("preferences", "box", {"OPAD_BENCH_PREFERENCES": "{prefix}"}),
    ("preferences-ar", "box", {"OPAD_BENCH_PREFERENCES": "{prefix}", "OPAD_LANG": "ar"}),
]
