"""Feedback regressions through real STDIO/desktop, with isolated Codex registration.

Usage: python tools/test_mcp_feedback.py APP CLI --codex PATH
The real user's Codex configuration and OPAD windows are never modified.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid
from test_live_agent import Client, Desktop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--codex", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/mcp-feedback-test"))
    args = parser.parse_args()
    cli, codex = args.cli.resolve(), args.codex.resolve()
    root = args.output.resolve() / str(uuid.uuid4())
    home = root / "codex-home"
    home.mkdir(parents=True)
    env = dict(os.environ, CODEX_HOME=str(home))
    desktop = Desktop(args.app.resolve(), cli, root / "desktop", environment={"CODEX_HOME": str(home)})
    client = Client(cli, desktop.discovery)
    try:
        # A call before live_bind says what to call instead of reporting a dropped connection.
        unbound = client.raw("context")["structuredContent"]["error"]
        assert unbound["code"] == "not_bound" and unbound["next"] == ["live_instances", "live_bind"], unbound
        diagnostic = client.call("live_diagnostics", include_example=True)
        assert diagnostic["connection"] == "unbound" and diagnostic["target"] is None
        assert diagnostic["guide"]["transactions"]["scope"] == "connection"
        guide = json.dumps(diagnostic["guide"])
        assert "@{cabin#/body_ids/0}" in guide and "without /result/" in guide
        tools = {t["name"]: t for t in client.request("tools/list")["tools"]}
        assert all(t["outputSchema"]["type"] == "object" for t in tools.values())
        assert "feature_id" in tools["feature"]["outputSchema"]["properties"]["result"]["required"]
        assert "@{cabin#/body_ids/0}" in tools["model_batch"]["description"] and "/result/" not in tools["model_batch"]["description"].replace("without /result/", "")
        for name in ("sketch", "sketch_edit"):
            assert "one id space" in tools[name]["inputSchema"]["properties"]["geometry"]["description"]
        # TODO 10 B2: the agent guide, before binding, as a resource and through live_diagnostics.
        listed = client.request("resources/list")["resources"]
        assert [r["uri"] for r in listed] == ["opad://guide/agent"], listed
        text = client.request("resources/read", {"uri": "opad://guide/agent"})["contents"][0]["text"]
        for fact in ("counter-clockwise", "normal = -Y", "one id space", "all_body_ids", "@{cabin#/body_ids/0}"):
            assert fact in text, fact
        client.send("resources/read", {"uri": "opad://nothing"})
        missing = client.responses.get(timeout=90)
        assert missing["error"]["code"] == -32002 and "opad://guide/agent" in missing["error"]["message"], missing
        assert client.call("live_diagnostics", include_guide=True)["agent_guide"] == text
        assert "docs/design.md" not in json.dumps(tools) and "opad://guide/agent" in tools["live_diagnostics"]["description"]
        chosen = client.call("live_instances")["instances"][0]
        client.call("live_bind", instance=chosen["instance"], target=chosen["target"])
        assert client.call("live_diagnostics", include_guide=True)["agent_guide"] == text
        diagnostic = client.call("live_diagnostics")
        assert diagnostic["connection"] == "bound" and diagnostic["units"] == "mm"
        assert diagnostic["permissions"]["edit"] and "guide" not in diagnostic
        base = diagnostic["revision"]
        begin = client.call("transaction_begin", expected_revision=base, request_id="begin", label="Feedback cube")
        assert begin["base_revision"] == base and begin["lifetime"]["scope"] == "connection"
        tx = begin["transaction"]
        staged = dict(transaction=tx, expected_revision=base)
        box = client.call("feature", kind="box", inputs={"length": 40, "width": 40, "height": 40}, request_id="cube", **staged)
        feature, body = box["result"]["feature_id"], box["result"]["body_ids"][0]
        # TODO 10 B3: the frame the plane resolved to, and what this command did to each body it changed.
        assert box["result"]["frame"]["normal"] == [0, 0, 1] and box["result"]["frame"]["x"] == [1, 0, 0], box["result"]
        changed = box["changes"]["bodies"]
        assert len(changed) == 1 and changed[0]["id"] == body and changed[0]["valid"], changed
        assert all(abs(size - 40) < 1e-9 for size in changed[0]["bbox"]["size"]) and abs(changed[0]["volume_mm3"] - 64000) < 1e-6, changed
        assert feature != body and body in box["changes"]["created"]
        assert box["changes"]["scope"] == "transaction" and box["revision"] == base
        sketch = client.call("sketch", name="Hole centers", geometry={}, request_id="sketch", **staged)
        sk = sketch["result"]["sketch_id"]
        assert body in sketch["changes"]["created"] and sk in sketch["changes"]["created"]
        # Typed body ID directly feeds the next operation; no guessing from changes.
        renamed = client.call("rename", target=body, name="Typed body", request_id="rename", **staged)
        assert renamed["revision"] == base
        assert client.call("live_diagnostics")["transaction_state"]["id"] == tx
        for n, x in enumerate((-10, 10)):
            client.call("feature", kind="cylinder", inputs={"diameter": 8, "height": 40,
                "operation": "cut", "targets": [body], "x": x}, request_id=f"hole-{n}", **staged)
        validated = client.call("validate", transaction=tx)["result"]
        expected = 40**3 - 2*math.pi*4**2*40
        assert validated["valid_page"] and validated["items"][0]["solids"] == 1
        assert abs(validated["items"][0]["volume_mm3"] - expected) < 1e-5
        committed = client.call("transaction_commit", id=tx, expected_revision=base, request_id="commit")
        assert committed["revision"] > base and committed["changes"]["scope"] == "transaction"
        assert client.call("request_status", request_id="cube")["result"]["body_ids"] == [body]
        desktop.action("undo")
        assert client.call("context")["result"]["bodies"] == 0
        desktop.action("redo")
        assert client.call("context")["result"]["bodies"] == 1
        # The wrong forms that were rejected in the Benchy build now say how to fix them.
        revision = client.state()["revision"]
        cabin = {"id": "cabin", "command": "feature", "arguments": {"kind": "box", "inputs": {"length": 20, "width": 12, "height": 10}}}
        def label(target):
            return [cabin, {"id": "label", "command": "rename", "arguments": {"target": target, "name": "Cabin"}}]
        wrong = client.raw("model_batch", steps=label("@{cabin#/result/body_ids/0}"), expected_revision=revision, request_id="wrong-path")
        message = wrong["structuredContent"]["error"]["message"]
        assert wrong["isError"] and "step 'cabin' result has no 'result'" in message, wrong
        assert "body_ids" in message and "feature_id" in message and "@{cabin#/body_ids/0}" in message, message
        unknown = client.raw("model_batch", steps=label("@{cabn#/body_ids/0}"), expected_revision=revision, request_id="wrong-step")
        assert "earlier steps are cabin" in unknown["structuredContent"]["error"]["message"], unknown
        index = client.raw("model_batch", steps=label("@{cabin#/body_ids/3}"), expected_revision=revision, request_id="wrong-index")
        assert "list of 1 item" in index["structuredContent"]["error"]["message"], index
        slash = client.call("model_batch", steps=label("@{cabin/body_ids/0}"), expected_revision=revision, request_id="slash-form")
        assert slash["state"] == "committed" and [s["state"] for s in slash["result"]["steps"]] == ["computed", "computed"], slash
        duplicate = client.raw("sketch", name="Duplicate ids", geometry={"points": [{"id": 1, "x": 0, "y": 0}, {"id": 2, "x": 5, "y": 0}],
            "entities": [{"id": 1, "type": "line", "p": [1, 2]}]}, expected_revision=client.state()["revision"], request_id="duplicate-ids")
        message = duplicate["structuredContent"]["error"]["message"]
        assert duplicate["isError"] and "point 1 and entity 1" in message and "share one id space" in message, duplicate
        desktop.action("undo")
        print("Batch reference forms, path errors naming the step's keys, sketch id collisions and not_bound: PASS", flush=True)

        abandoned = client.write("transaction_begin", label="Disconnect test", request_id="abandon")
        client.write("param", name="abandoned", expr="1 mm", transaction=abandoned["transaction"])
        client.close()
        client = desktop.bind(cli)
        assert client.call("live_diagnostics")["transaction_state"]["state"] == "none"
        failure = client.raw("context", transaction=abandoned["transaction"])
        assert failure["structuredContent"]["error"]["code"] == "unknown_transaction"
        assert "Disconnect discards" in failure["structuredContent"]["error"]["message"]
        assert client.raw("request_status", request_id="abandon")["structuredContent"]["state"] == "cancelled"
        desktop.action("access", edit=False)
        assert not client.call("live_diagnostics")["permissions"]["edit"]
        desktop.action("access", edit=True)
        print("Typed IDs, cumulative changes, persistent transaction, disconnect cleanup and diagnostics: PASS", flush=True)

        # A live-process lock keeps deliberately unavailable test endpoints discoverable.
        fake = root / "fake-discovery"
        fake.mkdir()
        lock = next(desktop.discovery.glob("*.json.lock"))
        shutil.copyfile(lock, fake / "test.json.lock")
        descriptor = dict(chosen, endpoint="opad-missing-" + str(uuid.uuid4()))
        file = fake / "test.json"
        file.write_text(json.dumps(descriptor), encoding="utf-8")
        probe = Client(cli, fake)
        try:
            result = probe.raw("live_bind", instance=chosen["instance"], target=chosen["target"])
            error = result["structuredContent"]["error"]
            assert error["code"] == "endpoint_unavailable", result
            assert error["transport"]["message"] and isinstance(error["transport"]["qt_error"], int)
            assert probe.call("live_diagnostics")["last_error"] == error
            descriptor["enabled"] = False
            file.write_text(json.dumps(descriptor), encoding="utf-8")
            assert probe.raw("live_bind", instance=chosen["instance"], target=chosen["target"])["structuredContent"]["error"]["code"] == "access_disabled"
            assert probe.raw("live_bind", instance=chosen["instance"], target="old-target")["structuredContent"]["error"]["code"] == "target_changed"
            if os.name == "nt":
                # Exercise real Windows access denial, not a guessed sandbox diagnosis.
                import ctypes
                from ctypes import wintypes
                class SecurityAttributes(ctypes.Structure):
                    _fields_ = [("length", wintypes.DWORD), ("descriptor", ctypes.c_void_p), ("inherit", wintypes.BOOL)]
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                advapi = ctypes.WinDLL("advapi32", use_last_error=True)
                advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
                kernel.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(SecurityAttributes)]
                kernel.CreateNamedPipeW.restype = wintypes.HANDLE
                kernel.CloseHandle.argtypes = [wintypes.HANDLE]
                kernel.LocalFree.argtypes = [ctypes.c_void_p]
                sd = ctypes.c_void_p()
                assert advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW("D:(D;;GA;;;WD)", 1, ctypes.byref(sd), None)
                security = SecurityAttributes(ctypes.sizeof(SecurityAttributes), sd, False)
                endpoint = "opad-denied-" + str(uuid.uuid4())
                pipe = kernel.CreateNamedPipeW("\\\\.\\pipe\\"+endpoint, 3, 0, 1, 8192, 8192, 0, ctypes.byref(security))
                try:
                    assert pipe != ctypes.c_void_p(-1).value, ctypes.get_last_error()
                    descriptor.update(enabled=True, endpoint=endpoint)
                    file.write_text(json.dumps(descriptor), encoding="utf-8")
                    denied = probe.raw("live_bind", instance=chosen["instance"], target=chosen["target"])
                    assert denied["structuredContent"]["error"]["code"] == "access_denied", denied
                    assert denied["structuredContent"]["error"]["transport"]["message"]
                finally:
                    if pipe != ctypes.c_void_p(-1).value:
                        kernel.CloseHandle(pipe)
                    kernel.LocalFree(sd)
        finally:
            probe.close()
        print("Unavailable/access-denied endpoints retain transport errors; disabled and changed targets distinguished: PASS", flush=True)

        def registration(operation, expected):
            desktop.action("registration", operation=operation, executable=str(codex))
            deadline = time.monotonic()+20
            while time.monotonic()<deadline:
                status = desktop.action("registration")["status"]
                if expected in status:
                    return
                time.sleep(.1)
            raise AssertionError(status)
        subprocess.run([str(codex), "mcp", "add", "keep-other", "--", "unrelated-tool"], env=env, cwd=home, check=True, capture_output=True)
        registration("check", "Not registered")
        registration("connect", "Registered with Codex.")
        config = json.loads(subprocess.run([str(codex), "mcp", "get", "opad", "--json"], env=env, cwd=home, check=True, capture_output=True).stdout)
        assert Path(config["transport"]["command"]).resolve() == cli
        assert config["transport"]["args"] == ["mcp", "--live", "--discovery", str(desktop.discovery).replace("\\", "/")]
        registration("connect", "Already registered")
        registration("remove", "registration removed")
        subprocess.run([str(codex), "mcp", "get", "keep-other", "--json"], env=env, cwd=home, check=True, capture_output=True)
        subprocess.run([str(codex), "mcp", "add", "opad", "--", "other-installation"], env=env, cwd=home, check=True, capture_output=True)
        registration("check", "different installation")
        registration("remove", "was not removed")
        desktop.action("disconnect")
        dropped = client.raw("context")
        assert dropped["isError"] and dropped["structuredContent"]["error"]["code"] == "disconnected", dropped
        desktop.action("settings", tab=1)
        time.sleep(.5)
        assert (desktop.control / "settings-client.png").exists()
        desktop.action("settings")
        time.sleep(.5)
        assert (desktop.control / "settings.png").exists()
        print("Settings register/check/remove via real Codex CLI, preserve other servers, refuse foreign removal and disconnect live clients: PASS", flush=True)
    finally:
        client.close()
        desktop.close()


if __name__ == "__main__":
    main()
