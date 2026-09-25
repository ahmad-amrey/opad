"""Exercise live MCP save/Save As through the real STDIO bridge, without UI saving."""
import argparse
import json
from pathlib import Path
import subprocess
import time
import uuid

from test_live_agent import Desktop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/live-save-test"))
    args = parser.parse_args()
    app, cli = args.app.resolve(), args.cli.resolve()
    root = args.output.resolve() / str(uuid.uuid4())
    desktop = Desktop(app, cli, root)
    client = None
    try:
        desktop.action("new")
        client = desktop.bind(cli)
        tools = {tool["name"]: tool for tool in client.request("tools/list")["tools"]}
        assert "save" in tools and not tools["save"]["annotations"]["readOnlyHint"]
        client.write("feature", kind="box", inputs={"length": 10, "width": 20, "height": 30})

        def rejected(code=None, **arguments):
            arguments.setdefault("expected_revision", client.state()["revision"])
            arguments.setdefault("request_id", str(uuid.uuid4()))
            result = client.raw("save", **arguments)
            assert result.get("isError"), result
            if code:
                assert result["structuredContent"]["error"]["code"] == code, result
            return result

        def validate(path, volume=6000):
            # Validate the exact saved bytes using a plain path for CLI runtimes
            # whose argv handling predates Unicode path support.
            copy = root / "disk-check.opad"
            copy.write_bytes(path.read_bytes())
            data = json.loads(subprocess.run([str(cli), "validate", str(copy)], capture_output=True,
                                             text=True, encoding="utf-8", check=True).stdout)
            assert data["valid_page"] and abs(data["items"][0]["volume_mm3"]-volume) < 1e-6, data

        rejected("save_failed")  # no current path
        rejected("save_failed", path="relative.opad")
        rejected("save_failed", path=str(root / "wrong.step"))
        rejected("stale_revision", expected_revision=0, path=str(root / "stale.opad"))
        desktop.action("access", edit=False)
        rejected("read_only", path=str(root / "readonly.opad"))
        desktop.action("access", edit=True)
        tx = client.write("transaction_begin", label="Uncommitted")["transaction"]
        rejected("prepared_active", path=str(root / "transaction.opad"))
        client.call("transaction_cancel", id=tx)
        preview = client.write("feature", kind="box", inputs={"length": 1, "width": 1, "height": 1}, preview=True)
        rejected("prepared_active", path=str(root / "preview.opad"))
        client.call("preview_cancel", id=preview["preview_id"])
        assert client.state()["dirty"]
        target = root / "first part.opad"
        revision = client.state()["revision"]
        request = dict(path=str(target), expected_revision=revision, request_id="first-save")
        saved = client.call("save", **request)
        assert saved["result"] == dict(path=str(target), saved_revision=revision, dirty=False), saved
        assert client.state()["revision"] == revision and not client.state()["dirty"]
        validate(target)
        original = target.read_bytes()
        desktop.action("undo")
        assert client.call("context")["result"]["bodies"] == 0, "Save added an Undo step"
        assert client.state()["dirty"]
        desktop.action("redo")
        assert not client.state()["dirty"], "Redo did not return to the saved baseline"
        client.write("param", name="marker", expr="7 mm")
        assert client.state()["dirty"]
        assert client.call("save", **request) == saved
        assert target.read_bytes() == original and client.state()["dirty"], "Retry saved a newer revision"
        again = client.write("save")
        assert again["result"]["path"] == str(target) and not client.state()["dirty"]
        assert target.read_bytes() != original
        validate(target)
        print("First save, subsequent save, retry receipts, dirty state and Undo/Redo: PASS", flush=True)

        collision = root / "existing.opad"
        collision.write_text("unrelated existing file", encoding="utf-8")
        rejected("save_failed", path=str(collision))
        assert collision.read_text(encoding="utf-8") == "unrelated existing file"
        assert client.state()["path"] == str(target)
        client.write("save", path=str(collision), overwrite=True)
        validate(collision)
        unicode_path = root / "part-\u0642\u0637\u0639\u0629.opad"
        client.write("save", path=str(unicode_path))
        assert client.state()["path"] == str(unicode_path)
        validate(unicode_path)
        client.write("param", name="marker", expr="8 mm")
        rejected("save_failed", path=str(root / "missing-directory" / "part.opad"))
        assert client.state()["dirty"] and client.state()["path"] == str(unicode_path)
        print("Explicit overwrite, Unicode Save As and failed-save state preservation: PASS", flush=True)

        for interruption in ("stop", "revoke", "disconnect"):
            destination = root / f"cancelled-{interruption}.opad"
            if interruption == "stop":
                destination.write_bytes(b"preserve this existing destination")
            desktop.action("delay", ms=900)
            request_id = f"cancel-save-{interruption}"
            client.send("tools/call", {"name": "save", "arguments": dict(path=str(destination),
                        overwrite=interruption == "stop", expected_revision=client.state()["revision"], request_id=request_id)})
            time.sleep(.2)
            tick = time.monotonic()
            if interruption == "stop":
                desktop.action("stop")
            elif interruption == "revoke":
                desktop.action("access", edit=False)
            else:
                client.process.kill()  # Only this test-owned transport; close during in-flight I/O.
                client.process.wait()
                client.close()
            assert time.monotonic()-tick < .6, "Save stalled the UI event loop"
            if interruption != "disconnect":
                assert client.receive().get("isError")
            time.sleep(1)
            desktop.action("delay", ms=0)
            if interruption == "revoke":
                desktop.action("access", edit=True)
            if interruption == "disconnect":
                client = desktop.bind(cli)
            if interruption == "stop":
                assert destination.read_bytes() == b"preserve this existing destination", "Cancelled save replaced its destination"
            else:
                assert not destination.exists(), "Cancelled save published its temporary file"
            assert client.raw("request_status", request_id=request_id).get("isError")
            assert client.state()["dirty"]
        # A completed save remains queryable after reconnect and leaves the binding intact.
        completed = client.write("save", request_id="durable-save")
        client.close()
        client = desktop.bind(cli)
        assert client.call("request_status", request_id="durable-save") == completed
        assert not client.state()["dirty"]
        validate(unicode_path)
        print("Cancellation, access revocation, disconnect and durable save receipts: PASS", flush=True)
        body = client.call("validate")["result"]["items"][0]["id"]
        face = client.call("entity_details", ref=f"{body}/face/0")["result"]["reference"]
        sketch = client.write("sketch", name="Save editor guard", plane={"face": face["ref"]}, geometry={}, references=[face])["result"]["sketch_id"]
        desktop.action("edit_sketch", id=sketch)
        time.sleep(.4)
        rejected("edit_session_busy")
        desktop.action("cancel_edit")
        client.write("save")
        print("Active-editor refusal and subsequent save: PASS", flush=True)
    finally:
        if client:
            client.close()
        desktop.close()


if __name__ == "__main__":
    main()
