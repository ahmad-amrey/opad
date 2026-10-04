"""gui_benches cases of the 2D drawing area (drawing2d); the benches are in app/Drawing2DBench.cpp."""


def dxf(path, layers, entities, linetypes=()):
    """A minimal DXF: layers [(name, aci, flags, extra groups)], entities [(type, layer, groups)] as (code, value) pairs,
    linetypes [(name, dashes)] in the LTYPE table."""
    pairs = [(0, "SECTION"), (2, "HEADER"), (9, "$INSUNITS"), (70, "4"), (0, "ENDSEC"), (0, "SECTION"), (2, "TABLES")]
    if linetypes:
        pairs += [(0, "TABLE"), (2, "LTYPE")]
        for name, dashes in linetypes:
            pairs += [(0, "LTYPE"), (2, name), (73, str(len(dashes)))] + [(49, str(d)) for d in dashes]
        pairs += [(0, "ENDTAB")]
    pairs += [(0, "TABLE"), (2, "LAYER")]
    for name, aci, flags, extra in layers:
        pairs += [(0, "LAYER"), (2, name), (70, str(flags)), (62, str(aci))] + list(extra)
    pairs += [(0, "ENDTAB"), (0, "ENDSEC"), (0, "SECTION"), (2, "ENTITIES")]
    for kind, layer, groups in entities:
        pairs += [(0, kind), (8, layer)] + list(groups)
    pairs += [(0, "ENDSEC"), (0, "EOF")]
    path.write_text("".join(f"{code}\n{value}\n" for code, value in pairs), encoding="ascii")
    return path


def line(layer, x0, y0, x1, y1, color=None):
    return ("LINE", layer, ([(62, str(color))] if color is not None else []) + [(10, x0), (20, y0), (11, x1), (21, y1)])


def solid(layer, x0, y0, x1, y1):
    return ("SOLID", layer, [(10, x0), (20, y0), (11, x1), (21, y0), (12, x0), (22, y1), (13, x1), (23, y1)])


def contrast_file(root, document):
    """Colour 7 (white, the drawing's foreground) as lines, a fill, and a fill with a line in one body; a red line."""
    return dxf(root / "contrast.dxf", [("Lines", 7, 0, ()), ("Fill", 7, 0, ()), ("Mixed", 7, 0, ()), ("Red", 1, 0, ())],
               [line("Lines", 0, 0, 100, 0), solid("Fill", 10, 10, 40, 40), solid("Mixed", 60, 10, 90, 40), line("Mixed", 60, 50, 90, 50),
                line("Red", 0, 60, 100, 60)])


def layers_file(root, document=None):
    """Walls locked, dashed and 0.5 mm; Notes off and not plotted; Old frozen; Plain as it comes. A line on each, and on
    Plain a second one in colour 7 of its own (a layer colour leaves it). The file's DASHED has a dot (in inches, as
    acad.lin's): Walls is drawn with it, not with acad.lin's."""
    return dxf(root / "layers.dxf", [("Walls", 1, 4, [(6, "DASHED"), (370, "50")]), ("Notes", -3, 0, [(290, "0")]), ("Old", 2, 1, ()), ("Plain", 5, 0, ())],
               [line(name, 0, 10 * i, 100, 10 * i + 5) for i, name in enumerate(["Walls", "Notes", "Old", "Plain"])] + [line("Plain", 0, 45, 100, 45, 7)],
               [("CONTINUOUS", []), ("DASHED", [0.5, -0.25, 0, -0.25])])


def layers_document(root, document):
    """The same drawing imported into an .opad (layer states are view ops: an editable document)."""
    return document("layers", ("import", "--file", str(layers_file(root)), "--center", "true"))


