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
    box = opad("feature", doc, kind="box", inputs={"length": 20, "width": 10, "height": 5})["body_ids"][0]
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
    # Drawings (TODO 11 UI-76): sheets, views and items carry no target, so two people adding views and dimensions to
    # one sheet merge; part properties merge key by key; the same field changed on both sides is refused.
    run("git", "checkout", "-q", "main", cwd=root)
    sheet = opad("sheet", doc, size="A4")["id"]
    front = opad("sheet_view", doc, sheet=sheet, orient="front", at=[100, 150])["id"]
    run("git", "commit", "-qam", "sheet", cwd=root)
    run("git", "checkout", "-qb", "dims-a", cwd=root)
    width = opad("sheet_item", doc, sheet=sheet, view=front, type="horizontal", refs=[f"{box}/edge/9"])
    assert width["result"]["value"] == 20, width
    opad("part_properties", doc, target=box, set={"part_number": "OP-1002"})
    run("git", "commit", "-qam", "width", cwd=root)
    run("git", "checkout", "-qb", "dims-b", "main", cwd=root)
    opad("sheet_view", doc, sheet=sheet, parent=front, side="bottom")
    height = opad("sheet_item", doc, sheet=sheet, view=front, type="vertical", refs=[f"{box}/edge/0"])
    assert height["result"]["value"] == 5, height
    opad("part_properties", doc, target=box, set={"material": "PA12"})
    run("git", "commit", "-qam", "height", cwd=root)
    run("git", "checkout", "-q", "main", cwd=root)
    run("git", "merge", "-q", "--no-edit", "dims-a", cwd=root)
    run("git", "merge", "-q", "--no-edit", "dims-b", cwd=root)
    info = opad("sheet_info", doc, sheet=sheet)
    assert len(info["views"]) == 2 and len(info["items"]) == 2 and not info["unresolved"], info
    assert sorted(item["current"]["value"] for item in info["items"]) == [5, 20], info
    assert opad("properties", doc, node=box)["part"] == {"part_number": "OP-1002", "material": "PA12"}
    assert opad("info", doc)["unresolved"] == 0
    drawn = root / "drawn.opad"
    shutil.copyfile(doc, drawn)

    def merges(left, right):
        for path, change in ((ours, left), (theirs, right)):
            shutil.copyfile(drawn, path)
            change(path)
        return subprocess.run([sys.executable, str(driver), str(drawn), str(ours), str(theirs)], capture_output=True).returncode == 0

    assert not merges(lambda p: opad("part_properties", p, target=box, set={"material": "PA6"}),
                      lambda p: opad("part_properties", p, target=box, set={"material": "POM"}))
    assert merges(lambda p: opad("part_properties", p, target=box, set={"material": "PA6"}),
                  lambda p: opad("part_properties", p, target=box, set={"description": "Housing"}))
    assert not merges(lambda p: opad("sheet_edit", p, target=front, set={"at": [90, 150]}),
                      lambda p: opad("sheet_edit", p, target=front, set={"at": [110, 150]}))
    assert merges(lambda p: opad("sheet_edit", p, target=front, set={"at": [90, 150]}),
                  lambda p: opad("sheet_edit", p, target=front, set={"style": {"hidden": True}}))
    assert not merges(lambda p: opad("delete", p, target=front), lambda p: opad("sheet_edit", p, target=front, set={"at": [90, 150]}))
print("Git merge: multiline drawings/sketches/comments and drawing sheets retained; overlapping edits rejected")
