#!/usr/bin/env python3
"""End-to-end tests for opad-cli, including the git merge story.

usage: test_cli.py <path-to-opad-cli> <fixtures-dir>
"""
import hashlib
import json
import os
import re
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
    # hidden-line projection (drawing views): typed curves with their sources, all of them in a file, a preview
    p = run("project", DOC, "--view", "front", "--quality", "exact", "--curves", "true")
    assert p["tier"] == "exact" and p["counts"]["visible"] > 0 and p["counts"]["hidden"] > 0
    assert all(c["type"] in ("line", "arc", "ellipse", "spline", "polyline") and "body" in c for c in p["curves"])
    assert abs(p["bounds"][2] - p["bounds"][0] - 100) < 1e-6 and len(p["bodies"]) == 10
    lines = os.path.join(tmp, "front.json")
    h = run("project", DOC, "--view", "front", "--quality", "hybrid", "--hidden", "false", "--out", lines)
    with open(lines) as f:
        full = json.load(f)
    assert h["tier"] == "hybrid" and h["counts"]["hidden"] == 0 and "curves" not in h
    assert len(full["curves"]) == h["counts"]["curves"] and full["fingerprint"] == h["fingerprint"]
    b = run("project", DOC, "--view", "iso", "--quality", "hybrid", "--curves", "true", "--bezier", "true")
    curved = [c for c in b["curves"] if c["type"] in ("arc", "ellipse", "spline")]
    assert curved and all(c["bezier"] and all(len(q) == 8 for q in c["bezier"]) for c in curved)
    assert all("bezier" not in c for c in b["curves"] if c["type"] in ("line", "polyline"))
    png = os.path.join(tmp, "iso-lines.png")
    run("project", DOC, "--view", "iso", "--out", png, "--width", "400")
    assert open(png, "rb").read(4) == b"\x89PNG"
    # a view of the model as a 2D drawing (DXF R2000 / SVG): visible and hidden lines on their layers, 6 decimals
    dxf = os.path.join(tmp, "front.dxf")
    v = run("export", DOC, "--format", "dxf", "--view", "front", "--hidden", "true", "--out", dxf)
    assert v["bodies"] == 10 and v["layers"]["Visible"] > 0 and v["layers"]["Hidden"] > 0 and v["view"]["hidden"]
    text = open(dxf, encoding="ascii").read()
    assert "AC1015" in text and "\nHIDDEN\n" in text and "$INSUNITS" in text
    assert not re.search(r"\n-?\d+\.\d{7,}\n", text)
    svg = os.path.join(tmp, "iso.svg")
    v = run("export", DOC, "--format", "svg", "--view", "iso", "--out", svg)
    assert "Hidden" not in v["layers"] and open(svg, encoding="utf-8").read().startswith("<?xml")
    # and as PDF (one vector page on the smallest ISO sheet) and PNG, painted by Qt offscreen in the CLI
    pdf = os.path.join(tmp, "front.pdf")
    v = run("export", DOC, "--format", "pdf", "--view", "front", "--hidden", "true", "--out", pdf)
    data = open(pdf, "rb").read()
    assert data.startswith(b"%PDF-") and len(re.findall(rb"/Type /Page\b(?!s)", data)) == 1 and v["paper"].startswith("A"), v
    png = os.path.join(tmp, "front.png")
    v = run("export", DOC, "--format", "png", "--view", "front", "--dpi", "100", "--out", png)
    assert open(png, "rb").read(4) == b"\x89PNG" and v["dpi"] == 100 and v["pixels"][0] > 100, v
    # a drawing sheet (on a copy) as a PDF page of its own paper, named by its name
    sheet_doc = os.path.join(tmp, "sheet.opad")
    shutil.copy(DOC, sheet_doc)
    s = run("sheet", sheet_doc, "--size", "A3", "--name", "Assembly")
    iso = run("sheet_view", sheet_doc, "--sheet", s["id"], "--orient", "iso", "--scale", "auto")
    pdf = os.path.join(tmp, "sheet.pdf")
    v = run("export", sheet_doc, "--sheet", "Assembly", "--format", "pdf", "--out", pdf)
    assert v["paper"] == "A3" and v["page"] == [420, 297] and v["sheet"]["views"] == 1 and v["layers"]["Visible"] > 0, v
    data = open(pdf, "rb").read()
    assert data.startswith(b"%PDF-") and b"/Type /Font" in data  # its title block's text, in an embedded font
    # lettered in the OFL font compiled into the program (UI-139), also with no system fonts to take from
    nofonts = os.path.join(tmp, "nofonts")
    os.makedirs(nofonts, exist_ok=True)
    lone = os.path.join(tmp, "lone.pdf")
    p = subprocess.run([CLI, "export", sheet_doc, "--sheet", "Assembly", "--format", "pdf", "--out", lone], capture_output=True, text=True,
                       env=dict(os.environ, QT_QPA_FONTDIR=nofonts))
    assert p.returncode == 0, p.stderr
    data = open(lone, "rb").read()
    assert b"/BaseFont /LiberationSans" in data and b"/FontFile2" in data, "the drawing font is not the compiled-in Liberation Sans"
    # a parts list, auto-balloons and an issued revision whose PDF is written and hashed (UI-84)
    run("sheet_item", sheet_doc, "--sheet", s["id"], "--kind", "parts_list")
    balloons = run("sheet_balloons", sheet_doc, "--sheet", s["id"], "--view", iso["id"])
    assert balloons["ids"] and not balloons["created"], balloons
    issued_pdf = os.path.join(tmp, "issued.pdf")
    issue = run("sheet_issue", sheet_doc, "--sheet", s["id"], "--description", "First release", "--out", issued_pdf)
    assert issue["rev"] == "A" and issue["frozen"] == 1 and issue["pdf"] == "issued.pdf", issue
    assert issue["pdf_sha256"] == hashlib.sha256(open(issued_pdf, "rb").read()).hexdigest()
    info = run("sheet_info", sheet_doc, "--sheet", s["id"])
    assert info["issues"][0]["rev"] == "A" and not info["issues"][0]["changed"]["views"], info["issues"]
    # exported as issued after a note was added and a balloon deleted: the sheet as it stood then, pixel for pixel
    at_issue, as_issued = os.path.join(tmp, "at-issue.png"), os.path.join(tmp, "as-issued.png")
    run("export", sheet_doc, "--sheet", s["id"], "--format", "png", "--dpi", "60", "--out", at_issue)
    run("sheet_item", sheet_doc, "--sheet", s["id"], "--kind", "note", "--text", "LATER", "--at", "[40,40]")
    run("delete", sheet_doc, "--target", balloons["ids"][0])
    v = run("export", sheet_doc, "--sheet", s["id"], "--format", "png", "--dpi", "60", "--issue", "A", "--out", as_issued)
    assert v["issue"] == "A" and open(as_issued, "rb").read() == open(at_issue, "rb").read(), v
    run("export", sheet_doc, "--sheet", s["id"], "--format", "png", "--dpi", "60", "--out", as_issued)
    assert open(as_issued, "rb").read() != open(at_issue, "rb").read()
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


