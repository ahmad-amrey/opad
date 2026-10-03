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


CASES = [
    # UI-10: a drawing in colour 7 on every background in both themes stands out by 4.5:1 or more. <prefix>.<theme>.<bg>.png
    ("contrast", contrast_file, {"OPAD_BENCH_CONTRAST": "{prefix}"}),
]