def room_file(root, document=None):
    """The Area tool's: a 100 x 50 room split at x = 60 into two cells, every wall meeting the others at its ends; a second
    room at x = 200 whose dividing wall (x = 240) meets the long walls in their middles; a 50 x 40 outline at x = 400 whose
    four lines run 5 past each other's ends."""
    walls = [(0, 0, 60, 0), (60, 0, 100, 0), (100, 0, 100, 50), (100, 50, 60, 50), (60, 50, 0, 50), (0, 50, 0, 0), (60, 0, 60, 50)]
    walls += [(200, 0, 300, 0), (300, 0, 300, 50), (300, 50, 200, 50), (200, 50, 200, 0), (240, 0, 240, 50)]
    walls += [(395, 0, 455, 0), (450, -5, 450, 45), (455, 40, 395, 40), (400, 45, 400, -5)]
    return dxf(root / "room.dxf", [("Walls", 7, 0, ())], [line("Walls", *wall) for wall in walls])


def snaps_file(root, document=None):
    """The room, a circle (Holes, red: centre (30, 25), radius 10) and a line across both cells (Axis, blue: (10, 20) to
    (90, 20)), each layer its own body: a crossing between bodies, a centre, quadrants and points on a circle."""
    walls = [(0, 0, 60, 0), (60, 0, 100, 0), (100, 0, 100, 50), (100, 50, 60, 50), (60, 50, 0, 50), (0, 50, 0, 0), (60, 0, 60, 50)]
    return dxf(root / "snaps.dxf", [("Walls", 7, 0, ()), ("Holes", 1, 0, ()), ("Axis", 5, 0, ())],
               [line("Walls", *wall) for wall in walls] + [("CIRCLE", "Holes", [(10, 30), (20, 25), (40, 10)]), line("Axis", 10, 20, 90, 20)])


def far_file(root, document=None):
    """Two lines a million millimetres from (0, 0): read near it, opened centred, shown in the file's own coordinates."""
    return dxf(root / "far.dxf", [("Site", 7, 0, ())], [line("Site", 1000010, 2000020, 1000110, 2000020), line("Site", 1000010, 2000020, 1000010, 2000080)])


def plot_file(root, document=None):
    """A 200 x 100 frame in colour 7, a red line on a 0.70 mm layer, a blue fill, a green line on a layer left out of plots."""
    frame = [(0, 0, 200, 0), (200, 0, 200, 100), (200, 100, 0, 100), (0, 100, 0, 0)]
    return dxf(root / "plot.dxf", [("Frame", 7, 0, ()), ("Red", 1, 0, [(370, "70")]), ("Fill", 5, 0, ()), ("Hidden", 3, 0, [(290, "0")])],
               [line("Frame", *edge) for edge in frame] + [line("Red", 20, 50, 180, 50), solid("Fill", 150, 10, 190, 40), line("Hidden", 20, 80, 180, 80)])


def osnap_sketch_document(root, document):
    """One sketch on XY: a 100 x 50 rectangle of four lines and a circle at (30, 25), radius 10."""
    import json
    points = [{"id": i + 1, "x": x, "y": y} for i, (x, y) in enumerate([(0, 0), (100, 0), (100, 50), (0, 50), (30, 25)])]
    entities = [{"id": 10 + i, "type": "line", "p": [i + 1, (i + 1) % 4 + 1]} for i in range(4)] + [{"id": 20, "type": "circle", "p": [5], "r": 10}]
    return document("osnap-sketch", ("sketch", "--geometry", json.dumps({"points": points, "entities": entities})))


def picture_file(root, document=None):
    """A 200 x 100 mm frame and a red 8 x 8 PNG embedded at (20, 10), 60 x 40 (SVG y down)."""
    import base64, struct, zlib
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    rows = b"".join(b"\x00" + b"\xff\x00\x00" * 8 for _ in range(8))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    href = "data:image/png;base64," + base64.b64encode(png).decode("ascii")
    path = root / "picture.svg"
    path.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="200mm" height="100mm" viewBox="0 0 200 100">'
                    '<path d="M 0 0 L 200 0 L 200 100 L 0 100 Z" fill="none" stroke="black"/>'
                    f'<image x="20" y="10" width="60" height="40" preserveAspectRatio="none" href="{href}"/></svg>', encoding="ascii")
    return path


