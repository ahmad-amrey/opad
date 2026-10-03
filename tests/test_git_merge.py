"""Actual Git-driver merge of multiline records plus conservative conflict checks."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

driver = Path(__file__).resolve().parents[1] / "tools" / "opad_merge.py"
spec = importlib.util.spec_from_file_location("opad_merge", driver)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
cli = str(Path(sys.argv[1]).resolve())


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, text=True, encoding="utf-8", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert result.returncode == 0, (args, result.stdout)
    return result.stdout


def opad(command, doc, **args):
    flags = [part for key, value in args.items() for part in ("--" + key, value if isinstance(value, str) else json.dumps(value))]
    return json.loads(run(cli, command, str(doc), *flags))


with tempfile.TemporaryDirectory(prefix="opad-merge-") as folder:
    root = Path(folder)
    doc = root / "part.opad"
    opad("new", doc)
    opad("feature", doc, kind="box", inputs={"length": 20, "width": 10, "height": 5})
    note = opad("annotate", doc, anchor="point/0,0,0", text="Base note")["id"]
    base = root / "base.txt"
    shutil.copyfile(doc, base)
    run("git", "init", "-q", "-b", "main", str(root))
    for key, value in (("user.name", "OPAD test"), ("user.email", "opad@example.invalid"),
                       ("commit.gpgsign", "false"), ("core.autocrlf", "false"),
                       ("merge.opad.driver", f'"{Path(sys.executable).as_posix()}" "{driver.as_posix()}" %O %A %B')):
        run("git", "config", key, value, cwd=root)
    (root / ".gitattributes").write_text("*.opad text eol=lf merge=opad\n", encoding="utf-8")
    run("git", "add", "part.opad", ".gitattributes", cwd=root)
    run("git", "commit", "-qm", "base", cwd=root)
    run("git", "checkout", "-qb", "drawing", cwd=root)
    drawing = {"plane": {"origin": [0, 0, 0], "x": [1, 0, 0], "y": [0, 1, 0]},
               "strokes": [{"color": "red", "width": 4, "points": [[0, 0], [4, 5], [8, 0]]}]}
    annotation = opad("annotate", doc, anchor="point/0,0,0", text="Round this", style="ai_agent", drawing=drawing)["id"]
    assert '"strokes": [\n' in doc.read_text(encoding="utf-8")
    run("git", "commit", "-qam", "drawing request", cwd=root)
    run("git", "checkout", "-qb", "sketch", "main", cwd=root)
    opad("sketch", doc, name="Branch sketch", plane={"base": "xy"}, geometry={"points": [], "entities": [], "constraints": []})
    opad("annotate", doc, anchor="point/0,0,0", text="Independent comment", reply_to=note)
    run("git", "commit", "-qam", "sketch and comment", cwd=root)
    run("git", "checkout", "-q", "main", cwd=root)
    run("git", "merge", "-q", "--no-edit", "drawing", cwd=root)
    run("git", "merge", "-q", "--no-edit", "sketch", cwd=root)
    assert opad("info", doc)["unresolved"] == 0
    assert opad("annotations", doc, id=annotation)["annotations"][0]["drawing"] == drawing
    assert len(opad("annotations", doc, id=note)["annotations"][0]["comments"]) == 1
    assert opad("context", doc)["sketches"] == 1
    raw = doc.read_bytes()
    assert b"\r" not in raw
    assert raw.split(b"#bodies\n", 1)[1] == base.read_bytes().split(b"#bodies\n", 1)[1]
    # Two different changes to one note must report a conflict, never silently win.
    ours, theirs = root / "ours.opad", root / "theirs.opad"
    for path, text in ((ours, "left"), (theirs, "right")):
        shutil.copyfile(base, path)
        opad("append", path, op={"op": "edit", "target": note, "set": {"text": text}})
    before = ours.read_bytes()
    result = subprocess.run([sys.executable, str(driver), str(base), str(ours), str(theirs)], capture_output=True)
    assert result.returncode == 1 and ours.read_bytes() == before
    # Same-ID/different-content and body hash corruption are hard failures.
    shutil.copyfile(ours, theirs)
    theirs.write_text(theirs.read_text(encoding="utf-8").replace('"left"', '"different"'), encoding="utf-8")
    result = subprocess.run([sys.executable, str(driver), str(base), str(ours), str(theirs)], capture_output=True)
    assert result.returncode == 1
    shutil.copyfile(base, theirs)
    contents = theirs.read_text(encoding="utf-8")
    position = contents.index("#body ") + len("#body ")
    contents = contents[:position] + ("0" if contents[position] != "0" else "1") + contents[position + 1:]
    theirs.write_text(contents, encoding="utf-8")
    result = subprocess.run([sys.executable, str(driver), str(base), str(ours), str(theirs)], capture_output=True)
    assert result.returncode == 1 and b"hash mismatch" in result.stderr
    # TODO 11 UI-35: an exploded view is an optional field of a view op. Saving into one view (an edit) merges with a new
    # exploded view from another branch; two edits of the same view's explode are a conflict.
    view = opad("explode", doc, levels=0, name="Exploded")["id"]
    run("git", "commit", "-qam", "exploded view", cwd=root)
    run("git", "checkout", "-qb", "explode-edit", cwd=root)
    opad("explode", doc, view=view, mode="stack", update=True)
    run("git", "commit", "-qam", "stack it", cwd=root)
    run("git", "checkout", "-qb", "explode-new", "main", cwd=root)
    other = opad("explode", doc, levels=1, name="Exploded 2")["id"]
    run("git", "commit", "-qam", "another exploded view", cwd=root)
    run("git", "checkout", "-q", "main", cwd=root)
    run("git", "merge", "-q", "--no-edit", "explode-edit", cwd=root)
    run("git", "merge", "-q", "--no-edit", "explode-new", cwd=root)
    views = {v["id"]: v for v in opad("annotations", doc)["views"]}
    assert views[view]["explode"]["mode"] == "stack" and views[other]["explode"]["levels"] == 1, views
    shutil.copyfile(doc, base)
    for path, spacing in ((ours, 2), (theirs, 3)):
        shutil.copyfile(base, path)
        opad("explode", path, view=view, spacing=spacing, update=True)
    before = ours.read_bytes()
    result = subprocess.run([sys.executable, str(driver), str(base), str(ours), str(theirs)], capture_output=True)
    assert result.returncode == 1 and ours.read_bytes() == before and b"explode" in result.stderr
    # TODO 11 UI-33: features and sketches made in a component on two branches merge; their bodies stay in it.
    lid = opad("component", doc, name="Lid")["component_id"]
    run("git", "commit", "-qam", "lid", cwd=root)
    made = {}
    for branch, x in (("lid-a", 0), ("lid-b", 40)):
        run("git", "checkout", "-qb", branch, "main", cwd=root)
        sketch = opad("sketch", doc, component=lid, geometry={"shapes": [{"kind": "rect2", "picks": [[x, 50], [x + 10, 60]]}]})["sketch_id"]
        made[branch] = opad("feature", doc, kind="extrude", component=lid, inputs={"profiles": [{"sketch": sketch, "at": [x + 5, 55]}], "distance": 3})["body_ids"][0]
        run("git", "commit", "-qam", "pad in the lid", cwd=root)
    run("git", "checkout", "-q", "main", cwd=root)
    run("git", "merge", "-q", "--no-edit", "lid-a", cwd=root)
    run("git", "merge", "-q", "--no-edit", "lid-b", cwd=root)
    tree = opad("tree", doc)
    assert not tree["unresolved"], tree["unresolved"]
    lid_node = next(n for n in tree["roots"] if n["id"] == lid)
    assert {c["id"] for c in lid_node["children"]} == set(made.values()), lid_node
    assert sum(f.get("component") == lid for f in opad("features", doc)) == 4
print("Git merge: multiline drawings/sketches/comments, exploded views and component work retained; overlapping edits rejected")
