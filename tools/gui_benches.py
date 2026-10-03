"""Desktop safety net: the in-app benches that drive real Qt events in hidden windows (TODO 10 A13).

usage: python tools/gui_benches.py APP CLI [--only NAME ...] [--output DIR]

Each bench runs in its own hidden window with isolated settings and must log PASS and no FAIL. It needs a desktop
session with OpenGL (Windows here); `ctest --preset windows-gui` runs it, `ctest --preset windows` leaves it out.
The unit tests only see core and a few app classes; these benches are what notices a view, handle, panel or
workflow that stopped doing what it did (a drag that no longer previews live, a card that no longer follows its
object, a pointer that leaves highlights behind).
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "gui-benches")
    args = parser.parse_args()
    app, cli, output = args.app.resolve(), args.cli.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="opad-gui-") as directory:
        root = Path(directory)

        def document(name, *commands):
            path = root / f"{name}.opad"
            subprocess.run([str(cli), "new", str(path)], check=True, capture_output=True)
            for command in commands:
                subprocess.run([str(cli), command[0], str(path)] + list(command[1:]), check=True, capture_output=True)
            return path

        empty = document("empty")
        box = document("box", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'))
        round_part = document("cylinder", ("feature", "--kind", "cylinder", "--inputs", '{"diameter":"20 mm","height":"10 mm"}'))
        drawing = root / "layers.svg"
        drawing.write_text('<svg width="40mm" viewBox="0 0 40 40"><g id="Outline"><rect width="20" height="10"/></g>'
                           '<g id="Guide"><circle cx="5" cy="5" r="2"/></g></svg>', encoding="utf-8")
        # A drawing of 30,000 separate segments (UI-29: converting it to a sketch was quadratic; UI-27: snapping over it).
        large = root / "segments.dxf"
        lines = [f"0\nLINE\n8\nLines\n10\n{(i % 300) * 3}\n20\n{(i // 300) * 3}\n11\n{(i % 300) * 3 + 2}\n21\n{(i // 300) * 3 + 1}\n" for i in range(30000)]
        large.write_text("0\nSECTION\n2\nENTITIES\n" + "".join(lines) + "0\nENDSEC\n0\nEOF\n", encoding="utf-8")
        overlapping = document("overlap", ("feature", "--kind", "box", "--inputs", '{"length":"20 mm","width":"20 mm","height":"10 mm"}'),
                               ("feature", "--kind", "cylinder", "--inputs", '{"x":"8 mm","diameter":"10 mm","height":"20 mm"}'))
        overhang = document("overhang", ("feature", "--kind", "box", "--inputs", '{"length":"10 mm","width":"10 mm","height":"10 mm"}'),
                            ("feature", "--kind", "box", "--inputs", '{"plane":{"origin":[0,0,10],"normal":[0,0,1]},"length":"30 mm","width":"10 mm","height":"2 mm","operation":"join"}'))
        screw = ROOT / "tests" / "corpus" / "occt-screw.step"
        cases = [
            ("design", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_UISHOT": "{prefix}.ui.png", "OPAD_BENCH_RULE": "1"}),
            ("extrude-handle", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_EXTRUDE_HANDLE": "1", "OPAD_BENCH_HANDLESHOT": "{prefix}"}),
            ("sketch-handles", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_HANDLES": "{prefix}"}),
            ("sketch-primitives", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_PRIMITIVES": "{prefix}"}),
            ("sketch-grid", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_GRID": "{prefix}"}),
            ("sketch-ladder", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_LADDER": "{prefix}"}),
            ("sketch-keys", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_KEYS": "{prefix}"}),
            ("sketch-shapes", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_SHAPES": "{prefix}"}),
            ("sketch-crosslock", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_CROSSLOCK": "{prefix}"}),
            ("sketch-snaps", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_SNAPS": "{prefix}"}),
            ("sketch-steps", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_STEPS": "{prefix}"}),
            ("sketch-constraints", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_CONSTRAINTS": "{prefix}"}),
            ("leave", box, {"OPAD_BENCH_LEAVE": "1"}),
            ("two-d", box, {"OPAD_BENCH_TWOD": "{prefix}.png"}),
            ("instances", box, {"OPAD_BENCH_INSTANCES": "1"}),
            ("drawing-import", empty, {"OPAD_BENCH_DRAWING_IMPORT": str(drawing)}),
            ("drawing-to-sketch", drawing, {"OPAD_BENCH_WIZARD": "{prefix}.png", "OPAD_BENCH_WIZARD_CREATE": "1"}),
            ("sketch-large", large, {"OPAD_BENCH_LARGE": "{prefix}.json"}),
            ("drawing-preview", large, {"OPAD_BENCH_WIZARD": "{prefix}.png", "OPAD_BENCH_WIZARD_PREVIEW": "30000"}),
            ("notes", empty, {"OPAD_BENCH_NOTES": "{prefix}"}),
            ("zoom-refinement", round_part, {"OPAD_BENCH_SCENE": "{prefix}.png", "OPAD_BENCH_VIEW": "iso", "OPAD_BENCH_ZOOM": "40"}),
            ("interference", overlapping, {"OPAD_BENCH_CHECK": "interference", "OPAD_BENCH_UISHOT": "{prefix}"}),
            ("print-check", overhang, {"OPAD_BENCH_CHECK": "print", "OPAD_BENCH_UISHOT": "{prefix}"}),
        ]
        if screw.exists():
            cases.append(("picking", screw, {"OPAD_BENCH_PICKING": "1"}))
            # Viewer mode: a STEP opened read-only beside a drawing (so there is a next file in the folder).
            folder = root / "viewer"
            folder.mkdir()
            (folder / "a-screw.step").write_bytes(screw.read_bytes())
            (folder / "b-layers.svg").write_bytes(drawing.read_bytes())
            cases.append(("viewer", folder / "a-screw.step", {"OPAD_BENCH_VIEWER": str(folder / "a-screw.opad")}))
        # These open a STEP or a drawing and then edit it: as with viewer mode turned off in the settings.
        editing = {"drawing-to-sketch", "drawing-preview", "sketch-large", "picking"}
        failures = []
        for name, doc, switches in cases:
            if args.only and name not in args.only:
                continue
            log = output / f"{name}.log"
            log.write_text("", encoding="utf-8")
            env = {key: value for key, value in os.environ.items() if not key.startswith("OPAD_BENCH_")}
            env.update(OPAD_LANG="en", OPAD_BENCH_SETTINGS=str(root / f"{name}-settings"), OPAD_TRACE=str(log))
            env.update({key: value.format(prefix=output / name) for key, value in switches.items()})
            if name in editing:
                ini = root / f"{name}-settings" / "opad" / "OPAD.ini"
                ini.parent.mkdir(parents=True, exist_ok=True)
                ini.write_text("[files]\nviewerMode=false\n", encoding="utf-8")
            startup = None
            if os.name == "nt":
                startup = subprocess.STARTUPINFO()
                startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0
            started = time.monotonic()
            try:
                result = subprocess.run([str(app), str(doc), "--bench-select"], env=env, startupinfo=startup, capture_output=True, timeout=240)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
            trace = log.read_text(encoding="utf-8", errors="replace")
            benches = [line for line in trace.splitlines() if "bench:" in line and ("PASS" in line or "FAIL" in line)]
            ok = code == 0 and "FAIL" not in trace and "CRASH:" not in trace and (name == "zoom-refinement" or benches)
            if name == "zoom-refinement":  # the frame exists and the refinement ran
                ok = ok and "refine: done" in trace
            print(f"{name}: {'PASS' if ok else 'FAIL'} ({time.monotonic() - started:.1f} s)", flush=True)
            if not ok:
                failures.append(name)
                print("\n".join(trace.splitlines()[-25:]), flush=True)
        if failures:
            print("failed: " + ", ".join(failures), flush=True)
            sys.exit(1)


if __name__ == "__main__":
    main()
