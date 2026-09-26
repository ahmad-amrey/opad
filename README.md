# OPAD - git-native CAD and review

OPAD opens STEP files (AP203/AP214/AP242, assemblies included), saves what you do with them in a single
plain-text `.opad` file that diffs and merges cleanly in git, and exposes everything it can do to scripts and
AI agents through a headless CLI, a Python module and a [stdio MCP server](docs/mcp.md). The desktop app follows Autodesk Fusion's navigation
and screen layout so Fusion users feel at home.

Live MCP clients can call `save` after committing edits to persist the open document without UI
interaction. Supply an absolute `.opad` `path` for Save As, or omit it to save the current file.
Like other live writes, saving requires editing permission, `expected_revision`, and a unique
`request_id`; replacing a different existing file also requires `overwrite: true`.

`wait_for_idle` reports whether the bound connection can proceed without cancelling a
human editor. Busy failures include a failure-time editing snapshot, owner/client IDs,
revision and retry guidance. Background document snapshots are coordinated with writes.

Live `viewport_image` accepts `select` (body/component IDs), `hide`, `ignore_visibility`
and a custom `camera` or preset `view`. It renders from a temporary scene and returns the
actual camera, revision, render time and visible body IDs without moving the user's view.
`fit: true` frames only the rendered selection; `hide` also works when including hidden bodies.

Live `model_batch` groups up to 50 typed modeling steps into one atomic operation. It
supports components, parameters, sketches, features, naming, parenting, appearance and
transforms. Identifier strings such as `@{solid#/body_ids/0}` (or `@{solid/body_ids/0}`) refer to earlier
step results. The path starts at that step's result (`feature_id`, `body_ids`, `sketch_id`, `component_id`);
a path that does not exist is reported with the step's name and the keys it has:

```json
{
  "expected_revision": 12,
  "request_id": "battery-part-1",
  "steps": [
    {"id": "solid", "command": "feature", "arguments": {
      "kind": "box", "inputs": {"length": 70, "width": 50, "height": 4.4}
    }},
    {"id": "name", "command": "rename", "arguments": {
      "target": "@{solid#/body_ids/0}", "name": "Battery"
    }}
  ]
}
```

Without `transaction` or `preview`, a successful batch commits as one Undo step. Pass a
transaction ID to stage it with earlier work, then validate, commit and save separately.
Every step's receipt reports computation, not persistence. Failed batches discard their
own work and preserve the preceding staged transaction. Inputs and backward dependencies
are checked before execution; dynamic reference paths and geometry are checked as steps run.
There is no remote code execution, nesting or file I/O inside a batch. Existing subshape
references go in each step's `references` array and must still match when that step executes.

Creation results include `component_id`, `feature_id`/`body_ids` or `sketch_id`; live
modeling results also expose `operation_ids`. Live replies, including commit and save,
report `elapsed_ms` (server processing through response preparation, excluding transport
and serialization; staged modeling calls retain their computation timer).

OPAD combines CAD modelling, review and a 2D drafting foundation: import, inspect, measure,
section, annotate, sketch, build features and export. [Drawing and mesh support](docs/drawings.md)
describes supported DXF/SVG entities, mesh reference objects and the optional DWG converter.

```
opad-cli new review.opad
opad-cli import review.opad gearbox.step --by alice
opad-cli tree review.opad                       # component/body hierarchy as JSON
opad-cli inspect review.opad <body-uuid>/face/12
opad-cli annotate review.opad <body-uuid> "check wall thickness here" --by alice
opad-cli export review.opad --format stl --out housing.stl --select <component-uuid>
opad-cli render review.opad --view iso --out shot.png
git add review.opad && git commit -m "review gearbox"
```

## Layout

| Directory | What |
|---|---|
| `core/` | Pure C++20 library (no Qt): document, op log, body store, STEP I/O via OCCT XCAF, tessellation, measurement, deterministic software renderer, diff, command layer, plugin host |
| `cli/` | `opad-cli`, JSON in/out over the command layer |
| `python/` | The `opad` pybind11 module (pip package via scikit-build-core) |
| `app/` | Qt 6 desktop application (browser, timeline, viewport, panels) |
| `plugins/` | C plugin ABI (`plugins/include/opad/plugin.h`) and a sample PLY exporter |
| `tests/` | Headless tests: format, geometry, CLI, git merge story, plugin, Python |
| `docs/` | [File format](docs/format.md), [CLI reference](docs/cli.md), [architecture](docs/architecture.md) |

