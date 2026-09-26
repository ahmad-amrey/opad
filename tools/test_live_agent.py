"""Real stdio MCP -> local pipe -> isolated desktop acceptance test. Requires OpenGL."""
from recovery_record import document_text
import argparse
import base64
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid


class Client:
    def __init__(self, cli, discovery, name="OPAD live acceptance"):
        self.process = subprocess.Popen([str(cli), "mcp", "--live", "--discovery", str(discovery)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.serial = 0
        self.responses = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.responses.put(json.loads(line))
        threading.Thread(target=read, daemon=True).start()
        self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": name, "version": "1"}})

    def send(self, method, params=None):
        self.serial += 1
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.serial, "method": method, "params": params or {}}) + "\n")
        self.process.stdin.flush()

    def receive(self):
        response = self.responses.get(timeout=90)
        assert response["id"] == self.serial and "error" not in response, response
        return response["result"]

    def request(self, method, params=None):
        self.send(method, params)
        return self.receive()

    def raw(self, tool, **args):
        return self.request("tools/call", {"name": tool, "arguments": args})

    def call(self, tool, **args):
        result = self.raw(tool, **args)
        assert not result.get("isError"), result
        return result["structuredContent"]

    def state(self):
        return self.call("live_state")["result"]

    def write(self, tool, **args):
        args.setdefault("expected_revision", self.state()["revision"])
        args.setdefault("request_id", str(uuid.uuid4()))
        return self.call(tool, **args)

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


