"""gui_benches cases of version control (T3); the benches are in app/VcsBench.cpp (DiskSync::bench, GitWatch::bench,
CompareMode::bench), app/VersionBench.cpp (VersionControl::bench) and app/RecoveryBench.cpp."""
import json
import os
import shutil
import stat
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


def recovered(root, document, name="recovery"):
    """Recovery with diff (UI-59): <name>/model.opad with three boxes, saved; the bench changes, snapshots, restores and saves
    it."""
    (root / name).mkdir(exist_ok=True)
    box = lambda x, size: ("feature", "--kind", "box", "--inputs", json.dumps({"x": f"{x} mm", "length": f"{size} mm", "width": "20 mm", "height": "10 mm"}))
    return document(f"{name}/model", box(0, 30), box(50, 20), box(100, 10))


def versioned_with_remote(root, document, name="version"):
    """Version control (UI-62): <name>/model.opad (one box) committed on main with .gitattributes asking for OPAD's driver,
    the bare remote <name>-remote.git added as origin and not pushed yet (<name>-other: the bench's other clone). The bench
    commits, pushes, branches, merges, pulls what the other clone pushed, compares, restores, opens read-only and aborts a
    conflicting merge."""
    folder = root / name
    folder.mkdir(exist_ok=True)
    doc = document(f"{name}/model", ("feature", "--kind", "box", "--inputs", '{"length":"5 mm","width":"5 mm","height":"5 mm"}'))
    git = shutil.which("git")
    if not git:
        return folder / "no-git.opad"  # skipped: the case needs git
    (folder / ".gitattributes").write_bytes(b"*.opad text eol=lf merge=opad diff=opad\n")
    quiet = dict(cwd=folder, check=True, capture_output=True)
    subprocess.run([git, "init", "-q", "-b", "main"], **quiet)
    subprocess.run([git, "add", "-A"], **quiet)
    subprocess.run([git, "-c", "user.name=bench", "-c", "user.email=bench@example.com", "commit", "-q", "-m", "first"], **quiet)
    subprocess.run([git, "init", "-q", "--bare", "-b", "main", str(root / f"{name}-remote.git")], **quiet)
    subprocess.run([git, "remote", "add", "origin", str(root / f"{name}-remote.git")], **quiet)
    return doc


def read_only(root, document, name="read-only"):
    """A version opened read-only (UI-62): <name>/model.opad (one box) write-protected, as the history writes a version's
    copy, and <name>/writable.opad, the same left writable, which the bench opens with --read-only."""
    (root / name).mkdir(exist_ok=True)
    doc = document(f"{name}/model", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'))
    shutil.copy(doc, root / name / "writable.opad")
    os.chmod(doc, stat.S_IREAD)
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
    # Recovery with diff: the offer's detail pane, Restore into file, Merge into current, Review changes…, Discard; also in Arabic.
    # Version control: commit, push, branch, merge, pull, history; git's global config the run's own (the author is asked).
    ("version", versioned_with_remote, {"OPAD_BENCH_VERSION": "{prefix}", "OPAD_BENCH_CLI": "{cli}", "GIT_CONFIG_GLOBAL": "{root}/version-global",
                                        "GIT_CONFIG_NOSYSTEM": "1"}),
    # The same right to left: the panel, the dialogs and the toasts translated.
    ("version-ar", lambda root, document: versioned_with_remote(root, document, "version-ar"),
     {"OPAD_BENCH_VERSION": "{prefix}", "OPAD_BENCH_CLI": "{cli}", "GIT_CONFIG_GLOBAL": "{root}/version-ar-global", "GIT_CONFIG_NOSYSTEM": "1",
      "OPAD_LANG": "ar"}),
    # A read-only document: view changes kept out of "unsaved", edits ask for a copy, Save a copy, --read-only in another OPAD.
    ("read-only", read_only, {"OPAD_BENCH_READONLY": "{prefix}"}),
    ("read-only-ar", lambda root, document: read_only(root, document, "read-only-ar"), {"OPAD_BENCH_READONLY": "{prefix}", "OPAD_LANG": "ar"}),
    ("recovery-diff", recovered, {"OPAD_BENCH_RECOVERY_DIFF": "{prefix}"}),
    ("recovery-diff-ar", lambda root, document: recovered(root, document, "recovery-ar"), {"OPAD_BENCH_RECOVERY_DIFF": "{prefix}", "OPAD_LANG": "ar"}),
]
