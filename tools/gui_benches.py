"""Desktop safety net: the in-app benches that drive real Qt events in hidden windows (TODO 10 A13).

usage: python tools/gui_benches.py APP CLI [--only NAME ...] [--output DIR] [--list]

Each bench runs in its own hidden window with isolated settings and must log PASS and no FAIL. It needs a desktop
session with OpenGL (Windows here); `ctest --preset windows-gui` runs it, `ctest --preset windows` leaves it out.
The unit tests only see core and a few app classes; these benches are what notices a view, handle, panel or
workflow that stopped doing what it did (a drag that no longer previews live, a card that no longer follows its
object, a pointer that leaves highlights behind).

Besides the cases below, every tools/bench_cases/<area>.py module adds its CASES (a new case is a new file): tuples
(name, document, switches[, settings]) as here, where document is a fixture name ("empty", "box", "cylinder",
"drawing", "overlap", "overhang", "screw", "far"), a path (relative to the repository) or a callable (root, document)
-> path that makes its own file (document(name, *cli commands) runs opad-cli as below); switches are the environment
("{prefix}" = the output prefix of the case, OPAD_LANG may be overridden); settings is the OPAD.ini text to start with.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def area_cases():
    """(area, case) for every CASES entry of tools/bench_cases/*.py, by file name."""
    for path in sorted((ROOT / "tools" / "bench_cases").glob("*.py")):
        spec = importlib.util.spec_from_file_location(f"bench_cases_{path.stem}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for case in getattr(module, "CASES", []):
            yield path.stem, case


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # a failing case's log tail can be Arabic (cp1252 consoles)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "gui-benches")
    parser.add_argument("--list", action="store_true", help="print the case names (built-in and tools/bench_cases) and quit")
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
        overlapping = document("overlap", ("feature", "--kind", "box", "--inputs", '{"length":"20 mm","width":"20 mm","height":"10 mm"}'),
                               ("feature", "--kind", "cylinder", "--inputs", '{"x":"8 mm","diameter":"10 mm","height":"20 mm"}'))
        overhang = document("overhang", ("feature", "--kind", "box", "--inputs", '{"length":"10 mm","width":"10 mm","height":"10 mm"}'),
                            ("feature", "--kind", "box", "--inputs", '{"plane":{"origin":[0,0,10],"normal":[0,0,1]},"length":"30 mm","width":"10 mm","height":"2 mm","operation":"join"}'))
        screw = ROOT / "tests" / "corpus" / "occt-screw.step"
        # A 30 x 20 x 12 m block 600 m from the origin, Y up, as SketchUp exports a house with its site coordinates.
        far = root / "far-block.obj"
        far.write_text("v 24000 0 -601000\nv 54000 0 -601000\nv 54000 0 -621000\nv 24000 0 -621000\n"
                       "v 24000 12000 -601000\nv 54000 12000 -601000\nv 54000 12000 -621000\nv 24000 12000 -621000\n"
                       "f 1 2 3 4\nf 5 8 7 6\nf 1 5 6 2\nf 2 6 7 3\nf 3 7 8 4\nf 4 8 5 1\n", encoding="ascii")
        cases = [
            ("fit-far", far, {"OPAD_BENCH_FIT": "model", "OPAD_BENCH_FITSHOT": "{prefix}"}),
            ("fit-near", box, {"OPAD_BENCH_FIT": "origin", "OPAD_BENCH_FITSHOT": "{prefix}"}),
            ("fit-wide", far, {"OPAD_BENCH_FIT": "origin"}),
            ("design", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_UISHOT": "{prefix}.ui.png", "OPAD_BENCH_RULE": "1"}),
            ("extrude-handle", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_EXTRUDE_HANDLE": "1", "OPAD_BENCH_HANDLESHOT": "{prefix}"}),
            ("sketch-handles", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_HANDLES": "{prefix}"}),
            ("sketch-primitives", empty, {"OPAD_BENCH_DESIGN": "{prefix}.png", "OPAD_BENCH_SKETCH_PRIMITIVES": "{prefix}"}),
            ("leave", box, {"OPAD_BENCH_LEAVE": "1"}),
            ("two-d", box, {"OPAD_BENCH_TWOD": "{prefix}.png"}),
            ("instances", box, {"OPAD_BENCH_INSTANCES": "1"}),
            ("drawing-import", empty, {"OPAD_BENCH_DRAWING_IMPORT": str(drawing)}),
            ("drawing-to-sketch", drawing, {"OPAD_BENCH_WIZARD": "{prefix}.png", "OPAD_BENCH_WIZARD_CREATE": "1"}),
            ("notes", empty, {"OPAD_BENCH_NOTES": "{prefix}"}),
            ("zoom-refinement", round_part, {"OPAD_BENCH_SCENE": "{prefix}.png", "OPAD_BENCH_VIEW": "iso", "OPAD_BENCH_ZOOM": "40"}),
            ("interference", overlapping, {"OPAD_BENCH_CHECK": "interference", "OPAD_BENCH_UISHOT": "{prefix}"}),
            ("print-check", overhang, {"OPAD_BENCH_CHECK": "print", "OPAD_BENCH_UISHOT": "{prefix}"}),
            ("ip", box, {"OPAD_BENCH_IP": "{prefix}"}),
        ]
        if screw.exists():
            cases.append(("picking", screw, {"OPAD_BENCH_PICKING": "1"}))
            # Viewer mode: a STEP opened read-only beside a drawing (so there is a next file in the folder).
            folder = root / "viewer"
            folder.mkdir()
            (folder / "a-screw.step").write_bytes(screw.read_bytes())
            (folder / "b-layers.svg").write_bytes(drawing.read_bytes())
            cases.append(("viewer", folder / "a-screw.step", {"OPAD_BENCH_VIEWER": str(folder / "a-screw.opad")}))
        # Settings before the start. These open a STEP or a drawing and then edit it: viewer mode off. The fits load with
        # the grid on; fit-wide's 1 km minimum grid lies around the origin and would pull a box-less FitAll there.
        editing, grid = "[files]\nviewerMode=false\n", "[view]\ngrid=true\n"
        settings = {"drawing-to-sketch": editing, "picking": editing, "fit-far": grid, "fit-near": grid, "fit-wide": grid + "gridExtent=1000000\n"}
        fixtures = {"empty": empty, "box": box, "cylinder": round_part, "drawing": drawing, "overlap": overlapping,
                    "overhang": overhang, "screw": screw, "far": far}
        known = {case[0] for case in cases}
        for area, (name, doc, switches, *ini) in area_cases():
            if name in known:
                sys.exit(f"bench_cases/{area}.py: case {name} exists already")
            known.add(name)
            if args.only and name not in args.only or args.list:
                continue  # its document is not made
            if callable(doc):
                doc = doc(root, document)
            elif isinstance(doc, str) and doc in fixtures:
                doc = fixtures[doc]
            else:
                doc = ROOT / doc
            if not Path(doc).exists():
                print(f"{name}: skipped ({doc} does not exist)", flush=True)
                continue
            cases.append((name, doc, switches))
            if ini:
                settings[name] = ini[0]
        if args.list:
            print("\n".join(sorted(known)))
            return
        failures = []
        for name, doc, switches in cases:
            if args.only and name not in args.only:
                continue
            log = output / f"{name}.log"
            log.write_text("", encoding="utf-8")
            env = {key: value for key, value in os.environ.items() if not key.startswith("OPAD_BENCH_")}
            env.update(OPAD_LANG="en", OPAD_BENCH_SETTINGS=str(root / f"{name}-settings"), OPAD_TRACE=str(log))
            env.update({key: value.format(prefix=output / name) for key, value in switches.items()})
            if name in settings:
                ini = root / f"{name}-settings" / "opad" / "OPAD.ini"
                ini.parent.mkdir(parents=True, exist_ok=True)
                ini.write_text(settings[name], encoding="utf-8")
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
