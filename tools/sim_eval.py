#!/usr/bin/env python3
"""Mechanisms, physics and structures built through OPAD's MCP server and checked against textbook results.

Each scenario talks to `opad-cli mcp` (the headless MCP server an AI agent uses) exactly as an agent would: it makes the
parts from features (gears with the gear feature, the rest from boxes, cylinders and cuts), joins them with joints and
relations, drives them with joint_set and studies, loads them for CalculiX, and compares every number it gets back with
the closed-form answer an engineering textbook gives. The documents it builds stay in the output folder, with pictures
(and GIFs of the motions when Pillow is installed).

    python3 tools/sim_eval.py build/linux/bin/opad-cli [--out DIR] [--only NAME[,NAME]] [--no-images] [--list]

Exit status 0 when every check passed. report.json and report.md in the output folder list each check with what was
expected, what came back and the error.
"""
import argparse
import json
import math
import pathlib
import subprocess
import sys
import tempfile
import time
import traceback

PI = math.pi
G = 9.80665


class McpError(Exception):
    pass


class Mcp:
    """A persistent stdio MCP client of opad-cli mcp."""

    def __init__(self, cli, env=None):
        self.stderr = tempfile.TemporaryFile(mode="w+")
        self.process = subprocess.Popen([cli, "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
                                        text=True, encoding="utf-8", env=env)
        self.serial = 0
        self.calls = 0
        init = self.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "sim-eval", "version": "1"}})
        assert "tools" in init["capabilities"], init
        self.process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        self.process.stdin.flush()
        self.tools = {t["name"]: t for t in self.request("tools/list")["tools"]}
        self.guide = self.request("resources/read", {"uri": "opad://guide/agent"})["contents"][0]["text"]

    def request(self, method, params=None):
        self.serial += 1
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.serial, "method": method, "params": params or {}}) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            self.stderr.seek(0)
            raise McpError("the MCP server stopped: " + self.stderr.read()[-2000:])
        response = json.loads(line)
        if "error" in response:
            raise McpError(json.dumps(response["error"]))
        return response["result"]

    def call(self, tool, **args):
        self.calls += 1
        result = self.request("tools/call", {"name": tool, "arguments": args})
        if result.get("isError"):
            message = result.get("structuredContent", {}).get("error", {}).get("message") or result["content"][0]["text"]
            raise McpError(f"{tool}: {message}")
        return json.loads(result["content"][0]["text"])

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=30)


class Scenario:
    """One mechanism or part: the document, its checks and what it shows."""

    def __init__(self, mcp, out, name, images):
        self.mcp, self.out, self.name, self.images = mcp, out, name, images
        self.doc = str(out / f"{name}.opad")
        self.checks, self.notes, self.pictures = [], [], []
        mcp.call("new", doc=self.doc)

    # ---- modelling helpers (all through MCP)
    def run(self, tool, **args):
        return self.mcp.call(tool, doc=self.doc, **args)

    def feature(self, kind, name, inputs, **extra):
        return self.run("feature", kind=kind, name=name, inputs=inputs, **extra)

    def body(self, kind, name, inputs, color=None, **extra):
        if color:
            extra["color"] = color
        return self.feature(kind, name, inputs, **extra)["body_ids"][0]

    def box(self, name, at, l, w, h, normal=(0, 0, 1), centered=True, color=None, **extra):
        return self.body("box", name, {"plane": {"origin": list(at), "normal": list(normal)}, "length": l, "width": w, "height": h, "centered": centered}, color, **extra)

    def cyl(self, name, at, d, h, normal=(0, 0, 1), color=None, **extra):
        return self.body("cylinder", name, {"plane": {"origin": list(at), "normal": list(normal)}, "diameter": d, "height": h}, color, **extra)

    def cut_cyl(self, target, at, d, h, normal=(0, 0, 1)):
        self.feature("cylinder", "Hole", {"plane": {"origin": list(at), "normal": list(normal)}, "diameter": d, "height": h, "operation": "cut", "targets": [target]})

    def join_cyl(self, target, at, d, h, normal=(0, 0, 1)):
        self.feature("cylinder", "Boss", {"plane": {"origin": list(at), "normal": list(normal)}, "diameter": d, "height": h, "operation": "join", "targets": [target]})

    def join_box(self, target, at, l, w, h, normal=(0, 0, 1), centered=True):
        self.feature("box", "Block", {"plane": {"origin": list(at), "normal": list(normal)}, "length": l, "width": w, "height": h, "centered": centered,
                                      "operation": "join", "targets": [target]})

    def gear(self, name, at, module, teeth, width, color=None, **inputs):
        args = {"plane": {"origin": list(at), "normal": [0, 0, 1]}, "module": module, "teeth": teeth, "width": width}
        args.update(inputs)
        return self.body("gear", name, args, color)

    def joint(self, **args):
        return self.run("joint", **args)

    def material(self, nodes, material):
        self.run("part_properties", targets=nodes, set={"material": material})

    # ---- checks
    def check(self, label, got, want, tol, rel=True):
        got, want = float(got), float(want)
        err = abs(got - want) / (abs(want) if rel and want else 1.0)
        ok = bool(err <= tol)
        self.checks.append({"check": label, "got": got, "want": want, "error": err, "tolerance": tol, "relative": rel, "ok": ok})
        return ok

    def expect(self, label, ok, detail=""):
        self.checks.append({"check": label, "ok": bool(ok), "detail": detail})
        return ok

    def note(self, text):
        self.notes.append(text)

    # ---- pictures
    def picture(self, file, **args):
        if not self.images:
            return None
        path = str(self.out / file)
        args.setdefault("width", 960)
        args.setdefault("height", 640)
        args.setdefault("shading", "smooth")
        self.run("render", out=path, **args)
        self.pictures.append(file)
        return path

    def animation(self, file, frames, duration_ms=60, **args):
        """frames: a list of render arguments (joints or study frames); a GIF when Pillow is there, else the PNGs."""
        if not self.images:
            return
        paths = []
        for i, f in enumerate(frames):
            a = dict(args)
            a.update(f)
            p = self.out / f"{pathlib.Path(file).stem}-{i:03d}.png"
            a.setdefault("width", 640)
            a.setdefault("height", 440)
            a.setdefault("shading", "smooth")
            self.run("render", out=str(p), **a)
            paths.append(p)
        try:
            from PIL import Image
        except ImportError:
            self.pictures += [p.name for p in paths]
            return
        images = [Image.open(p).convert("P", palette=Image.ADAPTIVE, colors=128) for p in paths]
        images[0].save(self.out / file, save_all=True, append_images=images[1:], duration=duration_ms, loop=0, optimize=True)
        for p in paths:
            p.unlink()
        self.pictures.append(file)


def overlap(s, skip=()):
    """The largest overlap (mm3) the validate interference check finds, pairs touching a body in skip left out."""
    v = s.run("validate", checks=["interference"])
    worst = 0.0
    for item in v.get("interference", {}).get("items", []):
        if item.get("kind") != "interference" or item.get("a") in skip or item.get("b") in skip:
            continue
        worst = max(worst, item.get("volume_mm3", 0.0))
    return worst


def values(s):
    """Every joint's first coordinate now, by id (from mechanism)."""
    return {j["id"]: (j.get("values") or [None])[0] for j in s.run("mechanism")["joints"]}


def series(report, name):
    for s in report.get("series", []):
        if s["name"] == name:
            return s["v"]
    raise KeyError(f"no series {name}: " + ", ".join(s["name"] for s in report.get("series", [])))


def edge_rule(body, z, r, axis="z", near=None):
    """A circular edge of radius r on the plane axis = z, picked by what it is (a rule), not its number; near (x, y)
    narrows it to the one around that point."""
    select = {"curve": "circle", "at_plane": {"axis": axis, "value": z}, "radius_min": r - 0.01, "radius_max": r + 0.01}
    if near:
        select["bounds"] = {"min": [near[0] - r - 1, near[1] - r - 1, z - 1], "max": [near[0] + r + 1, near[1] + r + 1, z + 1]}
    return {"body": body, "kind": "edge", "select": select, "expect": 1}