class Desktop:
    def __init__(self, app, cli, root, document=None, settings=None, environment=None):
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        self.settings = settings or root / "settings"
        self.control = root / "control"
        self.control.mkdir(exist_ok=True)
        document = document or root / "source.opad"
        if not document.exists():
            subprocess.run([str(cli), "new", str(document)], check=True, capture_output=True)
        env = dict(os.environ, OPAD_LANG="en", OPAD_BENCH_SETTINGS=str(self.settings),
                   OPAD_BENCH_AGENT=str(self.control), OPAD_TRACE=str(root / "trace.log"))
        env.update(environment or {})
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        self.log = (root / "stderr.log").open("w", encoding="utf-8")
        self.process = subprocess.Popen([str(app), str(document)], env=env, startupinfo=startup,
                                        stdout=self.log, stderr=self.log)
        start = time.monotonic()
        while time.monotonic() - start < 90:
            assert self.process.poll() is None, (self.process.returncode, root)
            files = list(self.settings.rglob("agent/*.json"))
            for file in files:
                try:
                    data = json.loads(file.read_text(encoding="utf-8"))
                    if data.get("target") and data.get("pid") == self.process.pid:
                        self.discovery = file.parent
                        self.descriptor = data
                        break
                except (OSError, ValueError):
                    pass
            if hasattr(self, "descriptor"):
                break
            time.sleep(.1)
        else:
            raise AssertionError("Desktop discovery timed out")

    def action(self, action, **args):
        output = self.control / "result.json"
        output.unlink(missing_ok=True)
        temporary = self.control / "next.json"
        temporary.write_text(json.dumps(dict(action=action, **args)), encoding="utf-8")
        temporary.replace(self.control / "action.json")
        start = time.monotonic()
        while time.monotonic() - start < 30:
            if output.exists():
                result = json.loads(output.read_text(encoding="utf-8"))
                assert "error" not in result, result
                return result
            assert self.process.poll() is None, (self.process.returncode, self.root)
            time.sleep(.02)
        raise AssertionError("Desktop action timed out: " + action)

    def bind(self, cli, name="OPAD live acceptance"):
        client = Client(cli, self.discovery, name=name)
        instances = client.call("live_instances")["instances"]
        chosen = next(i for i in instances if i["instance"] == self.descriptor["instance"])
        client.call("live_bind", instance=chosen["instance"], target=chosen["target"])
        return client

    def close(self):
        if self.process.poll() is None:
            try:
                self.action("quit")
                self.process.wait(timeout=10)
            except (subprocess.TimeoutExpired, AssertionError):
                self.process.kill()
                self.process.wait()
        self.log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/live-agent-test"))
    args = parser.parse_args()
    app, cli, root = args.app.resolve(), args.cli.resolve(), args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    desktop = Desktop(app, cli, root / str(uuid.uuid4()))
    client = None
    try:
        start = time.monotonic()
        client = desktop.bind(cli)
        tools = {t["name"]: t for t in client.request("tools/list")["tools"]}
        assert "doc" not in tools["feature"]["inputSchema"]["properties"]
        assert {"request_id", "expected_revision"} <= set(tools["feature"]["inputSchema"]["required"])
        assert "append" not in tools
        initial = client.state()
        assert not initial["busy"]
        print("discovery and bind", time.monotonic()-start, flush=True)
        client.write("param", name="thickness", expr="10 mm")
        client.write("feature", kind="box", name="Plate", inputs={"length": "60 mm", "width": "40 mm", "height": "thickness"})
        client.write("feature", kind="cylinder", name="Through hole", inputs={"diameter": "10 mm", "height": "thickness", "operation": "cut"})
        def verify(thickness):
            data = client.call("validate")["result"]
            assert data["valid_page"] and len(data["items"]) == 1 and data["items"][0]["solids"] == 1, data
            assert abs(data["items"][0]["volume_mm3"] - (2400-math.pi*25)*thickness) < 1e-5, data
            return data["items"][0]["id"]
        body = verify(10)
        camera = client.state()["camera"]
        rev = client.state()["revision"]
        operation = dict(name="thickness", expr="15 mm", expected_revision=rev, request_id="thickness-15")
        changed = client.call("param", **operation)
        assert client.call("param", **operation) == changed, "Duplicate request applied twice"
        verify(15)
        assert client.state()["camera"] == camera, "Follow off moved camera"
        assert client.raw("param", name="thickness", expr="20 mm", expected_revision=rev, request_id="stale")["isError"]
        desktop.action("undo")
        verify(10)
        desktop.action("redo")
        verify(15)
        desktop.action("access", edit=False)
        assert client.raw("param", name="thickness", expr="20 mm", expected_revision=client.state()["revision"], request_id="readonly")["isError"]
        assert client.call("context")["result"]["bodies"] == 1
        desktop.action("access", edit=True)
        preview = client.write("param", name="thickness", expr="12 mm", preview=True, request_id="cancelled-preview")
        assert preview["state"] == "staged"
        preview_image = client.raw("viewport_image", preview_id=preview["preview_id"], view="top")
        assert not preview_image.get("isError") and any(c["type"]=="image" for c in preview_image["content"]), preview_image
        verify(15)
        client.call("preview_cancel", id=preview["preview_id"])
        assert client.raw("request_status", request_id="cancelled-preview")["isError"]
        tx = client.write("transaction_begin", label="Thickness group")["transaction"]
        client.write("param", name="thickness", expr="12 mm", transaction=tx, request_id="grouped-thickness")
        assert abs(client.call("validate", transaction=tx)["result"]["items"][0]["volume_mm3"] - (2400-math.pi*25)*12) < 1e-5
        verify(15)
        client.write("transaction_commit", id=tx)
        assert client.call("request_status", request_id="grouped-thickness")["state"]=="committed"
        verify(12)
        desktop.action("undo")
        verify(15)
        preview = client.write("param", name="thickness", expr="11 mm", preview=True)
        desktop.action("manual", command="rename", arguments={"target": body, "name": "User renamed plate"})
        assert client.raw("preview_commit", id=preview["preview_id"], expected_revision=client.state()["revision"], request_id="aftermanual")["isError"]
        verify(15)
        # A fresh face token is required; a bare ordinal cannot silently retarget geometry.
        detail = client.call("entity_details", ref=f"{body}/face/0")["result"]
        assert "reference" in detail
        desktop.action("select", ref=f"{body}/face/0")
        time.sleep(.5)
        selected = client.state()["selection"]
        assert selected and "reference" in selected[0], selected
        client.call("live_select", refs=[f"{body}/face/1"], expected_revision=client.state()["revision"])
        time.sleep(.3)
        assert client.state()["selection"][0]["index"] == 1
        # Failed geometry preserves the live document and the connection.
        bad = client.raw("feature", kind="box", inputs={"length": -1}, expected_revision=client.state()["revision"], request_id="bad-box")
        assert bad["isError"]
        verify(15)
        client.write("export", format="step", out=str(root / "plate.step"))
        # Simulate a kernel call that finishes after cancellation. The UI control
        # stays responsive and the computed result must never commit afterwards.
        for interruption in ("stop", "revoke", "manual", "disconnect"):
            revision = client.state()["revision"]
            desktop.action("delay", ms=900)
            request_id = "interrupted-" + interruption
            client.send("tools/call", {"name": "param", "arguments": dict(name="thickness", expr="19 mm",
                expected_revision=revision, request_id=request_id)})
            time.sleep(.2)
            tick = time.monotonic()
            if interruption == "stop":
                desktop.action("stop")
            elif interruption == "revoke":
                desktop.action("access", edit=False)
            elif interruption == "manual":
                desktop.action("manual", command="rename", arguments={"target": body, "name": "Concurrent user edit"})
            else:
                client.process.kill()
                client.process.wait()
            assert time.monotonic()-tick < .6, "UI blocked behind a kernel worker"
            if interruption != "disconnect":
                assert client.receive()["isError"]
            else:
                client = desktop.bind(cli)
            desktop.action("delay", ms=0)
            time.sleep(1)
            if interruption == "revoke":
                desktop.action("access", edit=True)
            verify(15)
            receipt = client.raw("request_status", request_id=request_id)
            assert receipt.get("isError"), receipt
        print("Stop, access revocation, concurrent user edit and disconnect discard late kernel results: PASS", flush=True)
        image = client.raw("viewport_image", fit=True)
        assert not image.get("isError"), image
        png = next(item["data"] for item in image["content"] if item["type"] == "image")
        (root / "plate.png").write_bytes(base64.b64decode(png))
        def capture_recovery(action, **arguments):
            previous = set(desktop.settings.rglob("*.opad-recovery"))
            if action == "save":
                client.write("save", **arguments)
            else:
                desktop.action(action, **arguments)
            deadline = time.monotonic()+10
            while time.monotonic()<deadline:
                created = set(desktop.settings.rglob("*.opad-recovery"))-previous
                if created:
                    path = max(created, key=lambda p: p.stat().st_mtime_ns)
                    # The atomic record arrives just before its small metadata file.
                    if path.with_suffix(path.suffix+".meta").exists():
                        return path, json.loads(path.read_text(encoding="utf-8"))
                time.sleep(.05)
            raise AssertionError("Agent edits were not captured by recovery")
        record_path, record = capture_recovery("autosave")
        base = record["delta"]["base"]
        client.write("rename", target=body, name="Incremental recovery check")
        record_path, record = capture_recovery("autosave")
        assert record["delta"]["base"] == base, "Ordinary edits rewrote the recovery base"
        assert record["delta"]["ops"] and not record["delta"]["bodies"]
        assert record_path.stat().st_size < (record_path.parent/base).stat().st_size
        recovered = root / "agent-recovery-check.opad"
        recovered.write_text(document_text(record_path), encoding="utf-8")
        saved_validation = json.loads(subprocess.run([str(cli), "validate", str(recovered)], capture_output=True, text=True, check=True).stdout)
        assert saved_validation["valid_page"] and abs(saved_validation["items"][0]["volume_mm3"]-(2400-math.pi*25)*15)<1e-5
        _, checkpoint = capture_recovery("save", path=str(root / "plate.opad"))
        assert checkpoint["delta"]["base"] != base, "Explicit Save did not refresh the recovery base"
        assert not checkpoint["delta"]["ops"] and not checkpoint["delta"]["bodies"]
        print("Incremental recovery reuses its base and explicit Save refreshes it: PASS", flush=True)
        client.close()
        client = desktop.bind(cli)
        assert client.call("request_status", request_id="thickness-15")["state"] == "committed"
        assert client.call("param", **operation) == changed
        verify(15)
        desktop.action("access", enabled=False)
        assert client.raw("context")["isError"]
        desktop.action("access", enabled=True, edit=True)
        client.close()
        client = desktop.bind(cli)
        other = Desktop(app, cli, root / str(uuid.uuid4()), settings=desktop.settings)
        second = None
        try:
            assert len(client.call("live_instances")["instances"]) == 2
            second = other.bind(cli)
            second.write("feature", kind="box", inputs={"length": 2, "width": 3, "height": 4})
            assert abs(second.call("validate")["result"]["items"][0]["volume_mm3"]-24) < 1e-8
            verify(15)
        finally:
            if second:
                second.close()
            other.close()
        print("Enable/disable and two explicitly bound application targets: PASS", flush=True)
        face = client.call("entity_details", ref=f"{body}/face/0")["result"]["reference"]
        sketch_args = dict(name="Face sketch", plane={"face": face["ref"]}, geometry={})
        # A face this connection was never given needs its token; one it was given does not (TODO 10 B6).
        unchecked = client.raw("sketch", **dict(sketch_args, plane={"face": f"{body}/face/2"}), expected_revision=client.state()["revision"], request_id="unchecked-face")
        assert unchecked["isError"] and "unchecked_reference" in unchecked["structuredContent"]["error"]["message"], unchecked
        client.write("sketch", **sketch_args, references=[face])
        sketch = client.call("context", section="sketches")["result"]["items"][0]["id"]
        desktop.action("edit_sketch", id=sketch)
        time.sleep(.6)
        active = client.state()
        assert active["edit_session"] == "sketch" and active["active_sketch"]["id"] == sketch, active
        assert client.raw("param", name="thickness", expr="18 mm", expected_revision=active["revision"], request_id="during-edit")["isError"]
        desktop.action("cancel_edit")
        desktop.action("follow", enabled=True)
        previous_camera = client.state()["camera"]
        client.write("param", name="thickness", expr="16 mm")
        time.sleep(.5)
        assert client.state()["camera"] != previous_camera, "Follow on did not frame changed geometry"
        stable_camera = client.state()["camera"]
        client.write("rename", target=body, name="Plate label only")
        assert client.state()["camera"] == stable_camera, "Rename moved camera"
        assert client.raw("sketch", **sketch_args, references=[face], expected_revision=client.state()["revision"], request_id="stale-face")["isError"]
        desktop.action("settings")
        time.sleep(.5)
        assert (desktop.control / "settings.png").exists()
        print("Checked face references, active-editor refusal/context, Follow on and unrelated edits: PASS", flush=True)
        desktop.action("new")
        assert client.raw("context")["isError"], "Connection silently retargeted a new document"
        client.close()
        client = desktop.bind(cli)
        assert client.state()["changes"] == [], "New document leaked old document's change context"
        print("live reads/writes, revision checks, undo/redo, read-only, preview, grouped transaction, manual edits, selection, failure, export/image, save, reconnect/retry, target identity: PASS", flush=True)
    finally:
        if client:
            client.close()
        desktop.close()


if __name__ == "__main__":
    main()
