"""Persistent stdio MCP client for an interactive agent-driven desktop design session.

No design is scripted here. The agent writes request.json, examines result.json and
returned PNGs, then decides the next CAD tool call. Every exchange is recorded.
"""
import argparse
import base64
import json
from pathlib import Path
import time
import uuid
from test_live_agent import Client, Desktop


def exchange(root, request, timeout=90):
    root = Path(root)
    output = root / "result.json"
    output.unlink(missing_ok=True)
    temporary = root / "request.tmp"
    temporary.write_text(json.dumps(request), encoding="utf-8")
    temporary.replace(root / "request.json")
    start = time.monotonic()
    while time.monotonic()-start < timeout:
        if output.exists():
            return json.loads(output.read_text(encoding="utf-8"))
        time.sleep(.05)
    raise TimeoutError("Agent exchange timed out")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--document", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    desktop = Desktop(args.app.resolve(), args.cli.resolve(), root / "desktop", document=args.document.resolve() if args.document else None)
    client = desktop.bind(args.cli.resolve(), name="Codex interactive design")
    (root / "ready.json").write_text(json.dumps(client.state()), encoding="utf-8")
    index = 0
    try:
        while True:
            request_file = root / "request.json"
            if not request_file.exists():
                time.sleep(.05)
                continue
            request = json.loads(request_file.read_text(encoding="utf-8"))
            request_file.unlink()
            index += 1
            tool = request["tool"]
            if tool == "__close":
                break
            try:
                if tool == "__desktop":
                    result = desktop.action(**request["arguments"])
                elif tool == "__tools":
                    result = client.request("tools/list")
                else:
                    arguments = request.get("arguments", {})
                    if request.get("write"):
                        arguments.setdefault("expected_revision", client.state()["revision"])
                        arguments.setdefault("request_id", str(uuid.uuid4()))
                    result = client.raw(tool, **arguments)
                    for item in result.get("content", []):
                        if item.get("type") == "image":
                            path = root / f"image-{index}.png"
                            path.write_bytes(base64.b64decode(item.pop("data")))
                            item["path"] = str(path)
            except Exception as error:
                result = {"client_error": str(error)}
            with (root / "transcript.jsonl").open("a", encoding="utf-8") as log:
                log.write(json.dumps({"request": request, "result": result}) + "\n")
            temporary = root / "result.tmp"
            temporary.write_text(json.dumps(result), encoding="utf-8")
            temporary.replace(root / "result.json")
    finally:
        client.close()
        desktop.close()


if __name__ == "__main__":
    main()
