"""Real stdio client: discover tools, model a drilled plate, inspect it and export STEP."""
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="opad-mcp-") as folder:
    stderr = tempfile.TemporaryFile(mode="w+")
    process = subprocess.Popen([sys.argv[1], "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=stderr, text=True, encoding="utf-8")
    serial = 0

    def request(method, params=None):
        global serial
        serial += 1
        process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": serial, "method": method, "params": params or {}}) + "\n")
        process.stdin.flush()
        response = json.loads(process.stdout.readline())
        assert response["id"] == serial and "error" not in response, response
        return response["result"]

    def call(tool, **args):
        result = request("tools/call", {"name": tool, "arguments": args})
        assert not result.get("isError"), result
        return json.loads(result["content"][0]["text"])

    try:
        start = time.monotonic()
        init = request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                      "clientInfo": {"name": "cad-test", "version": "1"}})
        assert init["protocolVersion"] == "2025-11-25" and "resources" in init["capabilities"]
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        process.stdin.flush()
        tools = {t["name"]: t for t in request("tools/list")["tools"]}
        names = set(tools)
        # The agent guide ships inside the binary (TODO 10 B2) and the descriptions point at it, not at docs/.
        resources = request("resources/list")["resources"]
        assert [r["uri"] for r in resources] == ["opad://guide/agent"] and resources[0]["mimeType"] == "text/markdown", resources
        guide = request("resources/read", {"uri": "opad://guide/agent"})["contents"][0]["text"]
        for fact in ("counter-clockwise", "normal = -Y", "one id space", "all_body_ids", "@{cabin#/body_ids/0}"):
            assert fact in guide, fact
        assert "docs/design.md" not in json.dumps(tools) and "opad://guide/agent" in tools["sketch"]["description"]
        assert {"new", "feature", "feature_kinds", "inspect", "export"} <= names
        assert {"context", "entity_details", "sketch_details", "resolve_reference", "validate", "feature_schema"} <= names
        assert tools["feature"]["inputSchema"]["properties"]["kind"]["enum"]
        assert "doc" in tools["feature"]["inputSchema"]["required"]
        assert not tools["feature"]["inputSchema"]["additionalProperties"]
        assert call("feature_schema", kind="extrude")["inputSchema"]["properties"]["profiles"]["items"]
        bad = request("tools/call", {"name": "feature", "arguments": {"doc": "missing.opad", "kind": "box", "typo": 1}})
        assert bad["isError"] and "unknown field typo" in bad["structuredContent"]["error"]["message"]
        doc = str(pathlib.Path(folder) / "drilled-plate.opad")
        call("new", doc=doc)
        call("param", doc=doc, name="thickness", expr="10 mm")
        call("feature", doc=doc, kind="box", name="Plate", by="MCP test",
             inputs={"length": "60 mm", "width": "40 mm", "height": "thickness"})
        call("feature", doc=doc, kind="cylinder", name="Through hole", by="MCP test",
             inputs={"diameter": "10 mm", "height": "thickness", "operation": "cut"})
        tree = call("tree", doc=doc)

        def bodies(nodes):
            for node in nodes:
                if node["type"] == "body":
                    yield node["id"]
                yield from bodies(node.get("children", []))

        ids = list(bodies(tree["roots"]))
        assert len(ids) == 1, tree
        props = call("properties", doc=doc, node=ids[0])
        assert abs(props["volume"] - (24000 - math.pi * 250)) < 1e-5, props
        assert call("context", doc=doc)["bodies"] == 1
        assert "ai_agent" in tools["annotate"]["inputSchema"]["properties"]["style"]["enum"]
        drawing = {"plane": {"origin": [0, 0, 15], "x": [1, 0, 0], "y": [0, 1, 0]},
                   "strokes": [{"color": "blue", "width": 4, "points": [[0, 0], [5, 2], [10, 0]]}]}
        note = call("annotate", doc=doc, anchor=ids[0] + "/face/0", text="Review this marked area", style="ai_agent", drawing=drawing)["id"]
        call("annotate", doc=doc, anchor=ids[0], text="Check before changing", reply_to=note)
        assert call("context", doc=doc)["ai_agent_notes"] == 1
        assert call("context", doc=doc, section="ai_agent_notes", limit=1)["items"][0]["id"] == note
        assert call("annotations", doc=doc, id=note, style="ai_agent")["annotations"][0]["drawing"] == drawing
        bad_drawing = dict(drawing, strokes=[{"color": "purple", "width": 4, "points": [[0, 0], [1, 1]]}])
        failed = request("tools/call", {"name": "annotate", "arguments": {"doc": doc, "anchor": ids[0], "text": "bad", "drawing": bad_drawing}})
        assert failed["isError"]
        call("delete_annotation", doc=doc, target=note)
        assert call("context", doc=doc)["ai_agent_notes"] == 0
        validation = call("validate", doc=doc)
        assert validation["valid_page"] and validation["items"][0]["solids"] == 1
        assert len(call("context", doc=doc, section="features", limit=1)["items"]) == 1
        call("param", doc=doc, name="thickness", expr="15 mm")
        props = call("properties", doc=doc, node=ids[0])
        assert abs(props["volume"] - (36000 - math.pi * 375)) < 1e-5, props
        output = str(pathlib.Path(folder) / "plate.step")
        call("export", doc=doc, format="step", out=output)
        assert pathlib.Path(output).stat().st_size > 100
        # TODO 11 UI-35: an exploded view through the same tools (one body: one unit, moved by hand), saved as a view.
        assert tools["explode"]["inputSchema"]["properties"]["groups"]["items"]["type"] == "array"
        exploded = call("explode", doc=doc, levels=0, mode="stack", axis=[0, 0, 1], groups=[[ids[0]]], offsets={ids[0]: [0, 0, 5]}, name="Exploded")
        assert exploded["units"][0]["id"] == ids[0] and exploded["offsets"][ids[0]] == [0, 0, 5], exploded
        assert call("annotations", doc=doc)["views"][-1]["explode"]["offsets"] == {ids[0]: [0, 0, 5]}
        # TODO 10 B4: a phone frame outline (a rounded rectangle and its inner offset) and a text label, as shapes.
        framed = call("sketch", doc=doc, name="Frame", plane={"base": "xy"}, geometry={"shapes": [
            {"kind": "rounded_rect", "picks": [[100, 0], [170, 150]], "options": {"radius": 8}},
            {"kind": "offset", "options": {"shape": 0, "distance": -2}},
            {"kind": "text", "picks": [[135, 60]], "options": {"text": "Sub", "height": 8, "align": "center"}}]})
        shapes = framed["id_map"]
        assert [s["kind"] for s in shapes] == ["rounded_rect", "offset", "text"] and len(shapes[2]["profiles"]) == 3, shapes
        profiles = call("sketch_details", doc=doc, sketch=framed["sketch_id"], section="profiles", limit=100)
        assert profiles["total"] == 2 + 3 + 2, profiles  # the frame, the inside, three letters (capitals: SUB) and B's two holes
        wrong = request("tools/call", {"name": "sketch", "arguments": {"doc": doc, "geometry": {"shapes": [{"kind": "rounded_rect", "picks": [[0, 0], [10, 10]], "options": {"radius": 6}}]}}})
        assert wrong["isError"] and "shapes[0] (rounded_rect)" in wrong["structuredContent"]["error"]["message"], wrong
        error = request("tools/call", {"name": "feature", "arguments": {"doc": doc, "kind": "invalid"}})
        assert error["isError"]
        assert request("ping") == {}
        print(f"MCP drilled plate: volume verified, STEP exported, errors recoverable; {time.monotonic()-start:.2f}s")
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        stderr.close()
