"""Measure real local MCP context latency and UI response on supplied CAD assets."""
import argparse
import json
from pathlib import Path
import re
import statistics
import time
import uuid
from test_live_agent import Desktop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("assets", type=Path, nargs="+")
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    results = []
    for asset in args.assets:
        folder = root / (asset.stem + "-" + str(uuid.uuid4())[:8])
        start = time.monotonic()
        desktop = Desktop(args.app.resolve(), args.cli.resolve(), folder, document=asset.resolve())
        loaded = time.monotonic()-start
        client = None
        try:
            start = time.monotonic()
            client = desktop.bind(args.cli.resolve())
            bind = (time.monotonic()-start)*1000
            latencies = []
            sizes = []
            for _ in range(5):
                start = time.monotonic()
                result = client.call("context")
                latencies.append((time.monotonic()-start)*1000)
                sizes.append(len(json.dumps(result).encode("utf-8")))
            start = time.monotonic()
            page = client.call("context", section="nodes", limit=25)
            page_ms = (time.monotonic()-start)*1000
            assert len(page["result"]["items"]) <= 25
            ui = []
            for _ in range(5):
                start = time.monotonic()
                desktop.action("state")
                ui.append((time.monotonic()-start)*1000)
            entry = dict(asset=str(asset), load_seconds=loaded, bind_ms=bind,
                cold_context_ms=latencies[0], warm_context_ms=statistics.median(latencies[1:]),
                summary_bytes=max(sizes), page_ms=page_ms, page_bytes=len(json.dumps(page).encode()),
                ui_control_ms=max(ui), summary=result["result"], folder=str(folder))
            results.append(entry)
            print(json.dumps(entry), flush=True)
        finally:
            if client:
                client.close()
            desktop.close()
        log = (folder / "trace.log").read_text(encoding="utf-8", errors="replace")
        results[-1]["watchdog_lines"] = [line for line in log.splitlines() if "stall" in line.lower()]
        (root / "metrics.json").write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
