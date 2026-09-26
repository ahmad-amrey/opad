"""Live staging and commit cost on a large document (gap log #3): a transaction of camera views, its commit, a read
and a new transaction straight after, and a direct parameter edit, each timed, with the desktop's trace kept.

usage: python tools/bench_live_commit.py APP CLI DOCUMENT [--output DIR]
The document is copied first; the desktop runs hidden with isolated settings.
"""
import argparse
import json
import shutil
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_live_agent import Desktop  # noqa: E402


def timed(label, fn, times):
    start = time.monotonic()
    value = fn()
    times[label] = round(time.monotonic() - start, 3)
    print(f"{label}: {times[label]:.2f} s", flush=True)
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("document", type=Path)
    parser.add_argument("--output", type=Path, default=Path("build/bench-live-commit"))
    args = parser.parse_args()
    folder = args.output.resolve() / str(uuid.uuid4())[:8]
    folder.mkdir(parents=True)
    copy = folder / "document.opad"
    shutil.copyfile(args.document, copy)
    times = {}
    desktop = timed("open", lambda: Desktop(args.app.resolve(), args.cli.resolve(), folder, document=copy), times)
    client = None
    try:
        client = timed("bind", lambda: desktop.bind(args.cli.resolve()), times)
        def settle():  # a large document keeps loading and meshing after it opens; wait_for_idle waits 10 s at most
            deadline = time.monotonic() + 900
            while time.monotonic() < deadline:
                if client.call("wait_for_idle", timeout_ms=10000)["result"]["idle"]:
                    return
            raise AssertionError("the document never became idle")
        timed("wait_for_idle", settle, times)
        base = client.state()["revision"]
        tx = timed("transaction_begin", lambda: client.call("transaction_begin", label="Views", expected_revision=base, request_id="begin"), times)["transaction"]
        start = time.monotonic()
        for i in range(15):
            client.call("view", name=f"Bench {i}", camera={"eye": [100 + i, 100, 100], "target": [0, 0, 0], "up": [0, 0, 1]},
                        transaction=tx, expected_revision=base, request_id=f"view-{i}")
        times["stage 15 views"] = round(time.monotonic() - start, 3)
        print(f"stage 15 views: {times['stage 15 views']:.2f} s", flush=True)
        timed("transaction_commit", lambda: client.call("transaction_commit", id=tx, expected_revision=base, request_id="commit"), times)
        timed("context right after", lambda: client.call("context"), times)
        revision = client.state()["revision"]
        second = timed("transaction_begin right after", lambda: client.call("transaction_begin", label="Again", expected_revision=revision, request_id="begin-2"), times)
        client.call("transaction_cancel", id=second["transaction"])
        timed("param outside a transaction", lambda: client.raw("param", name="bench_unused", expr="1 mm", expected_revision=client.state()["revision"], request_id="param"), times)
    finally:
        if client:
            client.close()
        desktop.close()
    trace = (folder / "trace.log").read_text(encoding="utf-8", errors="replace").splitlines()
    times["stalls"] = [line for line in trace if "stalled" in line][:40]
    times["jobs"] = [line for line in trace if "job" in line.lower() and ("done" in line or "ms" in line)][:80]
    (folder / "times.json").write_text(json.dumps(times, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in times.items() if not isinstance(v, list)}, indent=1))
    print(f"trace: {folder / 'trace.log'}")


if __name__ == "__main__":
    main()
