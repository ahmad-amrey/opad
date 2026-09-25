"""Viewport/workflow regressions using isolated OPAD instances and in-app Qt events.

Requires a GUI/OpenGL session. Optional --model should contain two circular edges
(for the picking regression); --drawing can be repeated for SVG/DXF fixtures.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--drawing", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, default=Path("build/todo8-workflows"))
    args = parser.parse_args()
    app, cli, output = args.app.resolve(), args.cli.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="opad-todo8-") as directory:
        root = Path(directory)
        empty = root / "empty.opad"
        subprocess.run([str(cli), "new", str(empty)], capture_output=True, check=True)
        cases = [
            ("planes", empty, {"OPAD_BENCH_PLANES": "{prefix}"}),
            ("offset", empty, {"OPAD_BENCH_DESIGN": "{prefix}", "OPAD_BENCH_SKETCH_HANDLES": "{prefix}"}),
            ("extrude", empty, {"OPAD_BENCH_DESIGN": "{prefix}", "OPAD_BENCH_EXTRUDE_HANDLE": "1"}),
            ("shortcuts", empty, {"OPAD_BENCH_SHORTCUTS": "{prefix}"}),
        ]
        if args.model:
            cases += [("picking", args.model.resolve(), {"OPAD_BENCH_PICKING": "1"}),
                      ("instances", args.model.resolve(), {"OPAD_BENCH_INSTANCES": "1"})]
        for index, drawing in enumerate(args.drawing):
            cases.append((f"drawing-{index}-{drawing.stem}", drawing.resolve(),
                          {"OPAD_BENCH_WIZARD": "{prefix}", "OPAD_BENCH_WIZARD_CREATE": "1"}))
        for name, document, switches in cases:
            log = output / f"{name}.log"
            log.write_text("", encoding="utf-8")
            env = {key: value for key, value in os.environ.items() if not key.startswith("OPAD_BENCH_")}
            env.update(OPAD_LANG="en", OPAD_BENCH_SETTINGS=str(root / name), OPAD_TRACE=str(log))
            env.update({key: value.format(prefix=output / name) for key, value in switches.items()})
            startup = None
            if os.name == "nt":
                startup = subprocess.STARTUPINFO()
                startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0
            result = subprocess.run([str(app), str(document), "--bench-select"], env=env,
                                    startupinfo=startup, capture_output=True, timeout=180)
            trace = log.read_text(encoding="utf-8", errors="replace")
            assert result.returncode == 0, (name, result.returncode, trace[-8000:])
            assert "PASS" in trace and "FAIL" not in trace and "CRASH:" not in trace, (name, trace[-8000:])
            print(f"{name}: PASS", flush=True)


if __name__ == "__main__":
    main()