def text_file(root, document=None):
    """Text as AutoCAD 2013 writes it (UTF-8): three behs (Joined), 'III' (Latin), an Arabic word right-aligned on a guide
    line at x = 100 (Right, Guide), Latin and Arabic in one line (Mixed), an Arabic paragraph wrapped at 60 (Para), 'III'
    in a shape font beside the drawing (Shape: mini.shx, whose 'I' is a stroke 10 up, then 6 on), MTEXT formatted part by
    part (Rich, Ruled, List), "I." in a right-to-left override (Override)."""
    def text(layer, x, y, s, extra=()):
        return [(0, "TEXT"), (8, layer), (10, x), (20, y), (40, 10), (1, s)] + list(extra)
    pairs = [(0, "SECTION"), (2, "HEADER"), (9, "$ACADVER"), (1, "AC1027"), (9, "$INSUNITS"), (70, "4"), (0, "ENDSEC"),
             (0, "SECTION"), (2, "TABLES"), (0, "TABLE"), (2, "STYLE"), (0, "STYLE"), (2, "STANDARD"), (70, "0"), (40, "0"), (41, "1"),
             (3, "arial.ttf"), (0, "STYLE"), (2, "MINI"), (70, "0"), (40, "0"), (41, "1"), (3, "mini.shx"),
             (0, "ENDTAB"), (0, "ENDSEC"), (0, "SECTION"), (2, "ENTITIES")]
    pairs += text("Joined", 0, 0, "ببب") + text("Latin", 0, -20, "III")
    pairs += text("Right", 0, -40, "غرفة النوم", [(72, 2), (11, 100), (21, -40)])
    pairs += [(0, "LINE"), (8, "Guide"), (10, 100), (20, -45), (11, 100), (21, -25)]
    pairs += text("Mixed", 0, -60, "Room غرفة")
    pairs += [(0, "MTEXT"), (8, "Para"), (10, 120), (20, 0), (40, 5), (41, 60), (71, 1),
              (1, "هذا نص طويل يلتف على عدة أسطر (مع أقواس) 3.5")]
    pairs += text("Shape", 0, -80, "III", [(7, "MINI")])
    pairs += text("Override", 40, -80, "\u202eI.\u202c")  # a right-to-left override: drawn ".I"
    # MTEXT in parts (5 high, bottom left on the point): a red part and a stacked fraction; an underlined word.
    pairs += [(0, "MTEXT"), (8, "Rich"), (10, 120), (20, -60), (40, 5), (71, 7), (1, r"Plain {\C1;Red} 1\S1/2;")]
    pairs += [(0, "MTEXT"), (8, "Ruled"), (10, 120), (20, -75), (40, 5), (71, 7), (1, r"\LUnder\l")]
    # A numbered list: a hanging indent of 3 text heights with a tab stop there, its items red.
    pairs += [(0, "MTEXT"), (8, "List"), (10, 120), (20, -45), (40, 5), (71, 7), (1, r"\pxi-3,l3,t3;1.^I{\C1;HE}\P2.^I{\C1;EH}")]
    pairs += [(0, "ENDSEC"), (0, "EOF")]
    (root / "mini.shx").write_bytes(b"AutoCAD-86 shapes 1.0\r\n\x1a" + bytes([0, 0, 0x49, 0, 2, 0, 0, 0, 6, 0, 0x49, 0, 7, 0]) +
                                    b"T\x00\x0a\x02\x00\x00" + bytes([0, 1, 0xA4, 2, 0xAC, 0x60, 0]))
    path = root / "text.dxf"
    path.write_text("".join(f"{code}\n{value}\n" for code, value in pairs), encoding="utf-8")
    return path


