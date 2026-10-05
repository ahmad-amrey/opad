# OPAD - git-native CAD and review

OPAD views CAD models, meshes and drawings: STEP (AP203/AP214/AP242, assemblies included), IGES, BREP, STL, 3MF,
OBJ, PLY, glTF/GLB, VRML, DXF, DWG (through LibreDWG's converter, which the portable package includes) and SVG. It saves what you do with them in a single
plain-text `.opad` file that diffs and merges cleanly in git, and exposes everything it can do to scripts and
AI agents through a headless CLI, a Python module and a [stdio MCP server](docs/mcp.md). The desktop app offers mouse
navigation presets familiar to users of other CAD tools (Fusion-style, SOLIDWORKS-style, Onshape-style, Blender-style).

Both MCP servers ship an agent guide (source: `core/res/agent_guide.md`, compiled into the binaries) as the
resource `opad://guide/agent`; live `live_diagnostics` returns it with `include_guide: true`. It covers units and
expressions, base-plane frames, sketch geometry, profiles, references, feature conventions, what `body_ids`
holds per feature kind and batch references.

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
a path that does not exist is reported with the step's name and the keys it has. `@{row#/body_ids/*}` is the
whole list, wherever a list is accepted:

```json
{
  "expected_revision": 12,
  "request_id": "battery-part-1",
  "parent": "@{group#/component_id}",
  "steps": [
    {"id": "group", "command": "component", "arguments": {"name": "Battery pack"}},
    {"id": "solid", "command": "feature", "arguments": {
      "kind": "box", "name": "Battery", "color": [0.1, 0.1, 0.1],
      "inputs": {"length": 70, "width": 50, "height": 4.4}
    }},
    {"id": "row", "command": "feature", "arguments": {
      "kind": "pattern_rect", "inputs": {"bodies": ["@{solid#/body_ids/0}"], "count": 3, "spacing": 60}
    }},
    {"id": "names", "command": "rename", "arguments": {
      "targets": ["@{solid#/body_ids/0}", "@{row#/body_ids/*}"], "name": "Cell {n}"
    }}
  ]
}
```

A feature's new bodies are named after it ("Battery"; several are numbered "Battery 1", "Battery 2", ...);
copies and pieces (patterns, mirror, move with copy, split) are named after their source ("Battery 2") and go
into its component with its colour. `body_name`, `color` and `parent` on a `feature` name, colour and place
its new bodies in the same step, and a batch's `parent` is the default for all of its feature steps: they are
written as the ordinary `rename`, `appearance` and `reparent` operations, so older builds and merges see
nothing new. `rename`, `appearance` and `reparent` take `targets` (a list) instead of `target`; `rename`
then numbers the names, `{n}` marking where. Feature results list `body_ids` (the bodies made or changed)
and, for mirror and patterns, `all_body_ids` (the picked bodies too). The desktop's feature panels have the
same New body section (name, colour, component; the component selected in the browser by default), and
Rename, Colour, Hide and Reparent on a multiple selection are one step each.

Wherever a plane is taken, `{"origin": [x,y,z], "normal": [x,y,z]}` (with an optional `x`) defines it directly,
and the construction plane has a point-and-normal mode.

Two acceptance builds drive the live bridge the way an agent does (`ctest --preset windows-gui`, label `gui`):
`tools/test_agent_benchy.py` builds a Benchy-style boat in four write calls (one valid solid, 60 x 31 x 48 mm, volume
against a committed baseline) and `tools/test_agent_assembly.py` a 38-body phone-style assembly in 10 components with
names, colours and components set where bodies are made (bookkeeping steps under 10%) and no interference but the
intended one.

`validate` also checks interference (overlapping pairs with their volume and box, or pairs closer than a clearance;
bounding boxes first, exact Booleans only on candidates) and 3D printing (overhangs against a build direction,
thin walls, thin features, build-plate contact). The desktop's Inspect tab (Review and Design) has both as Interference
and Print check, listing findings in the tool panel; clicking one highlights the bodies and their overlap, or the faces.

Live write tools take `verbosity: "compact"` for replies that list only what that command changed.

Reported sizes (`info`, `properties`, `validate`, `changes.bodies`, the bounding-box measurement and the desktop's
Properties panel) are the tight box of the exact geometry, not the padded box used to fit views: a 60 x 31 x 21 mm
block reports exactly that. They are measured on workers and cached per body shape.

Volumes, areas and centres of mass are integrated with a Gauss rule per span of every spline (curves and
surfaces) instead of OCCT's fixed count per edge, which read a disc bounded by a 640-point spline 4% high and one
Engine casting at a third of its volume; over the Engine's 1295 bodies they agree with fine meshes and take a third
of the time. `measure` is a read unless pinned (no revision is spent), takes `queries` for several measurements in
one call, and measures distances between surfaces, face pair by face pair from the meshes (a document without
meshes gets a private coarse one), rejecting an exact answer its meshes rule out.

`viewport_image` and `render` take `views` (a labelled grid of fitted views in one image), the model's edges as
lines (`edges` / `edge_lines`), `highlight` (faces and edges tinted) and `shading: "smooth"`; left out, images are
byte-identical to before.

Extrude also goes up to a face (a tilted plane is followed) or up to a body, besides a distance or through all. A profile
named by its boundary (the signed entity ids `sketch_details` lists) is that region, so a ring between two
circles is extruded without a hard-coded point; a boundary no region has is an error listing the ones there are.

A closed fit spline (`periodic`, the first point's id repeated, or a last point lying on the first) is the C2 cubic
through its points with knots at chord lengths, solved as a cyclic system: the same curve whichever point starts it
or whichever way the points run (OCCT's periodic interpolation fixes an estimated tangent at the first point and
moved a 24-point outline by up to 0.05 mm with the seam). Open fit splines take `start_tangent`/`end_tangent`, and a
control-point spline needs only its `degree` (uniform knots, clamped or periodic, unit weights). Horizontal and
vertical dimensions take `signed: true` (they then drive q - p, a negative value putting q before p), and on a
single point they are its signed coordinates, so a point can be fixed at (expression, expression).

Expressions compare (`< <= > >= == !=`, `&&`, `||`, `!`), choose (`c ? a : b`, `if`, `select`), `clamp` and take
`mod`/`%`; a branch not taken may fail, so guards work. `assert(condition, "message")` makes a parameter a check: a
change that makes any parameter fail is refused naming it with its expression's reason or the assert's message, and
live edits that leave operations unresolved say which and why. A plain number a function computed is no longer
taken as degrees when added to an angle; a fractional power of a length says how to normalise it.

A sketch spline can be an equation: `equation: {x, y, t0, t1, tolerance}` with x(t) and y(t) written over the
parameters. Computing the sketch samples it until the spline through the samples is within the tolerance, so the
point count follows from the tolerance and parameter edits reshape it; the samples are stored with the sketch, and
an older build reads them as the fit spline they are. The arm's cycloidal disc, 640 points driven by 1,282 dimension
expressions, is one entity: 1441 points at 0.001 mm in 0.6 s.

Smaller agent frictions: `model_batch` takes up to 100 steps and may refer to steps of earlier batches on the same
connection; `undo` and `redo` are live tools; `export` inside a transaction writes its staged state; `combine` takes
several targets; construction axes and points take points in space (`"point/x,y,z"`, `{"point": [...]}`,
`[x, y, z]`); sketch patterns need only their seeds and inputs; and a STEP export of the same document is the same
file (the header carries the document's last change and the file's name, assembly occurrences are numbered in order).

An `interference` feature keeps an interference and clearance check in the timeline: its report (pairs, overlap
volumes, distances) is stored with the design and computed again whenever its bodies change, and with `fail_on` a
clash is the feature's error, so the edit that causes it says so. In the desktop program it is Inspect > Interference's
Keep as check: the feature's panel in Design with the bodies and clearance just checked, stored when OK is pressed (one
Interference command).

A feature can be suppressed by an expression over the parameters (`suppress_if: "joints < 3"`): the design walk
evaluates it, keeps the answer in the feature's result for replay, and regenerates when a parameter flips it; the
timeline's tooltip shows the condition. Older builds ignore the condition.

Scripted builds can be reproducible: with `OPAD_DETERMINISTIC=<seed>` in the environment of `opad-cli` (and the
headless MCP server), identifiers are derived from the seed, the command and the document's state, and timestamps are
a fixed time, so running the same script again writes the same bytes. Use a seed per document; the desktop ignores
the variable.

Face, edge and vertex inputs can be rules instead of numbers: `{"body": id, "kind": "edge", "select":
{"parallel_to": "z"}, "expect": 4}` picks the matching entities again whenever the feature regenerates and fails,
rather than guessing, when the count changes. The desktop offers the same through "By rule…" on pick inputs, and
marks timeline items whose reference had to be re-picked by nearest match.

Live face, edge and vertex inputs need no token repeated back when the connection was given that reference and its
body has not changed since; a token itself needs only `ref`, `geometry` and `placement`.

Sketches take high-level `shapes` next to raw points and curves: rectangles, rounded rectangles, arcs by three
points or a radius, paths with fillets and tangent arcs, slots, offsets and text in a built-in font (the same on
every machine; the desktop's text tool uses it too). They become ordinary points, curves and constraints before the
sketch is stored, and `id_map` reports what each shape made, including a point inside each letter to extrude or
engrave it.

A body may hold several separate solids: a join into named targets, or `combine`, accepts material that
does not touch them (three screws or a word's letters as one body, no tie bars needed), and a cut that
parts a body keeps the pieces in that body; Split body makes separate bodies.

Without `transaction` or `preview`, a successful batch commits as one Undo step. Pass a
transaction ID to stage it with earlier work, then validate, commit and save separately.
Every step's receipt reports computation, not persistence. Failed batches discard their
own work and preserve the preceding staged transaction. Inputs and backward dependencies
are checked before execution; dynamic reference paths and geometry are checked as steps run.
There is no remote code execution, nesting or file I/O inside a batch. Existing subshape
references go in each step's `references` array and must still match when that step executes.

Creation results include `component_id`, `feature_id`/`body_ids` or `sketch_id`; live
modeling results also expose `operation_ids`. Sketches and features that take a plane return the `frame`
(origin, x, y, normal) it resolved to, and live write results list `changes.bodies`: each body the command made,
changed or moved with its tight bounding box, volume and validity. Live replies, including commit and save,
report `elapsed_ms` (server processing through response preparation, excluding transport
and serialization; staged modeling calls retain their computation timer).

OPAD combines CAD viewing, modelling, review and a 2D drafting foundation: view, import, inspect, measure,
section, annotate, sketch, build features and export. A drawing opened on its own is centred on the
grid; imported, it goes onto the selected planar face, or onto a plane you pick and then drag, offset
or snap it on; converted to a sketch, it keeps exactly that plane and origin. [Drawing and mesh support](docs/drawings.md)
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

Preferences > Keyboard and mouse > Scroll wheel / trackpad says what a scroll in the 3D viewport does:

- **Mouse wheel zooms**: every scroll zooms around the pointer.
- **Trackpad pans**: drag with two fingers to pan, hold Shift while dragging with two fingers to orbit, and pinch to
  zoom around the pointer (Ctrl with two fingers zooms too).
- **Automatic**: guesses from each scroll, for people who switch between a mouse and a trackpad: a wheel zooms and two
  fingers pan.

Until you choose, Windows and Linux take every scroll for a mouse wheel, and the first scroll in the view asks once
("Scrolling zooms the view. Using a trackpad?"): Trackpad pans or Keep zoom saves the answer, and closing the card (or
leaving it until it goes, after 30 seconds) keeps the zoom without saving it; either way it is not asked again. macOS
uses Automatic and asks nothing, since a trackpad's scroll comes with scroll phases there and is told from a wheel
reliably. The choice works the same in every mouse navigation preset.

Automatic tells a wheel from a trackpad by what the system reports, which is where a guess can go wrong. On Linux OPAD
runs on X11, through XWayland on a Wayland desktop, and Qt reports XWayland's pointer (and mice on the old evdev driver)
as a touchpad; there wheel steps (whole notches, or eighths of one from a high-resolution wheel) zoom and other steps pan,
so the first step of a two-finger scroll can be taken for a wheel's. On Windows a precision touchpad's scroll usually
arrives as a wheel's and zooms.

Orbiting over geometry uses the surface under the pointer. Over empty space, OPAD pivots on the
visible geometry nearest the pointer (a surface, or a drawing's or sketch's curve), never on empty
air. Navigation-cube dragging and orientation clicks use the visible surface nearest the viewport
center. Hidden and clipped geometry is excluded; an empty view retains its current camera focus. Preferences > Keyboard and mouse > View cube
edges and corners turn the view (on by default) can make only the cube's six faces views.

## Desktop viewing and review

Open shows any other format in viewer mode: read-only and fast, since nothing is prepared for saving (no
healing, BREP text or hashing). Measure, section, hide, isolate, colour and inspect freely; the title, status bar
and a viewer card at the top of the view say so. Anything that edits asks to save first: Save (or the card's Save
to edit) makes the file an OPAD document in place, keeping hidden layers and colours and the meshes on screen;
"Edit unsaved copy" does the same without choosing a file yet. The file you opened is never written. Opening a
file while another loads drops that load. A slow read (a big STEP or IGES) is remembered in the user cache with its display
meshes, so opening the unchanged file again skips the translation (Hydrostatic: 23 s, then 1.8 s). Preferences > Files >
Open other formats read-only turns viewer mode off (they then open as editable, unsaved documents), and Tools > File
types… (also on Preferences > Files) registers OPAD for these formats with Windows (current user only, removable), with thumbnails of the model or
drawing in Explorer and the Open dialog (`opad-thumbnails.dll`, which runs `opad-cli thumbnail <file> --out x.png`).
Viewing a DXF, DWG or SVG turns 2D mode on, whose grid follows the view without end. Import adds a file to the
current document. Properties show a body's material as the file named it, its source file and whether it is a
solid, a mesh or a 2D drawing. DWG opens through LibreDWG's `dwg2dxf`, which the build compiles from the
`third_party/libredwg` submodule and puts beside OPAD (an installed ODA File Converter is used instead only when
Preferences > Files > Use the ODA File Converter for DWG is on, or `OPAD_USE_ODA=1`: ODA allows non-members non-commercial use only); the
DXF reader shows model space with its blocks, hatches, dimensions, text and colours ([details](docs/drawings.md)); text is shaped with HarfBuzz, so Arabic joins and right-to-left lines read in order, and AutoCAD shape fonts (`.shx`: txt, romans, isocp ...) are drawn in their own strokes when DWG TrueView's or AutoCAD's are installed or the font lies beside the drawing (else in a plain sans-serif). `opad-cli probe <file> --viewer --mesh` reports what opening a file costs, phase by phase.
A KiCad board (`.kicad_pcb`) opens as the board itself (its Edge.Cuts outline with the drills, thickness and solder-mask
colour) and its footprints' 3D models, placed as KiCad places them and found as KiCad finds them (`${KIPRJMOD}`, the
`KICAD*_3DMODEL_DIR` variables from the project, the environment or KiCad's settings, KiCad's install folders, then the
folders in Preferences > Files > KiCad boards); a model that is not found shows as a translucent box over the footprint. Importing a
board asks what to build (components, do-not-populate parts, vias, the origin, the boxes' height); Preferences > Files > KiCad
boards keeps those choices for opening boards too. KiCad's model libraries are
not part of OPAD: when a board names models of KiCad's library that are not installed, OPAD offers to download them from
the library (gitlab.com/kicad/libraries/kicad-packages3D, CC-BY-SA 4.0 with KiCad's design exception) into your user
cache, and shows them (`opad-cli kicad_models board.kicad_pcb [--download true]` lists and fetches them). Models
embedded in the board (KiCad 9) are read from it, and a footprint with only KiCad's VRML model shows that. After the board
changes in KiCad, `opad-cli kicad_sync_preview doc.opad` lists what reading it again would change, per reference
designator (moved, turned, flipped, model changed, added, removed) and for the board (thickness, drills, outline).
With KiCad 7 or later installed, a board can instead be read through KiCad's own STEP export (Read with: KiCad's own
STEP export in the KiCad dialog; `opad-cli import doc.opad board.kicad_pcb --kicad_cli tracks,pads --link true`): OPAD
runs `kicad-cli pcb export step` (found in KiCad's install folders or on PATH, or set `OPAD_KICAD_CLI`) at its own origin,
with the copper tracks, pads and silkscreen if asked (KiCad 8/9), names each part after its footprint's reference and
links the import to the board, so the board is watched and synced and the STEP is made again where it is missing.
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

Select similar (Inspect, Edit and the context menu) works without a dialog: on a picked face or
edge it selects the ones like it (holes of its size, fillets of its radius, faces facing its way),
on a body picked whole its top/bottom perimeter, edges parallel to X/Y/Z, circular edges, upward
planar faces, all holes, fillets and chamfers; pressing it again moves to the next rule, and the
status bar names the current and the next one. The same filters are available through the paged
`query_entities` command (also `{"recognized":"hole","diameter":6}`), with geometric evidence and
fresh checked reference tokens.

The timeline keeps operation markers at a readable size for long histories. Scroll with the
mouse wheel, trackpad or horizontal scrollbar; Left/Right steps through operations and
Home/End jumps to the first/last marker. Selecting an operation scrolls it into view.

Workspace shortcuts are Ctrl+1 Review, Ctrl+2 Design, Ctrl+3 Drawings and Ctrl+4 Drafting (Command on macOS); standard
views use Ctrl+Alt+1 through 7. Review looks, measures, marks up, compares and shares (View, Inspect, Markup, Compare,
Share); Design models (Solid, Assemble, Construct, Inspect, Insert, View), and a sketch adds its Sketch tab in front of
them until it is finished; a viewed DXF, DWG or SVG file comes into Drafting (Draw on drawing: a sketch on the drawing's
plane with the Sketch tab first in Drafting; Drawing to sketch, layers, measuring, plot), and coming there by itself is
not remembered for the next start. A Design command started from Review (E for Extrude, Fillet on edges picked there)
switches to Design with those picks. Solid > History has Edit feature with Suppress and Roll back to here under its
arrow (for the marker selected on the timeline); Construct has Origin planes and axes (shown over a model, picked only
in New sketch's plane step); the View tabs have Named views, Rendering and Panels dropdowns. The menu bar: File, Edit,
View, Insert (Import, Link as asset, KiCad, canvases and their submenus), Inspect (measuring, notes, Compare), Design,
Sketch (while sketching), Version, Tools (AI integration, Agent activity, File types), Help.
Annotations are created and edited inline, with type selection and comment threads. Set your display
name in Settings to identify new annotations, comments and design operations.
Drag a note's title to move its card without changing the document; the card stays attached to its
object at that offset while you orbit and pan. The Annotations panel filters by
type across both the panel and viewport; Delete removes a note and remains undoable.
Review > Markup > **Note** (N) and **Hand drawing** (Shift+N) work like the guided measuring tools: the
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
configures, builds and tests. Clone with `git clone --recurse-submodules` (or run `git submodule update --init` in an
existing clone): `third_party/libredwg` is built along the way into the DWG converters beside the app (once, about a
minute and a half; `-DOPAD_DWG=OFF` skips it).

| Host and target | Install | Build |
|---|---|---|
| Windows | [MSYS2](https://www.msys2.org), then in its shell: `pacman -S mingw-w64-x86_64-{cmake,ninja,gcc,opencascade,qt6-base,nlohmann-json,pybind11,python}` | `cmake --workflow --preset windows` |
| Linux (Ubuntu 24.04) | `sudo apt install cmake ninja-build g++ libocct-*-dev libtbb-dev qt6-base-dev nlohmann-json3-dev pybind11-dev python3-dev libgl1-mesa-dev` | `cmake --workflow --preset linux` |
| macOS | `xcode-select --install`, then `brew install cmake ninja opencascade qt nlohmann-json pybind11 python` | `cmake --workflow --preset macos` |

The three steps can also be run one by one: `cmake --preset <os>`, `cmake --build --preset <os>`,
`ctest --preset <os>`. `ctest --preset windows-gui` adds the desktop safety net: in-app benches that drive
the real app in hidden windows (drag handles, note cards, 2D mode, drawing placement, picking, zoom
refinement...). It needs a desktop session with OpenGL, so the default presets leave it out. Outputs land in `build/<os>/bin`: `opad` (app), `opad-cli`, `opad.pyd`/`opad.so`, the sample
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
  fails if the exe imports anything but Windows' own DLLs. `THIRD-PARTY-NOTICES.txt` goes beside the exes (they also
  carry it compiled in). The exes link Qt, OCCT and other LGPL libraries statically: read [Licence](#licence) before
  handing them out.
- **Single file on Linux and macOS:** `cmake --workflow --preset linux-single` builds
  `build/linux/single/OPAD-<version>-linux-x86_64.AppImage` with [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy)
  and its Qt plugin (both on PATH); `cmake --workflow --preset macos-single` builds
  `build/macos/single/OPAD-<version>-macos.dmg` with macdeployqt. Both run the OS-package build and bundle its
  libraries; `cmake --build --preset <os>-single` runs just the packaging step. `cmake/bundle_stage.cmake` then writes
  `THIRD-PARTY-NOTICES.txt` from the bundled libraries' Debian packages or Homebrew formulae (into the bundle, where
  Help > Third-party licences reads it, and beside it) and stops on GPL FFmpeg, codec, FreeImage or OpenVR libraries
  as the portable target does.
- **Windows portable folder:** `cmake --build --preset windows-portable` (or `cmake --workflow --preset
  windows-portable` for configure, build, test and package) writes `build/windows/portable/OPAD-<version>-windows-x64`
  and the same folder zipped. It holds `opad.exe`, `opad-cli.exe` and every DLL and Qt plugin they load, so it runs
  on a machine with no MSYS2, Qt or OCCT. The `opad.portable` file beside the exe makes the app keep settings and
  cache in the folder's `data` directory instead of the registry and `%LOCALAPPDATA%`. `THIRD-PARTY-NOTICES.txt` lists
  every DLL's package, version, licence and source, with the licence files in `licenses/`. With MSYS2's own OCCT the
  target stops, because that OCCT pulls in GPL FFmpeg (with the x264/x265/xvid encoders) and FreeImage; see
  [Licence](#licence) (`-DOPAD_ALLOW_GPL_DLLS=ON` stages them anyway, for local use only). The exe icon is the logo's
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

## Technical drawings (sheets)

A drawing is a set of sheets in the document itself. `sheet`, `sheet_view` and `sheet_item` operations hold the
definitions: paper size and standard (ISO or ASME, first or third angle projection, scale, title block values),
views (a base view of the model or of chosen components, and views projected from it, which stay aligned with their
parent and follow it when it moves), dimensions and notes. A `properties` operation gives bodies and components part
properties (part number, description, material, BoM flag) for parts lists. The commands are `sheet`, `sheet_view`,
`sheet_item`, `sheet_edit`, `sheet_info` and `part_properties` (CLI, MCP and Python):

```sh
opad-cli sheet plate.opad --size A4 --values '{"title": "Plate"}'
opad-cli sheet_view plate.opad --sheet <sheet> --orient front --at '[100,150]'
opad-cli sheet_view plate.opad --sheet <sheet> --parent <front view> --side bottom
opad-cli sheet_item plate.opad --sheet <sheet> --view <top view> --type diameter --refs '["<body>/edge/9"]'
opad-cli sheet_info plate.opad --sheet <sheet>
opad-cli sheet_view plate.opad --sheet <sheet> --kind section --parent <front view> --cut '[[0,-10],[0,20]]'
opad-cli sheet_view plate.opad --sheet <sheet> --kind detail --parent <front view> --center '[25,5]' --radius 8 --scale 4:1
```

Section views take a cutting line drawn on their parent (two points a full section, more an offset or half section, or
an aligned one when a segment is inclined: each segment revolved onto the first one's line, as through a flange's holes):
the bodies it crosses are cut where it is swept through the model and the cut faces are hatched (ISO 128-50: at 45
degrees to each part's main outlines, parts beside each other turned apart, narrow faces filled; `hatch` or the view's
Hatching… sets the angle, the spacing and the bodies' material symbols, after ASME Y14.2; shafts, pins and fasteners
stay whole, by the view's `whole` list or in every section by the part property `section: false`, Part properties'
Never cut in section views); detail views enlarge a
circle of their parent, auxiliary views look square to a slanted edge, and any view can be cropped to a box or broken to
shorten a long part (dimensions across a break keep their true value; break lines ruled with a zigzag or freehand). A
broken-out section opens up a view within a smooth closed outline down to a depth picked in a view beside it (`breakouts`
on the view): its floor hatched, a thin break line where it ends over the part. An exploded view (`--explode <view op>`,
Drawings > Views > Exploded view) draws the parts where a saved exploded view puts them, seen from its camera, with thin
phantom trail lines from where they sit in the assembly (left out where a part hides them); projected views, balloons,
dimensions, hole callouts and hole tables follow it, and it follows the exploded view when that is updated. A base view
already placed switches between assembled and a saved exploded view from its own side (its context menu's View state,
`sheet_edit` `explode`), and Auto-balloon prefers an exploded view when none is selected.

Annotations are `sheet_item` kinds measured from the model and kept with what they showed: dimensions (with precision,
tolerances and fits), centre marks and lines, hole callouts read from the hole's own faces (depth or THRU, counterbores,
countersinks, "4×" for equal holes) and hole tables, datum symbols, feature control frames, surface texture symbols and
ordinate, baseline or chain dimension sets (`sheet_datum_dimensions` makes them from a view's datums). A parts list
numbers the drawing's bill of materials and keeps the numbers settled; balloons show their part's number
(`sheet_balloons` balloons a whole view at once, each leader to an edge of its part that the view shows, from the side
of the view where it crosses the fewest other lines, off the title block, other views and tables). `sheet_issue` releases a revision: the values, the views' linework
and, with `out`, a PDF and its SHA-256 are kept, the revision table and title block show it, and the sheet can later
be exported exactly as issued; in the app Issue revision… also saves, commits and tags it in git. Print… (Ctrl+Alt+P)
prints the sheets at actual size or fitted to the printer's paper; **Publish PDF…** (File, Review > Share) writes a
drawing's sheets as one PDF from any workspace. In the Drawings workspace section, detail and
auxiliary views, crops, breaks and broken-out sections are drawn with the mouse on the selected view (Views > From a view, or the view's menu); while a tool runs its value card beside the
pointer takes the numbers by keyboard (a section's or auxiliary view's gap, a detail's radius and scale, a crop's width
and height, a break's length, a broken-out section's depth below the part's front), Tab to the next, Enter to take them.
A view's menu also sets a detail's own scale and a section's, detail's or auxiliary view's letter.

The hidden-line linework of a view is never stored: it is a pure function of the bodies' content keys, their
placements and the view's definition, so it is projected when a sheet is shown or exported and cached under that
fingerprint (`opad-cli project`). Opening a document with sheets costs nothing, and a model edit adds no drawing lines
to a diff. A dimension keeps the value it was made with, as a pinned measurement does; `sheet_info` measures it again
and marks the ones the model has changed. Sheets, views and dimensions carry no `target`, so two people adding views and
dimensions to one sheet merge without a conflict; part properties merge field by field.

In the app the browser lists them in a Drawings folder: drawing, sheets, their views (named by the standard view they
show when they have no name of their own) with their dimensions, and the sheet's notes; a record a newer OPAD wrote is
marked with the reason. F2 renames a drawing, sheet or view; Del or the row's menu deletes in one step (a sheet takes its
views and items with it, a base view the views projected from it) and Ctrl+Z brings them back; Copy id gives the id the
commands above take. These operations are not design steps, so the timeline does not show them.

Compatibility: these are new operation types and the format version is unchanged, so a document without drawings is
exactly what it was and opens everywhere. A document with sheets or part properties opens in builds that keep
operation types they do not know (the tolerant loader); older builds refuse it with "unknown op type: sheet".
Within the drawing records the same rule holds one level down: a view kind, dimension type, standard or orientation
this build does not know is kept, written back unchanged and listed among the unresolved operations as needing a
newer OPAD.

### Materials and mass

`material` takes a library id or any name; `opad-cli materials` lists the library (steel, stainless steel, aluminium
6061, brass, copper, titanium, ABS, PLA, PETG, nylon PA6 and PA12, polycarbonate, POM, FR-4 and glass) with densities
and display colours, and `opad-cli materials --match "Aluminum 6061-T6"` shows what a name maps to. A body is made of
the nearest material set upwards (its own, else its component's), else the material its file names (STEP, glTF and
OBJ material names such as "Stainless Steel 316L", "SS304", "PA12" or "Plastic - ABS" map onto the library). Its mass
is the enclosed volume times the density; a `density` property (g/cm3) overrides the library's and a `mass` property
(g) the whole computation, for purchased parts modelled as shells. The Properties panel and `opad-cli properties` show
`material`, `density` and `mass` (g, for a component the sum of its bodies when all of them have one).
`part_properties --appearance true` also colours the targets as their material.

```sh
opad-cli part_properties housing.opad --target <body> --set '{"material": "PETG", "part_number": "OP-1002"}'
```

### Bill of materials

`opad-cli bom` lists the parts of a document (or of one component, `--root`) with their quantities, part properties
and masses. A part is a body, or a component marked `bom: purchased` (bought as one; what is in it is not listed).
Parts are the same when they share a part number, or else the same shape and material: instances of one body entry,
and also copies stored as their own geometry (design patterns, mirrors of symmetric parts, STEP files that write each
occurrence out) when they are the same solid moved and turned. A mirror image of an asymmetric part is a part of its
own. Assemblies are the same when they hold the same items in the same places. `bom: exclude` leaves a node out with
everything under it; mesh and drawing bodies are left out unless `--references true`. Occurrence numbers from CAD
exports ("Bracket:2", "Bolt<3>") and instance numbers shared by a row ("Screw 1" to "Screw 4") are dropped from the
names.

* `--mode parts` (default): every part once, its quantity in the whole product.
* `--mode top`: the items of the assembly itself (a document with one root component is that assembly).
* `--mode indented`: assemblies with their items below them, numbered 1, 1.2, 1.2.1, with the quantity per assembly
  and in total.

Masses are in g (`--mass_unit kg|lb`); a row whose material has no density says why (`mass_error`), and the totals say
whether the mass is complete. `--format csv` writes RFC 4180 text in UTF-8 with a byte order mark (Excel opens it as
such) and CRLF line ends, to `--out` or to stdout; text a spreadsheet would run as a formula (`=`, `+`, `-`, `@`) gets a
leading `'`; `--separator ";"` for locales that use the comma as decimal point. Columns: Item, (Level,) Qty, (Total
qty,) Part number, Name, Description, Material, Mass, Total mass, Vendor, Purchased, Source, Notes and one per custom
property.

```sh
opad-cli bom robot.opad --mode indented --format csv --out robot-bom.csv
opad-cli bom robot.opad --mode top --mass_unit kg
```

## Using it in a git repository

OPAD documents remain readable UTF-8 text with LF endings, append-only operations and immutable
geometry. Existing operation text is preserved on save. New sketches and hand drawings use readable
multiline records, so use the record-aware merge driver rather than Git's union driver:

```
*.opad text eol=lf merge=opad diff=opad
```

Configure the driver in each clone (git never copies it; use the absolute path for your machine). It is built into
`opad-cli` and into the desktop program, so the portable and single-file builds need neither Python nor the CLI:

```sh
git config merge.opad.name "OPAD append-only records"
git config merge.opad.driver '"C:/path/to/opad-cli" merge-driver %O %A %B %P'
# with the desktop program only:
git config merge.opad.driver '"C:/path/to/OPAD/opad.exe" --merge-driver %O %A %B %P'
```

`tools/opad_merge.py` (Python 3: `python "C:/path/to/opad/tools/opad_merge.py" %O %A %B`) is the reference the
built-in driver is tested against (`tests/test_git_merge.py` runs both on the same branches, cases and damaged
files); either works the same. The driver merges independent records and reports overlapping edits, rewritten
history or pruned body stores for review. A conflict leaves the ours file intact and prints the reason; inspect both
branches before resolving it. Without configuration Git falls back to normal text merging. After merging design
changes, check unresolved references and regenerate/validate dependencies.
Large meshes and embedded images can still produce large diffs; Git LFS is optional and gives up normal
text diffs/merges. The detailed [format guide](docs/format.md#git) explains the record layout.

`diff=opad` makes `git diff`, `git log -p` and `git show` print a readable outline of each version (history,
parameters, sketches, features, the tree, notes, one line per body) instead of BREP text:

```sh
git config diff.opad.textconv '"C:/path/to/opad-cli" textconv'   # or '"C:/path/to/OPAD/opad.exe" --textconv'
git config diff.opad.cachetextconv true
```

The desktop program does all of this for you: the git chip in the status bar (branch, untracked / uncommitted /
conflict, ahead and behind its upstream, "not in git", "git not found") has **Set up repository…**, which runs
`git init -b main` when needed, writes the `.gitattributes` line above (plus `assets/**` in Git LFS when git-lfs is
installed, with `.opad` files kept out of LFS), a `.gitignore` for temporary saves, portable data, caches and recovery
snapshots, runs `git lfs install --local`, and points this clone's `merge.opad.driver`, `diff.opad.textconv` (with
`diff.opad.cachetextconv`) and `difftool.opad.cmd` at the running installation (`opad.managed=true`; OPAD rewrites them
when that installation has moved), so `git difftool -t opad` opens two versions in OPAD's Compare. A clone whose
`.gitattributes` asks for `merge=opad` but has no driver configured shows "set up merging" on the chip and a banner
over the view with **Set up merging**. **File > Clone repository…** (also on the chip) clones an address or a folder,
sets the copy up the same way (driver config, `git lfs install --local` and `git lfs pull` when it uses LFS) and opens
its document, or asks which one when it holds several. The chip follows git by file events (HEAD, index, config,
refs, the document's folder), not by polling.

OPAD runs the git command line (Git for Windows, or the `git` on PATH; a portable `git/` or `PortableGit/` folder
beside OPAD is found too, and **Locate git…** on the chip points it at any other). git never waits on a terminal:
sign-in goes through your credential helper (Git Credential Manager) or, without one, through a small OPAD dialog
(`opad.exe` is git's `GIT_ASKPASS`, nothing is stored); SSH runs in BatchMode unless you set `core.sshCommand`, so a
key that needs a passphrase must be loaded into ssh-agent or Pageant. OPAD asks for your name and email before the
first commit when git has none, offers **Trust this folder** when git refuses a repository owned by another account
(`safe.directory`), and explains git's errors in plain words.

`opad-cli diff` compares two versions semantically: parameters, sketch entities and dimensions, feature inputs
before -> after, bodies added, removed, moved, renamed, restyled, reparented or with new geometry, notes resolved or
answered, and how the histories relate. A side is a file or `git:REV[:path]`; one file alone is compared with `HEAD`.

```sh
opad-cli diff model.opad --text                  # what changed since the last commit
opad-cli diff --a git:main~3 model.opad          # JSON; --metrics adds volume and area of changed bodies
```

A big STEP, mesh, drawing or KiCad board you design around without editing can be linked instead of copied:
`opad-cli import doc.opad board.step --link true`. The import records the file's path beside the document (and its
absolute path), its SHA-256 and how it was read; its bodies are never written into the document but read from the file
whenever the document opens (a slow read is remembered by content in the user cache, so a clone or another branch opens
it fast). A file outside the document's folder or its git work tree is read only once you agree (OPAD asks, and can trust
the folder for good; `--trust_assets true` for opad-cli); a missing one leaves only its own bodies out.
`opad-cli asset doc.opad --action status|sync|embed|pack` reports each link (ok, changed, missing, untrusted), syncs a
changed file as one edit of its import (parts keep their ids with their renames, colours, placements and references,
unchanged parts keep their geometry keys, and what depends on the rest is regenerated), embeds a link as ordinary,
editable bodies, or packs the file into `assets/` beside the document. A document with links opens in OPAD builds without
them: the linked parts are listed in the browser and their bodies shown as missing; editing and saving it there keeps the
links.

A record of a type this build does not know (written by a newer OPAD, such as a drawing sheet) is kept as it is: the
file opens, the record is listed as needing a newer OPAD, is never applied or edited, and is saved back byte for byte.
Builds older than this tolerant loader refuse such files with "unknown op type"; open them with a current build.

### Version control in the desktop program

- **Version control panel** (Alt+4, the Version menu, Review > Compare > Versions, the git chip's
  menu): the branch against its remote, the document's state, a merge in progress (Abort, Commit the merge), and
  History / Branches pages. **Commit…** saves first, suggests the message from what changed, asks for your name once,
  offers to amend while the last commit is not pushed and to push after, and offers **Pack** when loose objects pile
  up. **Push** sets the upstream (adding a remote when there is none) and warns about big files outside Git LFS.
  **Pull** fetches, then shows the incoming commits and what the merge does to the document (conflicts, design changed
  on both sides, **Preview in Compare**) before it merges, and offers **Regenerate** afterwards. **Fetch in the
  background** (on by default, every 10 minutes, never asking for a sign-in) keeps the chip's ↓ count current; turn it
  off in the Version menu or the chip's menu. Branches are switched (unsaved and uncommitted changes are asked
  about first), created here or from a commit, merged with the same preview and deleted (unmerged ones are asked about
  twice). A commit of the history can be compared with this session or with the commit before it, opened read-only in
  another window, restored as new changes (one undo step) or branched from.
- **Resolve conflicts…** (the toast after a merge that stopped, the panel's merge bar, the Version menu, the
  chip, and the bar over the view when git wrote conflict markers into the file): lists what both sides changed, with
  mine or theirs to pick for each, all mine, all theirs, or a whole side; the result is written, added to git and opened,
  then Commit… finishes the merge.
- **Compare versions…** (the Version and Inspect menus, Review > Compare, the chip; `opad --compare a.opad b.opad`,
  `git difftool -t opad`; **Compare with a file…** under its arrow): A
  and B picked from this session, the saved file, HEAD and the file's commits, recovery snapshots or another file;
  B's bodies tinted added / modified / moved over A's ghosts, overlay or side by side, ] and [ step through the
  changes.
- **The file on disk** is watched while it is open (a pull or checkout in a terminal, another OPAD, `opad-cli`). New
  changes there come in by themselves when nothing is unsaved; otherwise a bar over the view offers **Merge** (the
  file's changes, then yours), **Reload…**, **Save as…** or **Overwrite…** (asked first). A rewritten history, another
  document, conflict markers or a deleted file get their own bar, and Save never writes over a file that changed on disk.
  The bar's buttons are reachable with Tab; Enter presses one, Esc cancels or closes it.
- **Recovery** (File > Recover documents…, also Review > Compare and the gear menu) lists what each snapshot holds and compares it with its file as it is now: **Restore into file** (the
  changes come back unsaved, after the file's newer ones), **Merge into current** (one undo step), **Restore as copy**,
  **Compare** and **Discard**. **Show unsaved changes** (also **Review changes…** in the unsaved-changes question)
  compares the session with its saved file.
- **Read-only**: `opad --read-only model.opad`, or a write-protected file, opens without letting anything change it;
  **Save a copy** makes an editable copy.
- **Where files are**: File > **Open file location** (Shift+Alt+R) and **Copy path** (Shift+Alt+C), the status path's
  menu (also **Copy relative path**), and an import's timeline marker for the file it came from.
- **Who changed what**: in a repository the timeline's tooltips say who added each operation in which commit and who
  edited it since; **Show in version history** on a marker or an object narrows the History page to the commits that
  touched it.

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
build123d. Build the pip package with `pip install ./python` (needs OCCT on the build machine). The wheel carries
THIRD-PARTY-NOTICES.txt for the libraries the module links; before handing it to other machines, repair it (delvewheel,
auditwheel or delocate) so OCCT's shared libraries travel inside it, then check it with
`cmake -DWHEEL=<the .whl> -P cmake/wheel_guard.cmake`. Repairing against an OCCT built with FFmpeg, FreeImage or OpenVR
(MSYS2's, and often a distribution's) copies those GPL libraries into the wheel; the check refuses it, and such a wheel
must not be distributed (build OCCT without them, as for the portable package).

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

OPAD's own code is MIT ([LICENSE](LICENSE)). It is built on Open CASCADE Technology (LGPL 2.1 with the OCCT
exception), Qt 6 (LGPL 3) and other libraries under their own licences. Every build lists them with versions, licences
and where their source is: Help > Third-party licences, `opad-cli licenses`, and `THIRD-PARTY-NOTICES.txt` in each
package (generated by `cmake/notices.cmake` from the link libraries or the shipped DLLs).

- The `windows` build and the portable package link Qt, OCCT and the rest **dynamically**: each is a separate DLL that
  can be replaced. MSYS2's OCCT pulls in FFmpeg (GPL 3, with the x264/x265/xvid encoders), FreeImage and OpenVR, which
  OPAD never uses; the portable target refuses to stage them unless `-DOPAD_ALLOW_GPL_DLLS=ON`, and such a package must
  not be distributed. An OCCT built without them (as the single-file build does) is needed before the portable zip can
  be handed out. The AppImage and the dmg apply the same guard to the libraries they bundle (distribution OCCT
  packages are often built with FFmpeg and FreeImage too); `cmake/wheel_guard.cmake` checks a repaired Python wheel.
- The single-file build (`windows-static`, `opad-single`) links Qt, OCCT, FreeType, HarfBuzz, glib, libintl, graphite2
  and the other libraries in its notices **statically**. The LGPL requires that recipients can relink such an
  executable with modified versions of those libraries; how that is offered (OPAD's source, or its object files and
  link command, with each release) is not decided yet, so do not distribute the single-file exes until it is.
- LibreDWG's `dwg2dxf` / `dxf2dwg` (GPL 3 or later) are separate programs that OPAD runs and never links; the portable
  package carries their licence, notice, source archive and build scripts in `licenses/LibreDWG/`.
- The ODA File Converter is third-party software that OPAD runs for DWG only when switched on (see above); OPAD never
  bundles or downloads it.
- Drawings are lettered in Liberation Sans and Noto Sans Arabic (SIL Open Font License 1.1), compiled into the programs
  from `third_party/fonts`, where their licences are; the portable package carries them in `licenses/`.

Trademarks: Autodesk, AutoCAD, DWG, DWG TrueView, Fusion and ViewCube are trademarks of Autodesk, Inc.; SOLIDWORKS of
Dassault Systèmes; Onshape of PTC Inc.; Blender of the Blender Foundation; KiCad of the Linux Foundation; Git of the
Software Freedom Conservancy; Qt of The Qt Company; Open CASCADE of Open Cascade SAS; ODA and ODA File Converter of the
Open Design Alliance; Codex and ChatGPT of OpenAI. They are named only to describe compatibility; OPAD is not
affiliated with or endorsed by any of them.
