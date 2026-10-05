"""gui_benches cases of the core area (T0); the benches are in app/CoreBench.cpp, app/AreaBench.cpp and app/ContractsBench.cpp."""
import uuid


def newer_file(root, document):
    """A box document with records of a newer build: a one-line sheet and a multi-line sheet item (UI-65), as types no
    build knows (drawing sheets themselves are known since UI-76)."""
    path = document("newer", ("feature", "--kind", "box", "--inputs", '{"length":"30 mm","width":"20 mm","height":"10 mm"}'))
    text = path.read_text(encoding="utf-8")
    sheet = str(uuid.uuid4())
    records = (f'{{"op":"newer_sheet","id":"{sheet}","ts":"2026-10-03T10:00:00Z","by":"newer","name":"Sheet 1","size":"A3"}}\n'
               f'{{"op":"newer_item","id":"{uuid.uuid4()}","ts":"2026-10-03T10:00:01Z","by":"newer","sheet":"{sheet}",\n'
               '  "points": [\n    [0,0],\n    [10,5]]}\n')
    path.write_text(text.replace("#bodies\n", records + "#bodies\n", 1), encoding="utf-8", newline="\n")
    return path


CASES = [
    # The extension seams (UI-119), run in Arabic so the translation fragments are looked up through tr().
    ("seams", "empty", {"OPAD_BENCH_SEAMS": "1", "OPAD_LANG": "ar"}),
    # The feature-area hooks (AreaController), browser providers and property sections through a probe area; in Arabic
    # too, since the browser paints fixed left-to-right columns in a right-to-left UI. The first starts in the probe's own
    # workspace (saved by id, ui/workspace 0 for earlier builds), the second in Design as earlier builds saved it (1).
    ("areas", "box", {"OPAD_BENCH_AREAS": "{prefix}", "OPAD_BENCH_AREAS_WORKSPACE": "probe"}, "[ui]\nworkspace=0\nworkspaceId=probe\n"),
    ("areas-rtl", "box", {"OPAD_BENCH_AREAS": "{prefix}", "OPAD_BENCH_AREAS_WORKSPACE": "design", "OPAD_LANG": "ar"}, "[ui]\nworkspace=1\n"),
    # The shared UI contracts (UI-120, app/ContractsBench.cpp). The command registry on a STEP file in viewer mode.
    ("commands", "screw", {"OPAD_BENCH_COMMANDS": "1"}),
    # The panel footer in the feature panel and the guided tools' panel, left to right and mirrored.
    ("footer", "box", {"OPAD_BENCH_FOOTER": "1", "OPAD_BENCH_UISHOT": "{prefix}"}),
    ("footer-rtl", "box", {"OPAD_BENCH_FOOTER": "1", "OPAD_BENCH_UISHOT": "{prefix}", "OPAD_LANG": "ar"}),
    # Toasts over the viewport: stacking, an Undo action on a real edit, timing, theme, mirroring.
    ("toast", "box", {"OPAD_BENCH_TOAST": "{prefix}"}),
    ("toast-rtl", "box", {"OPAD_BENCH_TOAST": "{prefix}", "OPAD_LANG": "ar"}),
    # The ribbon at 1280 and 1600 px in every workspace and tab: titled groups, adaptive collapse, never elided; the tab
    # row's cluster (quick access, search compact when narrow), the compact Select control, Undo/Redo step lists; a
    # contextual tab with a split button. <prefix>.<width>.<workspace>.png, <prefix>.narrow.png.
    ("ribbon", "box", {"OPAD_BENCH_RIBBON": "1280,1600", "OPAD_BENCH_UISHOT": "{prefix}"}),
    ("ribbon-rtl", "box", {"OPAD_BENCH_RIBBON": "1280,1600", "OPAD_BENCH_UISHOT": "{prefix}", "OPAD_LANG": "ar"}),
    # Every command of the window and the areas has a place: a ribbon tab of a workspace or a menu of the menu bar
    # (UI-102/104, app/PlacesBench.cpp). <prefix>.json lists them: id, label, group, menu path, workspaces, help id;
    # <prefix>.<workspace>.png shows the window in each workspace.
    ("places", "box", {"OPAD_BENCH_PLACES": "{prefix}.json", "OPAD_BENCH_UISHOT": "{prefix}"}),
    # The workspace list under the chip: one line per workspace, no description; a row's hint after a moment of hovering.
    ("workspace-menu", "box", {"OPAD_BENCH_WORKSPACE_MENU": "{prefix}"}),
    ("workspace-menu-rtl", "box", {"OPAD_BENCH_WORKSPACE_MENU": "{prefix}", "OPAD_LANG": "ar"}),
    # The workspaces' promises (UI-104, app/WorkspacesBench.cpp): Extrude and New sketch from Review switch to Design; the
    # Sketch tab first with Finish sketch primary, Design kept while sketching, back where it started; Interference's Keep
    # as check opens the stored check in Design; a viewed drawing comes into Drafting and the next document goes back.
    ("workspaces", "box", {"OPAD_BENCH_WORKSPACES": "{root}/layers.svg"}),
    # A file of a newer build (unknown op types) opens, hides those records from the timeline and saves them back (UI-65).
    ("tolerant", newer_file, {"OPAD_BENCH_TOLERANT": "{prefix}.opad"}),
    # The units service (UI-123): the document switched to inches from the status bar, a Distance result in inches, live
    # precision and fractions, the overhang box in radians, feature and sketch-tool defaults and a sketch dimension in
    # inches, undo back to millimetres. <prefix>.status.png, <prefix>.panel.png, <prefix>.sketch.png.
    ("units", "box", {"OPAD_BENCH_UNITS": "{prefix}"}),
    # The per-body look compositor (UI-121): ghost + tint on one body (hovered as inactive), a component's tint, ghosts
    # pickable, an explode offset picked where drawn with its note following, a candidate layer under the X-ray
    # selection, everything cleared, then a sketch's look (faded, hidden, cleared). <prefix>.ghost.png.
    ("looks", "overlap", {"OPAD_BENCH_LOOKS": "{prefix}"}),
    # The same on the Engine (beside the repository; skipped where it is not): a ghost layer over every body, ten explode
    # ticks and the clearing, with no event-loop gap over 50 ms.
    ("looks-engine", "../opad_resources/bench_step_files/Engine V8-XT Turbo.opad", {"OPAD_BENCH_LOOKS": "{prefix}"}),
]