def pens_file(root, document=None):
    """Walls red, DASHED and 0.5 mm: a line in its style, one in CENTER of its own, one 1.00 mm of its own, one at linetype
    scale 0.5; Plain blue: a block whose line is by block, inserted in HIDDEN."""
    line = lambda layer, y, extra=(): [(0, "LINE"), (8, layer)] + list(extra) + [(10, 0), (20, y), (11, 100), (21, y)]
    pairs = [(0, "SECTION"), (2, "HEADER"), (9, "$INSUNITS"), (70, "4"), (0, "ENDSEC"), (0, "SECTION"), (2, "TABLES"),
             (0, "TABLE"), (2, "LTYPE"), (0, "LTYPE"), (2, "DASHED"), (73, 2), (49, 12.7), (49, -6.35),
             (0, "LTYPE"), (2, "CENTER"), (73, 4), (49, 31.75), (49, -6.35), (49, 6.35), (49, -6.35),
             (0, "LTYPE"), (2, "HIDDEN"), (73, 2), (49, 6.35), (49, -3.175), (0, "ENDTAB"),
             (0, "TABLE"), (2, "LAYER"), (0, "LAYER"), (2, "Walls"), (70, 0), (62, 1), (6, "DASHED"), (370, 50),
             (0, "LAYER"), (2, "Plain"), (70, 0), (62, 5), (0, "ENDTAB"), (0, "ENDSEC"),
             (0, "SECTION"), (2, "BLOCKS"), (0, "BLOCK"), (2, "K"), (70, 0), (10, 0), (20, 0)] + line("0", 0, [(6, "BYBLOCK")]) + \
            [(0, "ENDBLK"), (0, "ENDSEC"), (0, "SECTION"), (2, "ENTITIES")]
    pairs += line("Walls", 0) + line("Walls", 10, [(6, "CENTER")]) + line("Walls", 20, [(370, 100)]) + line("Walls", 40, [(48, 0.5)])
    pairs += [(0, "INSERT"), (8, "Plain"), (6, "HIDDEN"), (2, "K"), (10, 0), (20, 30), (0, "ENDSEC"), (0, "EOF")]
    path = root / "pens.dxf"
    path.write_text("".join(f"{code}\n{value}\n" for code, value in pairs), encoding="ascii")
    return path


def room_document(root, document):
    return document("room", ("import", "--file", str(room_file(root)), "--center", "true"))


def damaged_document(root, document):
    """The far drawing, then the room, in one document edited by hand: the room's body entries are lost (gc, a merge) and
    the far drawing's layer and a layer state have fields of other types. It opens (both used to terminate the app), the
    room unresolved, the layer as by default, and the readout still follows the far drawing."""
    import json, uuid
    path = document("damaged", ("import", "--file", str(far_file(root)), "--center", "true"), ("import", "--file", str(room_file(root))))
    lines, out, i = path.read_bytes().split(b"\n"), [], 0
    site = None
    while i < len(lines):
        if lines[i].startswith(b"{") and b'"far.dxf"' in lines[i]:
            stack = list(json.loads(lines[i])["nodes"])
            while stack:
                node = stack.pop()
                if node.get("type") == "component" and node.get("name") == "Site":
                    site = node["id"]
                stack += node.get("children", [])
        if lines[i] == b"#bodies":
            bad = {"plot": "no", "off": 1, "frozen": "yes", "lineweight": "x", "linetype": 5, "pattern": "x"}
            out.append(json.dumps({"op": "appearance", "id": str(uuid.uuid4()), "target": site, "visible": True, "layer": bad}).encode())
            out.append(json.dumps({"op": "view", "id": str(uuid.uuid4()), "name": "Hand made", "camera": {"eye": [0, 0, 1], "target": [0, 0, 0], "up": [0, 1, 0]},
                                   "display": {"layers": {site: {"name": 3, "on": "x", "color": ["r", 0, 0]}}}}).encode())
        if lines[i].startswith(b"#body ") and b"room.dxf" in lines[i]:
            i += 1 + int(lines[i].split(b" ")[2])
            continue
        out.append(lines[i])
        i += 1
    assert site and len(out) < len(lines) + 2, "the far drawing's layer or the room's body entries were not found"
    path.write_bytes(b"\n".join(out))
    return path


