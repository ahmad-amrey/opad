"""Phone-report regression checks against an isolated desktop and real MCP transport."""
import argparse
from pathlib import Path
import uuid
import time
from test_live_agent import Desktop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--groups", type=int, default=100)
    args = parser.parse_args()
    root = Path("build/phone-regression") / str(uuid.uuid4())
    desktop = Desktop(args.app.resolve(), args.cli.resolve(), root.resolve())
    client = desktop.bind(args.cli.resolve())
    try:
        parent = client.write("component", name="Parts")["result"]["id"]
        revision = client.state()["revision"]
        sketch_id = None
        bodies = []
        for follow in (True, False):
            desktop.action("follow", enabled=follow)
            for i in range(args.groups):
                tx = client.call("transaction_begin", label="Part", expected_revision=revision, request_id=str(uuid.uuid4()))["transaction"]
                shared = dict(transaction=tx, expected_revision=revision)
                def stage(tool, **kw):
                    return client.call(tool, request_id=str(uuid.uuid4()), **shared, **kw)["result"]
                sketch_id = stage("sketch", name="Profile", geometry={})["sketch_id"]
                body = stage("feature", kind="box", inputs={"length": 10, "width": 8, "height": 3})["body_ids"][0]
                bodies.append(body)
                stage("rename", target=body, name=f"Part {follow}-{i}")
                stage("reparent", target=body, parent=parent)
                stage("appearance", target=sketch_id, visible=False)
                assert client.call("validate", transaction=tx, select=[body])["result"]["valid_page"]
                revision = client.call("transaction_commit", id=tx, expected_revision=revision, request_id=str(uuid.uuid4()))["revision"]
                saved = client.call("save", expected_revision=revision, request_id=str(uuid.uuid4()))
                assert not saved["result"]["dirty"]
            print(f"{args.groups} consecutive groups, Follow changes={follow}: PASS", flush=True)
        assert client.call("wait_for_idle")["result"]["idle"]
        desktop.action("edit_sketch", id=sketch_id)
        conflict = client.raw("appearance", target=sketch_id, visible=True, expected_revision=revision, request_id=str(uuid.uuid4()))
        error = conflict["structuredContent"]["error"]
        assert error["code"] == "edit_session_busy" and error["editing"]["human_edit"] and not error["retryable"], error
        assert error["editing"]["revision"] == revision and error["client_id"]
        assert not client.call("wait_for_idle", timeout_ms=100)["result"]["idle"]
        desktop.action("cancel_edit")
        # Scrolling stays usable after hundreds of operations, with keyboard selection kept visible.
        end = desktop.action("timeline", key="end", image=str((root / "timeline-end.png").resolve()))
        assert end["maximum"] > end["page"] and end["scroll"] > 0, end
        home = desktop.action("timeline", key="home", image=str((root / "timeline-home.png").resolve()))
        assert home["scroll"] == 0 and home["current"] != end["current"], home
        right = desktop.action("timeline", key="right")
        assert right["current"] != home["current"]
        query = client.call("query_entities", body=bodies[-1], filters={"at_plane": {"axis": "z", "value": 3}})["result"]
        assert query["total"] == 4
        before = client.state()
        rendered = client.raw("viewport_image", select=[bodies[-1]], hide=[bodies[0]], view="top", width=320, height=240, visible_ids=True)
        assert not rendered.get("isError"), rendered
        meta = rendered["structuredContent"]
        assert meta["revision"] == before["revision"] and meta["result"]["visible_ids"] == [bodies[-1]]
        assert meta["result"]["camera"]["absolute"]
        replay = client.raw("viewport_image", select=[bodies[-1]], camera=meta["result"]["camera"], width=320, height=240)
        assert not replay.get("isError"), replay
        original_png = next(item["data"] for item in rendered["content"] if item["type"] == "image")
        replay_png = next(item["data"] for item in replay["content"] if item["type"] == "image")
        assert original_png == replay_png
        hidden = client.call("viewport_image", select=[bodies[-1]], hide=[bodies[-1]], ignore_visibility=True, width=64, height=64)
        assert hidden["result"]["visible_count"] == 0 and "visible_ids" not in hidden["result"]
        after = client.state()
        assert before["camera"] == after["camera"] and before["revision"] == after["revision"]
        client.call("live_select", refs=[bodies[-1]], expected_revision=after["revision"])
        for attempt in range(100):
            selection = client.state()["selection"]
            if len(selection) == 1 and selection[0].get("type") == "body" and selection[0].get("id") == bodies[-1]:
                break
            time.sleep(.05)
        else:
            raise AssertionError(selection)
        desktop.action("select_similar")  # the whole body: its top perimeter first
        for attempt in range(100):
            selection = client.state()["selection"]
            if len(selection) == 4 and all(item.get("type") == "edge" for item in selection):
                break
            time.sleep(.05)
        else:
            raise AssertionError(selection)
        print("Human edit conflicts, scrolling history, geometric query and isolated render: PASS", flush=True)
    finally:
        client.close()
        desktop.close()


if __name__ == "__main__":
    main()
