"""gui_benches cases of the documentation area (TODO 11 track t5a: drawings, part properties, bills of materials, 2D
export); the benches are in app/DrawingsBench.cpp, app/BomBench.cpp, app/ExportBench.cpp and app/SheetBench.cpp, the area
in app/DocsArea.cpp and app/DocsWorkspace.cpp."""

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
    # The Drawings workspace and the sheet canvas (UI-78): Ctrl+3, New drawing from an ISO A3 template with front, top,
    # side and iso views, drafts before the final linework, dragging a base view (its projected views stay aligned) and a
    # projected one (its gap), placing a base and a projected view, hidden lines, sheet and document properties, a DXF template, a new
    # sheet, PDF export, Del and Esc, snaps on the views (marker, readout, switch), template fields from placeholders and
    # placed with the mouse. <prefix>.empty.png, .sheet.png, .snap.png, .final.png, .window.png, .fields.png, .template.png.
    ("sheet", "empty", {"OPAD_BENCH_SHEET": "{prefix}"}),
    # The same workspace on the Engine (beside the repository; skipped where it is not): an A2 drawing of four views laid
    # out on a worker, drawn and dragged with no event-loop gap over 250 ms. <prefix>.png.
    ("sheet-engine", "../opad_resources/bench_step_files/Engine V8-XT Turbo.opad", {"OPAD_BENCH_SHEET_LOADED": "{prefix}"}),
    # Annotating a sheet (UI-79, UI-80, UI-81) with mouse and key events on the canvas: smart dimensions (an edge, a hole's
    # diameter with a tolerance from the options bar, a corner to a centre), hole callouts from hole features (also from the side), a centre mark
    # and line, a note with a leader, datums, a feature control frame, surface texture, a chain set, dimensions from datums,
    # a hole table, Esc stepping back, select / edit in the bar / drag / Del / Ctrl+Z, a dangling dimension re-attached from
    # the sheet bar, the views' own centre marks. <prefix>.annotate.png, .bar.png, .marks.png.
    ("sheet-annotate", "empty", {"OPAD_BENCH_SHEET_ANNOTATE": "{prefix}"}),
]
