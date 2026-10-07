"""An agent drives the running desktop app over live MCP: it builds a single-cylinder engine (block with its bore, crank,
connecting rod, piston) in one model_batch, joins it with five joints, checks the mechanism, runs a motion study (the
piston's stroke and path against the slider-crank formula) and a dynamic one (Chrono, 3000 rpm: the motor's work against
the kinetic energy), turns the crank with joint_set, and leaves pictures: the live viewport (viewport_image) and the
whole screen with the app showing what the agent did. Run under a display (xvfb-run on Linux).

usage: python tools/sim_live_agent.py APP CLI OUT
"""
import argparse
import base64
import json
import math
import subprocess
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_live_agent import Desktop  # noqa: E402

R, L = 30.0, 100.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app", type=Path)
    ap.add_argument("cli", type=Path)
    ap.add_argument("out", type=Path)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    desktop = Desktop(args.app.resolve(), args.cli.resolve(), args.out / "desktop")
    checks, notes = [], []

    def check(label, ok, detail=""):
        checks.append({"check": label, "ok": bool(ok), "detail": detail})
        print(("  [ok ] " if ok else "  [FAIL] ") + label + (f": {detail}" if detail else ""), flush=True)

    try:
        client = desktop.bind(args.cli.resolve(), "OPAD simulation agent")
        write, call = client.write, client.call
        started = time.monotonic()
        plane = lambda o, n=(0, 0, 1): {"origin": list(o), "normal": list(n)}
        box = lambda o, l, w, h, n=(0, 0, 1): {"plane": plane(o, n), "length": l, "width": w, "height": h}
        cyl = lambda o, d, h, n=(0, 0, 1): {"plane": plane(o, n), "diameter": d, "height": h}
        # 1. The engine in one batch: every later step refers to the bodies earlier steps made.
        steps = [
            {"id": "block", "command": "feature", "arguments": {"kind": "box", "name": "Cylinder block", "color": [0.55, 0.57, 0.6], "inputs": box((150, 0, -40), 180, 70, 80)}},
            {"id": "bore", "command": "feature", "arguments": {"kind": "cylinder", "inputs": dict(cyl((59, 0, 0), 46, 190, (1, 0, 0)), operation="cut", targets=["@{block#/body_ids/0}"])}},
            {"id": "crank", "command": "feature", "arguments": {"kind": "cylinder", "name": "Crankshaft", "color": [0.75, 0.6, 0.25], "inputs": cyl((0, 0, -30), 20, 30)}},
            {"id": "web", "command": "feature", "arguments": {"kind": "box", "inputs": dict(box((0, 0, 0), 2 * R + 20, 24, 8), operation="join", targets=["@{crank#/body_ids/0}"])}},
            {"id": "pin", "command": "feature", "arguments": {"kind": "cylinder", "inputs": dict(cyl((R, 0, 8), 12, 10), operation="join", targets=["@{crank#/body_ids/0}"])}},
            {"id": "rod", "command": "feature", "arguments": {"kind": "box", "name": "Connecting rod", "color": [0.4, 0.45, 0.55], "inputs": box((R + L / 2, 0, 8), L, 10, 6)}},
            {"id": "eye", "command": "feature", "arguments": {"kind": "cylinder", "inputs": dict(cyl((R, 0, 8), 22, 6), operation="join", targets=["@{rod#/body_ids/0}"])}},
            {"id": "piston", "command": "feature", "arguments": {"kind": "cylinder", "name": "Piston", "color": [0.82, 0.83, 0.86], "inputs": cyl((R + L - 20, 0, 0), 44, 40, (1, 0, 0))}},
        ]
        batch = write("model_batch", steps=steps)
        made = {s["id"]: s["result"] for s in batch["result"]["steps"]}
        block, crank, rod, piston = (made[k]["body_ids"][0] for k in ("block", "crank", "rod", "piston"))
        check("model_batch builds the engine's four parts in one call", all(b["valid"] for b in batch["changes"]["bodies"]))
        write("appearance", targets=[block], opacity=0.3)  # the crank, rod and piston seen through the block
        # 2. The joints: the block held, the crank in its main bearing, rod on the crank pin, piston on the rod and in the bore.
        z = {"z": [0, 0, 1]}
        write("joint", kind="ground", part=block, at={"origin": [0, 0, 0], **z})
        jc = write("joint", kind="revolute", name="Main bearing", base=block, part=crank, at={"origin": [0, 0, 0], **z})["result"]["id"]
        write("joint", kind="revolute", name="Big end", base=crank, part=rod, at={"origin": [R, 0, 0], **z})
        write("joint", kind="revolute", name="Gudgeon pin", base=rod, part=piston, at={"origin": [R + L, 0, 0], **z})
        write("joint", kind="cylindrical", name="Bore", base=block, part=piston, at={"origin": [R + L, 0, 0], "z": [1, 0, 0]})
        mech = call("mechanism")["result"]
        check("the mechanism has one degree of freedom", mech["dof"] == 1, f"dof {mech['dof']}, redundant {mech.get('redundant')}")
        # 3. A motion study: two turns at 3000 rpm, the gudgeon pin traced.
        mo = write("study", kind="motion", name="Two turns", settings={"duration": 0.04, "frames": 361, "drivers": [{"joint": jc, "speed": 18000}],
                                                                        "traces": [{"part": piston, "point": [R + L, 0, 0], "name": "Pin"}]},
                   series=["Pin x", "Main bearing rotation"], samples=361)["result"]
        ser = {s["name"]: s["v"] for s in mo["series"]}
        x, th = ser["Pin x"], ser["Main bearing rotation"]
        worst = max(abs(xi - (R * math.cos(math.radians(a)) + math.sqrt(L * L - (R * math.sin(math.radians(a))) ** 2))) for xi, a in zip(x, th))
        check("the piston follows x = r cos t + sqrt(l^2 - r^2 sin^2 t)", worst < 1e-5, f"worst {worst:.2e} mm")
        check("its stroke is 2 r", abs(max(x) - min(x) - 2 * R) < 1e-6, f"{max(x) - min(x):.6f} mm")
        # 4. A dynamic study: held at 3000 rpm by a speed motor, no gravity; the motor's work is the kinetic energy's change.
        dy = write("study", kind="dynamic", name="3000 rpm", settings={"duration": 0.04, "frames": 241, "step": 2e-5, "gravity": False,
                                                                        "drivers": [{"joint": jc, "mode": "speed", "value": 18000}]},
                   series=["Main bearing motor power", "Kinetic energy", "Main bearing motor torque"], samples=241)["result"]
        ser = {s["name"]: s["v"] for s in dy["series"]}
        P, KE, t = ser["Main bearing motor power"], ser["Kinetic energy"], dy["t"]
        work, worst = 0.0, 0.0
        for i in range(1, len(t)):
            work += 0.5 * (P[i] + P[i - 1]) * (t[i] - t[i - 1])
            worst = max(worst, abs(KE[i] - KE[0] - work))
        check("energy balance: motor work = change of kinetic energy", worst / (max(KE) - min(KE)) < 0.02, f"{worst / (max(KE) - min(KE)):.2%} of its swing")
        tq = ser["Main bearing motor torque"]
        notes.append(f"motor torque to hold 3000 rpm: {min(tq) / 1000:.2f} to {max(tq) / 1000:.2f} N.m; kinetic energy {min(KE):.3f}-{max(KE):.3f} J")
        # 5. Turn the crank where the user sees it, and look.
        write("joint_set", values={jc: 120})
        image = client.raw("viewport_image", fit=True, view="iso", width=1280, height=800)
        for item in image.get("content", []):
            if item.get("type") == "image":
                (args.out / "live_viewport.png").write_bytes(base64.b64decode(item["data"]))
        check("viewport_image returns the live model", (args.out / "live_viewport.png").exists())
        time.sleep(3)  # the window catches up (browser, timeline) before the screen is taken
        subprocess.run(["import", "-window", "root", str(args.out / "live_screen.png")], check=False)
        saved = args.out / "live_engine.opad"
        write("save", path=str(saved.resolve()))
        check("the document is saved with its joints, poses and studies", saved.exists())
        notes.append(f"{time.monotonic() - started:.1f} s of agent time")
    finally:
        desktop.close()
    (args.out / "live_report.json").write_text(json.dumps({"checks": checks, "notes": notes}, indent=1))
    for n in notes:
        print("  note:", n)
    failed = [c for c in checks if not c["ok"]]
    print(f"{len(checks) - len(failed)} checks passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
