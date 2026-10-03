"""gui_benches cases of the 2D drawing area (drawing2d); the benches are in app/Drawing2DBench.cpp."""


def dxf(path, layers, entities):
    """A minimal DXF: layers [(name, aci, flags, extra groups)], entities [(type, layer, groups)] as (code, value) pairs."""
    pairs = [(0, "SECTION"), (2, "HEADER"), (9, "$INSUNITS"), (70, "4"), (0, "ENDSEC"), (0, "SECTION"), (2, "TABLES"), (0, "TABLE"), (2, "LAYER")]
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
    Plain a second one in colour 7 of its own (a layer colour leaves it)."""
    return dxf(root / "layers.dxf", [("Walls", 1, 4, [(6, "DASHED"), (370, "50")]), ("Notes", -3, 0, [(290, "0")]), ("Old", 2, 1, ()), ("Plain", 5, 0, ())],
               [line(name, 0, 10 * i, 100, 10 * i + 5) for i, name in enumerate(["Walls", "Notes", "Old", "Plain"])] + [line("Plain", 0, 45, 100, 45, 7)])


def layers_document(root, document):
    """The same drawing imported into an .opad (layer states are view ops: an editable document)."""
    return document("layers", ("import", "--file", str(layers_file(root)), "--center", "true"))


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
]
