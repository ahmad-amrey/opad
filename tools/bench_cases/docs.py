"""gui_benches cases of the documentation area (TODO 11 track t5a: drawings, part properties, bills of materials, 2D
export); the benches are in app/DrawingsBench.cpp, app/BomBench.cpp and app/ExportBench.cpp, the area in app/DocsArea.cpp."""

CASES = [
    # The browser's Drawings folder (UI-76): nested and worded rows, F2 and Del through the folder, Ctrl+Z, the rows' menu;
    # no timeline marker for drawing ops; Properties names the material and its density, then the mass.
    ("drawings-browser", "empty", {"OPAD_BENCH_DRAWINGS": "{prefix}"}),
    # The area's commands and their places; the PART section and its dialog; File > Export bill of materials (UI-83, UI-140).
    ("bom", "empty", {"OPAD_BENCH_BOM": "{prefix}"}),
    # A STEP's bill of materials in viewer mode; Part properties asks to save it as OPAD first.
    ("bom-viewer", "screw", {"OPAD_BENCH_BOM_OPEN": "{prefix}.png", "OPAD_BENCH_BOM_VIEWER": "1"}),
    # 2D views of solids from the Export dialog (DXF, SVG, PDF, PNG; UI-87), a sheet and a drawing from their rows (UI-86).
    ("export-view", "empty", {"OPAD_BENCH_EXPORT": "{prefix}"}),
]
