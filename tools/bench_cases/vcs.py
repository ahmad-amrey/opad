"""gui_benches cases of version control (T3); the benches are in app/VcsBench.cpp (DiskSync::bench, GitWatch::bench,
CompareMode::bench)."""
import json
import shutil
import subprocess


def external(root, document):
    """Changed on disk while open (UI-56): a document of its own, since the bench appends to it and rewrites it."""
    return document("external", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'))


def versioned(root, document):
    """Git (UI-61, UI-136): a document in a folder of its own, outside any repository."""
    (root / "git").mkdir(exist_ok=True)
    return document("git/model")


def compared(root, document, name="compare"):
    """Compare (UI-58): <name>/model.opad with four parts, committed when git is there and copied to first.opad; the
    bench makes Box1 longer, adds Sphere1 and saves, then moves Box2 and deletes Box3 without saving."""
    folder = root / name
    folder.mkdir(exist_ok=True)
    box = lambda x, y, size: ("feature", "--kind", "box", "--inputs", json.dumps({"x": f"{x} mm", "y": f"{y} mm", "length": f"{size} mm", "width": "20 mm", "height": "10 mm"}))
    doc = document(f"{name}/model", box(0, 0, 30), box(50, 0, 20), ("feature", "--kind", "cylinder", "--inputs", '{"x":"100 mm","diameter":"20 mm","height":"10 mm"}'),
                   box(0, 50, 10))
    shutil.copy(doc, folder / "first.opad")
    git = shutil.which("git")
    if git:
        quiet = dict(cwd=folder, check=True, capture_output=True)
        subprocess.run([git, "init", "-q", "-b", "main"], **quiet)
        subprocess.run([git, "add", "model.opad"], **quiet)
        subprocess.run([git, "-c", "user.name=bench", "-c", "user.email=bench@example.com", "commit", "-q", "-m", "four parts"], **quiet)
    return doc


CASES = [
    ("external-change", external, {"OPAD_BENCH_EXTERNAL_CHANGE": "{prefix}", "OPAD_BENCH_CLI": "{cli}"}),
    # git without this machine's settings: a global config of the run's own (the bench sets the author there), no system one.
    ("git", versioned, {"OPAD_BENCH_GIT": "{prefix}", "OPAD_BENCH_CLI": "{cli}", "GIT_CONFIG_GLOBAL": "{root}/git-global",
                        "GIT_CONFIG_NOSYSTEM": "1"}),
    # Compare: OPAD_BENCH_COMPARE_GIT says whether the case could commit (git on PATH); the bench then expects HEAD as A.
    ("compare", compared, {"OPAD_BENCH_COMPARE": "{prefix}", "OPAD_BENCH_COMPARE_GIT": "1" if shutil.which("git") else "0",
                           "GIT_CONFIG_GLOBAL": "{root}/git-global", "GIT_CONFIG_NOSYSTEM": "1"}),
    # The same right to left: the legend chips, the slider and the table mirrored, the texts translated.
    ("compare-ar", lambda root, document: compared(root, document, "compare-ar"),
     {"OPAD_BENCH_COMPARE": "{prefix}", "OPAD_BENCH_COMPARE_GIT": "1" if shutil.which("git") else "0", "GIT_CONFIG_GLOBAL": "{root}/git-global",
      "GIT_CONFIG_NOSYSTEM": "1", "OPAD_LANG": "ar"}),
]