def bill_of_materials():
    doc = os.path.join(tmp, "bom.opad")
    run("new", doc)
    run("import", doc, os.path.join(FIXTURES, "assembly.step"))
    parts = run("bom", doc)
    assert parts["mode"] == "parts" and parts["assembly"]["name"] == "Fixture"
    assert [(r["name"], r["qty"]) for r in parts["rows"]] == [("Plate", 1), ("Lid", 1), ("Bolt[1]", 4), ("Bolt[2]", 4)]
    assert [(r["item"], r["qty"], r["total_qty"]) for r in run("bom", doc, "--mode", "indented")["rows"]][2:4] == [("3", 1, 1), ("3.1", 4, 4)]
    plate = find_node(run("tree", doc), "Plate")["id"]
    p = run("part_properties", doc, "--target", plate, "--set", '{"material": "Aluminum 6061-T6", "part_number": "OP-7"}')
    assert p["material"]["id"] == "aluminium-6061"
    assert abs(run("properties", doc, "--node", plate)["mass"] - 100 * 60 * 5 * 2.7 / 1000) < 1e-6
    row = run("bom", doc, "--mode", "top", "--mass-unit", "kg")["rows"][0]
    assert row["part_number"] == "OP-7" and abs(row["mass"] - 0.081) < 1e-9
    # CSV: on stdout byte for byte (byte order mark, CRLF), or into a file
    raw = subprocess.run([CLI, "bom", doc, "--format", "csv"], capture_output=True).stdout
    assert raw.startswith(b"\xef\xbb\xbfItem,Qty,Part number,Name,") and raw.count(b"\r\n") == 5 and b"\r\r" not in raw
    assert b"\r\n1,1,OP-7,Plate,,Aluminum 6061-T6,81.00,81.00," in raw
    out = os.path.join(tmp, "bom.csv")
    assert run("bom", doc, "--format", "csv", "--out", out)["rows"] == 4
    with open(out, "rb") as f:
        assert f.read() == raw
    assert run("materials", "--match", "SS304")["match"]["id"] == "stainless"


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
test(bill_of_materials)
test(deterministic_builds)
shutil.rmtree(tmp, ignore_errors=True)
sys.exit(1 if FAILED else 0)
