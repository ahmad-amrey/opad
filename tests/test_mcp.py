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
        assert request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                      "clientInfo": {"name": "cad-test", "version": "1"}})["protocolVersion"] == "2025-11-25"
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        process.stdin.flush()
        names = {t["name"] for t in request("tools/list")["tools"]}
        assert {"new", "feature", "feature_kinds", "inspect", "export"} <= names
        doc = str(pathlib.Path(folder) / "drilled-plate.opad")
        call("new", doc=doc)
        call("feature", doc=doc, kind="box", name="Plate", by="MCP test",
             inputs={"length": "60 mm", "width": "40 mm", "height": "10 mm"})
        call("feature", doc=doc, kind="cylinder", name="Through hole", by="MCP test",
             inputs={"diameter": "10 mm", "height": "10 mm", "operation": "cut"})
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
        output = str(pathlib.Path(folder) / "plate.step")
        call("export", doc=doc, format="step", out=output)
        assert pathlib.Path(output).stat().st_size > 100
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
