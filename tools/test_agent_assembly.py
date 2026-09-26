"""Acceptance (TODO 10 B18): an agent builds a phone-style assembly through the live bridge.

usage: python tools/test_agent_assembly.py APP CLI

38 bodies in 10 components: patterns (screws, cameras, microphones, antennas), multi-profile extrudes (chips, speakers,
buttons), cuts (camera holes, the USB port), a join (the camera bump) and a fillet picked by rule (B7). Names, colours
and components are set in the steps that make the bodies (B14) or follow from their source (copies), and one bulk
recolour uses a whole-list reference (B15). Checks: no rejected call, rename/appearance/reparent steps at most 10% of
all steps, every body with its intended name, colour and component, and no interference but the intended one (B17).
"""
import argparse
from pathlib import Path
import time
import uuid

from test_live_agent import Desktop

GREY, DARK, GLASS, BLACK = [0.6, 0.6, 0.62], [0.15, 0.15, 0.18], [0.7, 0.85, 0.95], [0.05, 0.05, 0.08]
BLUE, WHITE, GREEN, CHIP, STEEL = [0.2, 0.3, 0.7], [0.95, 0.95, 0.95], [0.1, 0.45, 0.2], [0.1, 0.1, 0.1], [0.8, 0.8, 0.82]
SILVER, SIM, MIC, SPEAKER, LENS, GOLD = [0.75, 0.75, 0.78], [0.5, 0.5, 0.52], [0.3, 0.3, 0.3], [0.25, 0.25, 0.25], [0.02, 0.02, 0.02], [0.85, 0.65, 0.2]

COMPONENTS = ["Frame", "Back", "Screen", "Battery", "Board", "Ports", "Audio", "Camera", "Buttons", "Antennas"]
EXPECTED = {  # name: (component, colour)
    "Frame": ("Frame", GREY), "Back plate": ("Back", DARK), "Cover glass": ("Screen", GLASS), "Display": ("Screen", BLACK),
    "Battery cell": ("Battery", BLUE), "Battery label": ("Battery", WHITE), "Main board": ("Board", GREEN), "Vibration motor": ("Board", SILVER),
    **{f"Chip {n}": ("Board", CHIP) for n in range(1, 7)},
    **{name: ("Board", STEEL) for name in ["Screw"] + [f"Screw {n}" for n in range(2, 9)]},
    "USB connector": ("Ports", SILVER), "SIM tray": ("Ports", SIM), "Mic": ("Audio", MIC), "Mic 2": ("Audio", MIC),
    "Speaker 1": ("Audio", SPEAKER), "Speaker 2": ("Audio", SPEAKER),
    "Camera": ("Camera", LENS), "Camera 2": ("Camera", LENS), "Camera 3": ("Camera", LENS),
    **{f"Button {n}": ("Buttons", GREY) for n in range(1, 4)},
    **{name: ("Antennas", GOLD) for name in ["Antenna", "Antenna 2", "Antenna 3", "Antenna 4"]},
}


def xy(z):
    return {"origin": [0, 0, z], "normal": [0, 0, 1]}


def at(x, y, z):
    return {"origin": [x, y, z], "normal": [0, 0, 1]}


COMPONENT_IDS = {}  # filled once the first batch made them: references reach only within one batch