def face_rule(body, axis, value, surface="plane"):
    return {"body": body, "kind": "face", "select": {"surface": surface, "at_plane": {"axis": axis, "value": value}}, "expect": 1}


SCENARIOS = []


def scenario(name, title):
    def wrap(fn):
        SCENARIOS.append((name, title, fn))
        return fn
    return wrap


# ======================================================================================================== mechanisms
@scenario("control_panel_knobs", "Knobs snapped into a panel: revolute joints with limits, a locked knob, a detent spring, gravity")
def control_panel_knobs(s):
    # A 160 x 60 x 4 aluminium panel with three 8 mm holes; three steel knobs (8 mm shaft, 28 mm grip cap with ribs) built
    # away from it and snapped in by their shaft rims. The panel is grounded; with gravity on, the joints hold the knobs.
    panel = s.box("Panel", (0, 0, 0), 160, 60, 4, color=[0.75, 0.77, 0.8])
    holes = [(-50, 0), (0, 0), (50, 0)]
    for x, y in holes:
        s.cut_cyl(panel, (x, y, -1), 8, 6)
    s.material([panel], "aluminium-6061")
    s.joint(kind="ground", part=panel, at={"origin": [0, 0, 0], "z": [0, 0, 1]})
    knobs, joints = [], []
    for i, (x, y) in enumerate(holes):
        k = s.cyl(f"Knob {i + 1}", (200 + 60 * i, 150, 20), 8, 14, color=[0.15, 0.15, 0.17])
        s.join_cyl(k, (200 + 60 * i, 150, 34), 28, 12)
        for r in range(6):  # grip ribs
            a = 2 * PI * r / 6
            s.join_box(k, (200 + 60 * i + 14 * math.cos(a), 150 + 14 * math.sin(a), 34), 4, 4, 12)
        s.material([k], "steel")
        # The shaft's bottom rim onto the hole's rim on the panel's top face, sunk 4 mm (the panel's thickness) in.
        j = s.joint(kind="revolute", name=f"Knob {i + 1} turn", at=edge_rule(panel, 4, 4, near=(x, y)), at_part=edge_rule(k, 20, 4), offset=-4,
                    limits={"rotation": [0, 270]})
        knobs.append(k)
        joints.append(j)
        bottom = s.run("context", section="nodes")
        s.expect(f"knob {i + 1} snapped onto its hole", j.get("part") == k and j.get("base") == panel and len(j.get("moved", [])) == 1, json.dumps(j)[:300])
    mech = s.run("mechanism")
    s.check("degrees of freedom: three knobs turning", mech["dof"], 3, 0, rel=False)
    # Knob 3 locked: it cannot be turned; knob 1 turned to 135 deg, knob 2 asked for 300 deg stops at its 270 limit.
    s.joint(id=joints[2]["id"], locked=True)
    s.run("joint_set", values={joints[0]["id"]: 135})
    held = s.run("joint_set", values={joints[1]["id"]: 300})
    s.expect("a value past the limit is held at it and said so", "limit" in json.dumps(held.get("notes", [])), json.dumps(held))
    vals = values(s)
    s.check("knob 1 at 135 deg", vals[joints[0]["id"]], 135, 1e-9, rel=False)
    s.check("knob 2 held at 270 deg", vals[joints[1]["id"]], 270, 1e-9, rel=False)
    try:
        s.run("joint_set", values={joints[2]["id"]: 10})
        s.expect("the locked knob refuses to turn", False)
    except McpError as e:
        s.expect("the locked knob refuses to turn", "locked" in str(e), str(e))
    # The knob's shaft axis stays on the hole's axis through any turn (a pure rotation).
    p = s.run("properties", node=knobs[0])
    s.check("knob 1's centre of mass stays over its hole (x)", p["center_of_mass"][0], -50, 1e-6, rel=False)
    # Dynamics: gravity on, a detent spring on knob 1 (rest 135 deg), knob 1 released at 165 deg: it rocks about 135 deg with
    # period 2 pi sqrt(I / k), and no knob falls.
    s.run("joint_set", values={joints[0]["id"]: 165})
    k_nmm_deg = 50.0
    s.joint(id=joints[0]["id"], spring={"stiffness": k_nmm_deg, "rest": 135})
    study = s.run("study", kind="dynamic", name="Detent spring", settings={"duration": 0.3, "frames": 601, "step": 5e-5, "gravity": True},
                  series=["Knob 1 turn rotation"], samples=601)
    theta = series(study, "Knob 1 turn rotation")
    t = study["t"]
    swing = [v - 135 for v in theta]
    ups = [t[i - 1] + (t[i] - t[i - 1]) * (-swing[i - 1]) / (swing[i] - swing[i - 1]) for i in range(1, len(swing)) if swing[i - 1] < 0 <= swing[i]]
    # I about the shaft axis from the knob's solid (the study reports each part's inertia about its centre; the axis runs
    # through the centre): T = 2 pi sqrt(I / k).
    I = study["parts"]["Knob 1"]["inertia_kg_mm2"][8] * 1e-6  # kg.m2
    k = k_nmm_deg * 180 / PI / 1000  # N.m/rad
    T = (ups[-1] - ups[0]) / (len(ups) - 1)
    s.check("detent oscillation period = 2 pi sqrt(I / k) (s)", T, 2 * PI * math.sqrt(I / k), 2e-3)
    amp = max(abs(v) for v in swing[len(swing) // 2:])
    s.check("undamped: it keeps swinging 30 deg about the detent", amp, 30, 0.01)
    s.check("the panel and knobs stay put under gravity (joints hold them)", study["outputs"]["Knob 2 turn rotation"]["max"] - study["outputs"]["Knob 2 turn rotation"]["min"], 0, 1e-6, rel=False)
    s.picture("control_panel.png", view="iso")
    s.animation("control_panel_detent.gif", [{"study": {"id": study["id"], "t": tt}} for tt in [i * 0.01 for i in range(0, 31)]], view="iso")


@scenario("engine_slider_crank", "Single-cylinder engine: crank, connecting rod, piston in its bore; travel, acceleration and energy")
def engine_slider_crank(s):
    r, l = 30.0, 100.0
    block = s.box("Cylinder block", (150, 0, -40), 180, 70, 80, color=[0.55, 0.57, 0.6])  # x 60..240
    s.cut_cyl(block, (59, 0, 0), 46, 190, normal=(1, 0, 0))   # the bore along +X
    crank = s.cyl("Crankshaft", (0, 0, -30), 20, 30, color=[0.75, 0.6, 0.25])
    s.join_box(crank, (0, 0, 0), 2 * r + 20, 24, 8)                 # web
    s.join_box(crank, (-18, 0, 0), 30, 50, 8)                      # counterweight
    s.join_cyl(crank, (r, 0, 8), 12, 10)                           # crank pin
    rod = s.box("Connecting rod", (r + l / 2, 0, 8), l, 10, 6, color=[0.4, 0.45, 0.55])
    s.join_cyl(rod, (r, 0, 8), 22, 6)
    s.join_cyl(rod, (r + l, 0, 8), 16, 6)
    s.cut_cyl(rod, (r, 0, 7), 12.2, 8)
    s.cut_cyl(rod, (r + l, 0, 7), 8.2, 8)
    piston = s.cyl("Piston", (r + l - 20, 0, 0), 44, 40, normal=(1, 0, 0), color=[0.82, 0.83, 0.86])
    s.feature("box", "Skirt slot", {"plane": {"origin": [r + l, 0, 4], "normal": [0, 0, 1]}, "length": 30, "width": 46, "height": 12, "operation": "cut", "targets": [piston]})
    s.material([crank, rod], "steel")
    s.material([piston], "aluminium-6061")
    z = {"z": [0, 0, 1]}
    s.joint(kind="ground", part=block, at={"origin": [0, 0, 0], **z})
    jc = s.joint(kind="revolute", name="Main bearing", base=block, part=crank, at={"origin": [0, 0, 0], **z})["id"]
    s.joint(kind="revolute", name="Big end", base=crank, part=rod, at={"origin": [r, 0, 0], **z})
    s.joint(kind="revolute", name="Gudgeon pin", base=rod, part=piston, at={"origin": [r + l, 0, 0], **z})
    jp = s.joint(kind="cylindrical", name="Bore", base=block, part=piston, at={"origin": [r + l, 0, 0], "z": [1, 0, 0]})["id"]
    m = s.run("mechanism")
    s.check("one degree of freedom (the crank)", m["dof"], 1, 0, rel=False)
    # Kinematics: two revolutions at 3000 rpm; the gudgeon pin's path against x = r cos t + sqrt(l^2 - r^2 sin^2 t).
    rpm = 3000.0
    w = rpm * 2 * PI / 60
    dur = 2 * 60 / rpm
    mo = s.run("study", kind="motion", name="Two revolutions", settings={"duration": dur, "frames": 721, "drivers": [{"joint": jc, "speed": rpm * 6}],
                                                                          "traces": [{"part": piston, "point": [r + l, 0, 0], "name": "Pin"}]},
               series=["Pin x", "Main bearing rotation", "Pin speed"], samples=721)
    x, th = series(mo, "Pin x"), series(mo, "Main bearing rotation")
    worst = max(abs(xi - (r * math.cos(math.radians(a)) + math.sqrt(l * l - (r * math.sin(math.radians(a))) ** 2))) for xi, a in zip(x, th))
    s.check("piston pin on x = r cos t + sqrt(l^2 - r^2 sin^2 t) at every frame (mm)", worst, 0, 1e-5, rel=False)
    s.check("stroke 2r", max(x) - min(x), 2 * r, 1e-6)
    # Peak piston acceleration at top dead centre: r w^2 (1 + r / l).
    acc = mo["joints"]["Bore"]["translation"]["max_acceleration"]
    s.check("peak piston acceleration r w^2 (1 + r/l) (finite differences, 0.5 deg frames)", acc, r * w * w * (1 + r / l), 2e-3)
    # Dynamics at 3000 rpm, no gravity, no friction: what the motor puts in is the parts' kinetic energy (energy balance).
    dy = s.run("study", kind="dynamic", name="3000 rpm", settings={"duration": dur, "frames": 241, "step": 2e-5, "gravity": False,
                                                                     "drivers": [{"joint": jc, "mode": "speed", "value": rpm * 6}]},
               series=["Main bearing motor power", "Kinetic energy", "Main bearing motor torque"], samples=241)
    P, KE, t = series(dy, "Main bearing motor power"), series(dy, "Kinetic energy"), dy["t"]
    work, worst, ke_span = 0.0, 0.0, max(KE) - min(KE)
    for i in range(1, len(t)):
        work += 0.5 * (P[i] + P[i - 1]) * (t[i] - t[i - 1])
        worst = max(worst, abs((KE[i] - KE[0]) - work))
    s.check("energy balance: motor work = change of kinetic energy (of its swing)", worst / ke_span, 0, 0.02, rel=False)
    tq = series(dy, "Main bearing motor torque")
    s.note(f"motor torque to keep 3000 rpm swings between {min(tq) / 1000:.2f} and {max(tq) / 1000:.2f} N.m (the reciprocating masses)")
    s.note(f"kinetic energy swings {min(KE):.3f}-{max(KE):.3f} J")
    s.run("appearance", target=block, opacity=0.3)  # the crank, rod and piston seen through the block
    s.picture("engine.png", view="iso")
    s.animation("engine.gif", [{"joints": {jc: a}} for a in range(0, 360, 10)], view="top")


@scenario("gearbox_two_stage", "Two-stage spur gearbox with involute gears: 9:1, no tooth interference through a turn, torque through it")
def gearbox_two_stage(s):
    m = 2.0
    z1, z2, z3, z4 = 18, 54, 17, 51  # 3 x 3: no pinion under the 17 teeth a 20 deg involute needs without undercut
    c1, c2 = m * (z1 + z2) / 2, m * (z3 + z4) / 2
    housing = s.box("Housing plate", (c1 / 2 + c2 / 2, 0, -12), c1 + c2 + 80, 110, 6, color=[0.6, 0.62, 0.66])
    s.joint(kind="ground", part=housing, at={"origin": [0, 0, -12], "z": [0, 0, 1]})
    g1 = s.gear("Input pinion", (0, 0, 0), m, z1, 12, color=[0.85, 0.55, 0.2], bore=6)
    s.join_cyl(g1, (0, 0, -6), 6, 30)  # input shaft
    g2 = s.gear("Intermediate wheel", (c1, 0, 0), m, z2, 10, color=[0.3, 0.55, 0.85], phase=180 + 180 / z2, bore=10)
    # The intermediate pinion on the same body, above the wheel, tooth 0 along +X towards the output.
    s.feature("gear", "Intermediate pinion", {"plane": {"origin": [c1, 0, 10], "normal": [0, 0, 1]}, "module": m, "teeth": z3, "width": 14, "operation": "join", "targets": [g2]})
    g4 = s.gear("Output wheel", (c1 + c2, 0, 10), m, z4, 14, color=[0.35, 0.75, 0.4], phase=180 + 180 / z4, bore=12)
    s.material([g1, g2, g4], "steel")
    z = [0, 0, 1]
    j1 = s.joint(kind="revolute", name="Input", base=housing, part=g1, at={"origin": [0, 0, 0], "z": z})["id"]
    j2 = s.joint(kind="revolute", name="Intermediate", base=housing, part=g2, at={"origin": [c1, 0, 0], "z": z})["id"]
    j3 = s.joint(kind="revolute", name="Output", base=housing, part=g4, at={"origin": [c1 + c2, 0, 0], "z": z})["id"]
    s.joint(kind="gear", name="Stage 1", joints=[j1, j2], teeth=[z1, z2])
    s.joint(kind="gear", name="Stage 2", joints=[j2, j3], teeth=[z3, z4])
    s.check("one degree of freedom", s.run("mechanism")["dof"], 1, 0, rel=False)
    # Turn the input one tooth pitch at a time for 2 teeth and look for overlapping teeth (validate's interference check).
    worst = 0.0
    for k in range(0, 13):
        s.run("joint_set", values={j1: k * 360 / z1 / 6})
        worst = max(worst, overlap(s, skip=(housing,)))
    s.check("no tooth overlap through two tooth pitches (largest overlap mm3)", worst, 0, 1e-3, rel=False)
    s.run("joint_set", values={j1: 720})
    out = values(s)[j3]
    s.check("output = input / 9 (two input turns)", out, 720 / 9, 1e-9)
    # Torque: input driven at 900 rpm, 20 N.m load on the output: the input motor gives 20 / 9 N.m once at speed.
    s.joint(id=j3, drive={"mode": "torque", "value": -20000})
    dy = s.run("study", kind="dynamic", name="Loaded", settings={"duration": 0.2, "frames": 81, "gravity": False, "drivers": [{"joint": j1, "mode": "speed", "value": 900 * 6}]},
               series=["Input motor torque", "Stage 2 torque", "Stage 1 torque"], samples=81)
    tq = series(dy, "Input motor torque")
    s.check("input torque = output torque / 9 at steady speed (N.mm)", tq[-1], 20000 / 9, 5e-3)
    s.check("stage 2 tooth torque on the output = the load (N.mm)", series(dy, "Stage 2 torque")[-1], 20000, 5e-3)
    s.picture("gearbox.png", view="iso")
    s.animation("gearbox.gif", [{"joints": {j1: a}} for a in range(0, 90, 3)], view="top")


@scenario("gear_undercut", "Involute limits: a 12-tooth pinion (under the 17-tooth minimum) cuts into its wheel, an 18-tooth one does not")
def gear_undercut(s):
    m = 2.0
    results = {}
    for z1, x in ((12, 0.0), (18, 120.0)):
        z2 = 3 * z1
        a = m * (z1 + z2) / 2
        p = s.gear(f"Pinion {z1}", (x, 0, 0), m, z1, 8, color=[0.85, 0.55, 0.2])
        w = s.gear(f"Wheel {z2}", (x + a, 0, 0), m, z2, 8, color=[0.3, 0.55, 0.85], phase=180 + 180 / z2)
        jp = s.joint(kind="revolute", part=p, at={"origin": [x, 0, 0], "z": [0, 0, 1]})["id"]
        jw = s.joint(kind="revolute", part=w, at={"origin": [x + a, 0, 0], "z": [0, 0, 1]})["id"]
        s.joint(kind="gear", joints=[jp, jw], teeth=[z1, z2])
        results[z1] = (p, w, jp)
    worst = {12: 0.0, 18: 0.0}
    for k in range(0, 9):
        s.run("joint_set", values={results[12][2]: k * 360 / 12 / 8, results[18][2]: k * 360 / 18 / 8})
        v = s.run("validate", checks=["interference"])
        for item in v.get("interference", {}).get("items", []):
            if item.get("kind") != "interference":
                continue
            for z, (p, w, _) in results.items():
                if {item["a"], item["b"]} == {p, w}:
                    worst[z] = max(worst[z], item["volume_mm3"])
    s.expect("the 12-tooth pinion interferes with its wheel (it would need undercut or a profile shift)", worst[12] > 1e-3, f"{worst[12]:.4f} mm3")
    s.check("the 18-tooth pinion meshes clear through a tooth pitch (mm3)", worst[18], 0, 1e-3, rel=False)
    s.note(f"largest overlap: 12 teeth {worst[12]:.4f} mm3, 18 teeth {worst[18]:.5f} mm3 (minimum teeth without undercut at 20 deg: 2 / sin^2 20 = 17.1)")


@scenario("planetary_gearset", "Planetary gearset with involute teeth: sun 21, three planets 18, ring 57 fixed; carrier ratio 3.714")
def planetary_gearset(s):
    # Every gear at 17 teeth or more (no undercut at 20 deg), zr = zs + 2 zp, and (zs + zr) / 3 whole: three planets equally spaced.
    m, zs, zp, zr = 1.5, 21, 18, 57
    a = m * (zs + zp) / 2
    sun = s.gear("Sun", (0, 0, 0), m, zs, 10, color=[0.9, 0.6, 0.2], bore=6)
    ring = s.gear("Ring", (0, 0, 0), m, zr, 10, color=[0.6, 0.62, 0.66], type="internal", rim=m * zr + 16, phase=180 / zr)
    carrier = s.cyl("Carrier", (0, 0, -6), 2 * a + 14, 4, color=[0.35, 0.45, 0.75])
    s.material([sun, ring, carrier], "steel")
    z = [0, 0, 1]
    jc = s.joint(kind="revolute", name="Carrier", part=carrier, at={"origin": [0, 0, 0], "z": z})["id"]
    js = s.joint(kind="revolute", name="Sun", part=sun, at={"origin": [0, 0, 0], "z": z})["id"]
    jr = s.joint(kind="revolute", name="Ring", part=ring, at={"origin": [0, 0, 0], "z": z}, locked=True)["id"]
    planets = []
    for i in range(3):
        phi = 2 * PI * i / 3
        x, y = a * math.cos(phi), a * math.sin(phi)
        # Phase so it meshes with the sun at phase 0: pi + pi/zp + phi (1 + zs/zp).
        phase = math.degrees(PI + PI / zp + phi * (1 + zs / zp))
        p = s.gear(f"Planet {i + 1}", (x, y, 0), m, zp, 10, color=[0.3, 0.7, 0.45], phase=phase, bore=5)
        s.join_cyl(p, (x, y, -2), 5, 2)  # its pin into the carrier
        s.material([p], "steel")
        jp = s.joint(kind="revolute", name=f"Planet {i + 1} pin", base=carrier, part=p, at={"origin": [x, y, 0], "z": z})["id"]
        s.joint(kind="gear", name=f"Sun-planet {i + 1}", joints=[js, jp], teeth=[zs, zp], carrier=carrier)
        s.joint(kind="gear", name=f"Ring-planet {i + 1}", joints=[jr, jp], teeth=[zr, zp], internal=True, carrier=carrier)
        planets.append((p, jp))
    s.check("one degree of freedom (with the ring held)", s.run("mechanism")["dof"], 1, 0, rel=False)
    worst = 0.0
    for k in range(0, 9):
        s.run("joint_set", values={jc: k * 2.5})
        worst = max(worst, overlap(s, skip=(carrier,)))  # the carrier plate under the gears is not a tooth mesh
    s.check("no tooth overlap anywhere in the set through 20 deg of carrier (mm3)", worst, 0, 2e-3, rel=False)
    s.run("joint_set", values={jc: 100})
    vals = values(s)
    s.check("sun = (1 + Zr/Zs) x carrier (Willis, ring fixed)", vals[js], (1 + zr / zs) * 100, 1e-9)
    s.check("planet on its pin = -(Zr/Zp) x carrier", vals[planets[0][1]], -zr / zp * 100, 1e-9)
    s.picture("planetary.png", view="top")
    s.animation("planetary.gif", [{"joints": {jc: k * 3}} for k in range(0, 40)], view="top")


@scenario("rack_and_pinion", "Rack and pinion steering: involute pinion on a rack, travel per turn, no overlap")
def rack_and_pinion(s):
    m, z = 2.0, 18
    r = m * z / 2
    p = PI * m
    pinion = s.gear("Pinion", (0, 0, 0), m, z, 16, color=[0.85, 0.55, 0.2], bore=8)
    # The rack's pitch line at y = -r, a tooth on the line of centres (the pinion has a gap there).
    K = 7
    rack = s.gear("Rack", (-(K + 0.5) * p, -r, 0), m, 16, 16, color=[0.55, 0.6, 0.65], type="rack", height=8)
    s.material([pinion, rack], "steel")
    jp = s.joint(kind="revolute", name="Steering column", part=pinion, at={"origin": [0, 0, 0], "z": [0, 0, 1]})["id"]
    jr = s.joint(kind="slider", name="Rack guide", part=rack, at={"origin": [0, -r, 0], "z": [1, 0, 0]}, limits={"translation": [-60, 60]})["id"]
    s.joint(kind="rack_pinion", name="Mesh", joints=[jp, jr], radius=r)
    worst = 0.0
    for k in range(0, 11):
        s.run("joint_set", values={jp: k * 6})
        worst = max(worst, overlap(s))
    s.check("no tooth overlap through 60 deg (mm3)", worst, 0, 1e-3, rel=False)
    s.run("joint_set", values={jp: 180})
    vals = values(s)
    s.check("rack travel per half turn = pi m z / 2", vals[jr], PI * m * z / 2, 1e-9)
    held = s.run("joint_set", values={jp: 720})
    vals = values(s)
    s.check("the rack's end stop (60 mm) holds the steering: rack at its limit (mm)", vals[jr], 60, 1e-4, rel=False)
    s.check("... and the pinion stopped where the rack met it: 60 / r rad (deg)", vals[jp], math.degrees(60 / r), 1e-6)
    s.note("joint_set says: " + "; ".join(held.get("notes", [])))
    s.picture("rack_pinion.png", view="iso")
    s.animation("rack_pinion.gif", [{"joints": {jp: a}} for a in range(-150, 151, 10)], view="top")


@scenario("screw_jack", "Screw jack: a lead screw lifting a platform; torque m g L / 2 pi, travel per turn")
def screw_jack(s):
    lead = 6.0
    base = s.box("Base", (0, 0, -10), 120, 120, 10, color=[0.5, 0.52, 0.55])
    screw = s.cyl("Screw", (0, 0, 0), 20, 160, color=[0.75, 0.75, 0.78])
    s.feature("coil", "Thread", {"plane": {"origin": [0, 0, 10], "normal": [0, 0, 1]}, "diameter": 20, "pitch": lead, "turns": 22, "section": "square", "size": 2.4,
                                 "operation": "join", "targets": [screw]})
    platform = s.box("Platform", (0, 0, 60), 140, 140, 12, color=[0.25, 0.45, 0.75])
    s.cut_cyl(platform, (0, 0, 59), 24, 14)
    s.join_box(platform, (0, 0, 72), 60, 60, 60)  # the load block
    s.material([screw, base], "steel")
    s.material([platform], "steel")
    s.joint(kind="ground", part=base, at={"origin": [0, 0, 0], "z": [0, 0, 1]})
    js = s.joint(kind="revolute", name="Screw bearing", base=base, part=screw, at={"origin": [0, 0, 0], "z": [0, 0, 1]})["id"]
    jp = s.joint(kind="slider", name="Platform guide", base=base, part=platform, at={"origin": [0, 0, 60], "z": [0, 0, 1]}, limits={"translation": [-40, 80]})["id"]
    s.joint(kind="lead_screw", name="Thread", joints=[js, jp], lead=lead)
    s.run("joint_set", values={js: 3600})
    vals = values(s)
    s.check("ten turns lift the platform 10 x lead", vals[jp], 10 * lead, 1e-9)
    s.run("joint_set", values={js: 0})
    dy = s.run("study", kind="dynamic", name="Lifting", settings={"duration": 1.0, "frames": 51, "gravity": True, "drivers": [{"joint": js, "mode": "speed", "value": 360}]},
               series=["Screw bearing motor torque"], samples=51)
    mass = dy["parts"]["Platform"]["mass_kg"]
    want = mass * G * lead / 1000 / (2 * PI) * 1000  # N.mm
    s.check("lifting torque = m g L / 2 pi (N.mm, steady speed)", series(dy, "Screw bearing motor torque")[-1], want, 5e-3)
    s.note(f"platform and load {mass:.2f} kg: {want:.0f} N.mm at the screw")
    s.picture("screw_jack.png", view="iso")
    s.animation("screw_jack.gif", [{"joints": {js: a}} for a in range(0, 3600 * 2, 240)], view="front")


@scenario("four_bar_linkage", "Crank-rocker four-bar with a coupler point: Freudenstein's equation and the coupler curve")
def four_bar_linkage(s):
    a, b, c, d = 30.0, 90.0, 70.0, 80.0  # crank, coupler, rocker, ground (Grashof: s + l <= p + q)
    def rocker(t2):
        ax, ay = a * math.cos(t2), a * math.sin(t2)
        dx, dy = d - ax, -ay
        L = math.hypot(dx, dy)
        al = (b * b - c * c + L * L) / (2 * L)
        h = math.sqrt(b * b - al * al)
        px, py = ax + al * dx / L, ay + al * dy / L
        bx, by = px - h * dy / L, py + h * dx / L
        return bx, by
    t0 = math.radians(40)
    ax, ay = a * math.cos(t0), a * math.sin(t0)
    bx, by = rocker(t0)
    ground = s.box("Ground link", (d / 2, 0, -8), d + 30, 16, 6, color=[0.5, 0.52, 0.55])
    crank = s.box("Crank", (ax / 2, ay / 2, 0), 8, 8, 4, color=[0.85, 0.55, 0.2])
    coupler = s.box("Coupler", ((ax + bx) / 2, (ay + by) / 2, 4), 8, 8, 4, color=[0.3, 0.55, 0.85])
    rock = s.box("Rocker", ((bx + d) / 2, by / 2, 8), 8, 8, 4, color=[0.35, 0.75, 0.4])
    z = [0, 0, 1]
    s.joint(kind="ground", part=ground, at={"origin": [0, 0, 0], "z": z})
    jc = s.joint(kind="revolute", name="Crank pivot", base=ground, part=crank, at={"origin": [0, 0, 0], "z": z})["id"]
    s.joint(kind="revolute", name="A", base=crank, part=coupler, at={"origin": [ax, ay, 0], "z": z})
    s.joint(kind="revolute", name="B", base=coupler, part=rock, at={"origin": [bx, by, 0], "z": z})
    jr = s.joint(kind="revolute", name="Rocker pivot", base=ground, part=rock, at={"origin": [d, 0, 0], "z": z})["id"]
    s.check("Grubler: one degree of freedom", s.run("mechanism")["dof"], 1, 0, rel=False)
    # The coupler point C, 40 mm off the coupler's middle, at right angles: its curve against the closed form.
    ux, uy = (bx - ax) / b, (by - ay) / b
    cx, cy = (ax + bx) / 2 - 40 * uy, (ay + by) / 2 + 40 * ux
    mo = s.run("study", kind="motion", name="One crank turn", settings={"duration": 1, "frames": 181, "drivers": [{"joint": jc, "to": 360}],
                                                                          "traces": [{"part": coupler, "point": [cx, cy, 0], "name": "C"}]},
               series=["C x", "C y", "Crank pivot rotation", "Rocker pivot rotation"], samples=181)
    worst = 0.0
    for X, Y, th in zip(series(mo, "C x"), series(mo, "C y"), series(mo, "Crank pivot rotation")):
        t2 = t0 + math.radians(th)
        Ax, Ay = a * math.cos(t2), a * math.sin(t2)
        Bx, By = rocker(t2)
        Ux, Uy = (Bx - Ax) / b, (By - Ay) / b
        worst = max(worst, math.hypot(X - ((Ax + Bx) / 2 - 40 * Uy), Y - ((Ay + By) / 2 + 40 * Ux)))
    s.check("coupler curve = closed form at every frame (mm)", worst, 0, 1e-5, rel=False)
    rk = series(mo, "Rocker pivot rotation")
    s.note(f"rocker swings {max(rk) - min(rk):.3f} deg")
    t4 = [math.atan2(rocker(t0 + math.radians(th))[1], rocker(t0 + math.radians(th))[0] - d) for th in series(mo, "Crank pivot rotation")]
    swing = math.degrees(max(t4) - min(t4))
    s.check("rocker swing angle = closed form (deg)", max(rk) - min(rk), swing, 1e-4)
    s.picture("four_bar.png", view="top")
    s.animation("four_bar.gif", [{"joints": {jc: k * 10}} for k in range(36)], view="top")


@scenario("falling_and_sliding", "Dynamics with contacts: a block on a 25 deg ramp slides with a = g (sin t - mu cos t)")
def falling_and_sliding(s):
    tilt, mu = 25.0, 0.2
    st, ct = math.sin(math.radians(tilt)), math.cos(math.radians(tilt))
    n = (0, -st, ct)
    s.box("Ramp", (0, 0, 0), 160, 600, 10, normal=n, color=[0.6, 0.62, 0.66])
    # The block on the ramp's top face (10 mm up its normal), a hair above it.
    org = (0, n[1] * 10.2, n[2] * 10.2)
    block = s.box("Block", org, 40, 40, 40, normal=n, color=[0.85, 0.45, 0.2])
    s.material([block], "steel")
    dy = s.run("study", kind="dynamic", name="Slide", settings={"duration": 0.5, "frames": 51, "free": [block], "contacts": [block], "friction": mu, "step": 2e-4,
                                                                 "traces": [{"part": block, "point": [org[0] + 20 * n[0], org[1] + 20 * n[1], org[2] + 20 * n[2]], "name": "Centre"}]},
               series=["Centre y", "Centre z", "Contacts"], samples=51)
    s.expect("contacts found", bool(max(series(dy, "Contacts")) >= 1))
    # Distance down the slope (the ramp's in-plane direction (0, -cos t, -sin t)); a fitted to s = s0 + v0 t + a t^2 / 2 after
    # the first 0.05 s.
    t, Y, Z = dy["t"], series(dy, "Centre y"), series(dy, "Centre z")
    pts = [(ti, -(y - Y[0]) * ct - (z - Z[0]) * st) for ti, y, z in zip(t, Y, Z) if ti >= 0.05]
    import numpy as np
    A = np.array([[1, ti, ti * ti / 2] for ti, _ in pts])
    coef = np.linalg.lstsq(A, np.array([d for _, d in pts]), rcond=None)[0]
    a = G * (st - mu * ct) * 1000
    s.check("sliding acceleration a = g (sin t - mu cos t) (mm/s2)", float(coef[2]), a, 0.03)
    s.note(f"slid {pts[-1][1]:.1f} mm in 0.5 s; fitted a = {coef[2]:.0f} mm/s2, closed form {a:.0f} mm/s2")
    s.picture("slide_end.png", study={"id": dy["id"], "t": 0.5}, view="right")


# ======================================================================================================== structures
@scenario("plate_with_hole", "Plate with a hole in tension: peak stress against Heywood's Kt (net section)")
def plate_with_hole(s):
    W, d, t, L, F = 100.0, 20.0, 5.0, 300.0, 10000.0
    plate = s.box("Plate", (0, 0, 0), L, W, t, color=[0.75, 0.77, 0.8])
    s.cut_cyl(plate, (0, 0, -1), d, t + 2)
    s.material([plate], "steel")
    s.run("load", kind="fixed", on=[face_rule(plate, "x", -L / 2)])
    s.run("load", kind="pressure", on=[face_rule(plate, "x", L / 2)], value=-F / (W * t))
    st = s.run("study", kind="static", name="Tension", settings={"mesh_size": 2.5})
    kt = 2 + (1 - d / W) ** 3  # Heywood, net section
    net = F / ((W - d) * t)
    s.check("peak von Mises at the hole = Kt_net x F / ((W - d) t) (MPa)", st["max_von_mises_MPa"], kt * net, 0.05)
    s.note(f"peak {st['max_von_mises_MPa']:.1f} MPa at {st['max_von_mises_at']}, Heywood {kt * net:.1f} MPa; mesh {st['nodes']} nodes")
    # Far from the hole the stress is F / (W t).
    s.picture("plate_hole_stress.png", study={"id": st["id"], "field": "von_mises"}, view="top")


@scenario("bolted_bracket", "Bolted L-bracket: a preloaded M10 bolt and a 1.5 kN side load; bolt stress, safety factors, first frequencies")
def bolted_bracket(s):
    base = s.box("Base plate", (0, 0, -10), 120, 80, 10, color=[0.55, 0.57, 0.6])
    s.cut_cyl(base, (0, 0, -11), 10.5, 12)
    bracket = s.box("Bracket", (0, 0, 0), 60, 50, 8, color=[0.3, 0.55, 0.85])
    s.join_box(bracket, (26, 0, 8), 8, 50, 60)          # the upright flange
    s.cut_cyl(bracket, (0, 0, -1), 10.5, 10)
    bolt = s.cyl("Bolt", (0, 0, -16), 16, 6, color=[0.8, 0.8, 0.82])  # nut under the base plate
    s.join_cyl(bolt, (0, 0, -10), 10, 18)                            # shank through plate and bracket
    s.join_cyl(bolt, (0, 0, 8), 16, 6)                               # head on the bracket
    s.material([base, bracket, bolt], "steel")
    s.run("load", kind="fixed", on=[face_rule(base, "z", -10)], case="Service")
    s.run("load", kind="bolt_preload", on=[bolt], value=15000, case="Service")
    top = {"body": bracket, "kind": "face", "select": {"surface": "plane", "at_plane": {"axis": "z", "value": 68}}, "expect": 1}
    s.run("load", kind="force", on=[top], vector=[-1500, 0, 0], case="Service")
    st = s.run("study", kind="static", name="Service load", settings={"case": "Service", "bodies": [base, bracket, bolt], "mesh_size": 3})
    b = st["bolts"][0]
    s.check("bolt section area = pi d^2 / 4 (mm2; the mesh's faceted section of the shank)", b["section_area_mm2"], PI * 25, 0.02)
    s.note(f"bolt nominal {b['nominal_stress_MPa']:.1f} MPa, shank {b.get('axial_stress_MPa', 0):.1f} MPa; peak {st['max_von_mises_MPa']:.0f} MPa at {st['max_von_mises_at']}")
    s.check("bolt shank stress = preload / area, the side load adding a little (MPa)", b["axial_stress_MPa"], 15000 / (PI * 25), 0.1)
    s.expect("the support carries the side load", abs(st["reactions_N"]["Fixed 1"][0] - 1500) < 15, json.dumps(st["reactions_N"]))
    s.note("the peak is at the bonded under-head corner, a stress singularity of a linear model that glues the parts; the shank and the plates away from it are what the safety factors should be read on")
    s.note("safety factors: " + ", ".join(f"{k} {v['safety_factor']:.2f}" for k, v in st["bodies"].items() if v.get("safety_factor")))
    s.picture("bracket_stress.png", study={"id": st["id"], "field": "von_mises"}, view="iso")
    s.picture("bracket_deformed.png", study={"id": st["id"], "field": "displacement"}, view="front")
    # The bracket alone, its foot held: first bending frequency of the flange.
    s.run("load", kind="fixed", on=[face_rule(bracket, "z", 0)], case="Modal")
    mo = s.run("study", kind="modal", name="Bracket modes", settings={"case": "Modal", "bodies": [bracket], "modes": 4, "mesh_size": 3})
    s.note("bracket modes: " + ", ".join(f"{f:.0f}" for f in mo["frequencies_Hz"]) + " Hz")
    # The upright flange as a cantilever 60 x 50 x 8 on a 8 mm foot: f1 about (1.875^2 / 2 pi) sqrt(EI / rho A L^4).
    E, rho, bw, h, Lf = 210000.0, 7.85e-9, 50.0, 8.0, 64.0
    f1 = 1.875104 ** 2 / (2 * PI) * math.sqrt(E * bw * h ** 3 / 12 / (rho * bw * h * Lf ** 4))
    s.check("flange's first bending mode near the cantilever formula (the foot is not a rigid wall)", mo["frequencies_Hz"][0], f1, 0.15)
    s.picture("bracket_mode1.png", study={"id": mo["id"], "mode": 1}, view="iso")


@scenario("printed_bracket", "3D-printed PETG L-bracket from a PrusaSlicer profile: standing on its foot against lying on its side")
def printed_bracket(s):
    # 60 x 30 x 5 foot held on its underside, a 5 mm upright 50 mm tall pushed sideways at its top with 50 N: the upright
    # bends, its stress running up it. Printed standing on the foot that stress pulls the layers apart; lying on its side
    # it runs along the roads.
    foot = s.box("Bracket", (0, 0, 0), 60, 30, 5, color=[0.95, 0.55, 0.2])
    s.join_box(foot, (27.5, 0, 5), 5, 30, 50)
    # A 5 mm fillet in the inside corner, as a printed bracket wants (a sharp one is a stress singularity: its peak grows
    # as the mesh shrinks, and the safety factor would be read there).
    corner = {"body": foot, "kind": "edge", "select": {"curve": "line", "bounds": {"min": [24.9, -16, 4.9], "max": [25.1, 16, 5.1]}}, "expect": 1}
    s.feature("fillet", "Corner fillet", {"edges": [corner], "radius": 5})
    s.run("load", kind="fixed", on=[face_rule(foot, "z", 0)])
    s.run("load", kind="force", on=[face_rule(foot, "z", 55)], vector=[-50, 0, 0])
    profile = s.out / "petg_0.2mm.ini"  # what PrusaSlicer exports (File > Export > Export Config)
    profile.write_text("# generated by PrusaSlicer\nlayer_height = 0.2\nperimeters = 3\ntop_solid_layers = 5\nbottom_solid_layers = 4\n"
                       "fill_density = 30%\nfill_pattern = gyroid\nperimeter_extrusion_width = 0.45\nnozzle_diameter = 0.4\n"
                       "filament_type = PETG\nextrusion_multiplier = 1\n")
    runs = {}
    for label, up in (("standing", [0, 0, 1]), ("on its side", [0, 1, 0])):
        st = s.run("study", kind="static", name=f"Printed {label}", settings={"mesh_size": 1.5, "print": {"profile": str(profile), "build_direction": up}})
        runs[label] = st
        p = st["print"]["Bracket"]
        s.note(f"{label}: {p['printed_mass_g']:.1f} g, safety factor {p['min_safety_factor']:.2f}, fails {p['fails']} at layer {p['weakest_layer']}"
               f" ({p['weakest_height_mm']:.1f} mm up)")
        s.picture(f"printed_bracket_{label.replace(' ', '_')}.png", study={"id": st["id"], "field": "failure_index"}, view="iso")
    stand, side = runs["standing"]["print"]["Bracket"], runs["on its side"]["print"]["Bracket"]
    s.expect("the profile's settings are used (PETG, 3 walls, gyroid 30 %)", stand["material"] == "PETG" and stand["walls"] == 3 and stand["pattern"] == "gyroid"
             and abs(stand["infill"] - 30) < 1e-9, json.dumps({k: stand[k] for k in ("material", "walls", "pattern", "infill")}))
    s.expect("standing, the upright fails between its layers", stand["fails"].startswith("between layers"), stand["fails"])
    s.expect("lying on its side it is stronger", side["min_safety_factor"] > 1.3 * stand["min_safety_factor"],
             f"{side['min_safety_factor']:.2f} against {stand['min_safety_factor']:.2f}")
    # The printed mass: each region's volume times what is printed there (a road fills 1 - (1 - pi/4) h / w of its box).
    phi = 1 - (1 - PI / 4) * 0.2 / 0.45
    reg = stand["regions"]
    volume = 60 * 30 * 5 + 5 * 30 * 50 + 5 ** 2 * (1 - PI / 4) * 30
    s.check("the regions add up to the bracket's volume (mm3)", sum(r["volume_mm3"] for r in reg.values()), volume, 1e-3)
    want = ((reg["wall"]["volume_mm3"] + reg["top/bottom"]["volume_mm3"]) * phi + reg["infill"]["volume_mm3"] * 0.3 * phi) * 1.27 / 1000
    s.check("printed mass from the regions, their fill and PETG's 1.27 g/cm3 (g)", stand["printed_mass_g"], want, 0.005)
    # The upright as a cantilever: its root's bending stress M c / I against the walls' between-layer strength. The
    # root's inside corner raises it: the model's safety factor is at most this.
    M, I, c = 50 * 50, 30 * 5 ** 3 / 12, 2.5
    s.expect("standing, the safety factor is no more than the between-layer strength over M c / I", stand["min_safety_factor"] <= 25 * phi / (M * c / I) * 1.05,
             f"{stand['min_safety_factor']:.2f} against {25 * phi / (M * c / I):.2f}")


@scenario("heatsink_fan", "A 30 W chip on a finned aluminium heatsink: still air, a 60 mm fan and an 80 mm fan; warming up over time")
def heatsink_fan(s):
    # 60 x 60 x 5 mm base, ten 1.5 mm fins 30 mm tall along X; a 20 x 20 mm chip under it giving 30 W.
    hs = s.box("Heatsink", (0, 0, 0), 60, 60, 5, centered=False, color=[0.75, 0.77, 0.8])
    gap = (60 - 10 * 1.5) / 9
    for i in range(10):
        s.run("feature", kind="box", inputs={"plane": {"origin": [0, i * (1.5 + gap), 5], "normal": [0, 0, 1]}, "length": 60, "width": 1.5, "height": 30,
                                             "centered": False, "operation": "join", "targets": [hs]})
    chip = s.box("Chip", (20, 20, -2), 20, 20, 2, centered=False, color=[0.15, 0.15, 0.17])
    s.material([hs], "aluminium-6061")
    s.run("load", kind="heat", on=[chip], value=30, case="Fan")
    s.run("load", kind="fan", on=[hs], fan="60x15", vector=[1, 0, 0], case="Fan")
    s.run("load", kind="heat", on=[chip], value=30, case="Bigger fan")
    s.run("load", kind="fan", on=[hs], fan="80x25", vector=[1, 0, 0], case="Bigger fan")
    s.run("load", kind="heat", on=[chip], value=5, case="Still air")
    s.run("load", kind="convection", on=[hs], h="natural", case="Still air")
    settings = {"mesh_size": 3, "bodies": [hs, chip], "materials": {chip: {"k": 150, "cp": 700, "density": 2.33, "name": "Silicon"}}}
    runs = {}
    for case in ("Fan", "Bigger fan", "Still air"):
        st = s.run("study", kind="thermal", name=case, settings=dict(settings, case=case))
        runs[case] = st
        s.check(f"{case}: the heat reaches the air (W)", st["to_air_W"], st["heat_W"], 0.005)
        s.picture(f"heatsink_{case.lower().replace(' ', '_')}.png", study={"id": st["id"]}, view="iso")
    fan, big = runs["Fan"]["fans"][0], runs["Bigger fan"]["fans"][0]
    s.expect("the heatsink's fins are found: 10 of 1.5 mm, 5 mm apart, 30 mm tall", fan["fins"]["fins"] == 10 and abs(fan["fins"]["gap_mm"] - 5) < 0.01
             and abs(fan["fins"]["fin_height_mm"] - 30) < 0.5, json.dumps(fan["fins"]))
    # The operating point is on the fan's curve: a straight line from shut-off to free flow.
    for f in (fan, big):
        on_curve = f["fan"]["shut_off_Pa"] * (1 - f["flow_m3h"] / f["fan"]["free_flow_m3h"])
        s.check(f"{f['fan']['name']}: the fins' pressure drop is the fan's pressure at its flow (Pa)", f["pressure_Pa"], on_curve, 0.01)
        rho, cp = 101325 / (287.05 * 298.15), 1006.0
        s.check(f"{f['fan']['name']}: the air warms by Q / (rho V cp) (degC)", f["air_rise_C"], f["heat_W"] / (rho * f["flow_m3h"] / 3600 * cp), 0.02)
    s.expect("the bigger fan moves more air and cools better", big["flow_m3h"] > fan["flow_m3h"] and big["thermal_resistance_C_W"] < fan["thermal_resistance_C_W"],
             f"{fan['flow_m3h']:.1f} -> {big['flow_m3h']:.1f} m3/h, {fan['thermal_resistance_C_W']:.2f} -> {big['thermal_resistance_C_W']:.2f} degC/W")
    still = runs["Still air"]
    r_still = (still["bodies"]["Heatsink"]["max_temperature_C"] - 25) / still["heat_W"]
    s.expect("in still air the heatsink holds 3 to 8 degC/W (a 60 mm heatsink's catalogue range)", 3 < r_still < 8, f"{r_still:.2f} degC/W")
    s.note(f"60 mm fan: {fan['flow_m3h']:.1f} m3/h at {fan['pressure_Pa']:.1f} Pa, {fan['channel_velocity_m_s']:.2f} m/s between the fins, h {fan['h_W_m2K']:.0f} W/m2K, "
           f"chip {runs['Fan']['bodies']['Chip']['max_temperature_C']:.1f} degC, heatsink {fan['thermal_resistance_C_W']:.2f} degC/W")
    s.note(f"80 mm fan: {big['flow_m3h']:.1f} m3/h at {big['pressure_Pa']:.1f} Pa, chip {runs['Bigger fan']['bodies']['Chip']['max_temperature_C']:.1f} degC, "
           f"heatsink {big['thermal_resistance_C_W']:.2f} degC/W")
    s.note(f"still air, 5 W: chip {still['bodies']['Chip']['max_temperature_C']:.1f} degC, heatsink {r_still:.2f} degC/W after {still['solves']} solves")
    # Warming up with the 60 mm fan: after the chip's quick first rise (its heat spreading into the base), the whole heatsink
    # warms as one body would: what is left of the rise decays as exp(-t / tau), tau = m c R.
    warm = s.run("study", kind="thermal", name="Warm-up", settings=dict(settings, case="Fan", duration=300, frames=61), series=True, samples=61)
    curve = next(x for x in warm["series"] if x["name"].startswith("Chip"))
    T, t = curve["v"], warm["t"]
    final = runs["Fan"]["bodies"]["Chip"]["max_temperature_C"]
    i1, i2 = t.index(min(t, key=lambda x: abs(x - 150))), len(t) - 1
    tau = (t[i2] - t[i1]) / math.log((final - T[i1]) / (final - T[i2]))
    m = 2.7e3 * (60 * 60 * 5 + 10 * 1.5 * 30 * 60) * 1e-9  # kg of aluminium
    # R as the slow mode sees it: the heatsink's mean temperature over the air's mean, per watt.
    R_mean = (runs["Fan"]["bodies"]["Heatsink"]["mean_temperature_C"] - 25 - fan["air_rise_C"] / 2) / fan["heat_W"]
    tau_lumped = m * 896 * R_mean
    s.check("warm-up: the heatsink's time constant against the lumped m c R, R from its mean temperature (s)", tau, tau_lumped, 0.1)
    s.note(f"warm-up: {T[-1]:.1f} degC after {t[-1]:.0f} s of {final:.1f}; late time constant {tau:.0f} s, m c R = {tau_lumped:.0f} s")
    s.picture("heatsink_warmup_30s.png", study={"id": warm["id"], "t": 30}, view="iso")


@scenario("heatsink_cfd", "The 80 mm fan on the finned heatsink with the air solved (OpenFOAM) against the correlations; streamlines")
def heatsink_cfd(s):
    if not s.run("mechanism").get("engines", {}).get("cfd"):
        s.note("OpenFOAM is not installed: the air solved is skipped")
        return
    hs = s.box("Heatsink", (0, 0, 0), 60, 60, 5, centered=False, color=[0.75, 0.77, 0.8])
    gap = (60 - 10 * 1.5) / 9
    for i in range(10):
        s.run("feature", kind="box", inputs={"plane": {"origin": [0, i * (1.5 + gap), 5], "normal": [0, 0, 1]}, "length": 60, "width": 1.5, "height": 30,
                                             "centered": False, "operation": "join", "targets": [hs]})
    chip = s.box("Chip", (20, 20, -2), 20, 20, 2, centered=False, color=[0.15, 0.15, 0.17])
    s.material([hs], "aluminium-6061")
    s.run("load", kind="heat", on=[chip], value=30, case="Fan")
    s.run("load", kind="fan", on=[hs], fan="80x25", vector=[1, 0, 0], case="Fan")
    settings = {"case": "Fan", "ambient": 25, "bodies": [hs, chip], "materials": {chip: {"k": 150, "cp": 700, "density": 2.33, "name": "Silicon"}}}
    model = s.run("study", kind="thermal", name="Correlations", settings=dict(settings, mesh_size=3))
    cfd = s.run("study", kind="thermal", name="Air solved", settings=dict(settings, air="cfd"))
    mf, cf = model["fans"][0], cfd["fans"][0]
    s.check("the heat put in leaves with the air (W)", cf["heat_to_air_W"], 30, 0.05)
    rho, cp = 101325 / (287.05 * 298.15), 1006.0
    s.check("the air warms by Q / (rho V cp) (degC)", cf["air_rise_C"], 30 / (rho * cf["flow_m3h"] / 3600 * cp), 0.05)
    s.check("the fan's flow against the correlations' operating point (m3/h)", cf["flow_m3h"], mf["flow_m3h"], 0.2)
    s.check("the chip's rise over the air against the correlations (degC)", cfd["bodies"]["Chip"]["max_temperature_C"] - 25,
            model["bodies"]["Chip"]["max_temperature_C"] - 25, 0.2)
    s.expect("streamlines from the inlet", cfd.get("streamlines", 0) > 10, str(cfd.get("streamlines")))
    s.note(f"correlations: {mf['flow_m3h']:.1f} m3/h, chip {model['bodies']['Chip']['max_temperature_C']:.1f} degC; "
           f"CFD: {cf['flow_m3h']:.1f} m3/h at {cf['inlet_static_Pa']:.1f} Pa, chip {cfd['bodies']['Chip']['max_temperature_C']:.1f} degC, "
           f"outlet air {cf['outlet_air_C']:.2f} degC, {cfd['cells']} cells, {cfd['heat_iterations']} heat iterations")
    for w in cfd.get("warnings", []):
        s.note("CFD: " + w)
    s.picture("heatsink_cfd_temperature.png", study={"id": cfd["id"]}, view="iso")
    s.picture("heatsink_cfd_air_speed.png", study={"id": cfd["id"], "field": "air_speed"}, view="iso")


# ======================================================================================================== runner
def write_markdown(report, out):
    """report.md from a report (as report.json keeps it): a table per scenario, its notes and pictures."""
    def short(detail):
        detail = str(detail).replace("|", "/").replace("\n", " ")
        return detail if len(detail) <= 90 else detail[:87] + "..."
    lines = ["# Simulation evaluation", "", "Built and run through `opad-cli mcp` as an AI agent would, each number checked against its textbook value.", ""]
    for e in report["scenarios"]:
        ok = sum(c["ok"] for c in e["checks"])
        lines.append(f"## {e['title']}")
        lines.append("")
        lines.append(f"`{e['document']}`: {ok}/{len(e['checks'])} checks passed, {e['mcp_calls']} MCP calls, {e['seconds']} s" + (f"; **error**: {e['error']}" if e["error"] else ""))
        lines.append("")
        lines.append("| Check | Expected | Got | Error |")
        lines.append("|---|---|---|---|")
        for c in e["checks"]:
            if "got" in c:
                lines.append(f"| {'✅' if c['ok'] else '❌'} {c['check']} | {c['want']:.6g} | {c['got']:.6g} | {c['error']:.2e} |")
            else:
                lines.append(f"| {'✅' if c['ok'] else '❌'} {c['check']} | | | {short(c.get('detail', ''))} |")
        for n in e["notes"]:
            lines.append(f"\n- {n}")
        for p in e["pictures"]:
            lines.append(f"\n![{p}]({p})")
        lines.append("")
    (out / "report.md").write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cli")
    ap.add_argument("--out", default="sim-eval")
    ap.add_argument("--only", default="")
    ap.add_argument("--no-images", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--markdown", action="store_true", help="only write report.md again from OUT/report.json")
    args = ap.parse_args()
    if args.list:
        for name, title, _ in SCENARIOS:
            print(f"{name:24} {title}")
        return 0
    if args.markdown:
        out = pathlib.Path(args.out)
        write_markdown(json.loads((out / "report.json").read_text()), out)
        return 0
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    mcp = Mcp(args.cli)
    engines = mcp.call("mechanism", doc=str(out / "_engines.opad")) if False else None
    report = {"tools": len(mcp.tools), "scenarios": []}
    failed = 0
    for name, title, fn in SCENARIOS:
        if only and name not in only:
            continue
        print(f"== {name}: {title}", flush=True)
        start = time.monotonic()
        calls = mcp.calls
        sc = Scenario(mcp, out, name, not args.no_images)
        error = None
        try:
            fn(sc)
        except Exception as e:  # noqa: BLE001 - a scenario that breaks is reported, the rest run
            error = f"{type(e).__name__}: {e}"
            traceback.print_exc()
        entry = {"name": name, "title": title, "document": pathlib.Path(sc.doc).name, "seconds": round(time.monotonic() - start, 1), "mcp_calls": mcp.calls - calls,
                 "checks": sc.checks, "notes": sc.notes, "pictures": sc.pictures, "error": error}
        bad = [c for c in sc.checks if not c["ok"]]
        failed += len(bad) + (1 if error else 0)
        for c in sc.checks:
            mark = "ok " if c["ok"] else "FAIL"
            extra = f" got {c['got']:.6g} want {c['want']:.6g} err {c['error']:.2e} (tol {c['tolerance']})" if "got" in c else (" " + c.get("detail", "") if not c["ok"] else "")
            print(f"  [{mark}] {c['check']}{extra}")
        for n in sc.notes:
            print(f"  note: {n}")
        if error:
            print(f"  ERROR {error}")
        print(f"  {entry['seconds']} s, {entry['mcp_calls']} MCP calls", flush=True)
        report["scenarios"].append(entry)
    mcp.close()
    (out / "report.json").write_text(json.dumps(report, indent=1))
    write_markdown(report, out)
    print(f"\n{sum(len(e['checks']) for e in report['scenarios']) - failed} checks passed, {failed} failed; report in {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