## Trackpad navigation

In the 3D viewport, drag with two fingers to pan, hold Shift while dragging with two fingers to orbit, and pinch
to zoom around the pointer. These gestures work independently of the selected mouse navigation preset. A mouse
wheel continues to zoom.

Orbiting over geometry uses the surface under the pointer. Over empty space, OPAD uses the
frontmost visible surface nearest the viewport center, searching outward if the center lies in a
gap. Navigation-cube dragging and orientation clicks use the same central surface. Hidden and
clipped geometry is excluded; an empty view retains its current camera focus.

## Desktop viewing and review

Open imports external geometry into a fresh, saveable document; Import adds to the current one.
Settings offers six rendering presets (Classic, Technical flat, Studio, Studio fine, ray traced
shadows and ray traced reflections), four backgrounds and a configurable auto-hide scene browser.
Unsupported ray tracing falls back to raster rendering. Coplanar faces receive a small display depth
bias to reduce flickering without changing model geometry. Bodies are meshed for the whole-model view;
when you zoom in close, the bodies in view get a finer mesh from a worker (within a triangle budget), so
curved outlines keep following the exact geometry, and the walls of extrusions and cylinders are meshed
as upright strips, so looking along an extrusion shows exactly its profile.

Use Settings > 2D projection mode to lock the camera for drawings or model projections. Hover an
endpoint to acquire an extension/alignment guide; Shift locks its direction. Layers live in the
browser, and imported mesh and drawing objects are labelled. See [tracking details](docs/drawings.md).

Inspect > Select by geometry finds top/bottom perimeters, parallel or circular edges, and
upward planar faces on a selected body. The same filters are available through the paged
`query_entities` command, with geometric evidence and fresh checked reference tokens.

The timeline keeps operation markers at a readable size for long histories. Scroll with the
mouse wheel, trackpad or horizontal scrollbar; Left/Right steps through operations and
Home/End jumps to the first/last marker. Selecting an operation scrolls it into view.

Workspace shortcuts are Ctrl+1/2 (Command+1/2 on macOS); standard views use Ctrl+Alt+1 through 7.
Annotations are created and edited inline, with type selection and comment threads. Set your display
name in Settings to identify new annotations, comments and design operations.
Drag a note's title to move its card without changing the document. The Annotations panel filters by
type across both the panel and viewport; Delete removes a note and remains undoable.
Review > Annotate > **Note** (N) and **Hand drawing** (Shift+N) work like the guided measuring tools: the
prompt bar asks for a body, face, edge or vertex (1-4 changes the selection filter; a single selected
object is taken as it is), the target is tinted in the selection blue inside a dashed outline under a
badge, and a floating panel holds the type, the pen and the text. Hand drawing: each stroke lies on the
plane through the picked point that faces the camera when the stroke starts, so orbiting (or clicking the
view cube) between strokes draws on another plane and builds up a 2.5D sketch; drawn strokes keep their
place. Pens are red, green, blue or white at 1, 2, 4 or 8 px, picked from swatches and line samples
(B pen, E eraser, 1-4 colour, [ ] width); strokes can be erased, removed from the list, cleared, and
undone or redone before Save. A note's target can be picked again by clicking another object.
Ctrl+Enter saves one annotation (one Undo step), Esc cancels without changing the document. Notes need
text; **AI agent notes** also need the request in words. MCP clients should review
`context(section="ai_agent_notes")`, fetch `annotations(id=...)` for the full text, comments and each
stroke's plane, and inspect current anchor references before editing.
`annotate` accepts typed `drawing` data and `reply_to` comments; `delete_annotation` removes a note
from review lists while retaining Undo/history. `delete` resolves a completed request.


## Building

OPAD builds natively on each OS against that OS's own packages: install the dependencies, then one command
configures, builds and tests.

