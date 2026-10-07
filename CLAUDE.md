# OPAD - working notes for Claude

## Build and test (Windows preset = MSYS2 mingw64)

```
export PATH=/c/msys64/mingw64/bin:$PATH
cmake --preset windows && cmake --build --preset windows && ctest --preset windows
```

Binaries land in `build/windows/bin`. Presets are one native target per OS (`windows`, `linux`, `macos`), each
against that OS's own packages (pacman, apt, Homebrew); `cmake --workflow --preset <os>` runs all three steps.
vcpkg and cross builds were dropped on 2026-09-19; a new target is a preset inheriting `base`.
`cmake/toolchain_speed.cmake` (options `OPAD_CCACHE`, `OPAD_LLD`, default ON): ccache as the C/C++ compiler launcher and
`-fuse-ld=lld` on MinGW when installed, as normal variables (never cached); an explicit `CMAKE_<LANG>_COMPILER_LAUNCHER`
(cache or env, empty = off), `-fuse-ld=` in the linker flags or `CMAKE_LINKER_TYPE` wins; `OPAD_STATIC` keeps GNU ld (its
FreeType group relies on archive order). ccache `base_dir` is the repo root, so `__FILE__` is relative: tests read sources
via `OPAD_SOURCE_DIR`. A stuck exe in `bin/` (e.g. `opad-cli.exe.locked-<pid>`) is renamed aside, never killed.

Git for Windows ships its own libstdc++-6.dll (newer GCC, no `__emutls_v._ZSt11__once_call` export) and Git Bash /
the VS Code terminal put it first on PATH, which gives "Entry Point Not Found" at launch. `cmake/mingw.cmake` (target `opad-runtime`) copies the
toolchain's libstdc++/libgcc/winpthread DLLs into bin/ after every build, so the loader takes those; still
prepend `/c/msys64/mingw64/bin` to PATH when running anything from Git Bash.

## Linux (Ubuntu 24.04, WSL)

