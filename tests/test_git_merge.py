"""Git merges of .opad files through the record-aware merge drivers (UI-60).

usage: test_git_merge.py <opad-cli> [<opad app>]

The Python reference (tools/opad_merge.py), `opad-cli merge-driver` and, when given, `opad --merge-driver` run the same
git story (multiline drawings, sketches and comments merged by real branches, then a conflict), the same three-way cases
and the same mutated files: they must merge byte for byte alike and refuse alike, for the same reason.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile

driver = Path(__file__).resolve().parents[1] / "tools" / "opad_merge.py"
spec = importlib.util.spec_from_file_location("opad_merge", driver)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
cli = str(Path(sys.argv[1]).resolve())
drivers = {"python": [sys.executable, str(driver)], "cli": [cli, "merge-driver"]}
if len(sys.argv) > 2:
    drivers["app"] = [str(Path(sys.argv[2]).resolve()), "--merge-driver"]
# Every run writes the same documents, so the drivers' merges compare byte for byte.
env = dict(os.environ, OPAD_DETERMINISTIC="git-merge", OPAD_AUTHOR="test")
# Line and paragraph separators, a next-line control: JSON text may hold them raw, str.splitlines() breaks at them.
SEPARATORS = chr(0x2028) + chr(0x85) + chr(0x2029)


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, env=env, text=True, encoding="utf-8", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert result.returncode == 0, (args, result.stdout)
    return result.stdout


def opad(command, doc, **args):
    flags = [part for key, value in args.items() for part in ("--" + key, value if isinstance(value, str) else json.dumps(value))]
    return json.loads(run(cli, command, str(doc), *flags))


def driver_config(name):
    exe, arg = drivers[name]
    if name == "python":
        return f'"{Path(exe).as_posix()}" "{Path(arg).as_posix()}" %O %A %B'
    return f'"{Path(exe).as_posix()}" {arg} %O %A %B %P'


def merge(name, folder, base, ours, theirs):
    """One driver on one case, as git calls it: (exit code, reason or None, the ours file afterwards)."""
    folder.mkdir(parents=True, exist_ok=True)
    paths = [folder / "base", folder / "ours", folder / "theirs"]
    for path, data in zip(paths, (base, ours, theirs)):
        path.write_bytes(data)
    extra = [] if name == "python" else ["part.opad"]
    result = subprocess.run(drivers[name] + [str(p) for p in paths] + extra, capture_output=True)
    prefix = b"OPAD merge conflict: " if name == "python" else b"OPAD merge conflict in part.opad: "
    reason = None
    if result.returncode:
        assert result.stderr.startswith(prefix), (name, result.stderr)
        reason = result.stderr[len(prefix):].decode("utf-8").strip()
    return result.returncode, reason, paths[1].read_bytes()


def story(root, name):
    """The branch story, merged by git through one driver: the merged file, the base, the note and the feature."""
    root.mkdir()
    doc = root / "part.opad"
    opad("new", doc)
    feature = opad("feature", doc, kind="box", inputs={"length": 20, "width": 10, "height": 5})["feature_id"]
    note = opad("annotate", doc, anchor="point/0,0,0", text="Base note")["id"]
    base = doc.read_bytes()
    run("git", "init", "-q", "-b", "main", str(root))
    for key, value in (("user.name", "OPAD test"), ("user.email", "opad@example.invalid"),
                       ("commit.gpgsign", "false"), ("core.autocrlf", "false"), ("merge.opad.driver", driver_config(name))):
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
    assert raw.split(b"#bodies\n", 1)[1] == base.split(b"#bodies\n", 1)[1]
    # Two different changes to one note: git stops with the file as ours had it, and the driver says why.
    run("git", "checkout", "-qb", "left", cwd=root)
    opad("append", doc, op={"op": "edit", "target": note, "set": {"text": "left"}})
    run("git", "commit", "-qam", "left", cwd=root)
    run("git", "checkout", "-qb", "right", "main", cwd=root)
    opad("append", doc, op={"op": "edit", "target": note, "set": {"text": "right"}})
    run("git", "commit", "-qam", "right", cwd=root)
    run("git", "checkout", "-q", "left", cwd=root)
    left = doc.read_bytes()
    result = subprocess.run(["git", "merge", "--no-edit", "right"], cwd=root, capture_output=True, text=True, encoding="utf-8")
    assert result.returncode != 0 and f"concurrent changes to {note}/text" in result.stdout + result.stderr, result
    assert doc.read_bytes() == left and "UU part.opad" in run("git", "status", "--porcelain", cwd=root)
    run("git", "merge", "--abort", cwd=root)
    return raw, base, note, feature


with tempfile.TemporaryDirectory(prefix="opad-merge-") as folder:
    root = Path(folder)
    stories = {name: story(root / name, name) for name in drivers}
    merged, base, note, feature = stories["python"]
    for name, (raw, *_rest) in stories.items():
        assert raw == merged, f"{name} merged the git story differently from the Python driver"

    work = root / "work"
    work.mkdir()

    def version(*commands, text=None):
        """The base, then opad-cli commands, then an edit of its text."""
        path = work / "version.opad"
        path.write_bytes(base)
        for command, args in commands:
            opad(command, path, **args)
        data = path.read_bytes()
        return text(data.decode("utf-8")).encode("utf-8") if text else data

    def last_op(data):  # the one-line record before the body store
        ops = data.decode("utf-8").split("#bodies\n")[0].rstrip("\n").split("\n")
        return ops[-1]

    def entry(geometry, meta=None):  # a body store entry (the drivers only check that its text hashes to its key)
        head = f"#body {hashlib.sha256(geometry.encode()).hexdigest()} {geometry.count(chr(10))}"
        return head + ("" if meta is None else " " + meta) + "\n" + geometry

    def insert(record):  # a record at the end of the op log
        return lambda t: t.replace("#bodies\n", record + "\n#bodies\n", 1)

    def swap(t, i, j):
        split = t.split("\n")
        split[i], split[j] = split[j], split[i]
        return "\n".join(split)

    edit = lambda text: ("append", {"op": {"op": "edit", "target": note, "set": {"text": text}}})
    comment = ("annotate", {"anchor": "point/1,2,3", "text": "Ours"})
    param = ("param", {"name": "wall", "expr": "2 mm"})
    unknown = '{"op":"sheet_item","id":"%s","ts":"2000-01-01T00:00:00Z","by":"t","target":"%s","x":%d}'
    ours_edit, theirs_edit = version(edit("left")), version(edit("right"))
    ours, theirs = version(comment), version(param)
    base_lines = base.decode("utf-8").split("\n")
    assert base_lines[5] == "#bodies" and json.loads(base_lines[4])["id"] == note  # the box feature, the note
    cases = {  # name: base, ours, theirs, None (merges) or the reason ("*": any)
        "independent": (base, ours, theirs, None),
        "same field": (base, ours_edit, theirs_edit, f"concurrent changes to {note}/text; manual review required"),
        "one id, two contents": (base, ours_edit, ours_edit.replace(b'"left"', b'"different"'),
                                 f"conflicting operation ID {json.loads(last_op(ours_edit))['id']}"),
        "same new op on both sides": (base, ours_edit, ours_edit, None),
        # Changes alike from both sides (UI-63): no conflict, both kept.
        # (each side through another command, so the two are different ops: deterministic ids follow the command)
        "same edit on both sides": (base, version(edit("alike")), version(("append", {"op": {"op": "edit", "target": note, "set": {"text": "alike", "style": "ok"}}})), None),
        "same delete on both sides": (base, version(("delete", {"target": note})), version(("append", {"op": {"op": "delete", "target": note}})), None),
        "same parameter on both sides": (base, version(("param", {"name": "wall", "expr": "3 mm"})),
                                         version(("append", {"op": {"op": "param", "name": "wall", "expr": "3 mm"}})), None),
        "same parameter, another value": (base, version(("param", {"name": "wall", "expr": "3 mm"})),
                                          version(("append", {"op": {"op": "param", "name": "wall", "expr": "4 mm"}})),
                                          "concurrent changes to parameter:wall/*; manual review required"),
        "delete against edit": (base, version(("delete", {"target": note})), theirs_edit, f"concurrent changes to {note}/*; manual review required"),
        "one parameter": (base, version(param), version(("param", {"name": "wall", "expr": "3 mm"})),
                          "concurrent changes to parameter:wall/*; manual review required"),
        "one feature edited": (base, version(("feature_edit", {"target": feature, "inputs": {"length": 30}})),
                               version(("feature_edit", {"target": feature, "inputs": {"length": 40}})),
                               f"concurrent changes to {feature}/geometry; manual review required"),
        "new bodies on both sides": (base, version(("feature", {"kind": "box", "inputs": {"length": 3, "width": 3, "height": 3}})),
                                     version(("feature", {"kind": "cylinder", "inputs": {"diameter": 4, "height": 2}})), None),
        "unknown op types": (base, version(text=insert(unknown % ("u-ours", note, 1))), version(text=insert(unknown % ("u-theirs", note, 2))),
                             f"concurrent changes to {note}/x; manual review required"),
        "op type not a string": (base, version(text=insert(unknown.replace('"sheet_item"', "5") % ("u-ours", note, 1))),
                                 version(text=insert(unknown % ("u-theirs", note, 2))), f"concurrent changes to {note}/x; manual review required"),
        "header changed": (base, ours, version(param, text=lambda t: t.replace('"units":"mm"', '"units":"in"', 1)),
                           "document header changed; manual review required"),
        "history rewritten": (base, version(text=lambda t: t.replace('"Base note"', '"Changed note"', 1)), theirs,
                              "history was rewritten; manual review required"),
        "same op written otherwise": (base, version(comment, text=lambda t: t.replace('"Base note"', ' "Base note" ', 1)), theirs, None),
        "body store pruned": (base, ours, version(param, text=lambda t: t[:t.index("#bodies\n") + 8]), "body store was pruned; manual review required"),
        "base ops reordered": (base, version(text=lambda t: swap(t, 3, 4)), theirs, "operation history was reordered"),
        "theirs between base ops": (base, ours, version(text=lambda t: t.replace(base_lines[4], last_op(theirs) + "\n" + base_lines[4], 1)),
                                    "branch operation order conflicts; manual review required"),
        "CRLF and CR": (base, ours.replace(b"\n", b"\r\n"), theirs.replace(b"\n", b"\r"), None),
        "format 1 base": (base.replace(b"#opad 2\n", b"#opad 1\n", 1), ours, theirs, None),
        "an op twice": (base, version(comment, text=insert(base_lines[4])), theirs, None),
        "an op twice, changed": (base, version(comment, text=insert(base_lines[4].replace("Base note", "Other"))), theirs,
                                 f"conflicting operation ID {note}"),
        "blank line among ops": (base, version(text=insert("")), theirs, "unexpected metadata in operation log"),
        "comment among ops": (base, version(text=insert("# a note")), theirs, "unexpected metadata in operation log"),
        "op without id": (base, version(text=insert('{"op":"rename"}')), theirs, "operation requires an ID"),
        "broken record": (base, version(text=insert('{"op":"rename",')), theirs, "*"),
        "no body store": (base, ours, theirs.split(b"#bodies\n")[0], "missing body store"),
        "body key": (base, ours, version(text=lambda t: re.sub(r"#body (.)", lambda m: "#body " + ("1" if m[1] == "0" else "0"), t, count=1)),
                     "body hash mismatch"),
        "body text": (base, ours, version(text=lambda t: t.replace("CASCADE Topology V1", "CASCADE Topology V2", 1)), "body hash mismatch"),
        "body length": (base, ours, version(text=lambda t: re.sub(r"(#body \S+ )\d+", r"\g<1>999999", t, count=1)), "invalid body length"),
        "body without meta": (base, ours, version(text=lambda t: t + entry("x\n")), "invalid body header"),
        "not UTF-8": (base, ours, theirs.replace(b"wall", b"w\xffll"), "*"),
        "line separators in names": (base, version(comment, text=lambda t: t + entry("x\ny\n", json.dumps({"name": "a" + SEPARATORS + "b"}, ensure_ascii=False))),
                                     version(edit("c" + SEPARATORS + "d")), None),
        "byte order mark": (base, b"\xef\xbb\xbf" + ours, theirs, "unsupported OPAD header"),
    }
    results, folders = {}, {}
    for index, (name, (b, o, t, expected)) in enumerate(cases.items()):
        folders[name] = work / f"case{index}"
        outcome = {driver_name: merge(driver_name, folders[name] / driver_name, b, o, t) for driver_name in drivers}
        code, reason, data = outcome["python"]
        for driver_name, (other_code, other_reason, other_data) in outcome.items():
            assert other_code == code and other_data == data, (name, driver_name, other_code, other_reason, code, reason)
            assert expected == "*" or other_reason == reason, (name, driver_name, other_reason, reason)
        if expected is None:
            assert code == 0, (name, reason)
        else:
            assert code == 1 and data == o, (name, code)
            assert expected == "*" or reason == expected, (name, reason, expected)
        results[name] = data
    assert results["independent"].startswith(ours.split(b"#bodies\n")[0]) and last_op(theirs).encode() in results["independent"]
    assert results["same new op on both sides"].count(b'"text":"left"') == 1
    assert b"\r" not in results["CRLF and CR"] and results["CRLF and CR"].startswith(b"#opad 2\n")
    assert results["format 1 base"].startswith(b"#opad 2\n")
    assert results["an op twice"].count(b'"Base note"') == 1
    assert opad("info", folders["new bodies on both sides"] / "cli" / "ours")["body_entries"] == 3
    assert ("a" + SEPARATORS + "b").encode() in results["line separators in names"]
    assert opad("tree", folders["line separators in names"] / "cli" / "ours") is not None
    for name in drivers:  # usage: three files and the optional name, nothing else
        assert subprocess.run(drivers[name] + ["only", "two"], capture_output=True).returncode == 1

    # Mutated files: whatever the reference makes of them, the C++ driver makes of them too.
    rng = random.Random(60)
    sources = [cases[name][:3] for name in ("independent", "new bodies on both sides", "line separators in names")]
    alphabet = '{}[]",:\\# \nx09'
    rounds = 80
    for round in range(rounds):
        files = list(rng.choice(sources))
        which = rng.choices((0, 1, 2), (15, 50, 35))[0]
        text = files[which].decode("utf-8")
        ops = text.index("#bodies\n") + 8
        at = rng.randrange(0, ops) if rng.random() < 0.85 else rng.randrange(0, len(text))
        kind = rng.randrange(8)
        if kind == 7:  # a record broken over lines after a comma or colon (still JSON outside strings)
            breaks = [i for i, c in enumerate(text[:ops]) if c in ",:"]
            at = rng.choice(breaks) + 1
            text = text[:at] + rng.choice(("\n", "\n  ", " ")) + text[at:]
        elif kind == 0:
            text = text[:at] + rng.choice(alphabet) + text[at + 1:]
        elif kind == 1:
            text = text[:at] + rng.choice(alphabet) + text[at:]
        elif kind == 2:
            text = text[:at] + text[at + 1:]
        elif kind == 6:
            text = text[:at]
        else:
            split = text.split("\n")
            line = min(text[:at].count("\n"), len(split) - 2)
            if kind == 3:
                del split[line]
            elif kind == 4:
                split.insert(line, split[line])
            else:
                split[line], split[line + 1] = split[line + 1], split[line]
            text = "\n".join(split)
        files[which] = text.encode("utf-8")
        for path, data in zip(("base", "ours", "theirs"), files):
            (work / path).write_bytes(data)
        try:
            expected, reason = module.merge(work / "base", work / "ours", work / "theirs").encode("utf-8"), None
        except Exception as error:  # noqa: BLE001 - the driver exits 1 on anything
            expected, reason = None, str(error)
        code, other_reason, data = merge("cli", work / "fuzz" / str(round), *files)
        if expected is None:
            assert code == 1 and data == files[1], (round, kind, reason)
            own = reason.startswith(("unsupported", "unexpected", "operation", "conflicting", "missing", "invalid body", "body",
                                     "document", "history", "branch", "concurrent"))
            assert not own or other_reason == reason, (round, kind, reason, other_reason)
        else:
            assert code == 0 and data == expected, (round, kind, other_reason)
print(f"Git merge: {', '.join(drivers)} drivers agree on the git story, {len(cases)} cases and {rounds} mutated files; "
      "multiline drawings/sketches/comments retained; overlapping edits rejected")
