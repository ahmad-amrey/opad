"""TODO 9 mouse/UI acceptance and live MCP annotation workflow; requires OpenGL."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from test_live_agent import Desktop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/todo9"))
    args = parser.parse_args()
    app, cli, output = args.app.resolve(), args.cli.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="opad-notes-") as directory:
        root = Path(directory)
        empty = root / "empty.opad"
        subprocess.run([str(cli), "new", str(empty)], capture_output=True, check=True)
        trace = output / "ui.log"
        trace.write_text("", encoding="utf-8")
        env = {key: value for key, value in os.environ.items() if not key.startswith("OPAD_BENCH_")}
        env.update(OPAD_LANG="en", OPAD_BENCH_SETTINGS=str(root / "settings"),
                   OPAD_BENCH_NOTES=str(output / "notes"), OPAD_TRACE=str(trace))
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        result = subprocess.run([str(app), str(empty), "--bench-select"], env=env, startupinfo=startup,
                                capture_output=True, timeout=90)
        log = trace.read_text(encoding="utf-8", errors="replace")
        assert result.returncode == 0 and "TODO9" in log and "PASS" in log, (result.returncode, log[-6000:])
        print("Mouse drawing, card dragging, filters, delete, undo/redo, cancel, save/reopen: PASS", flush=True)

        desktop = Desktop(app, cli, root / "live", document=output / "notes.opad")
        client = None
        try:
            client = desktop.bind(cli)
            tools = {item["name"]: item for item in client.request("tools/list")["tools"]}
            assert "drawing" in tools["annotate"]["inputSchema"]["properties"]
            summary = client.call("context")["result"]
            assert summary["ai_agent_notes"] == 1, summary
            note = client.call("context", section="ai_agent_notes")["result"]["items"][0]
            detail = client.call("annotations", id=note["id"])["result"]["annotations"][0]
            assert detail["style"] == "ai_agent" and len(detail["drawing"]["strokes"]) == 2
            assert "/face/" in detail["anchor"]
            desktop.action("annotation_editor")
            assert client.state()["editing"]["edit_session"] == "annotation"
            rejected = client.raw("delete_annotation", target=note["id"], expected_revision=client.state()["revision"], request_id="drawing-busy")
            assert rejected["isError"] and rejected["structuredContent"]["error"]["code"] == "edit_session_busy"
            desktop.action("annotation_editor")
            reference = client.call("entity_details", ref=detail["anchor"])["result"]["reference"]
            client.write("annotate", anchor=detail["anchor"], references=[reference], reply_to=note["id"], text="Agent reviewed the marked face")
            assert len(client.call("annotations", id=note["id"])["result"]["annotations"][0]["comments"]) == 1
            client.write("delete_annotation", target=note["id"])
            assert client.call("context")["result"]["ai_agent_notes"] == 0
            desktop.action("undo")
            assert client.call("context")["result"]["ai_agent_notes"] == 1
            print("Live MCP discovery, anchored drawing context, active-editor protection, comments, deletion and Undo: PASS", flush=True)
        finally:
            if client:
                client.close()
            desktop.close()


if __name__ == "__main__":
    main()
