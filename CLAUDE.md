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
Simulation engines (`cmake/sim_engines.cmake`): Eigen 3.4.1 fetched into `build/<preset>/eigen` (MSYS2 has only Eigen 5,
which Chrono 9 rejects); Chrono and Netgen are built at the first configure (~15 min) with MinGW patches
(`opad_patch_chrono/netgen`), their DLLs copied into bin/. Windows headers `#define far`/`near`: never name a variable so.

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

## macOS (Apple silicon, Homebrew)

`brew install cmake ninja opencascade qt nlohmann-json pybind11 python` (OCCT 7.9.3, Qt 6.11), `git submodule update --init
third_party/libredwg`, then `cmake --workflow --preset macos` (90 unit tests) and `cmake --build --preset macos-single`
(`build/macos/single/OPAD-<ver>-macos.dmg`, ad-hoc signed, every dylib inside: check with `DYLD_PRINT_LIBRARIES=1`).
The dmg is the folder `single/volume` (OPAD.app + an `Applications` link) laid out by `cmake/dmg.cmake`: AppleScript has
Finder place the icons on `app/res/dmg-background.tiff` (`swift tools/make_dmg_background.swift`) in a read-write image,
then UDZO; when Finder does not answer (2 min timeout, headless) it warns and writes the plain window (`-DOPAD_DMG_LAYOUT=OFF`).
No pkg-config needed: HarfBuzz falls back to its CMake config. The app is `bin/opad.app/Contents/MacOS/opad`: lookups
beside the program (dwg2dxf, ccx, opad-cli, the git merge driver) also look beside the bundle, and tests that run the app
use that path. Traps met: Chrono sets `CMAKE_MACOSX_RPATH OFF` (built with `CMAKE_INSTALL_NAME_DIR=@rpath`, else every
binary failed to load it); LibreDWG needs `_DARWIN_C_SOURCE` (memmem); libc++'s `file_time_type` counts in `__int128`
(cast before `std::to_string`); clang refuses int -> double narrowing in braces; `std::sort` ties order differently
from libstdc++ (seed orders that matter use `stable_sort`); freed memory is reused at once, so a pointer into a
temporary `Scene` or a reference into a vector that grows fails here first. Key texts are native (⌘, ↩): tests compare
with `keys::` output or pin `keys::setNativeStyle(keys::Style::Pc)`. Bench mode works (`OPAD_BENCH_OFFSCREEN=1`).

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
(`single_check.cmake`). Settings live in the user's folders as for any build ("Where settings live").
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

## Parasolid X_T / X_B (`core/src/xt/`, no dependency)

`xt_parse.cpp` reads text and neutral/typed binary (bare binary refused) into nodes; layouts: embedded edits (V14+ keys
`_13006`), else `schema_for(version)` = base 13006 (`xt_schema.cpp`, from the 2008 reference) minus V13/V11 differences,
plus `xt_layouts.inc` (harvested by `tools/xt_layouts.py` from `OPAD_XT_HARVEST`). `xt_brep.cpp` builds an XCAF doc
(metres -> mm, names/colours from SDL/TYSA attributes, assemblies, transforms row-major: checked, parts touch) fed to
`detail::import_xcaf`. Traps met: knot arrays padded past `n_knots`; spec swaps HELIX_CU/SU_FORM, cone opens along +A;
SP-curves go through `Approx_CurveOnSurface` (affine map of params, `occt_pcurve`) and are attached as pcurves, else
ShapeFix projects 1000-pole sampled curves (77 s on one file); rolling-ball blends are fitted grids (ring spines: whole
spine, periodic fit); faces healing blows up (blend edges projected off the patch) are dropped by `drop_runaway_faces`.
Tests: `ctest -R xt` (cube written from the base schema as text + binary; `tests/corpus/fetch.py` files when present).
Dev surveys (env-gated in test_xt): `OPAD_XT_CORPUS`, `OPAD_XT_BUILD=<dir>`, `OPAD_XT_ONE=<file>` (+ `OPAD_XT_NODES`;
bad faces with their source kind and FACE node), `OPAD_XT_FACES=<dir>` (bad faces by surface kind), `OPAD_XT_STATUS=<dir>`
(BRepCheck statuses of invalid solids). Most invalid solids fail at shell/wire level, not per face. Fixes that paid off:
blends on blends touch them on their charted contact curves (BLEND_BOUND intersections; not for exact supports: worse),
a tolerant edge takes a fin curve on an exact surface over one on a blend, spun circles -> spheres/ring tori, closed
clamped B-surfaces -> periodic (else a tube face splits into slivers), healed faces matched to built ones by surface
(healing gives rebuilt faces new TShapes; colours were lost), a reversed face takes its wires reversed (a loop runs
anticlockwise about the face's *own* normal: otherwise every loop of a reversed face was a hole, which healing turned
round on most surfaces but on a torus or sphere answered with natural bounds = a fillet drawn as the rest of its torus,
a "donut"; blends measure their normal against the ball's centre, `blend_normal_outward`; `public_corpus` checks no face
gains a natural bound, `OPAD_XT_SPILL=<file>` lists faces reaching past their edges). Tried and worse: extending blend arcs past the contacts,
padding the spine range beyond 2%. State: 33 of 1036 solids invalid in the 196-file public survey, mostly blend faces.

## Flow studies (`core/src/sim/flow.cpp`, study kind `flow`)

