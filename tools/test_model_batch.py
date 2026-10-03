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
    client = observer = None
    try:
        client = desktop.bind(args.cli.resolve())
        observer = desktop.bind(args.cli.resolve(), "Batch observer")
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
        # TODO 10 B14/B15: a pattern and a multi-profile extrude are named, coloured and moved to components in the
        # same batch that makes them, and body_name/color/parent on 10 bodies equal the long form.
        revision = client.state()["revision"]
        circles = {"points": [{"id": i + 1, "x": 40 + 6 * i, "y": 0} for i in range(3)],
                   "entities": [{"id": 10 + i, "type": "circle", "p": [i + 1], "r": 2} for i in range(3)], "constraints": []}
        styled = [
            {"id": "tray", "command": "component", "arguments": {"name": "Tray"}},
            {"id": "pin", "command": "feature", "arguments": {"kind": "cylinder", "name": "Pin", "inputs": {"x": 20, "diameter": 2, "height": 6}, "color": [.8, .1, .1]}},
            {"id": "pins", "command": "feature", "arguments": {"kind": "pattern_rect", "inputs": {"bodies": ["@{pin#/body_ids/0}"], "count": 4, "spacing": 5}}},
            {"id": "dots", "command": "sketch", "arguments": {"plane": {"base": "xy"}, "geometry": circles}},
            {"id": "studs", "command": "feature", "arguments": {"kind": "extrude", "name": "Stud", "body_name": "Stud {n}", "color": [.1, .4, .8],
                                                                "inputs": {"profiles": [{"sketch": "@{dots#/sketch_id}", "at": [40 + 6 * i, 0]} for i in range(3)], "distance": 3}}},
            {"id": "green", "command": "appearance", "arguments": {"targets": ["@{pins#/body_ids/*}", "@{studs#/body_ids/*}"], "color": [.2, .7, .2]}},
            {"id": "shelf", "command": "component", "arguments": {"name": "Shelf"}},
            {"id": "move", "command": "reparent", "arguments": {"targets": "@{pins#/all_body_ids/*}", "parent": "@{shelf#/component_id}"}},
            {"id": "names", "command": "rename", "arguments": {"targets": ["@{pins#/body_ids/*}"], "name": "Pin copy {n}"}},
        ]
        done = client.call("model_batch", steps=styled, parent="@{tray#/component_id}", expected_revision=revision, request_id="styled")
        results = {step["id"]: step["result"] for step in done["result"]["steps"]}
        assert len(results["pins"]["body_ids"]) == 3 and len(results["pins"]["all_body_ids"]) == 4, results["pins"]
        assert len(results["studs"]["body_ids"]) == 3 and len(results["green"]["ids"]) == 6, results
        nodes = {n["id"]: n for n in client.call("context", section="nodes", limit=100)["result"]["items"]}
        tray, shelf = results["tray"]["component_id"], results["shelf"]["component_id"]
        pin = results["pin"]["body_ids"][0]
        assert nodes[pin]["name"] == "Pin" and nodes[pin]["parent"] == shelf and nodes[pin]["color"] == [.8, .1, .1], nodes[pin]
        for index, body in enumerate(results["pins"]["body_ids"]):
            assert nodes[body]["name"] == f"Pin copy {index + 1}" and nodes[body]["parent"] == shelf and nodes[body]["color"] == [.2, .7, .2], nodes[body]
        for index, body in enumerate(results["studs"]["body_ids"]):
            assert nodes[body]["name"] == f"Stud {index + 1}" and nodes[body]["parent"] == tray and nodes[body]["color"] == [.2, .7, .2], nodes[body]
        # A whole list where one id is expected is refused before anything runs, naming targets.
        wrong = client.raw("model_batch", steps=styled[:3] + [{"id": "one", "command": "rename", "arguments": {"target": "@{pins#/body_ids/*}", "name": "X"}}],
                           expected_revision=done["revision"], request_id="whole-list-target")
        assert wrong["isError"] and "targets" in wrong["structuredContent"]["error"]["message"], wrong
        # Ten bodies named, coloured and placed by the feature step equal the long form, record for record.
        ten = {"points": [{"id": i + 1, "x": 10 * i, "y": 40} for i in range(10)],
               "entities": [{"id": 20 + i, "type": "circle", "p": [i + 1], "r": 2} for i in range(10)], "constraints": []}

        def rows(long_form, request):
            steps = [{"id": "box", "command": "component", "arguments": {"name": "Pins"}},
                     {"id": "sk", "command": "sketch", "arguments": {"plane": {"base": "xy"}, "geometry": ten}}]
            extrude = {"kind": "extrude", "name": "Pin row", "inputs": {"profiles": [{"sketch": "@{sk#/sketch_id}", "at": [10 * i, 40]} for i in range(10)], "distance": 6}}
            if not long_form:
                extrude.update(body_name="Row pin {n}", color=[.3, .3, .9], parent="@{box#/component_id}")
            steps.append({"id": "row", "command": "feature", "arguments": extrude})
            if long_form:
                for i in range(10):
                    body = f"@{{row#/body_ids/{i}}}"
                    steps += [{"id": f"n{i}", "command": "rename", "arguments": {"target": body, "name": f"Row pin {i + 1}"}},
                              {"id": f"c{i}", "command": "appearance", "arguments": {"target": body, "color": [.3, .3, .9]}},
                              {"id": f"p{i}", "command": "reparent", "arguments": {"target": body, "parent": "@{box#/component_id}"}}]
            before = client.state()["revision"]
            made = client.call("model_batch", steps=steps, expected_revision=before, request_id=request)
            ids = {step["id"]: step["result"] for step in made["result"]["steps"]}
            items = {n["id"]: n for n in client.call("context", section="nodes", limit=100)["result"]["items"]}
            return [(items[b]["name"], items[items[b]["parent"]]["name"], items[b]["color"]) for b in ids["row"]["body_ids"]]

        short = rows(False, "ten-short")
        assert short == rows(True, "ten-long") and len(short) == 10 and short[3] == ("Row pin 4", "Pins", [.3, .3, .9]), short
        print("Batch body names, colours, components, targets and whole-list references: PASS", flush=True)
        # TODO 11 UI-33: the batch's component is where its feature and sketch steps are made; a step's own wins.
        revision = client.state()["revision"]
        lidded = [
            {"id": "lid", "command": "component", "arguments": {"name": "Lid"}},
            {"id": "outline", "command": "sketch", "arguments": {"geometry": {"shapes": [{"kind": "rect2", "picks": [[0, 80], [10, 90]]}]}}},
            {"id": "pad", "command": "feature", "arguments": {"kind": "extrude", "inputs": {"profiles": [{"sketch": "@{outline#/sketch_id}", "at": [5, 85]}], "distance": 2}}},
            {"id": "free", "command": "feature", "arguments": {"kind": "box", "component": None, "inputs": {"x": 80, "length": 2, "width": 2, "height": 2}}},
        ]
        late = client.raw("model_batch", steps=lidded[1:] + lidded[:1], component="@{lid#/component_id}", expected_revision=revision, request_id="lid-late")
        assert late["isError"] and "batch component" in late["structuredContent"]["error"]["message"], late
        made = client.call("model_batch", steps=lidded, component="@{lid#/component_id}", expected_revision=revision, request_id="lid")
        ids = {step["id"]: step["result"] for step in made["result"]["steps"]}
        lid = ids["lid"]["component_id"]
        items = {n["id"]: n for n in client.call("context", section="nodes", limit=100)["result"]["items"]}
        assert items[ids["pad"]["body_ids"][0]]["parent"] == lid, items[ids["pad"]["body_ids"][0]]
        assert items[ids["free"]["body_ids"][0]].get("parent") in (None, ""), items[ids["free"]["body_ids"][0]]
        made_in = {f["id"]: f.get("component") for f in client.call("features")["result"]}
        assert made_in[ids["outline"]["sketch_id"]] == lid and made_in[ids["pad"]["feature_id"]] == lid and not made_in[ids["free"]["feature_id"]], made_in
        print("Batch component for feature and sketch steps: PASS", flush=True)
        # TODO 11 UI-33 phase 2: the lid moved, its sketch and body go along, and the next body made from the sketch
        # lands on it where it is now.
        outline, pad = ids["outline"]["sketch_id"], ids["pad"]["body_ids"][0]
        was = client.call("sketch_details", sketch=outline)["result"]
        low = lambda body: client.call("inspect", ref=body)["result"]["bbox"]["min"][2]
        pad_low = low(pad)
        client.write("transform", target=lid, matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 25, 0, 0, 0, 1])
        now = client.call("sketch_details", sketch=outline)["result"]
        assert now["component"] == lid and abs(now["frame"]["origin"][2] - was["frame"]["origin"][2] - 25) < 1e-9, (was, now)
        assert abs(low(pad) - pad_low - 25) < 1e-6, low(pad)
        again = client.write("feature", kind="extrude", component=lid, inputs={"profiles": [{"sketch": outline, "at": [5, 85]}], "distance": 2, "operation": "new"})
        assert abs(low(again["result"]["body_ids"][0]) - now["frame"]["origin"][2]) < 1e-6, low(again["result"]["body_ids"][0])
        print("Sketches follow their component: PASS", flush=True)
    finally:
        for connection in (client, observer):
            if connection:
                connection.close()
        desktop.close()


if __name__ == "__main__":
    main()