CASES = [
    # UI-10: a drawing in colour 7 on every background in both themes stands out by 4.5:1 or more. <prefix>.<theme>.<bg>.png
    ("contrast", contrast_file, {"OPAD_BENCH_CONTRAST": "{prefix}"}),
    # UI-89: the Layers manager on an .opad (cells, undo, colour, linetype, lineweight, isolate, walk, layer states saved and
    # restored, the file read back) and on the DXF itself in viewer mode (the same, a layer state asks to save first).
    ("layers", layers_document, {"OPAD_BENCH_LAYERS": "{prefix}"}),
    ("layers-viewer", layers_file, {"OPAD_BENCH_LAYERS": "{prefix}"}),
    # UI-118: the 2D vocabulary on a drawing (no Faces filter or display chips, Groups/Objects/Points filters, "Line on
    # Lines · 100 mm", the rollover card, a pick counted as an object, Properties in 2D words with the layer's section) and
    # on a solid (3D until 2D mode, back after it). <prefix>.card.png, .status.png, .properties.png
    ("vocabulary", contrast_file, {"OPAD_BENCH_VOCABULARY": "{prefix}"}),
    ("vocabulary-rtl", contrast_file, {"OPAD_BENCH_VOCABULARY": "{prefix}", "OPAD_LANG": "ar"}),
    ("vocabulary-3d", "box", {"OPAD_BENCH_VOCABULARY": "{prefix}"}),
    # UI-90: the Area tool on a room: a wall grows into its cell, an open boundary, a closed one pinned, a wall cut where
    # another meets it in its middle, lines that overshoot their corners, three corners.
    # <prefix>.prompt.png, .panel.png, .viewport.png
    ("area", room_document, {"OPAD_BENCH_AREA": "{prefix}"}),
    ("area-viewer", room_file, {"OPAD_BENCH_AREA": "{prefix}"}),
    # UI-90: object snap (F3) in the Distance tool's point picks: midpoint, a crossing of two layers, centre, quadrant,
    # nearest on the circle, perpendicular and tangent from a picked point, the sketch's kind switches, F3 off, none with
    # the Objects filter. <prefix>.snap.png, .perpendicular.png
    ("object-snap", snaps_file, {"OPAD_BENCH_OSNAP": "{prefix}"}),
    # UI-90: the same snaps on a sketch's curves in Review picks (indexed in its plane), perpendicular from a picked point.
    ("object-snap-sketch", osnap_sketch_document, {"OPAD_BENCH_OSNAP_SKETCH": "{prefix}"}),
    # UI-90: the cursor readout in the status bar: a far drawing's own coordinates (and a snapped point's), a model's X, Y, Z.
    ("readout", far_file, {"OPAD_BENCH_READOUT": "{prefix}"}),
    ("readout-3d", "box", {"OPAD_BENCH_READOUT": "{prefix}"}),
    # A document edited by hand opens and reads out its drawing: a drawing that lost a body entry (it used to terminate in
    # the readout's frames), a layer and a layer state with fields of other types (the layer model and the browser threw).
    ("readout-damaged", damaged_document, {"OPAD_BENCH_READOUT": "{prefix}"}),
    # UI-88: Plot: extents fit, monochrome, lineweights, the plot stamp, 1:N and a scale that does not fit, display and window areas, a printer
    # (to a PDF file) and a PDF. <prefix>.dialog.png, .preview.png, .pdf, .printer.pdf
    ("plot", plot_file, {"OPAD_BENCH_PLOT": "{prefix}"}),
    ("plot-rtl", plot_file, {"OPAD_BENCH_PLOT": "{prefix}", "OPAD_LANG": "ar"}),
    # UI-88: a drawing's raster image is plotted (red where it lies, grey in monochrome, an image in the PDF).
    ("plot-image", picture_file, {"OPAD_BENCH_PLOT_IMAGE": "{prefix}"}),
    # UI-92: drawing text shaped: Arabic letters joined in the view, a right-aligned Arabic word on its guide, Latin and
    # Arabic side by side in one line. <prefix>.png
    ("drawing-text", text_file, {"OPAD_BENCH_TEXT2D": "{prefix}"}),
    # UI-92: lines in a linetype or lineweight of their own (and by block) drawn so, and keeping it when their layer changes.
    ("entity-pens", pens_file, {"OPAD_BENCH_PENS": "{prefix}"}),
]