def part(kind, name, component, colour, inputs, **extra):
    parent = COMPONENT_IDS.get(component, f"@{{{component.lower()}#/component_id}}")
    arguments = {"kind": kind, "name": name, "inputs": inputs, "color": colour, "parent": parent}
    arguments.update(extra)
    return {"command": "feature", "arguments": arguments}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    args = parser.parse_args()
    root = (Path("build/agent-assembly") / str(uuid.uuid4())).resolve()
    desktop = Desktop(args.app.resolve(), args.cli.resolve(), root)
    client = None
    rejected, all_steps = [], []
    try:
        client = desktop.bind(args.cli.resolve(), "Assembly agent")
        started = time.monotonic()

        def call(tool, **arguments):
            result = client.raw(tool, **arguments)
            if result.get("isError"):
                rejected.append((tool, result["structuredContent"]["error"]))
                raise AssertionError(f"{tool} rejected: {result['structuredContent']['error']}")
            return result["structuredContent"]

        def batch(steps):
            numbered = []
            for i, step in enumerate(steps):
                numbered.append(dict(step, id=step.get("id", f"s{len(all_steps) + i}")))
            all_steps.extend(numbered)
            revision = call("live_state")["result"]["revision"]
            done = call("model_batch", steps=numbered, expected_revision=revision, request_id=str(uuid.uuid4()), verbosity="compact")
            return {s["id"]: s["result"] for s in done["result"]["steps"]}

        # Components, the frame (a rounded outline and its inner offset), the back plate with its camera bump and
        # holes, the USB port, the cover glass and the display.
        a = batch([{"id": c.lower(), "command": "component", "arguments": {"name": c}} for c in COMPONENTS] + [
            {"id": "ring", "command": "sketch", "arguments": {"name": "Frame outline", "plane": xy(0), "geometry": {"shapes": [
                {"kind": "rounded_rect", "picks": [[0, 0], [70, 150]], "options": {"radius": 8}}, {"kind": "offset", "options": {"shape": 0, "distance": -2}}]}}},
            dict(part("extrude", "Frame", "Frame", GREY, {"profiles": [{"sketch": "@{ring#/sketch_id}", "at": [1, 75]}], "distance": 8}), id="frame_part"),
            {"id": "plate", "command": "sketch", "arguments": {"name": "Back outline", "plane": xy(0), "geometry": {"shapes": [
                {"kind": "rounded_rect", "picks": [[2, 2], [68, 148]], "options": {"radius": 6}}]}}},
            dict(part("extrude", "Back plate", "Back", DARK, {"profiles": [{"sketch": "@{plate#/sketch_id}"}], "distance": 1}), id="back_part"),
            {"id": "bump", "command": "sketch", "arguments": {"name": "Camera bump", "plane": xy(0), "geometry": {"shapes": [
                {"kind": "rounded_rect", "picks": [[5, 108], [19, 142]], "options": {"radius": 3}}]}}},
            {"id": "bumped", "command": "feature", "arguments": {"kind": "extrude", "name": "Camera bump", "inputs": {
                "profiles": [{"sketch": "@{bump#/sketch_id}"}], "distance": 1, "flip": True, "operation": "join", "targets": ["@{back_part#/body_ids/0}"]}}},
            {"id": "lenses", "command": "sketch", "arguments": {"name": "Camera holes", "plane": xy(-1), "geometry": {"shapes": [
                {"kind": "circle", "picks": [[12, y]], "options": {"radius": 4}} for y in (115, 125, 135)]}}},
            {"id": "holes", "command": "feature", "arguments": {"kind": "extrude", "name": "Camera holes", "inputs": {
                "profiles": [{"sketch": "@{lenses#/sketch_id}"}], "distance": 2, "operation": "cut", "targets": ["@{back_part#/body_ids/0}"]}}},
            {"id": "port", "command": "feature", "arguments": {"kind": "box", "name": "USB port", "inputs": {
                "plane": at(35, 1, 1), "length": 9, "width": 2.2, "height": 3, "operation": "cut", "targets": ["@{frame_part#/body_ids/0}"]}}},
            {"id": "glass_outline", "command": "sketch", "arguments": {"name": "Glass outline", "plane": xy(8), "geometry": {"shapes": [
                {"kind": "rounded_rect", "picks": [[0, 0], [70, 150]], "options": {"radius": 8}}]}}},
            part("extrude", "Cover glass", "Screen", GLASS, {"profiles": [{"sketch": "@{glass_outline#/sketch_id}"}], "distance": 1}),
            {"id": "panel", "command": "sketch", "arguments": {"name": "Display outline", "plane": xy(7), "geometry": {"shapes": [
                {"kind": "rounded_rect", "picks": [[2.5, 2.5], [67.5, 147.5]], "options": {"radius": 5.5}}]}}},
            part("extrude", "Display", "Screen", BLACK, {"profiles": [{"sketch": "@{panel#/sketch_id}"}], "distance": 1})])
        COMPONENT_IDS.update({c: a[c.lower()]["component_id"] for c in COMPONENTS})
        # Inside: the battery with its label (meant to sit 0.05 mm into it) and a rounded top, the board with six
        # chips and eight screws, the motor, microphones, speakers, the USB connector and the SIM tray.
        b = batch([
            dict(part("box", "Battery cell", "Battery", BLUE, {"plane": at(35, 50, 1), "length": 55, "width": 70, "height": 4}), id="cell"),
            dict(part("box", "Battery label", "Battery", WHITE, {"plane": at(35, 50, 4.95), "length": 40, "width": 30, "height": 0.2}), id="label"),
            {"command": "feature", "arguments": {"kind": "fillet", "name": "Battery edges", "inputs": {"edges": [{"body": "@{cell#/body_ids/0}", "kind": "edge",
                "select": {"curve": "line", "at_plane": {"axis": "z", "value": 5}}, "expect": 4}], "radius": 0.5}}},
            part("box", "Main board", "Board", GREEN, {"plane": at(35, 120, 1), "length": 50, "width": 50, "height": 1}),
            {"id": "chips_sketch", "command": "sketch", "arguments": {"name": "Chips", "plane": xy(2), "geometry": {"shapes": [
                {"kind": "rect2", "picks": [[x, y], [x + 8, y + 10]]} for y in (104, 126) for x in (17, 31, 45)]}}},
            part("extrude", "Chips", "Board", CHIP, {"profiles": [{"sketch": "@{chips_sketch#/sketch_id}"}], "distance": 1}, body_name="Chip {n}"),
            dict(part("cylinder", "Screw", "Board", STEEL, {"plane": at(14, 100, 2), "diameter": 2, "height": 1}), id="screw"),
            {"id": "screws", "command": "feature", "arguments": {"kind": "pattern_rect", "name": "Screws", "inputs": {
                "bodies": ["@{screw#/body_ids/0}"], "axis": {"base": "x"}, "count": 4, "spacing": 14, "second": True, "axis2": {"base": "y"}, "count2": 2, "spacing2": 40}}},
            part("cylinder", "Vibration motor", "Board", SILVER, {"plane": at(55, 90, 1), "diameter": 6, "height": 3}),
            dict(part("cylinder", "Mic", "Audio", MIC, {"plane": at(20, 5, 1), "diameter": 2, "height": 1}), id="mic"),
            {"command": "feature", "arguments": {"kind": "pattern_rect", "name": "Mics", "inputs": {"bodies": ["@{mic#/body_ids/0}"], "axis": {"base": "x"}, "count": 2, "spacing": 30}}},
            {"id": "grilles", "command": "sketch", "arguments": {"name": "Speakers", "plane": xy(1), "geometry": {"shapes": [
                {"kind": "rect2", "picks": [[12, 88], [24, 92]]}, {"kind": "rect2", "picks": [[26, 88], [34, 92]]}]}}},
            part("extrude", "Speakers", "Audio", SPEAKER, {"profiles": [{"sketch": "@{grilles#/sketch_id}"}], "distance": 2}, body_name="Speaker {n}"),
            part("box", "USB connector", "Ports", SILVER, {"plane": at(35, 5.5, 1), "length": 9, "width": 7, "height": 3}),
            part("box", "SIM tray", "Ports", SIM, {"plane": at(4.5, 60, 1), "length": 4, "width": 12, "height": 1}),
            # One bulk recolour of every screw, the original included (B15: a whole-list reference into targets).
            {"id": "steel", "command": "appearance", "arguments": {"targets": "@{screws#/all_body_ids/*}", "color": STEEL}}])
        # Outside: the cameras in their holes, the buttons on the right side, the antennas in the corners.
        c = batch([
            dict(part("cylinder", "Camera", "Camera", LENS, {"plane": at(12, 115, -1), "diameter": 7.8, "height": 2}), id="camera"),
            {"command": "feature", "arguments": {"kind": "pattern_rect", "name": "Cameras", "inputs": {"bodies": ["@{camera#/body_ids/0}"], "axis": {"base": "y"}, "count": 3, "spacing": 10}}},
            {"id": "keys", "command": "sketch", "arguments": {"name": "Buttons", "plane": {"origin": [70, 0, 0], "normal": [1, 0, 0]}, "geometry": {"shapes": [
                {"kind": "rect2", "picks": [[100, 3], [110, 5]]}, {"kind": "rect2", "picks": [[85, 3], [97, 5]]}, {"kind": "rect2", "picks": [[70, 3], [82, 5]]}]}}},
            part("extrude", "Buttons", "Buttons", GREY, {"profiles": [{"sketch": "@{keys#/sketch_id}"}], "distance": 1}, body_name="Button {n}"),
            dict(part("box", "Antenna", "Antennas", GOLD, {"plane": at(6, 6, 1), "length": 4, "width": 4, "height": 1}), id="antenna"),
            {"command": "feature", "arguments": {"kind": "pattern_rect", "name": "Antennas", "inputs": {"bodies": ["@{antenna#/body_ids/0}"],
                "axis": {"base": "x"}, "count": 2, "spacing": 58, "second": True, "axis2": {"base": "y"}, "count2": 2, "spacing2": 138}}}])
        picture = client.raw("viewport_image", views=["iso", "top", "front", "right"], edges=True, width=960, height=720)
        png = next(item["data"] for item in picture["content"] if item["type"] == "image")
        (root / "assembly.png").write_bytes(__import__("base64").b64decode(png))
        # Every body: its name, colour and component.
        nodes = call("context", section="nodes", limit=100)["result"]["items"]
        by_id = {n["id"]: n for n in nodes}
        bodies = [n for n in nodes if n["type"] == "body"]
        names = sorted(n["name"] for n in bodies)
        assert names == sorted(EXPECTED), (sorted(set(names) ^ set(EXPECTED)), len(names))
        for n in bodies:
            component, colour = EXPECTED[n["name"]]
            assert by_id[n["parent"]]["name"] == component, (n["name"], by_id.get(n["parent"], {}).get("name"))
            assert n["color"] and all(abs(p - q) < 1e-9 for p, q in zip(n["color"], colour)), (n["name"], n["color"], colour)
        bookkeeping = sum(1 for s in all_steps if s["command"] in ("rename", "appearance", "reparent"))
        label = next(n["id"] for n in bodies if n["name"] == "Battery label")
        cell = next(n["id"] for n in bodies if n["name"] == "Battery cell")
        # No interference but the one meant: the label sits 0.05 mm into the cell.
        overlaps = call("validate", checks=["interference"], limit=50)["result"]["interference"]
        pairs = {frozenset((f["a"], f["b"])) for f in overlaps["items"] if f["kind"] == "interference"}
        clean = call("validate", checks=["interference"], ignore=[[label, cell]])["result"]["interference"]
        print(f"Assembly: {len(bodies)} bodies in {len(COMPONENTS)} components, {len(all_steps)} steps, {bookkeeping} rename/appearance/reparent, "
              f"{len(rejected)} rejected, overlaps {overlaps['interferences']}, {time.monotonic() - started:.1f} s", flush=True)
        assert not rejected and len(bodies) == 38
        assert bookkeeping <= 0.1 * len(all_steps)
        assert pairs == {frozenset((label, cell))}, [(f["a_name"], f["b_name"], f["volume_mm3"]) for f in overlaps["items"]]
        assert clean["status"] == "clear", clean
        print("Assembly through the live bridge: names, colours and components where bodies are made, patterns, rules, interference: PASS", flush=True)
    finally:
        if client:
            client.close()
        desktop.close()


if __name__ == "__main__":
    main()