OpenFOAM's simpleFoam, k-omega SST + Spalding wall functions (laminar below Re 2000), isothermal air or water. `inlet`/`outlet`
= one flat face each: a wall's rim (its hole is the opening, mode "wall") or the end of a body that is the fluid ("volume").
Stubs of straight pipe (`extend`, 2 and 6 diameters) go before and after; `pressure_drop_pa` = mass-weighted total pressure
at the inlet face minus 0.9 of the way down the outlet stub, the stub's own friction added back (its total-pressure slope
from 0.6 to 0.9 carried back to the outlet face; Haaland when the stub is short). Round about the openings' axis (rays at
six angles every 0.25 mm; Douglas-Peucker, corners put back where the lines meet; a wall steeper than 60 degrees, a sudden
step, goes 3D) = a 5 degree wedge in blockMesh (blocks per profile stretch, collapsed axis vertices, the flow and the fan
curve scaled by 5/360; past 30 degrees of wall the non-orthogonal correction), else blockMesh + snappyHexMesh (STLs
wall/inlet/outlet, 3 layers). The fan curve is written inline (`fanCurve table`): openfoam.org reads no curve file. Settings, plan (`plan_flow`) and case files
(`write_flow_case`) need no OpenFOAM: `study` with `run: false` returns `plan`. Recirculation = reversed cells (against
the local bulk way) in face-connected groups touching a wall. Shared OpenFOAM readers and the program runner:
`core/src/sim/foam_case.hpp` (moved out of cfd.cpp). `ctest -R flow`; the solves: `flow-solve` (label slow,
`OPAD_TEST_FLOW_3D=1` adds the full-part check); `OPAD_KEEP_CFD=1` keeps a case's folder.

## Floating tool panels (no right dock)