| Host and target | Install | Build |
|---|---|---|
| Windows | [MSYS2](https://www.msys2.org), then in its shell: `pacman -S mingw-w64-x86_64-{cmake,ninja,gcc,opencascade,qt6-base,nlohmann-json,pybind11,python}` | `cmake --workflow --preset windows` |
| Linux (Ubuntu 24.04) | `sudo apt install cmake ninja-build g++ libocct-*-dev libtbb-dev qt6-base-dev nlohmann-json3-dev pybind11-dev python3-dev libgl1-mesa-dev` | `cmake --workflow --preset linux` |
| macOS | `xcode-select --install`, then `brew install cmake ninja opencascade qt nlohmann-json pybind11 python` | `cmake --workflow --preset macos` |

The three steps can also be run one by one: `cmake --preset <os>`, `cmake --build --preset <os>`,
`ctest --preset <os>`. Outputs land in `build/<os>/bin`: `opad` (app), `opad-cli`, `opad.pyd`/`opad.so`, the sample
plugin and the test binaries. `tests/fixtures.cpp` generates the STEP fixtures used by the tests.

Requirements: CMake 3.25+, a C++20 compiler, Open CASCADE Technology 7.6+ (7.8+ recommended), nlohmann-json,
pybind11 (optional), Qt 6 Widgets (optional, app only).

- **Windows:** run the commands with `C:\msys64\mingw64\bin` first on PATH. The preset expects MSYS2 in `C:\msys64`;
  for another location override `CMAKE_PREFIX_PATH`, `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` with `-D` or in a
  `CMakeUserPresets.json`. The build stages GCC's runtime DLLs next to the executables, because Git for Windows
  puts an incompatible `libstdc++-6.dll` first on the PATH of its shells.
- **Single-file executable (Windows):** `cmake --workflow --preset windows-single` writes
  `build/windows-static/single/OPAD-<version>-windows-x64.exe` (and `opad-cli-<version>-windows-x64.exe`): one exe
  with Qt, OCCT and everything else linked in, nothing to unpack, no MSYS2 on the target machine. It keeps its
  settings and cache in an `opad-data` folder beside itself. The build needs `pacman -S mingw-w64-x86_64-{qt6-static,rapidjson,pkgconf}`
  on top of the packages above; the first configure downloads the OCCT source and builds its toolkits statically
  into `build/windows-static/occt` (once, about 15 minutes; the OS packages ship OCCT as DLLs only). The target
  fails if the exe imports anything but Windows' own DLLs.
- **Single file on Linux and macOS:** `cmake --workflow --preset linux-single` builds
  `build/linux/single/OPAD-<version>-linux-x86_64.AppImage` with [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy)
  and its Qt plugin (both on PATH); `cmake --workflow --preset macos-single` builds
  `build/macos/single/OPAD-<version>-macos.dmg` with macdeployqt. Both run the OS-package build and bundle its
  libraries; `cmake --build --preset <os>-single` runs just the packaging step.
- **Windows portable folder:** `cmake --build --preset windows-portable` (or `cmake --workflow --preset
  windows-portable` for configure, build, test and package) writes `build/windows/portable/OPAD-<version>-windows-x64`
  and the same folder zipped. It holds `opad.exe`, `opad-cli.exe` and every DLL and Qt plugin they load, so it runs
  on a machine with no MSYS2, Qt or OCCT. The `opad.portable` file beside the exe makes the app keep settings and
  cache in the folder's `data` directory instead of the registry and `%LOCALAPPDATA%`. The exe icon is the logo's
  cube mark; `python tools/make_icon.py <opad_logo.png>` (Pillow, numpy) regenerates `app/res` when the logo changes.
- **Linux:** other distros need the same packages under their own names. On Wayland the app runs through
  XWayland, since OCCT's viewer needs an X11 window.
- **Options:** `OPAD_BUILD_APP`, `OPAD_BUILD_CLI`, `OPAD_BUILD_PYTHON`, `OPAD_BUILD_PLUGINS`, `OPAD_BUILD_TESTS`
  (all ON). Pass them on the configure step, e.g. `cmake --preset linux -DOPAD_BUILD_APP=OFF` for core and CLI only.
  `OPAD_STATIC` (OFF; the `windows-static` preset turns it on) links everything statically, see above.
- **Adding a target** (another toolchain, architecture or package source): add a configure preset in
  `CMakePresets.json` that inherits `base` and sets what differs (compiler, `CMAKE_PREFIX_PATH`, toolchain file),
  plus matching build, test and workflow entries. It gets its own `build/<preset>` tree automatically.

## Languages

The desktop app ships English and Arabic (right-to-left); pick one under Settings (the gear) > Language, it applies
at the next start. Without a choice the system language is used when there is a translation for it.

A translation is one JSON file, `app/i18n/<code>.json`: `{ "source text": "translation" }`, plus `"@name"` (the
language's own name) and `"@rtl"`. To add a language, copy `ar.json`, translate the values, and list the file in
`app/i18n.qrc`; no Qt Linguist tools are involved. `python tools/i18n_check.py` lists the strings a file does not
cover yet. A file `i18n/<code>.json` next to `opad.exe` overrides the built-in one, so a translation can be tried
without rebuilding.

## Using it in a git repository

OPAD documents remain readable UTF-8 text with LF endings, append-only operations and immutable
geometry. Existing operation text is preserved on save. New sketches and hand drawings use readable
multiline records, so use the record-aware merge driver rather than Git's union driver:

```
*.opad text eol=lf merge=opad
```

Configure the driver in each clone (use the absolute path for your machine):

```sh
git config merge.opad.name "OPAD append-only records"
git config merge.opad.driver 'python "C:/path/to/opad/tools/opad_merge.py" %O %A %B'
```

The driver runs on Windows, Linux and macOS with Python 3. It merges independent records and reports
overlapping edits, rewritten history or pruned body stores for review. A conflict leaves the ours file
intact; inspect both branches before resolving it. Without configuration Git falls back to normal text
merging. After merging design changes, check unresolved references and regenerate/validate dependencies.
Large meshes and embedded images can still produce large diffs; Git LFS is optional and gives up normal
text diffs/merges. The detailed [format guide](docs/format.md#git) explains the record layout.

## Python

```python
import opad
d = opad.Document.create()
d.import_step("gearbox.step", by="agent")
plate = d.tree()["roots"][0]["children"][0]["id"]
print(d.properties(plate)["bbox"])
d.annotate(plate + "/face/3", "deburr this edge", by="agent")
d.export("step", "plate.step", select=[plate])
d.render("plate.png", view="top")
d.save_as("gearbox.opad")
```

`Document.body_brep(key)` and `Document.import_brep(text)` exchange OCCT ASCII BREP with OCP, CadQuery and
build123d. Build the pip package with `pip install ./python` (needs OCCT on the build machine; CI repairs the
wheel so OCCT's shared libraries ship inside it).

## Status

Implemented: single-file format with header / JSON-Lines op log / content-addressed body store, all ten
v1 ops, STEP import through XCAF with names, colours, assembly structure and instance dedup, export to
STEP (AP203/214/242), OBJ+MTL, STL (binary/ASCII, per body) and GLB, browse mode, the full CLI
plus `diff --image`, a deterministic software renderer for headless screenshots, the Python
module, the C plugin ABI with a sample exporter, the Qt app with Fusion-style navigation presets, view cube,
display styles, grid, section planes, hierarchical browser (visibility, colour, rename, drag-to-reparent,
isolate, filter, breadcrumb, instance badges), timeline with tombstones, properties, measurements, annotations,
named views, command search, dark/light themes, editable shortcuts and git branch/dirty state.

Not yet: the embedded Python console (may slip to v1.1), full DWG/AutoCAD entity coverage,
shadows are best-effort, interactive drag of the section plane (slider today), coarse-then-fine tessellation
(bodies appear as their fine mesh finishes on a worker thread), signed installers, CI and the iOS/Android core builds, 3MF export.

## Licence

MIT. OCCT (LGPL 2.1 with exception) and Qt 6 (LGPL 3) are linked dynamically.
