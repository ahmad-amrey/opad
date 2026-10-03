"""gui_benches cases of version control (T3); the benches are in app/VcsBench.cpp (DiskSync::bench, GitWatch::bench)."""


def external(root, document):
    """Changed on disk while open (UI-56): a document of its own, since the bench appends to it and rewrites it."""
    return document("external", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'))


def versioned(root, document):
    """Git (UI-61, UI-136): a document in a folder of its own, outside any repository."""
    (root / "git").mkdir(exist_ok=True)
    return document("git/model")


CASES = [
    ("external-change", external, {"OPAD_BENCH_EXTERNAL_CHANGE": "{prefix}", "OPAD_BENCH_CLI": "{cli}"}),
    # git without this machine's settings: a global config of the run's own (the bench sets the author there), no system one.
    ("git", versioned, {"OPAD_BENCH_GIT": "{prefix}", "OPAD_BENCH_CLI": "{cli}", "GIT_CONFIG_GLOBAL": "{root}/git-global",
                        "GIT_CONFIG_NOSYSTEM": "1"}),
]
