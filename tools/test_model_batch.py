"""Bounded typed batches through live MCP, including rollback and cancellation."""
import argparse
from pathlib import Path
import uuid
from test_live_agent import Desktop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    args = parser.parse_args()
    desktop = Desktop(args.app.resolve(), args.cli.resolve(), (Path("build/batch-regression") / str(uuid.uuid4())).resolve())
    client = desktop.bind(args.cli.resolve())
    observer = desktop.bind(args.cli.resolve(), "Batch observer")
    try:
        base = client.state()["revision"]
        tx = client.call("transaction_begin", label="Batch part", expected_revision=base, request_id="begin")["transaction"]
        steps = [
            {"id": "group", "command": "component", "arguments": {"name": "Part"}},
            {"id": "solid", "command": "feature", "arguments": {"kind": "box", "inputs": {"length": 10, "width": 8, "height": 3}}},
            {"id": "name", "command": "rename", "arguments": {"target": "@{solid#/body_ids/0}", "name": "Battery"}},
            {"id": "parent", "command": "reparent", "arguments": {"target": "@{solid#/body_ids/0}", "parent": "@{group#/component_id}"}},
            {"id": "color", "command": "appearance", "arguments": {"target": "@{solid#/body_ids/0}", "color": [.1, .4, .8]}},
        ]
        staged = client.call("model_batch", steps=steps, transaction=tx, expected_revision=base, request_id="part")
        body = staged["result"]["steps"][1]["result"]["body_ids"][0]
        assert staged["state"] == "staged" and staged["revision"] == base
        assert staged["result"]["persistence"] == "not_saved" and len(staged["result"]["steps"]) == 5
        assert client.call("validate", transaction=tx, select=[body])["result"]["valid_page"]
        assert client.call("context")["result"]["bodies"] == 0
        # Invalid later schemas are rejected before any step runs.
        invalid = client.raw("model_batch", steps=steps + [{"id": "bad", "command": "feature", "arguments": {"kind": "box", "inputs": {"heigth": 3}}}], transaction=tx, expected_revision=base, request_id="preflight")
        assert invalid["isError"]
        # A geometric failure discards this batch but keeps the preceding staged checkpoint.
        failed = client.raw("model_batch", steps=[steps[0], {"id": "bad", "command": "feature", "arguments": {"kind": "box", "inputs": {"height": "missing_dimension"}}}], transaction=tx, expected_revision=base, request_id="runtime")
        assert failed["isError"] and failed["structuredContent"]["result"]["failed_step"] == "bad", failed
        assert failed["structuredContent"]["result"]["steps"][0]["state"] == "discarded"
        assert client.call("context", transaction=tx)["result"]["components"] == 1
        assert client.call("validate", transaction=tx, select=[body])["result"]["valid_page"]
        forward = client.raw("model_batch", steps=[steps[2], steps[1]], transaction=tx, expected_revision=base, request_id="forward")
        assert forward["isError"]
        committed = client.call("transaction_commit", id=tx, expected_revision=base, request_id="commit")
        assert committed["revision"] > base and committed["elapsed_ms"] >= 0
        saved = client.call("save", expected_revision=committed["revision"], request_id="save")
        assert saved["elapsed_ms"] >= 0 and not saved["result"]["dirty"]
        assert client.call("request_status", request_id="part")["state"] == "committed"
        desktop.action("undo")
        assert client.call("context")["result"]["bodies"] == 0
        desktop.action("redo")
        assert client.call("context")["result"]["bodies"] == 1
        revision = client.state()["revision"]
        desktop.action("delay", ms=3000)
        client.send("tools/call", {"name": "model_batch", "arguments": {"steps": steps, "expected_revision": revision, "request_id": "cancel"}})
        # Separate connection observes work and cancels without needing to wait for the owner.
        import time
        for _ in range(100):
            if observer.call("live_diagnostics")["busy"]:
                break
            time.sleep(.01)
        busy = observer.raw("model_batch", steps=steps, expected_revision=revision, request_id="competing")["structuredContent"]["error"]
        assert busy["code"] in ("busy", "edit_session_busy") and busy["retryable"] and busy["retry_after_ms"] == 50
        assert busy["editing"]["active_operation_id"] == "cancel" and busy["editing"]["owner"]["client_id"] != busy["client_id"]
        assert not observer.call("wait_for_idle", timeout_ms=10)["result"]["idle"]
        observer.call("stop")
        assert client.receive()["isError"]
        time.sleep(3.2)
        assert observer.call("context")["result"]["bodies"] == 1
        print("Typed batch references, preflight, rollback, checkpoint save, Undo and cancellation: PASS", flush=True)
    finally:
        client.close()
        observer.close()
        desktop.close()


if __name__ == "__main__":
    main()
