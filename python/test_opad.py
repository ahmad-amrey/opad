"""Tests for the `opad` Python module. usage: test_opad.py <fixtures-dir> (PYTHONPATH must contain the module)."""
import os
import shutil
import sys
import tempfile

import opad

fixtures = sys.argv[1]
tmp = tempfile.mkdtemp(prefix="opad-py-")

assert opad.__version__
d = opad.Document.create()
r = d.import_step(os.path.join(fixtures, "assembly.step"), by="py")
assert r["bodies"] == 10 and r["new_entries"] == 4, r
info = d.info()
assert info["components"] == 6 and info["units"] == "mm"
tree = d.tree()
fixture = tree["roots"][0]
assert fixture["name"] == "Fixture" and len(fixture["children"]) == 3
plate = next(c for c in fixture["children"] if c["name"] == "Plate")
props = d.properties(plate["id"])
assert abs(props["volume"] - 100 * 60 * 5) < 1e-3
face = d.inspect(plate["id"] + "/face/0")
assert face["type"] == "face"
m = d.measure("bbox", [plate["id"]])
assert abs(m["size"][0] - 100) < 1e-6
a = d.annotate(plate["id"], "looks fine", by="py")
assert len(d.annotations()["annotations"]) == 1
d.append({"op": "rename", "target": plate["id"], "name": "Base"})
assert any(c["name"] == "Base" for c in d.tree()["roots"][0]["children"])
path = os.path.join(tmp, "a.opad")
d.save_as(path)
assert not d.dirty and os.path.getsize(path) > 1000

e = opad.open(path)
assert e.op_count == 3 and len(e.body_keys) == 4
brep = e.body_brep(plate["key"])
assert "CASCADE Topology" in brep[:80]
r = e.import_brep(brep, name="Copy", by="py")
assert r["bodies"] == 1 and r["new_entries"] == 0  # same content -> same key, no new entry
out = e.export("stl", os.path.join(tmp, "x.stl"), select=[plate["id"]])
assert os.path.getsize(out["files"][0]) > 84
out = e.export("step", os.path.join(tmp, "x.step"))
assert os.path.getsize(out["files"][0]) > 1000
png = e.render(os.path.join(tmp, "x.png"), view="top", width=200, height=100)
assert png["width"] == 200 and open(png["out"], "rb").read(4) == b"\x89PNG"
e.delete(a["id"])
assert len(e.annotations()["annotations"]) == 0
e.save()

b = opad.browse(os.path.join(fixtures, "box.step"))
assert b.info()["bodies"] == 1 and b.path == ""
assert "CASCADE Topology" in b.node_brep(b.tree()["roots"][0]["id"])[:80]

diff = opad.diff(path, path)
assert diff["same_document"] and diff["ops"]["added"] == []
generic = opad.run("info", doc=path)
assert generic["ops"] == 5, generic
assert any(c["name"] == "render" for c in opad.commands())

# A linked file: never stored, read again by open (it is in the document's folder).
linked_dir = os.path.join(tmp, "linked")
os.makedirs(linked_dir)
shutil.copy(os.path.join(fixtures, "assembly.step"), linked_dir)
linked_path = os.path.join(linked_dir, "linked.opad")
linked = opad.Document.create()
linked.save_as(linked_path)
r = linked.run("import", file=os.path.join(linked_dir, "assembly.step"), link=True)
assert r["info"]["linked"] and r["new_entries"] == 0, r
linked.save()
assert "#body " not in open(linked_path, encoding="utf-8").read()
reopened = opad.open(linked_path).info()
assert reopened["bodies"] == 10 and reopened["unresolved"] == 0, reopened
try:
    d.append({"op": "explode"})
    raise SystemExit("expected OpadError")
except opad.OpadError as ex:
    assert "unknown op type" in str(ex)
print("python bindings ok")
