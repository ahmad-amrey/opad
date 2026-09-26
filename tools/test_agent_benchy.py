"""Acceptance (TODO 10 B18): an agent builds a 3DBenchy-style boat through the live bridge.

usage: python tools/test_agent_benchy.py APP CLI

It reads the agent guide (B2), draws every section with shapes (B4) on planes given by origin and normal (B5), chains
batch steps by reference (B1), reads frames and changes.bodies from the replies (B3), picks the roof's top edges by
rule (B7) and sketches on a face it was given without a token (B6). Checks: no rejected call, at most 8 write calls,
one valid solid with a tight box of 60 x 31 x 48 mm (+-0.01) and a volume within 1% of the committed baseline.
"""
import argparse
from pathlib import Path
import time
import uuid

from test_live_agent import Desktop

VOLUME = 21686.2  # mm3: the committed baseline (a deliberate model change updates it)


def section(x, half, bottom, top, keel, fillet):
    """A hull cross-section at x as a closed path in the plane normal to +x (sketch u = world y, v = world z)."""
    points = [[-half, top], [-half, bottom + keel], [-half + keel, bottom], [half - keel, bottom], [half, bottom + keel], [half, top]]
    return {"plane": {"origin": [x, 0, 0], "normal": [1, 0, 0]},
            "geometry": {"shapes": [{"kind": "path", "picks": points, "options": {"fillets": [0, fillet, fillet, fillet, fillet, 0]}}]}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    args = parser.parse_args()
    root = (Path("build/agent-benchy") / str(uuid.uuid4())).resolve()
    desktop = Desktop(args.app.resolve(), args.cli.resolve(), root)
    client = None
    writes, rejected = 0, []
    try:
        client = desktop.bind(args.cli.resolve(), "Benchy agent")
        started = time.monotonic()
        raw = client.raw

        def call(tool, **arguments):
            result = raw(tool, **arguments)
            if result.get("isError"):
                rejected.append((tool, result["structuredContent"]["error"]))
                raise AssertionError(f"{tool} rejected: {result['structuredContent']['error']}")
            return result["structuredContent"]

        def write(tool, **arguments):
            nonlocal writes
            writes += 1
            arguments.setdefault("expected_revision", call("live_state")["result"]["revision"])
            arguments.setdefault("request_id", str(uuid.uuid4()))
            return call(tool, **arguments)

        # B2: the conventions first.
        guide = client.request("resources/read", {"uri": "opad://guide/agent"})["contents"][0]["text"]
        assert "normal = -Y" in guide and "tangent_next" in guide
        # 1. The hull: three sections lofted (ruled, so the sections are its extremes), the deck well cut from the top,
        #    rails along the gunwales.
        hull = write("model_batch", steps=[
            {"id": "stern", "command": "sketch", "arguments": dict(name="Stern section", **section(0, 13, 2, 15, 3, 2))},
            {"id": "mid", "command": "sketch", "arguments": dict(name="Mid section", **section(30, 15.5, 0, 15, 4, 2.5))},
            {"id": "bow", "command": "sketch", "arguments": dict(name="Bow section", **section(60, 2, 7.5, 15, 1, 0.3))},
            {"id": "hull", "command": "feature", "arguments": {"kind": "loft", "name": "Hull", "color": [0.85, 0.2, 0.15], "inputs": {
                "profiles": [{"sketch": "@{stern#/sketch_id}"}, {"sketch": "@{mid#/sketch_id}"}, {"sketch": "@{bow#/sketch_id}"}], "ruled": True}}},
            {"id": "deck", "command": "sketch", "arguments": {"name": "Deck well", "plane": {"origin": [0, 0, 15], "normal": [0, 0, 1]}, "geometry": {"shapes": [
                {"kind": "path", "picks": [[3, -11], [30, -13.5], [44, -7], [44, 7], [30, 13.5], [3, 11]], "options": {"fillet": 2}}]}}},
            {"id": "well", "command": "feature", "arguments": {"kind": "extrude", "name": "Deck well", "inputs": {
                "profiles": [{"sketch": "@{deck#/sketch_id}", "at": [20, 0]}], "distance": 4, "flip": True, "operation": "cut", "targets": ["@{hull#/body_ids/0}"]}}},
            # The sheer trim: a 1 mm rim on the bulwarks, between the deck well's outline and its outward offset.
            {"id": "rim", "command": "sketch", "arguments": {"name": "Sheer trim", "plane": {"origin": [0, 0, 15], "normal": [0, 0, 1]}, "geometry": {"shapes": [
                {"kind": "path", "picks": [[3, -11], [30, -13.5], [44, -7], [44, 7], [30, 13.5], [3, 11]], "options": {"fillet": 2}},
                {"kind": "offset", "options": {"shape": 0, "distance": 1}}]}}},
            {"id": "trim", "command": "feature", "arguments": {"kind": "extrude", "name": "Sheer trim", "inputs": {
                "profiles": [{"sketch": "@{rim#/sketch_id}", "at": [30, 14]}], "distance": 0.8, "operation": "join", "targets": ["@{hull#/body_ids/0}"]}}}])
        steps = {s["id"]: s["result"] for s in hull["result"]["steps"]}
        body = steps["hull"]["body_ids"][0]
        assert steps["stern"]["frame"]["normal"] == [1, 0, 0] and steps["stern"]["frame"]["x"] == [0, 1, 0], steps["stern"]["frame"]  # B3, B5
        assert steps["hull"]["body_ids"] == [body] and hull["changes"]["bodies"][0]["valid"], hull["changes"]
        # 2. The cabin with windows through it, the roof (its top edges rounded by rule) and the chimney.
        write("model_batch", steps=[
            {"id": "cabin", "command": "feature", "arguments": {"kind": "box", "name": "Cabin", "inputs": {
                "plane": {"origin": [30, 0, 11], "normal": [0, 0, 1]}, "length": 20, "width": 18, "height": 19, "operation": "join", "targets": [body]}}},
            {"id": "glazing", "command": "sketch", "arguments": {"name": "Windows", "plane": {"origin": [30, 0, 20], "normal": [0, 1, 0]}, "geometry": {"shapes": [
                {"kind": "rounded_rect", "picks": [[-8, -6], [-1, -1]], "options": {"radius": 1}}, {"kind": "rounded_rect", "picks": [[1, -6], [8, -1]], "options": {"radius": 1}}]}}},
            {"id": "windows", "command": "feature", "arguments": {"kind": "extrude", "name": "Windows", "inputs": {
                "profiles": [{"sketch": "@{glazing#/sketch_id}"}], "direction": "symmetric", "extent": "all", "operation": "cut", "targets": [body]}}},
            {"id": "roof", "command": "feature", "arguments": {"kind": "box", "name": "Roof", "inputs": {
                "plane": {"origin": [30, 0, 30], "normal": [0, 0, 1]}, "length": 24, "width": 22, "height": 2, "operation": "join", "targets": [body]}}},
            {"id": "round", "command": "feature", "arguments": {"kind": "fillet", "name": "Roof fillet", "inputs": {
                "edges": [{"body": body, "kind": "edge", "select": {"curve": "line", "at_plane": {"axis": "z", "value": 32}}, "expect": 4}], "radius": 1}}},
            {"id": "chimney", "command": "feature", "arguments": {"kind": "cylinder", "name": "Chimney", "inputs": {
                "plane": {"origin": [26, 0, 32], "normal": [0, 0, 1]}, "diameter": 6, "height": 16, "operation": "join", "targets": [body]}}}])
        # 3. Deck details: the cargo box, the rod holder, the hawsepipe through the bow.
        write("model_batch", steps=[
            {"id": "cargo", "command": "feature", "arguments": {"kind": "box", "name": "Cargo box", "inputs": {
                "plane": {"origin": [9, 0, 11], "normal": [0, 0, 1]}, "length": 8, "width": 10, "height": 6, "operation": "join", "targets": [body]}}},
            {"id": "rod", "command": "feature", "arguments": {"kind": "cylinder", "name": "Rod holder", "inputs": {
                "plane": {"origin": [5, 8, 11], "normal": [0, 0, 1]}, "diameter": 2, "height": 8, "operation": "join", "targets": [body]}}},
            {"id": "hawse", "command": "feature", "arguments": {"kind": "cylinder", "name": "Hawsepipes", "inputs": {
                "plane": {"origin": [52, -20, 11], "normal": [0, 1, 0]}, "diameter": 2, "height": 40, "operation": "cut", "targets": [body]}}}])
        # 4. A hatch on the deck floor: the face comes from a query and is used bare, without a token (B6).
        floor = call("query_entities", body=body, kind="face", filters={"normal": "+z", "at_plane": {"axis": "z", "value": 11}}, limit=5)["result"]
        assert floor["status"] == "matched" and floor["items"], floor
        face = floor["items"][0]["reference"]["ref"]
        face_ref = face if isinstance(face, str) else f"{face['body']}/face/{face['index']}"
        write("model_batch", steps=[
            {"id": "hatch", "command": "sketch", "arguments": {"name": "Hatch", "plane": {"face": face_ref}, "geometry": {"shapes": [
                {"kind": "circle", "picks": [[0, 0]], "options": {"radius": 1.5}}]}}},
            {"id": "cut", "command": "feature", "arguments": {"kind": "extrude", "name": "Hatch", "inputs": {
                "profiles": [{"sketch": "@{hatch#/sketch_id}"}], "distance": 0.5, "flip": True, "operation": "cut", "targets": [body]}}}])
        # A picture for people: four labelled views with the model's edges (B9).
        picture = raw("viewport_image", views=["iso", "front", "top", "right"], edges=True, shading="smooth", width=960, height=720)
        png = next(item["data"] for item in picture["content"] if item["type"] == "image")
        (root / "benchy.png").write_bytes(__import__("base64").b64decode(png))
        # The boat: one valid solid of the intended size and volume.
        checked = call("validate", select=[body])["result"]
        item = checked["items"][0]
        size = item["bbox"]["size"]
        print(f"Benchy: {writes} write calls, {len(rejected)} rejected, solids {item['solids']}, size {size}, volume {item['volume_mm3']:.1f} mm3, "
              f"{time.monotonic() - started:.1f} s", flush=True)
        assert not rejected and writes <= 8
        assert item["valid"] and item["solids"] == 1 and call("context")["result"]["bodies"] == 1
        assert all(abs(a - b) <= 0.01 for a, b in zip(size, [60, 31, 48])), size
        assert abs(item["volume_mm3"] - VOLUME) <= 0.01 * VOLUME, (item["volume_mm3"], VOLUME)
        print("Benchy through the live bridge: guide, shapes, planes, batch references, frames, rule fillet, bare face ref: PASS", flush=True)
    finally:
        if client:
            client.close()
        desktop.close()


if __name__ == "__main__":
    main()
