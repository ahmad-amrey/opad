# OPAD - git-native STEP viewer

OPAD opens STEP files (AP203/AP214/AP242, assemblies included), saves what you do with them in a single
plain-text `.opad` file that diffs and merges cleanly in git, and exposes everything it can do to scripts and
AI agents through a headless CLI and a Python module. The desktop app follows Autodesk Fusion's navigation
and screen layout so Fusion users feel at home.

v1 is a **viewer and review tool**, not a modeller: import, inspect, measure, section, annotate, export,
commit.

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
- **Linux:** other distros need the same packages under their own names. On Wayland the app runs through
  XWayland, since OCCT's viewer needs an X11 window.
- **Options:** `OPAD_BUILD_APP`, `OPAD_BUILD_CLI`, `OPAD_BUILD_PYTHON`, `OPAD_BUILD_PLUGINS`, `OPAD_BUILD_TESTS`
  (all ON). Pass them on the configure step, e.g. `cmake --preset linux -DOPAD_BUILD_APP=OFF` for core and CLI only.
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

Add this to the repository's `.gitattributes` (the OPAD repo itself ships the same file):

```
*.opad text eol=lf merge=union
```

`eol=lf` keeps body-store hashes stable across operating systems. `merge=union` tells git that two branches
which each appended operations should simply keep both sets of lines, so independent annotations, measurements
and imports merge with zero conflicts. Documents above 25 MB should go through git LFS
(`git lfs track "*.opad"`); the app and `opad-cli info` warn when a file crosses that size.

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

Not yet: the embedded Python console (may slip to v1.1), the MCP adapter (v1.1),
shadows are best-effort, interactive drag of the section plane (slider today), coarse-then-fine tessellation
(bodies appear as their fine mesh finishes on a worker thread), signed installers, CI and the iOS/Android core builds, 3MF export.

## Licence

MIT. OCCT (LGPL 2.1 with exception) and Qt 6 (LGPL 3) are linked dynamically.