Properties, Annotations and Section are `ToolPanel`s (`app/Panels.hpp`): frameless tool windows owned by the main
window, anchored to the viewport's top-right corner (right 8, top 186; `positionOverlays` re-anchors them), dragged
by the 32 px header, resized by the corner grip (width 280-480), place/size/pin persisted under `panels/<id>`;
double-click on the header restores the default place. Two kinds: a tool's panel and the document's (`setPersistent`:
Properties, Annotations, Section, Version, Parameters, Layers); `MainWindow::openPanel` hides the unpinned others of its
own kind only, a shown one of the other kind is beside it (the new one to its left, `setDefaultRight`), a pinned one of
its kind above it. Esc closes an unpinned panel (a tool's first) before it clears the measurement. A footer whose
primary says Enter (`PanelFooter::entersPrimary`: Canvas OK, KiCad Sync) is pressed by Enter / Shift+Enter in its panel
(`ToolPanel::enterFooter`) and in the view (`MainWindowView.cpp` `panelEnter`); while a tool's panel shows, a plain
Enter in the view is never Repeat. Properties opens only from the context menu
(`inspect.properties`, Ctrl+P) for the current selection (`m_selRefs`); a new selection closes it
(`selectionMoved`), unless pinned, in which case it follows the selection. Nothing is inspected while it is closed.

## Workspaces (`app/Ribbon.hpp`)

One document, one timeline; a workspace only swaps the ribbon's tab set (`RibbonBar::addWorkspace/addTab/setWorkspace`,
chip + 372 px list left of the tabs). Review (Ctrl+1) and Design (Ctrl+2; setting `ui/workspace`); Document is not
built. Design is the modelling workspace (next section); a third, *contextual* workspace "Sketch" (`Workspace::contextual`,
not in the chip's list, never saved to `ui/workspace`) is entered and left with sketch mode by `updateDesignState`.
Panels moved to Alt+1/2/3. Version control is Design's: Design > Version (panel, commit, branches, remote, compare); a
`vcs.*` command started from Review switches to Design (`switchesToDesign`). The Branches page is a tree
(`git::linkBranches` over `git rev-list --parents --branches --remotes`: each branch under the one it forked from). Set up repository on a
document that shares its folder first asks to move it into a folder of its own (`ownfolder::plan`, `GitWatch::askOwnFolder`:
Move / Use this folder / Cancel; the document alone with its `<name>.opad.*` sidecars is set up in place): `AppDocument::moveTo`
saves it into `<name>` (next free name) beside it with `saveAsync` (linked paths follow, unsaved changes go along), moves the
sidecars, removes the old file, `documentMoved` moves the recent entry; the trusted folders are left alone (linked files left
behind get the trust question at the next open; the move's toast says so). An agent's live `git_init` there refuses `shared_folder` unless `own_folder` says
(`gitagent::initFolder`; true: `AgentBridge` runs `moveTo`, then `git_init` again; `moved_to` in the result). `ctest -R own-folder`, benches own-folder(-ar).
Linked file paths are git friendly (`place_file` in assets.cpp): `path` relative to the document alone, `abs` only without
one (another drive, a share, an unsaved document: relative at its first save), a derived file in OPAD's cache as `cache`; an
import's `source_path` saved relative too; an older document's absolute ones go at its next save (`asset_path_edits`, one undo
step via `followAssetPaths`; its old lines stay: append-only). `rebase_asset_paths` runs before the edits and leaves a relative
path that only resolves from the destination (already written from there). In a repository, `Vcs::checkOutside` toasts the
linked files outside it with Copy into the repository (`AssetsArea::copyIntoProject` = Pack, one step); the own-folder question
offers the same copy (checked). Benches own-folder-linked(-keep), `ctest -R "assets|formats"`.
`.gitignore` (Git.hpp "the OPAD blocks"): Set up repository offers it checked, groups under Customise (`git::ignoreGroups`:
models, drawings, images, opad, system on; simulation off; exports per ExportDialog/Drawings/screenshot); OPAD writes only its
two marked blocks (rules where first written, so the user's later lines win; linked block always last). Linked files stay
tracked by `!/<path>` per document (`git::keepTracked`/`updateTracked`; app: `VcsArea::keepLinkedTracked` ->
`GitWatch::keepTracked`, a quiet job on every refresh/asset look whose input changed; agents: `git_commit`/`git_init` paths
via `opad::linked_files`, the .gitignore staged along when it had no other change), not `git add -f` (status would hide a new
asset, renames would drop out, other git clients refuse to add it). A file inside an ignored *folder* cannot be re-included
(git's rule). Issue revision adds its PDF with `-f` only when OPAD's block ignores it (`opadRuleAt`). `git_init` `ignore`:
false or group ids. `ctest -R git-ignore` (matcher checked against git check-ignore), benches git, own-folder-linked(-keep), issue. `QMainWindow::separator:vertical` is the *upright* bar (named after its shape).

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
  recomputed body is `same_geometry`; `plan_regenerate(doc, true)` must add no body entries (test_design). A failed
  feature's result keeps `held` (the new bodies' id, name, parent, colour from its last good result, `held_bodies`) and
  `materialize` takes them as the previous bodies when it works again: same node ids, so renames, appearance and later
  references survive a sketch change that broke the profile and a re-pick (`ctest -R design`).
- New feature kind = one `add(...)` in `build_specs()` + one `if (kind == ...)` in `compute_feature` (features.cpp).
  The panel, the menu, the CLI and `feature_kinds` follow from the spec; add the icon (Icons.cpp), the ribbon slot
  (`buildRibbon`) and the Arabic strings (labels/hints/choices go through `i18n::t`, so by hand into ar.json).
- New bodies are named in `Walk::materialize` and the result entry keeps it: from scratch = the feature's name
  (numbered if several); `Out::Body::source` set (pattern, mirror, move copy, split) = the source's name + next free
  number, its component (shape stored in that frame) and colour. Explicit body_name/color/parent (feature command,
  batch, panel's New body section) = `style_new_bodies` appending rename/appearance/reparent ops, never result fields.
- Creation features end in `apply_operation` (new/join/cut/intersect; automatic targets are written back into the
  op's inputs).
- Move (no copy) and Align (`align`, features.cpp `align_motion`/`placed_nodes`) output `Out::placed`: result
  `placements`, body keys kept, preview = `setPreviewMotion` (nothing meshed), commit = relocate (Engine, 1295 bodies:
  preview 0.57 s, commit 0.12 s; the old rebuild took 70 s). Align sketch = `design.alignSketch` -> `startSketchAlign`
  (the Align form without bodies; plan = `edit` of the sketch's plane via `align_sketch_plane`; `setPreviewSketch`).
  Benches `align`, `move-perf-engine` (tools/bench_cases/align.py); core `ctest -R align`. References carry a `hint` (centre, size, entity counts): same counts -> ordinal trusted, else nearest.
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
- Rigid (`c:rigid`, `SkConstraint::Type::Rigid`, any number of curves/points): like Fix it takes its shape from the sketch
  as each solve starts (`System::rigid`: every point in the frame of two of them, 2n-3 rows of at most six variables, radii
  held), so the group only moves and turns; a dimension inside it that would reshape it is refused. Saved in
  `more_constraints` (older builds skip it); deleting a member keeps the rest, a split curve's pieces stay in. Moving in
  place (`transform_entities`, not a copy) takes whole groups (`rigid_closure`), or a partial move would reshape the group.
  Sketch Align (`align`, `align_entities`): turns the picked curves about the reference line's end nearer the click so the
  line runs vertical/horizontal/parallel/onto a line, far end snapped exactly; optional holding constraint. The reference
  may be an arc or open spline (`align_ends`: its chord; a DXF polyline's straight-looking segment is often a flat bulge
  arc). Align and Rigid pick by the unsnapped press (`curveAtPress`): the snap near a line's end lands on its end point.
  `ctest -R sketch-rigid`, bench `sketch-align` (OPAD_BENCH_SKETCH_ALIGN).
- Offset relationship (`SkConstraint::Type::Offset`, a dimension: refs = the `sources` curves then what the offset made,
  `value` signed as the kernel takes it, `built` = the distance the curves were made at, `sharp`): the Offset tool's
  default (option `relation`, "none" = plain copy; the agent's `sketch_tool` offset too, `inputs.relation`). The solver
  holds it as Rigid (the chain is not reshaped either); `evaluate_dimensions` ends in `refresh_offsets`, which remakes the
  offset when value != built (the side read off the curves, so a mirrored copy keeps its side; same curves = points moved
  in place, ids kept; else replaced). Its label follows the curves (`offsetLeader`, `pos` unused), double-click edits it
  (the box shows the distance; a plain number keeps the side). Saved in `more_constraints`. `ctest -R sketch-offset`.
- Measure (`sketch.measure`, D in a sketch; Distance keeps D outside it, the scopes never overlap) adds nothing; Dimension is
  Shift+D (migration `shortcuts/measureDefaultsVersion`). `SketchMeasure.cpp`: picks in `m_picked` as Dimension's (Esc and
  Backspace step back alike), `measureResult()` drawn in `updateTransient` and written in the steps. A constraint's badge
  or a dimension's value hovered (`Hit::Dimension`): `hoverTip` puts `constraintTip` in the status bar's hover text at once
  and a QToolTip after 500 ms, also in Trim/Extend/Split (`tipTarget`: their clicks take the curve, `constraintAt` names the
  badge). Bench `sketch-measure`.
- Double-click on a curve in Select selects the chain connected to it through shared ends (`connected_entities`;
  Shift/Ctrl adds it: the view passes the modifiers to `sketchDoubleClick`); on a point it edits spline weights, on a
  dimension's value (an offset's distance too) it opens the value box, as before. Offset of an open chain goes to one side
  only, a parallel copy (`offset_into`: the kernel's open result, `BRepOffsetAPI_MakeOffset(..., IsOpenResult)`; collinear
  lines by hand, the kernel finds no plane for them): until the first pull, typed value or wheel step the arrow follows the
  pointer's side and `offset_side` picks the kernel sign whose copy lies on that side (negative: the other), `offsetSideKey`
  asking for a new preview when a chain's side changes; from the first value on each open chain's aim point is frozen
  (`holdOffsetSides`: the arrow's tip for the arrow's chain, the pointer for the others; `resolveOffsetAims` makes the kernel
  sign of it on the job's worker, never on a pointer move) and the arrow stays put
  (one offset session: `resetOffsetSides` on setTool, Apply, undo, redo, begin). With chain on, the Offset click and window
  take `chain_entities` (ends linked where only two curves of a construction kind meet, and at a branch between two that
  each take the other for the smooth way on, <= 15 degrees and clearly nearest; the chain is the path or loop of links, the
  same from any of its curves; a loose end back on a point the path runs through is trimmed, never the seed; coincident
  ends join); Align, Rigid
  and Move keep `connected_entities`, as does Select's double-click. A click on a picked curve drops that curve alone. Bench sketch-chaindim (also each
  dimension kind drawn whole, its lines, arrows and extension lines, while placed, typed and after: a line, two
  parallel lines, a point and a line, an angle), `ctest -R sketch-offset`.
- An Apply tool's preview (`runSketchEdit`) draws `changed_entities`: exact against the work's own result (before the solve:
  every change the tool made, however small) plus what the solve moved more than 1e-6 x the sketch's size; the solve of an
  under-constrained sketch, or one saved off its constraints (opening never solves), shifts every point by ~1e-7, and an
  exact diff drew the whole sketch dashed. No tool's work may solve the whole sketch itself. Bench sketch-handles (steps 19-20).
- A refused sketch change says why where the user looks: `SketchEditor::refused` (a toast, through `DesignController::notice`)
  besides the status line (a solve refused in `end_change`, a Sketch fillet that does not fit). Sketch fillet names its
  reason (`fillet_problem`: fewer or more than two curves ending at the point, a spline, a tangent join, a radius too big with
  the largest that fits). `ctest -R sketch-fillet`, bench sketch-fillet. The command palette follows the context
  (`CommandPalette(..., sketching)`): within each tier (an exact key, a word match, letters scattered) a sketch's tools come
  first while one is open, the 3D commands outside ("fillet": Sketch fillet or Fillet first).
- Least movement (`SolveOptions::moves`, solver.cpp `movement_stages`): a constraint added (`applyConstraint`: last pick
  first, then the other picks) or a dimension changed (`commitDimensionEdit`) solves with only that geometry movable (the
  held variables' column scale 0, so they keep their values exactly), then with the curves sharing its points, then with
  what the constraints on those hold; the whole sketch (the plain least-squares step, which spread a new constraint over
  every free point) only when no stage satisfies everything. `SketchEditor::m_moves` feeds `solveOptions()` for that one
  change. `ctest -R sketch-movement`.
- Bench without input driving: `opad-cli new x.opad`, then `OPAD_BENCH_DESIGN=<png> [OPAD_BENCH_SKETCHSHOT=<png>]
  opad.exe x.opad --bench-select` draws a dimensioned rectangle + circle through the tool code, extrudes it through
  the panel's plan/commit path, edits the extrude rolled back, fillets, saves, quits; trace lines `bench: design:` /
  `bench: sketch:`. Core coverage: `ctest -R "design|sketch"`.

## Mesh to solid (`core/src/meshsolid/`, `core/src/design/mesh_solid.cpp`, `app/MeshSolidWizard.cpp`)

`design.mesh_solid` is the feature kind's action, overridden to open its own panel (`MainWindow::meshToSolid`), which
previews `design::convert_mesh` (worker) and commits that plan (`commitPlanned`): the body committed is the one measured.
`convert_mesh` tries `mesh_profiles` (extrusion: caps + upright walls with one outline; revolution: vertices collapse to
one (r, z) profile, caps that only have rim vertices reach the axis) -> sketch op + extrude/revolve, kept only when
`mesh_deviation` passes (vertices within 1.5 tol, back distance within 2 tol + 1.5 sagitta, volume 10%); else a
`mesh_solid` feature whose compute is `mesh_to_brep` (fitted regions -> edges -> ShapeFix_Face per face -> sewing; then
planar facets; then one face per triangle). `Ctx::node_shape` refuses meshes, so that compute reads the body's shape
itself. Traps met: a fit checked on vertices only is ambiguous when vertices sit on rims (two rings of a cylinder lie on
a sphere): candidates are chosen by the facets' middles too (`centre_error`); vertex-only arc fits swallow long straight
sides (chord sagitta limited by `mesh_sagitta`); arcs need their centre on the ends' bisector or the sketch's wire does
not close. `OPAD_MESHSOLID_DEBUG=1` traces why a profile or attempt was refused. `ctest -R mesh_solid`; bench
`mesh-to-solid` (OPAD_BENCH_MESHSOLID).

## One-shot tools (close after use, Shift keeps them)

A tool normally used once ends after Apply / OK / Enter and closes its panel; Shift held (Shift+Enter, Shift+click on the
button) keeps it for another use. Sketch: the table `sketchkeys::afterApply` (app/SketchKeys.hpp, a new Apply tool
declares itself there); `applyTool(keep)` sets `m_applyCloses`, `applied()` (every Apply that changed the sketch) goes
back to Select; a failed Apply keeps the tool. The sketch panel's page the user opened (Constraints...) stays
(`SketchPanel::userPage`). Features: `runPreview(true, keep)` starts the same kind again after the commit (new features
only, not edits or Align sketch); Shift comes from `PanelFooter::shiftAccepted` (OK clicked), `ToolValues::shift` /
`DimensionHandle::accepted(bool)` (`DynamicInput::committed(bool)`), the view's eventFilter and a `KeyHook` on the panel.
Shift+Enter is also `edit.repeat`: the tool takes it only while it has an Apply ready (`keepsOnShiftEnter`, a feature
open), Repeat everywhere else. `PanelFooter::setShiftKeeps` shows the hint. Elsewhere through the footer's
`shiftAccepted` / `DrawingPlacer::keepOpen`: Mesh to solid and Drawing to sketch open again (their panels lose their
objectName on the way out, so the command makes a new one), drawing placement and Canvas insert place the same file again
on the same plane, KiCad Sync shows its preview again; Note / Hand drawing (`AnnotationEditor::anotherRequested`;
Shift+Enter in its text is a new line), Section's Pick face (a Shift-click keeps picking) and the Drawings view tools
(`SheetViewTool::commit` reads the keyboard's Shift) start again. Guided measuring tools, sheet annotations, Explode's
Save as view and Canvas editing stay open anyway. Benches sketch-oneshot, feature-oneshot, panels-stay.

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

`ctest --preset windows` = unit tests (74 after TODO 11; label `slow` left out: `ctest --preset windows-slow` runs `cfd-solve`, the
OpenFOAM solves, minutes each and silent meanwhile). `ctest --preset windows-gui` = `tools/gui_benches.py`: the
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

## Fly (`app/ViewportFly.cpp`, command `view.fly` in ViewNavigation)

The camera turns and moves itself (left drag looks around the eye, W/S A/D E/Q or arrows / PgUp PgDn, wheel steps, Esc
lands); speed = 0.8 x the distance to the surface ahead per second (one `navigationPoint` ray at the view centre, every
120 ms), so approaches slow down by themselves. Orthographic: the eye is backed out of the model at start (same
picture), a `Graphic3d_ClipPlane` at the eye cuts what is passed, scale = zoom x focus distance. A camera moved by
another command is taken up in `flyFrame` (from handleViewRedraw). Small parts are not hidden while flying. Off in
sketch / 2D mode. Bench `fly` (OPAD_BENCH_FLY, ViewportFlyBench.cpp). Fixed key names `flyForward`... for help texts.
Trap: `Graphic3d_Camera::SetCenter` returns early when the new centre is as far from the eye as the old one, so turning
about the eye needs `SetDirectionFromEye`.

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

## AI clients (`app/AgentSettings.cpp`, `AgentClients.*`, `AgentClientConfig.*`)

Settings > AI integration opens on Connect an AI client while no client is bound (`AgentBridge::settings(-1)`; the status bar's
AI chip too): rows for Claude Code, Claude Desktop and Codex (`AgentClientRow`), each with its state against this copy
(`agentclients::classify`: connected; not connected; not found; registered but its program missing; registered for another
OPAD or an old data folder = another opad-cli or a discovery folder other than `AgentBridge::m_directory`) and the action that
fits (Connect, Point it at this OPAD = the "opad" entry removed and added, Remove; another live OPAD's entry is never removed).
Claude Code: state read from `$CLAUDE_CONFIG_DIR/.claude.json` or `~/.claude.json` (`mcpServers.opad`, user scope), changed by
`claude mcp add/remove opad --scope user`; Codex: `codex mcp list --json`, `codex mcp add/remove opad`; both asynchronous
QProcess, no shell, 15 s. Claude Desktop: its claude_desktop_config.json rewritten in place (`writeServer`: only
mcpServers.opad, key order kept, <file>.bak first, QSaveFile, nothing written when unchanged). `ctest -R agent-clients`,
gui case agent-connect (APPDATA / HOME / XDG_CONFIG_HOME / CLAUDE_CONFIG_DIR pointed at the case's folders),
tools/test_mcp_feedback.py with a real Codex.
Agents' `expected_revision` is `AppDocument::contentRevision` (ops or the document changed: `refresh(cause, label)`,
`commitSnapshot`, `recover`), never `revision`, which every scene re-resolve moves (`refreshView`: timeline and editor
roll-backs, the icons, detach); `live_state.revision_change` says what moved it last. Appearance-only ops (hide, show,
colour, opacity, lock: `AppDocument::viewOnly`) move only `docRevision` (`refreshLook`; `view_edits` in revision_change),
which every other snapshot-and-swap caller checks (`commitSnapshot`: clipboard paste, recovery merge, version restore);
the agent commits through `commitRebased` (the snapshot's op ids in `Snapshot::baseIds`): the user's looks made since stay
and the agent's ops go after them, so staged work survives a hide or a roll-back (`AgentBridge::rebaseCommit`: `looksSince`
on the UI thread, ids only; `rebase` + the resolve on a worker; a look made meanwhile starts it over). live_state gives the last command's
changes (`change_history` for the kept 20); live `features`/`ops` are pages of compact items (`live_read_defaults`,
core `commands::page_list`; the CLI keeps the plain list). Only a write's `preview` is staging: a read keeps its own
(joint_set preview). The agent guide is served by `##`
section (`guide_index`, `guide_section`, `opad://guide/agent#<id>`); `include_guide` returns the index.

## Builds and the update channels (`core/CMakeLists.txt` build_info, `app/UpdateCheck.*`, `app/UpdatesArea.cpp`)

CI passes OPAD_BUILD_NUMBER (the Actions run number) and OPAD_BUILD_COMMIT to the configure step; a generated
`build_info.cpp` (not version.hpp: ccache) gives `opad::build_number()` / `build_commit()` (a local build: 0 and "").
`opad-cli version` reports them, About shows "X.Y.Z (build N)", settings backups key on "X.Y.Z+N" when N > 0. Help > Offer
development builds (`updates/channel` = development) reads the release list (`OPAD_UPDATE_LIST_URL` for tests): stable vX.Y.Z
and prereleases tagged `vA.B.C-dev.N` (A.B.C the next patch) or `vX.Y.Z-rc.N` (the tag only names it; its -dev/-rc says which,
N must equal the marker's), keyed by (version, build) from the `<!-- opad-build: N; opad-version: X.Y.Z -->` marker (the
binary's version: a dev build after 0.10.0 is v0.10.1-dev.N marked 0.10.0; a marker-less stable is build 0, a marker-less or
otherwise tagged prerelease is ignored), the greatest key above this copy's offered, a stable release before its candidate at
the same key; the API refused: the stable check. Skip stores a prerelease's tag.
`ctest -R update-check`, gui case updates.

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
A label too wide for the ribbon in one language gets a ribbon-only label `"ribbon|<command id>"` in that language's file
(read by `ribbonLabel` in Ribbon.cpp into the action's iconText; menus, palette and help keep the full name): never shorten
the command's own name for room (Arabic Design > Solid at 1600 px uses six).
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
- A mesh body's Face, Edge and Vertex filters pick through `BodyPrs::meshGroups` (BodyShape.cpp `MeshPickSet`: one BVH
  over the triangulations as stored, built on the mesh worker; `GroupSensitive` asks it, owners made as picked by
  `groupOwner`, ordinals as `opad::subshape` counts them), never a sensitive and owner per triangle, side or node: a
  3.3M-triangle scan's Edge filter (the Move panel's Axis box) took 217 s on the UI thread, its Vertex filter 118 s.
  `mesh_circles` (rims) welds by hash and sorts the sides (16 s -> 4 s on that scan). Bench `heavy-mesh`
  (OPAD_BENCH_HEAVYMESH, tools/bench_cases/heavy_mesh.py: a generated 1.2M-triangle tube); `BodyPrs::log` traces a big
  body's build phases.
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
  picking and highlights keep the base mesh; budgets 1.5M triangles per body, 4M per pass, 12M kept (LRU). Anything
  drawn from a body's mesh copies the drawn arrays (`BodyShape::drawnIndex`: selection, smart selection's candidate =
  the timeline hover, glows) and is rebuilt when a pass swaps them; Compare's parts (`refineCompareKey`, their prs must
  carry `deflection` or they are skipped) and its face tints (`refineTintKey`, coloured like the tint) refine too, a
  result landing only while the object's base arrays are the ones it was made from (`refineBase`). Bench:
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
lines from the file (the tombstone `delete` op is the history-preserving alternative). Deleting a step (timeline Del,
smart delete, Del on a sketch or a whole import, Restore on a Remove step or delete record) removes ops for good:
`Document::remove_ops` (the op + edits/deletes aimed at it) / `reinsert_ops`, `design::plan_removal` on a worker (the
engine's `Walk::removed` skips them, then regenerates) + `commit_removal`, `AppDocument::commitRemoval` (a Step with
`removed`/`removedIds`), entry `DesignController::removeOps`. Broken refs downstream are the designer's. The merge
driver (merge.cpp `merge_files` and tools/opad_merge.py, rule for rule) takes a base op missing on one side as deleted
there: left out of the merge, unless the other side's new ops target it ("was deleted on one side and changed on the
other"). DiskSync's `plan_merge` still refuses (as undo past a save). Resolving a note still tombstones. Clean state is not a
flag: `updateDirty()` compares op ids + body count with the snapshot `markSaved()` took at load/save, so
undoing back to the saved state clears the asterisk. Depth: setting `edit/undoDepth` (Settings > Undo
history…, default 50). The bench logs `bench: undo/redo:` with the dirty transitions.

## Variants: comparing files without a shared history (`core/src/variants.cpp`, `opad/variants.hpp`)

`read_variant` opens any file as a document (.opad in index mode, the rest in viewer mode). `match_bodies` pairs bodies
tier by tier, each over what is still unpaired: node id, the file's own ids (body meta `"ids"`: Parasolid gives
*faces* a `node_id`, not bodies; `xt_brep` collects them, `import_xcaf` writes `{"source", "faces"}` per face ordinal and
`follow_face_ids` keeps them through healing; a pair by face ids needs Jaccard >= 0.3 and to overlap or be of a like
size; STEP/IGES face names unique in the file through `detail::transfer_names` over the reader's TransientProcess, 3MF
p:UUID as body id), shape (`body_print`: counts + volume/area/principal moments within 1e-5, rotation-free; `same_place` =
unchanged), faces (no ids: per-face kind/area/centre marks, pairs vote for one translation, Jaccard >= 0.3 over the 8
nearest alike bodies), name (path below the root, or a unique name), near (box IoU). `variant_diff` gives Compare's change list (relation
"variants"), `variant_table` the multi-file table. CompareMode switches to it when the two documents' header uuids differ
(`Run::variants`); `vcs.compareFiles` / `openFiles(files)` (no document: the first file opens, viewed, then they are
compared); the Variants table (`ComparePanel::setVariants`) appears with two other files or more. Side by side with more
files: `Viewport::setSideExtras` adds panes C and D (views on the same context, the main camera copied in `drawPane`; parts
with `view` 'C'/'D' only there, the session's own objects masked out; 3 panes = columns A | C | B, 4 = a 2x2 grid),
filled by `CompareMode::buildExtras` from the table's bodies (`m_variantBodies`, the files in `m_variantDocs`). `opad-cli variants`,
`ctest -R "variants|xt"`, bench `compare-files` (OPAD_BENCH_COMPARE_FILES).

## CI and releases (`.github/workflows/ci.yml`, `promote.yml`, docs/releasing.md)

Branches: **work goes into `dev`** (never `main`), the pre-release branch: only by pull requests, each built, tested and
install-checked on every platform (CI runs on pull requests into `dev` and `main`); part of the setup: `dev`'s ruleset
requires a pull request (merge commits only), `Tested` and "up to date", and blocks force pushes and deletion (without
"up to date" a merge's tree is untested and its push run builds everything). A reused build's commit
(OPAD_BUILD_COMMIT) is the commit its pull request tested, an ancestor of `dev` with merge commits. On main the reuse
takes the run of the newest `vX.Y.Z-rc.<N>` on the commit (build N), so a release and its candidate share the build;
promote also requires that candidate to be published on dev's commit. Every merge into `dev` (the push) publishes a
GitHub pre-release `vA.B.(C+1)-dev.<build>` (the release candidate `vX.Y.Z-rc.<build>` when the version is new; notes:
`[Unreleased]`, or the version's section for the release candidate; marker `<!-- opad-build: N; opad-version: X.Y.Z -->`;
ten kept), reusing the pull request's packages (build once, below); Run workflow on `dev` with `publish` does it by hand.
A batch of work: a short-lived branch from `dev`, the pieces merged there, one pull request into `dev`. `main` = the latest release: it moves only by
*Actions > Promote dev to main* (promote.yml: checks Tested on dev's commit, `main` ancestor of `dev`, a new dated
version; then `git push <sha>:main` with the deploy key PROMOTE_DEPLOY_KEY, a secret of the environment `promote` that only main's copy of the
workflow may use, the deploy key being the only bypass of the `main` ruleset (PR + Tested); a second ruleset,
`main-history`, blocks force pushes and deletion with no bypass; a
GITHUB_TOKEN push would start no workflow), or, in an emergency only, by a hotfix PR into `main` (then merge `main` back into `dev` at once: nothing on `dev` can be
promoted until it holds `main`). Builds
carry OPAD_BUILD_NUMBER (run number of the run that built them) and OPAD_BUILD_COMMIT; a promotion reuses the release
candidate's packages, so a release and its candidate share (version, build). Promoting is Ahmad's; work is merged into `dev` with the coordinating session's go-ahead (Ahmad's, or a session he
authorised).
The version's one source is `project(opad VERSION X.Y.Z)`; a version is released
when it has a published release (tag `vX.Y.Z`), and the workflow creates both: the push to `main` that changes the
version (compared with `github.event.before`) to one without a release publishes it (`gh release create v$VERSION
--target $GITHUB_SHA`), so merging the pull request that raises the version releases it; a later push never does (re-run
or hand-push the tag). Pushes to main and tags get a concurrency group per commit (a pending main run is never cancelled).
Version job: the newest CHANGELOG.md version heading must equal the version (`[Unreleased]` may sit above), an untagged
version must be above the latest tag and have a dated non-empty section (`## [X.Y.Z] - YYYY-MM-DD`,
`tools/changelog_section.sh`); a PR then gets the notice "Merging this pull request publishes OPAD X.Y.Z"; a hand-pushed
tag must equal the version and be on main. Build once: a run that passed everywhere uploads `tested-<git tree>`; a later
run with the same tree (a merged PR that was up to date with its base, `dev` or `main`; the promotion) skips the platform jobs and the release downloads that
run's packages (`run-id`); only successful runs of ci.yml in this repo whose head commit has that tree count. `Tested`
(always runs) is the one required check. Windows = `windows-static-ucrt64` (MSYS2 has the static Qt only for
UCRT64/CLANG64), Linux = OCCT from source + AppImage + `opad-deb`, macOS = Homebrew + dmg (macos-15, Apple silicon);
each job tees build.log and annotates errors on failure; the Windows installer and the .deb are installed, updated and
removed in the job with a check that settings/data survive. Caches: restore at the start, deps saved right after the
configure, ccache saved `always()`; PR caches are PR-scoped and only a branch's own runs save its caches, so the run that
builds (a PR, or dev) uploads `caches-<system>` (deps tar + its cache key + ccache tar, kept 7 days; packages 30) and,
when a merge or promotion reused that run, `warm-*` on dev/main download it and `cache/save` it under that branch (same runner OS: a cache is found only on the system that saved it); without it they
build the missing deps. Actions pinned by SHA (update with the version comment). For each release write the changelog section
(for users: added, changed, fixed, notes; the opening sentences are the update notice's summary). The workflow file is
not called build*.yml: `.gitignore`'s `build*` would hide it; public docs need a `!/docs/<name>` line there.
Traps met on the UCRT64 runner (GCC 16, mingw-w64 v14, Qt 6.12 static): GCC 16 -O3 builds an OCCT that faults in
`Extrema_ExtCC::Perform` under every solid classification, so `occt_static.cmake` builds it `-O2 -fno-strict-aliasing`
under GCC >= 16; GCC 16 refuses Qt's `QDebug << json` meta-type probe (Windows' incomplete `MSG`), so `opad/json.hpp`
answers it false in Windows Qt code; `qt6-static` names none of the libraries its CMake configs look for (webp, tiff,
brotli... in `MSYS2_PACKAGES`), and a missing one used to skip the app silently: CI configures with `OPAD_REQUIRE_APP=ON`;
MSYS2's git answers with `/tmp/...` paths, so the tests take Git for Windows (`/c/Program Files/Git/cmd` on PATH); the
static exe links the offscreen platform (askpass in headless tests); `single_check.cmake` allows `dnsapi`/`mscms` (Qt
6.12). Test programs keep their symbols in the static build and `check.hpp` flushes after each case, so a crash shows its
case and gdb's backtrace (the "Backtraces of the failed tests" step).

## Update check (`app/UpdateCheck.*`, `app/UpdatesArea.cpp`)

Help > Check for updates and the automatic check (setting `updates/autoCheck`, ~20 s after start and hourly while open,
due once a day; only a check that got an answer counts) ask GitHub's latest-release API anonymously, compare the tag with
the app version and offer a newer one in a toast (What's new = the release body, Download = this system's installer from
the release, `installerAssetName`: setup exe / Apple-silicon dmg / .deb on x86-64 Debian-family, else the page; Skip this
version = `updates/skipVersion`); the browser downloads, the user installs. When the API refuses (403/429: the 60/h
anonymous limit is per address), the version comes from the HEAD redirect of `releases/latest` (no notes; What's new
opens the page). The Download / What's new buttons open only github.com/ahmad-amrey/opad/releases/ URLs
(`safeReleaseUrl`); links inside the notes open as they are. One notice at a time (`m_notice`); a user's check during the
automatic one is answered by it (`m_userAsked`). The Windows single exe (no `unins000.exe` beside it) downloads
`OPAD-windows-x64.exe`, an installed copy the setup. No automatic check while
any `OPAD_BENCH*` variable is set or with `OPAD_NO_UPDATE_CHECK`; `OPAD_UPDATE_URL` / `OPAD_UPDATE_PAGE_URL` point the two
requests elsewhere (bench `updates`; `ctest -R update-check` runs a fake GitHub on a local socket). HTTPS needs Qt's TLS
plugin: the static build imports `Qt6::QSchannelBackendPlugin`, the portable stage copies `tls/qschannelbackend.dll`.
Asset names are fixed by the release job's renames (version-free); renaming one means the README links and
`installerAssetName` too.

## Where settings live (`app/AppData.*`)

Every build, single-file included, keeps settings in Qt's native place (registry `HKCU\Software\opad\OPAD`,
`~/.config/opad/OPAD.conf`, the macOS plist), data in `AppLocalDataLocation`, templates in `AppDataLocation` (roaming
on Windows: `templates::folder()`); the cache is `opad::cache_dir()`. Only
an `opad.portable` file beside the exe (or `OPAD_BENCH_SETTINGS`) switches to INI + cache in `<exe dir>/data`
(`appdata::start` = use + adopt, from main and from opad-cli through `gitagent::useAppSettings`). Single-file builds
before 0.9.0 used `<exe dir>/opad-data`: `appdata::adoptOldPortable` copies its settings (when the native ones are
empty), templates and data folders (not the cache, not `agent/`) once and leaves it. A dev build and an installed copy on one machine share the settings. `ctest -R app-data`.

## Commits

One-line commit messages, no body or trailer.
No bare `@word` in a commit message, pull request title, changelog or release text unless it is a real GitHub login meant to be
notified: GitHub turns it into a mention (the release notes copy pull request titles; macOS's `@rpath` once listed an
unrelated account as a contributor). Put such words in backticks: `@rpath`, `@executable_path`.
