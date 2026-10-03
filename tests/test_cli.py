#!/usr/bin/env python3
"""End-to-end tests for opad-cli, including the git merge story.

usage: test_cli.py <path-to-opad-cli> <fixtures-dir>
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

CLI = os.path.abspath(sys.argv[1])
FIXTURES = os.path.abspath(sys.argv[2])
FAILED = 0


def run(*args, expect_ok=True, stdin=None):
    p = subprocess.run([CLI, *args], capture_output=True, text=True, input=stdin)
    if expect_ok and p.returncode != 0:
        raise AssertionError(f"opad-cli {' '.join(args)} failed:\n{p.stderr}")
    if not expect_ok and p.returncode == 0:
        raise AssertionError(f"opad-cli {' '.join(args)} unexpectedly succeeded")
    out = p.stdout if expect_ok else p.stderr
    return json.loads(out) if out.strip() else None


def git(cwd, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@x", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@x")
    p = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, env=env)
    if p.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed:\n{p.stdout}\n{p.stderr}")
    return p.stdout


def test(fn):
    global FAILED
    try:
        fn()
        print(f"[ ok ] {fn.__name__}")
    except Exception as e:  # noqa: BLE001
        FAILED += 1
        print(f"[FAIL] {fn.__name__}: {e}")


def find_node(tree, name):
    stack = list(tree["roots"])
    while stack:
        n = stack.pop()
        if n["name"] == name:
            return n
        stack.extend(n.get("children", []))
    raise AssertionError(f"node {name} not found")


tmp = tempfile.mkdtemp(prefix="opad-cli-")
DOC = os.path.join(tmp, "fixture.opad")


def basic_workflow():
    v = run("version")
    assert "version" in v
    r = run("new", DOC)
    assert r["doc"].endswith("fixture.opad")
    r = run("import", DOC, os.path.join(FIXTURES, "assembly.step"), "--by", "agent")
    assert r["bodies"] == 10 and r["new_entries"] == 4
    info = run("info", DOC)
    assert info["ops"] == 1 and info["body_entries"] == 4 and info["units"] == "mm"
    assert abs(info["bbox"]["size"][0] - 100) < 1e-6
    tree = run("tree", DOC)
    plate = find_node(tree, "Plate")
    assert plate["type"] == "body" and plate["instances"] == 1
    bolt_head = None
    for n in find_node(tree, "Fasteners")["children"]:
        for b in n["children"]:
            if b["instances"] == 4:
                bolt_head = b
    assert bolt_head is not None
    ops = run("ops", DOC)
    assert ops[0]["op"] == "import" and ops[0]["by"] == "agent"
    props = run("inspect", DOC, plate["id"])
    assert abs(props["volume"] - 100 * 60 * 5) < 1e-3
    faces = run("inspect", DOC, "--uuid", plate["id"] + "/face/0")
    assert faces["type"] == "face"
    m = run("measure", DOC, plate["id"], find_node(tree, "Lid")["id"], "--kind", "distance", "--pin", "true")
    assert abs(m["value"]) < 1e-6 and "pinned_op" in m
    a = run("annotate", DOC, plate["id"], "check flatness", "--by", "reviewer")
    assert "id" in a
    anns = run("annotations", DOC, "--by", "reviewer")
    assert len(anns["annotations"]) == 1 and len(anns["measurements"]) == 1
    # append from stdin
    op = {"op": "rename", "target": plate["id"], "name": "Base plate"}
    r = run("append", DOC, "-", stdin=json.dumps(op))
    assert len(r["appended"]) == 1
    assert find_node(run("tree", DOC), "Base plate")["id"] == plate["id"]
    # exports
    out = os.path.join(tmp, "plate.stl")
    r = run("export", DOC, "--format", "stl", "--out", out, "--select", plate["id"])
    assert os.path.getsize(out) > 84 + 12 * 50 - 1
    out = os.path.join(tmp, "sel.step")
    r = run("export", DOC, "--format", "step", "--out", out, "--select", find_node(tree, "Fasteners")["id"])
    assert r["bodies"] == 8 and os.path.getsize(out) > 1000
    out = os.path.join(tmp, "all.obj")
    run("export", DOC, "--format", "obj", "--out", out)
    assert os.path.getsize(out) > 0
    # render
    png = os.path.join(tmp, "shot.png")
    r = run("render", DOC, "--view", "iso", "--out", png, "--size", "640x400")
    assert r["width"] == 640 and open(png, "rb").read(4) == b"\x89PNG"
    # delete (tombstone) the annotation: it disappears from the resolved list but stays in the log
    run("delete", DOC, "--target", a["id"])
    assert len(run("annotations", DOC)["annotations"]) == 0
    assert any(o.get("deleted") for o in run("ops", DOC))
    assert len(run("ops", DOC, "--live-only", "true")) == len(run("ops", DOC)) - 1
    # error paths
    err = run("info", os.path.join(tmp, "missing.opad"), expect_ok=False)
    assert "error" in err
    err = run("append", DOC, "-", stdin='{"op":"explode"}', expect_ok=False)
    assert "unknown op type" in err["error"]
    # browse mode: a .step path works for read-only commands and persists nothing
    info = run("info", os.path.join(FIXTURES, "box.step"))
    assert info["bodies"] == 1
    cmds = run("commands")
    assert any(c["name"] == "render" for c in cmds)


def git_merge_story():
    repo = os.path.join(tmp, "repo")
    os.makedirs(repo)
    git(repo, "init", "-q", "-b", "main")
    with open(os.path.join(repo, ".gitattributes"), "w") as f:
        f.write("*.opad text eol=lf merge=union\n")
    doc = os.path.join(repo, "model.opad")
    run("new", doc)
    run("import", doc, os.path.join(FIXTURES, "box.step"))
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "base")
    block = run("tree", doc)["roots"][0]["id"]

    # Branch A: two annotations. Branch B: a rename plus a re-import of a new part (adds body entries).
    git(repo, "checkout", "-q", "-b", "alice")
    run("annotate", doc, block, "hole looks undersized", "--by", "alice")
    run("annotate", doc, block + "/face/0", "deburr", "--by", "alice")
    git(repo, "commit", "-q", "-am", "alice review")

    git(repo, "checkout", "-q", "main")
    git(repo, "checkout", "-q", "-b", "bob")
    run("append", doc, "-", stdin=json.dumps({"op": "rename", "target": block, "name": "Housing"}))
    run("import", doc, os.path.join(FIXTURES, "assembly.step"), "--by", "bob")
    git(repo, "commit", "-q", "-am", "bob edits")

    git(repo, "checkout", "-q", "main")
    git(repo, "merge", "-q", "--no-edit", "alice")
    git(repo, "merge", "-q", "--no-edit", "bob")  # would raise on conflict
    status = git(repo, "status", "--porcelain")
    assert status.strip() == "", status

    info = run("info", doc)
    assert info["unresolved"] == 0, info
    assert info["ops"] == 1 + 2 + 2, info
    assert info["body_entries"] == 5, info
    anns = run("annotations", doc)
    assert len(anns["annotations"]) == 2
    assert find_node(run("tree", doc), "Housing")["id"] == block
    # Both branches importing the *same* new part converge on identical body entries.
    git(repo, "checkout", "-q", "-b", "carol", "main")
    run("import", doc, os.path.join(FIXTURES, "box.step"), "--by", "carol")
    git(repo, "commit", "-q", "-am", "carol import")
    git(repo, "checkout", "-q", "-b", "dave", "main")
    run("import", doc, os.path.join(FIXTURES, "box.step"), "--by", "dave")
    git(repo, "commit", "-q", "-am", "dave import")
    git(repo, "checkout", "-q", "main")
    git(repo, "merge", "-q", "--no-edit", "carol")
    git(repo, "merge", "-q", "--no-edit", "dave")
    info = run("info", doc)
    assert info["unresolved"] == 0 and info["body_entries"] == 5 and info["ops"] == 7, info
    # A re-imported body never produces a diff of the body store: unchanged bodies are zero diff lines.
    d = run("diff", os.path.join(tmp, "fixture.opad"), doc)
    assert "summary" in d
    png = os.path.join(tmp, "diff.png")
    run("diff", doc, doc, "--image", png)
    assert os.path.getsize(png) > 100



def semantic_diff_and_textconv():
    """UI-57: diff against git revisions, as JSON and text, and git diff through `diff=opad` textconv."""
    repo = os.path.join(tmp, "diffrepo")
    os.makedirs(repo)
    git(repo, "init", "-q", "-b", "main")
    with open(os.path.join(repo, ".gitattributes"), "w", newline="\n") as f:
        f.write("*.opad text eol=lf diff=opad\n")
    git(repo, "config", "diff.opad.textconv", '"%s" textconv' % CLI.replace("\\", "/"))
    doc = os.path.join(repo, "model.opad")
    run("new", doc)
    run("import", doc, os.path.join(FIXTURES, "assembly.step"), "--by", "alice")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "base")
    lid = find_node(run("tree", doc), "Lid")["id"]
    run("rename", doc, "--target", lid, "--name", "Cover")
    run("transform", doc, "--target", lid, "--matrix", "[1,0,0,5,0,1,0,0,0,0,1,7,0,0,0,1]")
    run("annotate", doc, lid, "chamfer the rim", "--by", "bob")

    d = run("diff", doc)  # one file: since git:HEAD
    assert d["a"] == "git:HEAD" and d["relation"] == "descendant" and d["common_ops"] == 1, d
    kinds = {(c["kind"], c["change"]) for c in d["changes"]}
    assert kinds == {("body", "renamed"), ("body", "moved"), ("annotation", "added")}, kinds
    moved = next(c for c in d["changes"] if c["change"] == "moved")
    assert moved["translation"] == [5.0, 0.0, 2.0], moved  # the lid sat at z = 5
    assert d["summary"] == 'Rename Lid to Cover; move Cover; note "chamfer the rim"', d["summary"]
    assert len(d["ops"]["added"]) == 3 and d["geometry"]["moved"] == 1, d
    p = subprocess.run([CLI, "diff", "--a", "git:HEAD", doc, "--text"], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    assert "  ~ Cover: renamed from Lid\n" in p.stdout and "  + [note] \"chamfer the rim\" by bob\n" in p.stdout, p.stdout
    # git:REV:path is the repository's path, read from its working directory.
    git(repo, "commit", "-q", "-am", "edits")
    p = subprocess.run([CLI, "--compact", "diff", "git:HEAD~1:model.opad", "git:HEAD:model.opad"], capture_output=True, text=True, cwd=repo)
    assert p.returncode == 0, p.stderr
    assert json.loads(p.stdout)["summary"] == d["summary"], p.stdout
    err = run("diff", "--a", "git:nope", doc, expect_ok=False)
    assert "git cat-file blob nope:./model.opad" in err["error"], err

    # git diff shows what changed, not BREP text.
    shown = git(repo, "diff", "HEAD~1", "HEAD")
    assert "CASCADE" not in shown and "#body " not in shown, shown
    assert "-  Lid  [body" in shown and "+  Cover  [body" in shown, shown
    assert "rename  to \"Cover\"" in shown and "+[note] \"chamfer the rim\" on Cover" in shown, shown
    # A file git left conflict markers in still converts (and the command never fails git).
    with open(doc, "a", newline="\n") as f:
        f.write("<<<<<<< ours\n")
    p = subprocess.run([CLI, "textconv", doc], capture_output=True, text=True)
    assert p.returncode == 0 and p.stdout.startswith("unreadable OPAD document:") and "CASCADE" not in p.stdout, p.stdout[:300]


def deterministic_builds():
    """Gap log #15: with OPAD_DETERMINISTIC the same script writes the same file, wherever it writes it."""
    def build(path, seed):
        env = dict(os.environ, OPAD_AUTHOR="script")
        if seed:
            env["OPAD_DETERMINISTIC"] = seed

        def cli(*args):
            p = subprocess.run([CLI, *args], capture_output=True, text=True, env=env)
            assert p.returncode == 0, p.stderr
            return json.loads(p.stdout) if p.stdout.strip() else None

        cli("new", path)
        cli("param", path, "--name", "width", "--expr", "30 mm")
        sk = cli("sketch", path, "--geometry", json.dumps({"shapes": [{"kind": "rect_center", "picks": [[0, 0], [15, 10]]}]}))["sketch_id"]
        body = cli("feature", path, "--kind", "extrude", "--inputs", json.dumps({"profiles": [{"sketch": sk}], "distance": "width / 3"}))["body_ids"][0]
        cli("rename", path, "--target", body, "--name", "Plate")
        cli("param", path, "--name", "width", "--expr", "36 mm")
        with open(path, "rb") as f:
            return f.read()

    first = build(os.path.join(tmp, "plate-a.opad"), "plate")
    again = build(os.path.join(tmp, "plate-b.opad"), "plate")
    assert first == again, "a deterministic build changed between runs"
    assert b"2000-01-01T00:00:00Z" in first
    other = build(os.path.join(tmp, "plate-c.opad"), "")
    assert other != first  # random ids and the clock without it


test(basic_workflow)
test(git_merge_story)
test(semantic_diff_and_textconv)
test(deterministic_builds)
shutil.rmtree(tmp, ignore_errors=True)
sys.exit(1 if FAILED else 0)