apt packages: the README's list (cmake ninja-build g++ pkg-config qt6-base-dev libqt6opengl6-dev nlohmann-json3-dev
libfreetype/fontconfig/harfbuzz/zstd-dev, GL/GLU/X11/Xext/Xi dev, rapidjson-dev, pybind11-dev python3-dev, xvfb); no OCCT
package. Ubuntu ships OCCT 7.6 / Qt 6.4: OCCT < 7.8 is never patched around, `OPAD_OCCT_BUILD` (AUTO default; ON/OFF)
builds 7.9.2 from source at the first `cmake --preset linux` (`cmake/occt_bundled.cmake` + `occt_source.cmake`, the
recipe shared with `occt_static.cmake`; shared libs in `build/linux/occt/install`, found by build RPATH; ~11 min once).
Qt 6.4 is handled by version guards (`QImage::mirrored` < 6.9, `DevicePixelRatioChange` < 6.6, `QAccessibilityHints` <
6.10), newer branch unchanged. Clone: WSL `~/opad-ubuntu` (branch ubuntu-build; sync with `git pull /mnt/c/projects-c/opad
<branch>`, hand back with `git push /mnt/c/projects-c/opad <branch>:<transfer-branch>`). There: `CCACHE_DIR=~/.ccache`
(`~/.cache/ccache` is root's), `ninja -C build/linux` (~8 min cold), unit tests `QT_QPA_PLATFORM=offscreen ctest
--test-dir build/linux -LE gui` (72), benches `xvfb-run -a -s "-screen 0 1920x1080x24" python3 tools/gui_benches.py
build/linux/bin/opad build/linux/bin/opad-cli --only <case>`. Under Xvfb the window *is* drawn (unlike Windows' hidden
bench windows): camera moves animate and the pointer hovers, so benches wait for `cameraMoving()` and the view's own
hover. Redo's alternate key is whichever of Ctrl+Y / Ctrl+Shift+Z is not `QKeySequence::Redo`. Known limits: no FreeImage
in the bundled OCCT (headless renders draw canvases without their picture; test_canvas skips; GUI decodes with Qt),
linuxdeploy not installed (no `opad-single` AppImage target), pybind11-dev missing in WSL (no python module/test),
`tests/corpus/*.step` not in git (picking/viewer skip until `tests/corpus/fetch.py` or a copy), sheet-engine needs the
Engine file. Wayland runs through XWayland (OCCT's viewer needs X11).

## Single-file executable (`cmake --workflow --preset windows-single`)

Preset `windows-static` (`OPAD_STATIC=ON`, `build/windows-static`): Qt from MSYS2's `qt6-static` (first on
`CMAKE_PREFIX_PATH`, no ICU), OCCT built static from the 7.9.2 source tarball by `cmake/occt_static.cmake` at
configure time (download, `BUILD_LIBRARY_TYPE=Static`, only the six modules OPAD uses, no Tcl/TBB/FFmpeg/OpenVR,
installed to `build/windows-static/occt/install`; ~15 min once, delete `occt/install` to redo), FreeType and its
deps from the OS `.a` archives through `pkg-config --static`, appended as one `--start-group` to
`CMAKE_CXX_STANDARD_LIBRARIES` so they come after Qt's late FreeType copy (a static OCCT toolkit carries no
FreeType). `cmake/static.cmake`: `-static`, function sections + `--gc-sections`, `-s`, archives preferred by
`find_library`, HarfBuzz's CMake config disabled (it names the import lib; Qt then takes pkg-config's). Static Qt
imports every plugin by default: `qt_import_plugins` keeps only qjpeg of the image formats (TIFF alone wants zstd,
lzma, jbig, lerc, libdeflate) and no icon engine. Target `opad-single` (`cmake/single.cmake`) copies the exes to
`build/windows-static/single/OPAD-<ver>-windows-x64.exe` and fails if `objdump -p` shows a non-Windows import
(`single_check.cmake`). `OPAD_SINGLE_FILE` makes the app portable without the marker: data in `<exe dir>/opad-data`.
Python module and plugins are off in this preset. Linux/macOS `opad-single` = AppImage (linuxdeploy) / dmg
(macdeployqt) over the shared build; written, not run here.

## Portable Windows package and the logo

`cmake --build --preset windows-portable` (target `opad-portable`, `cmake/portable.cmake` + `portable_stage.cmake`)
stages `build/windows/portable/OPAD-<ver>-windows-x64` + `.zip`: windeployqt6 for Qt (plugin types generic,
networkinformation, tls skipped), `file(GET_RUNTIME_DEPENDENCIES)` for the rest (~146 DLLs, 268 MB; ffmpeg/ICU come
in through OCCT's TKService and Qt6Core, hard imports). Missing DLLs fail the target. `opad.portable` beside the exe
(checked in `main`) moves QSettings to `<dir>/data` (INI) and the cache to `<dir>/data/cache`. The link step fails
with "Permission denied" while the user has `opad.exe` open: ask them to close it, never kill it.
Logo: source is `C:\projects-c\opad_resources\opad_logo.png` (mark + wordmark, transparent; the cube's inner face is a
hole). Program/window icons are always the cube mark alone, hole filled white: `py -3 tools/make_icon.py <logo>`
(Pillow lives in the Windows Python, not MSYS2's) writes `app/res/opad.ico` (exe, via `res/opad.rc.in`) and
`opad-<n>.png` (`res.qrc`, `icons::appIcon`).

## Viewer mode

Opening any file other than `.opad` is viewer mode (`AppDocument::browse` + `viewing`, `ImportOptions::viewer`; setting
`files/viewerMode`, default on, `AppDocument::viewerOpens`): every reader (`import_file`; `detail::store_body` in
`core/src/import_common.hpp`) keeps its shapes live in the shape cache under random pseudo keys, with no healing, BREP
text or hashing. Such a document cannot be serialised. `AppDocument::run` accepts only `appearance` (hide, colour,
opacity, lock are view settings there) and is never dirty; `commitPlan`/`commitSnapshot` refuse. Edit actions stay
enabled: `MainWindow::isEditAction` routes them to `requireEditable` ("Save first to edit": Save as OPAD / Edit unsaved
copy / Cancel, then the action runs again). `opad::make_editable` (worker, `AppDocument::startEditable`) persists the
live bodies in place under content keys, keeping the ops (view changes) and node ids; `bodyKeysRenamed` ->
`Viewport::renameBodyKeys` keeps the meshes on screen (no second tessellation). Formats: STEP/IGES via XCAF walk
(`detail::import_xcaf`, colours and VisMaterial names/opacity, also from sub-shape labels), glTF/OBJ/VRML through
RWMesh readers (`plain_mesh` copies deferred glTF arrays; VRML gets metres and Y-up by hand), STL/PLY/3MF parsed in
`core/src/formats.cpp` (`mesh_face`: weld + 30 degree crease normals; 3MF = minimal zip via zlib + tag scanner, Bambu /
Prusa object names). Slow reads (> 1.5 s) are remembered (`viewer_cache.cpp`, user cache `viewer/`, BinTools with the
display meshes, keyed by normalised path + size + mtime, 2 GB LRU): Hydrostatic 23 s -> 1.8 s. Opening while a load runs
drops that load (`m_loadToken`). The chips row is mouse-receptive (its Save to edit card is a button); folder stepping
(PgUp/PgDown) was removed at the user's request. DWG: `third_party/libredwg` submodule (tag 0.14) built by
`cmake/libredwg.cmake` (ExternalProject, static, `-DOPAD_DWG=OFF` skips; `libredwg_config.cmake` strips the CR a CRLF
checkout puts into its config.h) into `bin/dwg2dxf.exe`/`dxf2dwg.exe`; `convert_dwg` prefers ODA when installed, runs
LibreDWG on ASCII copies in a scratch cwd (it opens ANSI names). DXF: `core/src/dxf_reader.cpp` (blocks by location,
hatches, text via StdPrs_BRepFont at cap height from 'H', ACI/true colours -> one body per layer colour, far drawings
read near (0,0) with `Drawing::origin` in the import placement); cursor reads must be one per statement (argument
order). `ctest -R dxf`. Thumbnails: `shell/thumbnails.cpp` (opad-thumbnails.dll, IThumbnailProvider +
IInitializeWithStream, static runtime, CLSID {67DD85AE-5101-4C3A-9231-39D7B7EFEC39}) copies the stream to a scratch file
and runs `opad-cli thumbnail <file> --out x.bgra` (white + black render -> alpha; drawings opaque on white); registered
per type by `associations::apply` (SystemFileAssociations + ProgID ShellEx, never over another app's HKCU handler);
`ctest -R thumbnails` drives it through COM without the registry. Viewing a drawing turns 2D mode on (`m_autoTwoD`);
2D mode lays the grid in the viewed plane around the view (`Viewport::updateInfiniteGrid`, from handleViewRedraw). Print
check takes meshes (`print_mesh` in checks.cpp: triangle regions, a grid for the wall rays). Paths: QString -> `std::filesystem::path(toStdU16String())`, JSON paths through
`opad::path_from_utf8` (narrow strings are ANSI on Windows). `opad-cli probe <file> --viewer --mesh --cache` times it.
Bench: `OPAD_BENCH_VIEWER=<out.opad>` (gui case "viewer"); benches that open a STEP/SVG and then edit it write
`files/viewerMode=false` into their settings. While any load runs, `LoadShade` (a translucent tool window owned by
the main window) covers the workspace, blocks the mouse and shows a spinner; actions other than file/panel ones wait.
Benches (`--bench-select`) keep every other top-level window off screen (`BenchQuiet`) and never ask on quit.

## Floating tool panels (no right dock)

Properties, Annotations and Section are `ToolPanel`s (`app/Panels.hpp`): frameless tool windows owned by the main
window, anchored to the viewport's top-right corner (right 8, top 186; `positionOverlays` re-anchors them), dragged
by the 32 px header, resized by the corner grip (width 280-480), place/size/pin persisted under `panels/<id>`;
double-click on the header restores the default place. `MainWindow::openPanel` hides the other unpinned panels;
Esc closes an unpinned panel before it clears the measurement. Properties opens only from the context menu
(`inspect.properties`, Ctrl+P) for the current selection (`m_selRefs`); a new selection closes it
(`selectionMoved`), unless pinned, in which case it follows the selection. Nothing is inspected while it is closed.

## Workspaces (`app/Ribbon.hpp`)

One document, one timeline; a workspace only swaps the ribbon's tab set (`RibbonBar::addWorkspace/addTab/setWorkspace`,
chip + 372 px list left of the tabs). Review (Ctrl+1) and Design (Ctrl+2; setting `ui/workspace`); Document is not
built. Design is the modelling workspace (next section); a third, *contextual* workspace "Sketch" (`Workspace::contextual`,
not in the chip's list, never saved to `ui/workspace`) is entered and left with sketch mode by `updateDesignState`.
Panels moved to Alt+1/2/3. `QMainWindow::separator:vertical` is the *upright* bar (named after its shape).

UI check without driving the desktop (the user works on it, see memory): `OPAD_BENCH_UISHOT=<png>` with
`--bench-select` saves `MainWindow::grab()` (widgets only, the native viewport is blank); `OPAD_BENCH_WORKSPACE=1`
switches to Design first; `OPAD_LANG=ar` for Arabic.

## Design (core `core/src/design`, app `DesignController` / `DesignPanels` / `SketchEditor` + `SketchTools` / `ViewportDesign.cpp`)

Model: docs/design.md. `param` / `sketch` / `feature` ops carry their *results* (node -> body key); replay never runs
the kernel or the solver. A change is an `edit` (or `delete`) op plus one `regen` op with every downstream result
whose fingerprint (`result.in`) changed. `effective_ops()` is the log with edits/regens merged (copy-on-write per op:
do not go back to copying `op.data`, imports are huge). `SceneBuilder` is the one replay; the engine's `Walk`
(`engine.cpp`) drives it while computing, `resolve(doc, until)` stops before an op = timeline roll-back
(`AppDocument::setRollback`, used while a feature or sketch is edited).
- Fingerprints (`feature_fingerprint`) must see every body an input names, strings included (`string_refs`, by spec
  input type), else an edit regenerates too much (a combine used to count as "automatic" and depend on every body:
  1.8 s and new keys per unrelated edit on the phone) or too little (fillets on "uuid/edge/N" tokens never followed
  their body). `materialize` hands later features the body parsed back from its BREP and keeps the stored key when a
  recomputed body is `same_geometry`; `plan_regenerate(doc, true)` must add no body entries (test_design).
- New feature kind = one `add(...)` in `build_specs()` + one `if (kind == ...)` in `compute_feature` (features.cpp).
  The panel, the menu, the CLI and `feature_kinds` follow from the spec; add the icon (Icons.cpp), the ribbon slot
  (`buildRibbon`) and the Arabic strings (labels/hints/choices go through `i18n::t`, so by hand into ar.json).
- New bodies are named in `Walk::materialize` and the result entry keeps it: from scratch = the feature's name
  (numbered if several); `Out::Body::source` set (pattern, mirror, move copy, split) = the source's name + next free
  number, its component (shape stored in that frame) and colour. Explicit body_name/color/parent (feature command,
  batch, panel's New body section) = `style_new_bodies` appending rename/appearance/reparent ops, never result fields.
- Creation features end in `apply_operation` (new/join/cut/intersect; automatic targets are written back into the
  op's inputs). References carry a `hint` (centre, size, entity counts): same counts -> ordinal trusted, else nearest.
- Threads: `design::plan_ops` only reads the document -> `JobRunner::async`; `commit` on the UI thread
  (`DesignController::applyOps`, `runPreview`). A cancelled job reports at once while its thread runs on, so commits
  go through `whenNobodyReads` (the `Reading` counter). `AppDocument::designBusy` makes `run()` refuse meanwhile.
  `delete`/restore go through `applyOps` as soon as the scene has sketches or features (they regenerate).
- Feature panel: one `FeaturePanel` in the "feature" `ToolPanel`. The active pick input owns the viewport selection
  (`ownsSelection` short-circuits `onViewportSelection`); things that are not body entities (sketch regions, points,
  lines, origin/construction planes and axes) are `Viewport::Candidate`s whose id is the JSON of the reference.
  Hints are added on the worker (`hint_refs`), never in the click handler. Preview = the plan's `changed` entries of
  the edited op, meshed on the worker, shown by `setPreviewBodies`.
- Candidates (`ViewportDesign.cpp`): `showCandidates` keeps those already shown with the same id and shape and displays
  only new ones (`displayCandidate`), `addCandidates` appends a sliced job's slice; either one ends the empty document's
  origin guide planes (`m_originPlanes`, UI-51), `clearCandidates` brings them back. The plane picker always shows the
  origin planes in the view itself (`PlanePicker::originPlanes`); with `adoptSelection=false` (a feature's plane input) it
  takes neither the selected face nor a selected origin plane.
- Sketch mode: `SketchEditor` gets the left button and unmodified keys from the viewport (`SketchInput`; keys arrive
  at the `ShortcutOverride` stage so L/C/D/Esc/Del beat the window shortcuts), hit-tests in sketch coordinates itself,
  draws everything through one overlay object (`SketchPrs`), solves after every change and *refuses* a change the
  solver cannot satisfy (never over-constrained). The solver runs on the UI thread on purpose (sketch-sized, ~1 ms,
  needed per mouse move while dragging); region finding for the profile shading is kernel work -> async job.
  Points are shared by id between curves (that is the coincidence); dimensions keep `expr` when it is not a plain number.
- Bench without input driving: `opad-cli new x.opad`, then `OPAD_BENCH_DESIGN=<png> [OPAD_BENCH_SKETCHSHOT=<png>]
  opad.exe x.opad --bench-select` draws a dimensioned rectangle + circle through the tool code, extrudes it through
  the panel's plan/commit path, edits the extrude rolled back, fillets, saves, quits; trace lines `bench: design:` /
  `bench: sketch:`. Core coverage: `ctest -R "design|sketch"`.

## Guided tools (`app/GuidedTool.hpp`, flow in `MainWindow` "guided tools")

Distance, Angle, Radius, Bounding box, Note and the section's "Pick face" are started first and then ask for their
picks: `PromptBar` (top centre of the viewport) + `ToolStepsPanel` in the one "tool" `ToolPanel`. While a tool runs
the viewport accumulates clicks (`setPickAccumulate`: left click = `AIS_SelectionScheme_XOR`, so clicking a picked
item un-picks it and a click on nothing keeps the picks); the viewport selection *is* the ordered pick list and
everything follows from `toolPicksChanged`. A selection made before starting the tool becomes its first picks. Esc
(`toolEscape`): result -> clear and measure again, else one step back (`deselectLast`), else leave. A pick after the
last step starts over (`keepLastSelected`). The measure runs through `JobRunner::async` and is dropped if the picks
moved on (`m_toolRun`); an error steps the last pick back. Changing the filter mid-tool clears the picks. The
distance preview line is point-to-point between the first click and where the mouse met the hovered entity
(`hoverPoint`, emitted only when the hovered owner changes), not an exact measure.
Speed (Engine, face to face): the wait after the last pick is `BRepExtrema_DistShapeShape`, not fetching the
sub-shapes (0 ms) or resolving the scene. `measure_distance` asks for the minimum only (`Extrema_ExtFlag_MIN`) and
runs multi-threaded: 391 ms -> 106 ms, same value. The tool calls `opad::measure_*` with `m_doc->scene` directly.
`AIS_ViewController` calls `OnSelectionChanged` after *every* click, also one that changed nothing: a click on the
view cube (`m_cubeClick`: swallowed in the viewport; the press also binds the Replace scheme for that click, since
only Replace hands the click to the cube's `HandleMouseClick`) or on empty space under XOR (`toolPicksChanged`
returns when the picks are the same). Either used to drop the result and run the measure again.
The cube animation's back-to-back 16 ms frames starve the watchdog timer ("stalled for ~550 ms"): not a real stall.
Bench: `OPAD_BENCH_TOOL=<tool>[,faces|edges|...]` with `--bench-select` starts the tool, clicks at the view centre
and at `OPAD_BENCH_CLICK2=fx,fy` (default lands on the same face = un-pick; `0.3,0.45` hits another face on the as1
sample), logs `bench: tool ...` after each step, saves `<OPAD_BENCH_UISHOT>.prompt.png/.panel.png`, then Esc x3.

## Notes and hand drawing (`app/AnnotationEditor.hpp`, view parts in `ViewportNotes.cpp`)

Note (N) and Hand drawing (Shift+N) are one `AnnotationEditor` (MainWindow `startAnnotation`; the same command
again closes it). It asks for its target like a guided tool (own `PromptBar`), highlights it (`TargetHighlight`:
selection-blue tint + dashed outline in Topmost, not a context selection) under a `Badge` (native child, rounded by a
mask), and fills the persistent "annotation" `ToolPanel` (not in `m_panels`: opening another panel never ends an
annotation; its close button and Esc cancel; a spontaneous hide when minimising does not). A single selected ref is
adopted ("selected first"). Input is a qApp event filter: a click picks (a note re-picks), a drag draws; presses on the
view cube pass through (`Viewport::cubeAt`, same ±96 gate as the viewport's press); double clicks are presses (quick
strokes); bodies are unpickable while a drawing is anchored (like sketch mode). Each stroke stores its own `plane`
(camera-facing at press, through the picked point) = the 2.5D drawing; widths are drawn times the display scale so
they match the picker samples. Save = one `annotate` op; Save needs strokes (drawing) / text (note, AI agent).
Benches: `OPAD_BENCH_NOTES=<prefix>` (tools/test_todo9.py, `--ui-only` skips MCP) drives everything through events
and saves `<prefix>.drawing-panel.png` / `.drawing-editor.png` / `.note-*.png` / `.*-target.png`;
`OPAD_BENCH_ANNOTATE=1` with `--bench-select` on the Engine times each handler (`bench: annotate: <step> N ms`),
unhiding in memory first if nothing is visible. The view cube is drawn lower than (w-100, 104) at scale 1.5: the
bench scans for it; a hidden window has no paint cycle, so a cube click cannot animate there.

## Drawings: placement on import (`app/DrawingPlacer.*`, `importDrawing` in `DrawingWizard.cpp`)

A drawing's plane and origin are one placement matrix on its import op's root component (`ImportOptions::placement`,
`center_drawing`; `import` command `placement`/`plane`/`center`). Open centres it; Import onto a selected planar face
resolves the face frame on the load worker; otherwise `pickSketchPlane(..., false)` then `DrawingPlacer` (preview parsed
on a worker, drag / typed offset / vertex snap via `originReferenceAt`) and Place imports with the final matrix (one op).
Drawing to sketch takes `design::drawing_frame` (the layers' own frame; refuses mixed planes), no plane step.
Benches: `OPAD_BENCH_DRAWING_IMPORT=<drawing>` (face, then picked plane + offset + snap), `OPAD_BENCH_WIZARD(_CREATE)`
(the sketch lands in the drawing's frame).

## Safety net (TODO 10 A13)

`ctest --preset windows` = unit tests (74 after TODO 11). `ctest --preset windows-gui` = `tools/gui_benches.py`: the
in-app benches in hidden windows (~253 cases; built-in list plus `tools/bench_cases/<area>.py`, each exposing `CASES`; a
full run takes about an hour on a loaded machine, so run `--only <case>` while working and the full net before merging).
Hidden bench windows land at display scale 1.0 or 1.5 at random: `big-drawing` and `highlight` fail at 1.0 (pin the
scale before trusting them). A bench must log `bench: ... PASS`/`FAIL` lines; the
runner fails on FAIL, a missing PASS or a non-zero exit. Add a case there when a desktop behaviour is fixed. Benches
must assert behaviour along the way (a preview during the drag), not only the end state.
Help coverage: `registeredIds` in tests/test_help.cpp finds command ids by pattern, with a per-file `helpers` map for an
area's own lambda (`command("..."` / `add("..."`); a new area helper goes there, or its commands are invisible to the
pending list (the richtip bench lists the live gaps). Keep tests/test_help.cpp LF: a CR CR LF line makes git take it
for binary and every merge conflicts across the whole file.

## Feature areas and shared seams (TODO 11)

A feature area is one `AreaController` subclass registered with `OPAD_AREA(Class)` (app/AreaController.hpp): it adds its
actions, ribbon slots, context-menu entries, status widgets and close checks through hooks, so it does not edit
MainWindow*.cpp. MainWindow.cpp/Panels.cpp are split per area/class (MainWindow<Area>.cpp, BrowserPanel, TimelineWidget,
CommandPalette, ToolPanel, PropertiesPanel, SectionPanel, ...). Sources are CMake CONFIGURE_DEPENDS globs: a new
app/*.cpp or tests/test_<name>.cpp needs no CMake edit. Per-area seams: Arabic fragments `app/i18n/ar/<area>.json`
(merged after ar.json), icons `OPAD_ICON_TABLE(area, ...)` (app/Icons.hpp), benches `OPAD_BENCH(variable, id)`
(app/BenchRegistry.hpp) plus a case in `tools/bench_cases/<area>.py`, browser row decorations/folders and Properties
sections through BrowserPanel/PropertiesPanel providers, commands through the CommandRegistry (app/Commands.hpp).
Shared services: `Viewport::setLookLayer(LookSource, deltas)` (app/BodyLook.hpp: one per-body look composed in a fixed
order - appearance, lock, activation ghost, compare tint, explode offset, smart-select candidate - applied with
RecomputePrsOnly, never Redisplay), `app/Units.hpp` (every shown length/angle/area goes through it), `app/KeyText.hpp`
(every key shown anywhere is the user's current binding; never write a key literally), `Toast`, `PanelFooter`, semantic
colour tokens in Theme. The ribbon is one table (MainWindowRibbon.cpp): workspaces Review, Design, Drafting, Drawings;
contextual Sketch, Explode and Canvas tabs. Unknown op types load as opaque records (older/newer builds stay compatible).
Parallel work: tracks ran in git worktrees under .claude/worktrees (configure there with `-DOPAD_DWG=OFF`; the libredwg
submodule is not checked out); `git rerere` is enabled for the merges. Keep text files LF (a stray CR makes git treat a
file as binary and every merge of it conflicts).

## Highlight switches and frame trace

`view.xrayHighlight` (Ctrl+/, setting view/xrayHighlight) puts the selection, its glows, sub-shape highlight and the four
OCCT highlight styles in Topmost (X-ray) or Top (depth-tested): `Viewport::setXrayHighlight` / `applyHighlightLayers`
(ViewportSettings.cpp). A tool that needs the selection depth-tested calls `suppressSelectionXray(bool)` (a temporary
override; `selectionXray()` = setting on and no override), never the setting. `view.hoverHighlight` (/, view/hoverHighlight)
hides only the drawn hover (in `trackHoverFade`); detection, hoverPoint and clicks stay. The cheat sheet is on `?`
(migration `shortcuts/highlightDefaultsVersion`). Bench: highlight-keys.
`OPAD_TRACE_FRAMES=1` with `OPAD_TRACE`: per-frame "frames:" lines (view show/hide/expose, WM_PAINT, page switches, load
shade), no pixels read; a launch where the start page stays on screen shows here whether the view was exposed and drew.
Benches launch-frames and start-cover; `OPAD_BENCH_OFFSCREEN` draws a bench window off the screen. Never capture the
screen or run Add-Type scripts on the user's desktop: it set off their antivirus.

## Command help (`app/help/*.json`, `tests/test_help.cpp`)

Every registered command has a record in `commands.json` + `commands.ar.json` (summary one sentence, details 2-4, keys
only as `{key:id}` / `{press:id}` / `{fixed:name}`) and a clip that shows what the tool really does; `test_help` has no
pending list, so a new command without help fails it (and the richtip/palette benches). An area's group comes from
`help::group` (assets, canvas, kicad -> Insert, as the menu names it). Clip texts need Arabic in `app/i18n/ar/help.json`; chips may only use the
common icons (area `OPAD_ICON_TABLE` icons are missing in `opad-test-help`). Look at the sheets: `OPAD_BENCH_CLIPS=<dir>
OPAD_BENCH_CLIPS_ONLY=<prefixes>` with `--bench-select` on an empty document (gui cases `clips`, `clips-ar`).

## Languages (`app/I18n.hpp`)

No Qt Linguist (MSYS2 has only qt6-base): `JsonTranslator` looks `tr()` source text up in `app/i18n/<code>.json`
(embedded via `i18n.qrc`, context ignored; `<bin>/i18n/<code>.json` overrides). `i18n::install` runs in `main`
before any widget: setting `ui/language`, else system locale; `OPAD_LANG=ar` forces one run (screenshots). Applies
at restart only. Text that is data (property names, `Ref::kind_name`, `opad::Error` messages shown by `guarded`,
yes/no) goes through `i18n::t()` and must be added to the JSON by hand; `python tools/i18n_check.py` covers the
`tr()` literals. New visible strings: always `tr("…")` (UTF-8 is fine), never `QString::fromUtf8("…")`.
Keys in texts (help records, clips, tr() strings and their Arabic) are tokens: `{key:<command>}` / `{press:<command>}` /
`{fixed:enter}` through `help::expand`, clip key elements `"command"` or `"fixed"`, never `"caps"`; `test_help`
(`help_texts_have_no_literal_keys`) flags "(H)", "Press D", "Ctrl+" (Ctrl/Shift/Alt + click/drag/Tab are allowed, in
Arabic write "Alt مع النقر"). Every registered command needs a record and a clip; `every_registered_command_has_help`'s
`pending` list holds those of tracks merged after the wave 3 help pass and fails when one gains help.
RTL: `@rtl` sets the app layout direction. Docks, cube, tool panels and overlays keep their sides. Custom-painted
widgets with fixed columns (`BrowserDelegate`, `TimelineWidget`) force their painter LTR, or `AlignLeft` flips;
property values are wrapped in LRE..PDF so vectors keep their order. Cube labels stay English (OCCT text has no
Arabic shaping).

## Rule: nothing long runs on the UI thread

Anything whose cost scales with the model (bodies, faces, triangles, files) must not block the Qt event
loop. The budget is one frame; the watchdog in `app/Jobs.cpp` logs every stall over 250 ms when
`OPAD_TRACE` is set, and a stall is a bug.

Every such operation goes through `JobRunner` (`app/Jobs.hpp`), the one place that owns progress UI and
cancel:

- `async(...)`: work on a worker thread. Use for core geometry, file IO, STEP reading. The worker gets a
  `Progress` handle (thread-safe) and must never touch Qt widgets or the OCCT context.
- `sliced(...)`: work that must stay on the UI thread (OCCT AIS calls, Qt widgets). Write it as a `step`
  that does one small unit and returns true while more remains; the runner calls it in ~10 ms slices
  between events. A step must itself be small (one body, one node), never "all of it".
- `begin(...)`: a job driven by something with its own threading (document loads in `AppDocument`).

`async` workers are `QThread::create` threads, never `std::thread`: Qt adopts a foreign thread that posts to it, and
on MinGW/Qt 6.10 that thread's TLS cleanup can fault at exit, which pauses the whole process in Windows Error
Reporting for ~2 s *after* the result arrived (the "freeze after the second Distance pick", on any model size; no
bench reproduced it and the trace showed nothing). A fixed-length freeze that ignores model size is a wait, not work.

The runner shows the status-bar `ProgressStrip` only after 0.5 s and wires its Cancel button to the job,
so callers do not add their own timers, dialogs or cancel flags. Tessellating/displaying bodies reports through the
load job while a load runs and through a "Displaying bodies" job otherwise (showing a hidden assembly, leaving
isolation), both fed by `Viewport::meshingProgress`. Benches: `OPAD_BENCH_LOADSHOT=<prefix>` (status bar every 2 s
of a load), `OPAD_BENCH_SHOWROOT=<prefix>` (show hidden roots after the load; the Engine .opad has its root hidden).

Concrete consequences already in place; keep them that way:

- Selection (`Viewport::selectNodes`) highlights one object per step and, if the first slice projects
  more than 0.5 s for the whole set, switches to translucent bounding boxes (`showShade`).
- Sub-shape selection never looks a sub-shape up in its body per item: `opad::subshape_index` walks the whole
  body, so `selection()` over a rubber band was quadratic (15 s for 33k edges). The ordinal comes from
  `SubShapeOwner::index()`. Bench: `OPAD_BENCH_FILTER=face|edge|vertex` switches mode and picks on the body
  with the most faces; with `OPAD_BENCH_BAND=1` it then rubber-bands the whole view and clears it
  (`OPAD_BENCH_SUBSHOT` / `OPAD_BENCH_BANDSHOT=<png>` dump the frames). What is left for select-everything on
  the Engine (~0.5 s) is OCCT's own synchronous `SelectRectangle`.
- Displaying bodies (`Viewport::sync` -> `displayBody`) is a sliced job; the shaded presentation is built
  on the mesh worker (`BodyShape`/`BodyPrs`), not in `Display()`.
- The load worker parses bodies and caches their bounding boxes (`opad::warm_shape_cache`), so the UI
  thread only ever hits cache (`body_shape`, `body_bbox`, `node_world_bbox`). Those are view boxes (padded, poles:
  the Engine's came out 8x too big); reported sizes use `node_tight_bbox` / `scene_tight_bbox` (AddOptimal, cached per
  key, `warm_tight_bboxes` measures unions in parallel: Engine root 138 s -> 21 s), workers only. The Properties
  panel shows `node_properties(geometry=false)` at once and fills geometry from `showNodeGeometry` (async).
- Exact measurements (`BRepGProp` volume/area, exact `BRepBndLib` boxes over many bodies) are never done
  in a click handler. `node_properties(..., geometry=false)` is the O(1) form.
- Never `QProcess::waitForFinished` on the UI thread; connect to `finished`.
- Body objects have `SetAutoTriangulation(false)` and are displayed with selection mode -1, then activated
  once. With auto-triangulation on, OCCT re-runs BRepMesh on the UI thread inside `ComputeSelection` for
  any face whose mesh is missing or coarser than the drawer asks, which froze the app for minutes.
- The STEP reader reports "translating <scope> i/n" through `ImportOptions::progress`; keep phase labels
  specific (file size, part counter, bodies done of total) so long phases visibly move.

To check a change: `OPAD_TRACE=<file> build/windows/bin/opad.exe <big.step> --bench-select` loads the
file, selects every root, logs job timings, slow steps and stalls, then fits and picks the deepest leaf
(`Viewport::benchPick`) and quits. A real click can be driven into the running app from PowerShell
(SetCursorPos + mouse_event on the main window); the trace logs `3D click: N selected`. Keep the mouse still
while it runs: a press/release pair that drifts is a rubber band, not a click.

## OCCT traps met so far (all in `app/Viewport.cpp`)

- Any context change made with `theToUpdateViewer=false` (Display, Redisplay, AddOrRemoveSelected,
  ClearSelected, Remove) must be followed by `redrawScene()` (Invalidate + update), not `requestRedraw()`.
  `FlushViewEvents` only redraws an invalidated view, so otherwise the change shows on the next orbit.
- `Display(obj, mode, -1, false)` does not register the object with the selection manager; `Activate`
  alone then never makes it pickable. `activateSelection` calls `m_ctx->Load(ais, -1)` first. The same
  applies to anything displayed while `SetAutoActivateSelection(false)` is on (the view cube is activated
  explicitly).
- The picker clips to the camera z range, which only `Redraw` (AutoZFit) updates: picking straight after
  `FitAll` without a frame in between detects nothing. Real use always has a frame; benches must `Redraw()`.
- `AIS_ViewCube` paints its hover fill with its own dynamic-highlight drawer's *shading aspect*, so theme
  the colour through `DynamicHilightAttributes()->ShadingAspect()`; replacing the drawer or calling
  `SetColor` on it does nothing visible. `OPAD_BENCH_SHOT=<png>` with `--bench-select` dumps a frame
  with the cube's TOP face hovered, which is how to check it without a steady mouse.
- The cube is `NavCube` (`app/NavCube.cpp`): a sharp cube (bevels sized to zero) whose edges and corners
  are still pick targets because the subclass describes them as bands on the faces (`createBoxPartTriangles`
  override) with owner priorities corner > edge > side; the bands are coplanar with the faces, so never
  lift them, or the face's hover fill gets a dark rim. `OPAD_BENCH_HOVER=dx,dy` moves the bench-shot hover
  point relative to the cube centre.
- X-ray selection: OCCT recolours a selected object's *own* structure in place, so the highlight style's
  ZLayer does nothing for whole bodies. `applySelectionLayers` moves selected bodies to
  `Graphic3d_ZLayerId_Topmost` (own depth buffer) and back; `_Top` inherits depth and does not show through.
  Bench: `OPAD_BENCH_SELECT=<node name> OPAD_BENCH_VIEW=bottom OPAD_BENCH_SHOT=<png>`.
- Selected faces/edges/vertices are not highlighted by OCCT. `StdSelect_BRepOwner` builds one presentation per
  selected sub-shape, in the parent's layer (the style's ZLayer only reaches the hover structure): a rubber band
  over thousands of them froze the app and never showed through. `BodyShape::ComputeSelection` swaps the stock
  owners for `SubShapeOwner` (hover only; stores the sub-shape's ordinal), and `Viewport::refreshSubHighlight`
  draws every selected sub-shape as one `SubHighlight` object in Topmost, copied from the existing meshes in a
  sliced job. Anything that changes the context's selection outside a click must call it.
- The load-time auto-fit (`m_needFit`) is cleared by any user camera move (fit, orbit, zoom, click), or the
  next displayed batch snaps the view back to the whole model.
- Line aspects ignore alpha here: a translucent band under a dashed line draws opaque and hides the dashes
  (`TargetHighlight` puts an opaque white band under the blue dashes). Dashes (`Aspect_TOL_DASH`) do work on
  segment arrays.
- BRepMesh fills a curved wall of an extrusion with triangles that also span along its rims, so they lean: seen along
  the extrusion they cover slivers past the rims, and the outline looks coarser than the edges drawn on it (the
  extruded DXF "artifacts"). `opad::straighten_ruled_faces` (core, after `mesh_shape`) rebuilds cylinders and
  extrusions as upright strips between rims sampled at matching points; the display and previews call it,
  `tessellate()` does not, so headless renders and exports stay byte-identical. Zoom refinement
  (`ViewportRefine.cpp`): once the camera is still for 350 ms, visible bodies whose chords exceed a pixel get
  drawing-only arrays (`BodyShape::setDisplayPrs`, `BodyPrs::build(..., drawingOnly)`) meshed on a copy on a worker;
  picking and highlights keep the base mesh; budgets 1.5M triangles per body, 4M per pass, 12M kept (LRU). Bench:
  `OPAD_BENCH_SCENE=<png>` + `OPAD_BENCH_CAMERA=<camera json>` or `OPAD_BENCH_VIEW=iso OPAD_BENCH_ZOOM=<factor>`
  (`OPAD_BENCH_WAIT` ms, default 3000; the hidden window never paints, so the bench asks for the refinement).
- Translucent elements of a layer are drawn when the next layer that clears the depth starts, else at the very end.
  TopOSD (no depth test, immediate) kept the depth, so Topmost tints were drawn over it (red strokes came out pink):
  `initViewer` sets TopOSD to clear the depth. `InsertLayerBefore/After` can only place custom layers before Top.
- `m_ctx->Redisplay(obj)` rebuilds the selection owners and drops a selected object from the selection. When only the
  look changes (colour, opacity, display style, ray bias) use `RecomputePrsOnly` and `HilightSelected` afterwards.
- A selected body's glow (`m_bodyGlows`) must use the arrays the body is drawn with (`BodyShape::displayPrs` after zoom
  refinement), and is rebuilt when refinement swaps them; on the base mesh it z-fought on curved faces.
- `m_prs` (the worker's shared arrays per body key) can be empty for displayed bodies (a reopened `.opad` draws
  through the stock `AIS_Shape` path): anything reusing body arrays needs a fallback (`showAnnotationTarget`
  builds them on a worker with `BodyPrs::build`).

## Undo / redo

Lives in `AppDocument` (app layer), not the format. Each `run()` that appends ops is one step; undo pops those
ops with `Document::truncate_ops` (their persisted `raw` text is kept), redo pushes them back with
`restore_ops`, so undo + redo + save is byte-identical. Undoing *past* a save and then saving does remove
lines from the file (the tombstone `delete` op is the history-preserving alternative). Clean state is not a
flag: `updateDirty()` compares op ids + body count with the snapshot `markSaved()` took at load/save, so
undoing back to the saved state clears the asterisk. Depth: setting `edit/undoDepth` (Settings > Undo
history…, default 50). The bench logs `bench: undo/redo:` with the dirty transitions.

## Commits

One-line commit messages, no body or trailer.
