<div align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/media/opad-logo-dark.png">
    <img src="docs/media/opad-logo.png" alt="OPAD" width="300">
  </picture>
  <br>
  <br>
  <b>Open-source parametric CAD whose files git can diff and merge.</b>
  <br>
  <br>

  **English** · [العربية](README.ar.md)
  <br>
  <br>

  ![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue)
  ![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)
  ![Qt 6](https://img.shields.io/badge/Qt-6-41CD52)
  ![Open CASCADE 7.8+](https://img.shields.io/badge/Open%20CASCADE-7.8%2B-orange)
  ![Windows | Ubuntu 24.04](https://img.shields.io/badge/platforms-Windows%20%7C%20Ubuntu%2024.04-lightgrey)
  ![MCP](https://img.shields.io/badge/AI%20agents-MCP-8A2BE2)
</div>

<p align="center">
  <img src="docs/media/hero.gif" alt="OPAD opening a STEP assembly: hovering parts, orbiting, exploding the assembly and putting it back together" width="1000">
</p>

**OPAD** (**O**pen-source **P**arametric C**AD**) is a 3D parametric CAD program for every level, from someone drawing
their first sketch to an engineer working on a thousand-part assembly. It opens 2D drawings, 3D models and meshes,
takes you from a 2D sketch to a solid with an editable history, and keeps the whole design in one plain-text file
that **git can diff and merge**. AI agents can work on the model in the same window you are using, and every command
also runs from the command line.

It runs on your own computer, with no account and no server, and is built on Open CASCADE and Qt.

## Contents

- [Features](#features)
  - [Git version control](#git-version-control)
  - [Opening files](#opening-files)
  - [From 2D to 3D](#from-2d-to-3d)
  - [Technical drawings](#technical-drawings)
  - [Notes, annotations and hand drawing](#notes-annotations-and-hand-drawing)
  - [KiCad integration](#kicad-integration)
  - [Motion and simulation (beta)](#motion-and-simulation-beta)
  - [Performance on big models and drawings](#performance-on-big-models-and-drawings)
- [Integration: AI agents, CLI and Python](#integration-ai-agents-cli-and-python)
- [Languages](#languages)
- [Supported platforms](#supported-platforms)
- [Installation](#installation) · [Usage](#usage) · [Contributing](#contributing) · [FAQ](#faq) · [Licence](#licence)

## Features

### Git version control

Most CAD files are binary: git stores them, but it cannot tell you what changed between two versions, and two people
who edit the same part cannot merge their work. OPAD's file format was designed so that git *can* do both.

#### A file git can read

An `.opad` file is UTF-8 text with two parts: a **log of operations** (one JSON record per sketch, feature, note,
rename...) and a **store of bodies**, each saved once as BREP text under the SHA-256 of its content. Saving appends new
records and never rewrites the old ones, and a body that did not change keeps its key. A diff therefore shows only what
you did, never a reshuffled file.

```text
#opad 2
{"uuid":"7c1c...","units":"mm","created":"2026-09-15T14:10:40Z","generator":"opad/0.1.0"}
#ops
{"op":"import","id":"6dda...","by":"alice","source":"gearbox.step","units":"mm","nodes":[...]}
{"op":"annotation","id":"1ffe...","by":"alice","anchor":{"body":"9dc9...","kind":"face","index":12},"text":"deburr"}
#bodies
#body a0d6fc12...058d 310 {"name":"Housing","color":[0.2,0.5,0.8],"units":"mm","source":"gearbox.step"}
...OCCT ASCII BREP...
```

The format is also safe across versions: records a build does not know are kept, listed as needing a newer OPAD and
saved back unchanged. [docs/format.md](docs/format.md) describes it.

#### Readable differences

`opad-cli diff` compares two versions (two files, or a file and any git revision) in design terms. Here Bob changed
the `height` parameter of Alice's bracket:

```text
$ opad-cli diff --a git:HEAD~1 bracket.opad --text
b continues a: 2 ops in common, 2 new.
Parameters
  ~ height: 25 mm -> 40 mm
Features
  ~ Bracket: regenerated
Bodies
  ~ Bracket: geometry changed (b6b0f6452a39 -> 6cd23d2ec334)
```

With OPAD's `textconv` set up, plain `git diff`, `git log -p` and `git show` print the same kind of outline (history,
parameters, sketches, features, the tree, one line per body) instead of BREP text.

#### Merging

A merge driver that understands OPAD's records (built into `opad-cli` and the desktop program) merges independent
changes from two branches and stops only on real conflicts, such as both sides editing the same feature.

#### In the desktop program

You never need a terminal:

- **Set up repository** creates the repository, `.gitattributes`, `.gitignore` and the merge and diff settings
  (Git LFS for big assets, when installed). **Clone repository** does the same for an existing project.
- The **Version control** panel commits (with a suggested message), pushes, pulls, switches and merges branches, and
  shows the history. Before a pull merges anything, it shows what the merge will do to the design.
- **Resolve conflicts** goes through a stopped merge change by change: mine, theirs, or all of one side.
- **Compare versions** shows two versions in one view: added, changed and moved parts tinted, a slider that fades
  between the two, and the list of changes with each value before and after, stepped through with `]` and `[`. Compare
  any two of: this session, the saved file, any commit, a recovery snapshot or another file.
- The timeline's tooltips say who added each step and in which commit.
- **Issue revision** freezes a drawing sheet as issued, keeps its PDF with its SHA-256, and commits and tags it.

![The Version control panel's history, then Compare between the first commit and now: the thicker plate and raised upright tinted as modified, the new pin as added, the slider fading to the first commit and back, and the changes stepped through](docs/media/compare.gif)

[docs/features.md](docs/features.md#using-it-in-a-git-repository) covers the git setup in detail.

### Opening files

#### Supported formats

| Kind | Open and import | Export |
|---|---|---|
| **3D solids and parametric models** | `.opad` (full history), STEP AP203/214/242 (assemblies, names, colours, materials), IGES, BREP | `.opad`, STEP |
| **3D meshes** | STL, 3MF, OBJ, PLY, glTF/GLB, VRML | STL, OBJ, GLB |
| **2D drawings** | DXF, DWG, SVG | DXF, DWG, SVG, PDF, PNG |
| **Electronics** | KiCad boards (`.kicad_pcb`) | |

DWG is read through LibreDWG, which is built with OPAD; the ODA File Converter can be used instead when it is
installed.

#### Viewer mode

Any file other than `.opad` opens in **viewer mode**: read-only and fast, because nothing is converted until you want
to edit. You can hide, colour, section, explode and measure freely, and the file you opened is never written.
**Save to edit** turns it into an OPAD document, keeping what you hid and coloured.

![The AS1 test assembly exploded at every level: brackets, rod, nuts and bolts move apart along their own directions, then the view orbits](docs/media/explode.gif)

#### Mesh to solid

A mesh can become a real solid: **Mesh to solid** rebuilds an STL, OBJ, 3MF or PLY as a sketch with an extrusion or
revolution when the mesh is one, and as fitted planes, cylinders, cones, spheres and tori otherwise. Before creating
it, it tells you how closely the result matches the mesh.

### From 2D to 3D

OPAD is a sketch-based modeller: you start in 2D and go into 3D.

#### Where the 2D comes from

- **A drawing you already have.** Open a DXF, DWG or SVG, place it on a plane or on the face of a part (drag it, type
  an offset or snap it to a point), then **Draw on drawing** (a sketch on its plane, to trace over it) or **Drawing
  to sketch** (its layers become an editable sketch).
- **A picture.** Place a photo or a scan on a plane, calibrate it with a distance you know, and trace it into a sketch.
- **A blank sketch.** A constraint solver, driving dimensions and snaps that become constraints. Inside a tool you
  type values straight into boxes beside the pointer: start Rectangle, type the width, Tab, the height, Enter.

![A sketch typed by keyboard (rectangle and circle), finished, then extruded by dragging the arrow and typing 25](docs/media/design.gif)

#### Solids from the sketch

Extrude, revolve, sweep, loft, pipe, coil, hole, fillet, chamfer, shell, draft, press pull, combine, split, mirror,
patterns, involute gears and more, each with drag arrows and typed values. Parameters are named expressions you can
use in any field.

#### An editable history

Double-click a step on the timeline, change a value, and everything after it is rebuilt: the fillets below follow the
taller extrude. Steps can be suppressed (also by a condition on the parameters), and the timeline can be rolled back
to any point.

![Four top edges picked and filleted, then the extrude edited from 25 to 40 mm on the timeline](docs/media/parametric.gif)

### Technical drawings

The **Drawings** workspace (Ctrl+3) turns the model back into 2D: sheets with views, dimensions and a title block,
kept in the same `.opad` file.

- **Sheets**: ISO or ASME, first or third angle projection, built-in templates or your own DXF/DWG frame and title
  block, with title block fields filled from the document.
- **Views**: base, projected, isometric, section (full, offset, half, aligned), detail, auxiliary, broken-out, cropped,
  broken and exploded views, drawn by a hidden-line engine. Section views are hatched to ISO 128-50, and shafts and
  fasteners can stay uncut.
- **Annotations**: dimensions with tolerances and fits; ordinate, baseline and chain sets; hole callouts read from the
  model (counterbores, countersinks, "4×"); centre marks and lines, datums, feature control frames, surface texture.
- **Tables**: a parts list with balloons (auto-balloon places them around a view), a hole table and a revision table.
- **Output**: PDF (one page per sheet), SVG, DXF, DWG or PNG, and printing at actual size.
- **Issue revision** freezes a sheet as issued: its values and views are kept, the revision table and title block show
  it, and the PDF is stored with its SHA-256.
- **Made for git**: the views' lines are never stored, only how each view is defined, so a model edit adds no drawing
  noise to a diff, and two people adding views and dimensions to one sheet merge without a conflict. A dimension keeps
  the value it was made with, and OPAD marks the ones the model has changed since.

![New drawing lays out four views of the part; three dimensions are picked on the views](docs/media/drawings.gif)

[docs/drawings.md](docs/drawings.md) has the details.

### Notes, annotations and hand drawing

- **Notes** (N) pin to a body, face, edge or point and stay attached while you orbit. Each note has a type (OK,
  Warning, Issue, Note, or **AI agent note**: a request that an AI agent picks up), a comment thread, and can be
  resolved.
- **Hand drawing** (Shift+N) works in 2.5D: each stroke lies on a plane facing the camera when you start it, so
  orbiting between strokes builds a sketch around the part. Pens come in four colours and widths, with an eraser.
- **Pinned measurements** keep a distance, angle or radius in the document.
- The **Annotations** panel lists them all and filters by type. Every note is saved in the `.opad` file, so a review
  travels with the design and shows up in its diff.

![A red ring around the bracket's bolts, an orbit, a blue arrow at the rod, then an AI agent note pinned to the rod's end](docs/media/notes.gif)

### KiCad integration

#### The board in 3D

A `.kicad_pcb` opens as the board (outline, drill holes, thickness, solder-mask colour) with its footprints' 3D models,
placed and found the way KiCad finds them. Models from KiCad's library that are not installed are downloaded on
request into your user cache; until then a box stands in for each. With KiCad 7 or later installed, a board can
instead be read through KiCad's own STEP export (with KiCad 8 or 9, also its copper tracks, pads and silkscreen).

![KiCad's pic_programmer demo board opened: its library models downloaded on request and shown on the board, then the view orbits](docs/media/kicad.gif)

#### Linked and synced

A board imported into a document stays **linked** to its file. When it changes in KiCad, **Preview KiCad sync** lists
what changed per reference designator (moved, turned, flipped, model changed, added, removed) and shows it on the
board before you sync.

![The board linked into a document; a socket moved 15 mm in the board file; Preview KiCad sync lists P3 as moved, shows it, and Sync moves it](docs/media/kicad-sync.gif)

#### Designing the enclosure

**Project KiCad board** brings the outline, mounting holes and chosen parts into a sketch, and **Check clearance to
board** measures the gap between the parts and the case around them.

### Motion and simulation (beta)

> This part is still being tested. Expect rough edges, and check results that matter by hand.

The **Simulate** workspace (Ctrl+5) turns an assembly into a mechanism and studies it. Every model in these pictures
was built through OPAD's MCP server by [tools/sim_eval.py](tools/sim_eval.py), which also checks each result against
its textbook formula.

#### Mechanisms

- **Joints**: revolute, slider, cylindrical, ball, planar, pin-in-slot, screw, rigid and ground, with limits.
  Gear, rack-and-pinion and lead-screw relations couple joints. Drag a joint's slider and the mechanism follows.
- **Motion studies**: joints driven over time, with traced points, speeds and accelerations.
- **Dynamics** (Project Chrono): gravity, motors, springs, friction, end stops and contacts, with forces, torque, power
  and energy plotted.

![A single-cylinder engine run at 3000 rpm with Project Chrono: crank, rod and piston played back while the chart follows the kinetic energy, then the motor torque](docs/media/motion.gif)

![A planetary gear set with involute teeth, driven by the carrier's joint slider: the planets roll round the fixed ring and the sun turns 3.7 times as fast](docs/media/gears.gif)

#### Structures

**Stress and vibration** studies (Netgen meshes, CalculiX solver) take fixed supports, forces, pressures, gravity and
bolt preload, and show stress, displacement, safety factors and mode shapes as colour maps.

#### Heat

Heat sources, convection, radiation and fans on heatsinks, steady or over time; with OpenFOAM, the air flow around the
parts is solved too. The **Cooling assistant** takes you through cooling a board in a vented box step by step.

![A 30 W chip on a finned heatsink with a fan: the warm-up study run with CalculiX, then played back over 300 s as the heatsink heats up and the chart follows its temperature](docs/media/thermal.gif)

#### 3D-printed parts

A study can treat a body as printed, with settings read from a PrusaSlicer, OrcaSlicer, Bambu Studio or Cura profile,
and says where and how the part would fail.

Each engine is checked against textbook cases (a pendulum's period, a cantilever's deflection, a plate with a hole,
a planetary gear set and others). The [simulation guide](docs/simulation-guide.md) walks through eleven use cases, and
the same guide is in the program.

### Performance on big models and drawings

OPAD is developed against a real engine assembly: 1,295 bodies, 374 MB of STEP. Anything slow on that model is
treated as a bug.

- **Second opens are fast.** A slow STEP or IGES read is remembered together with its display meshes, so opening the
  unchanged file again skips the translation: a 45 MB transmission takes 23 s the first time and 1.8 s after that.
- **Smooth navigation on heavy scenes.** Small parts can be hidden, and detail lowered, while the view moves, so big
  boards and assemblies turn smoothly.
- **Sharp up close.** When you zoom in, the parts in view get a finer mesh, so curved outlines keep following the exact
  geometry.
- **Big drawings.** A drawing layer of 100,000 lines opens without stalling the window, and a click still picks a
  single line exactly.
- **Fast measuring.** Distances are exact and measured on several cores: face to face on the engine takes about a
  tenth of a second.

## Integration: AI agents, CLI and Python

The desktop program, the command line, the Python module and the AI servers all go through the same command layer,
so anything one of them can do, the others can too.

### AI agents over MCP

`opad-cli mcp` is an MCP server that works on files. With **Tools > AI integration** on, `opad-cli mcp --live` connects
an agent to the window you have open instead: you watch it work, every change it makes is one undo step for you, and
**Agent activity** shows what it is doing, with a Stop button. Changes can be previewed or grouped in a transaction,
and agents can render pictures of the model. The setup page registers OPAD with Codex and gives the MCP JSON for other
local clients and VS Code.

![An agent builds a small boat through the live bridge in four batched calls; Agent activity lists each call](docs/media/agent.gif)

### Command line

`opad-cli` runs 75 commands without a window and prints JSON, for scripts and CI:

```sh
opad-cli new review.opad
opad-cli import review.opad gearbox.step --by alice
opad-cli tree review.opad                       # component/body hierarchy as JSON
opad-cli annotate review.opad <body-uuid> "check wall thickness here" --by alice
opad-cli export review.opad --format stl --out housing.stl --select <component-uuid>
opad-cli render review.opad --view iso --out shot.png
opad-cli bom review.opad --format csv --out bom.csv
```

`opad-cli commands` lists every command with its arguments. With `OPAD_DETERMINISTIC=<seed>`, running the same script
again writes the same bytes.

### Python and plugins

The `opad` Python module runs the same commands and exchanges BREP with OCP, CadQuery and build123d. A C plugin
interface adds export formats (a PLY exporter is the sample).

## Languages

OPAD ships in **English** and **Arabic**. In Arabic the whole window is laid out right to left, the help cards and
their animated clips are translated, and Arabic text in drawings is shaped correctly. A new language is one JSON file
(`app/i18n/<code>.json`), and `python tools/i18n_check.py` lists what a translation does not cover yet.

![The Design workspace in Arabic, right to left, with the Extrude card open](docs/media/arabic.png)

## Supported platforms

| Platform | Status |
|---|---|
| **Windows** (x64) | Built and tested. MSYS2 / MinGW-w64 toolchain. |
| **Ubuntu 24.04** (also under WSL) | Built and tested. Wayland desktops run through XWayland. |
| **macOS** | A build preset exists, but it has not been built yet. |

There are no published binaries yet: OPAD is built from source (see [Installation](#installation)). Besides the normal
build there is a portable Windows folder (settings and cache kept beside the program) and a single-file Windows
executable. AppImage and dmg packaging recipes exist but have not been run yet.

## Installation

```sh
git clone --recurse-submodules <this repository>
cd opad

# Windows, in an MSYS2 shell (C:\msys64\mingw64\bin first on PATH):
pacman -S mingw-w64-x86_64-{cmake,ninja,gcc,opencascade,qt6-base,nlohmann-json,pybind11,python}
cmake --workflow --preset windows            # configure, build and test; programs in build/windows/bin

# Ubuntu 24.04 (the first configure builds Open CASCADE 7.9.2, about 15 minutes, once):
sudo apt install cmake ninja-build g++ pkg-config qt6-base-dev libqt6opengl6-dev nlohmann-json3-dev \
  libfreetype-dev libfontconfig-dev libharfbuzz-dev libzstd-dev libgl-dev libglu1-mesa-dev libx11-dev \
  libxext-dev libxi-dev rapidjson-dev pybind11-dev python3-dev libeigen3-dev xvfb
cmake --workflow --preset linux              # programs in build/linux/bin
```

Other packages are one preset each: `cmake --workflow --preset windows-single` (one self-contained exe) and
`cmake --build --preset windows-portable` (a folder with every DLL, plus a zip). [docs/building.md](docs/building.md)
covers requirements, options, the Python wheel and the test benches.

## Usage

```sh
opad                         # the start page
opad model.step              # viewer mode: read-only and fast; Save to edit makes it an OPAD document
opad design.opad             # an OPAD document, editable
opad --read-only design.opad # look without changing it
opad --compare old.opad design.opad   # compare a version (a file or git:REV) with the file opened
opad-cli <command> [doc] [--key value ...]   # the same commands, headless, JSON out
```

The window has five workspaces that share one document and one timeline: **Review** (Ctrl+1: view, measure, mark up,
compare), **Design** (Ctrl+2: sketches, features, assemblies), **Drawings** (Ctrl+3: sheets), **Drafting** (Ctrl+4:
2D files) and **Simulate** (Ctrl+5).

Press **S** to search for any command and **?** for the shortcut sheet below, which always shows your own bindings
(change them in **Keyboard shortcuts**, Ctrl+K). Settings are in **Preferences** (Ctrl+,), which has a search.

![The shortcut cheat sheet: file, design, inspect, view, selection, panels and sketch keys](docs/media/keys.png)

## Contributing

Issues and pull requests are welcome. Before sending a change:

- Build and run the unit tests with your OS's preset (`cmake --workflow --preset windows` or `linux`). Desktop
  behaviour is covered by in-app benches: `ctest --preset windows-gui`, or one case with
  `python tools/gui_benches.py <opad> <opad-cli> --only <case>`; add a case when you fix something the desktop does.
- Nothing that grows with the model may run on the UI thread: use the job runner (`app/Jobs.hpp`).
- A feature area is one `AreaController` (`app/AreaController.hpp`) that adds its actions, ribbon slots and panels
  through hooks; new `app/*.cpp` and `tests/test_<name>.cpp` files are picked up by CMake without edits.
- Every new command needs a help record in `app/help/commands.json` and `commands.ar.json` (the help test fails
  without it), and visible strings go through `tr()` with their Arabic in `app/i18n` (`python tools/i18n_check.py`).
- Keep text files UTF-8 with LF line endings.

## FAQ

**Is it free? Does it need an account or a connection?**
The code is MIT-licensed, and OPAD runs entirely on your machine. It goes online only through git (your remotes,
including a background fetch every 10 minutes in a repository that has one, which you can turn off) and when you
accept its offer to download KiCad library models for a board.

**Can other CAD programs read my work?**
Yes: export to STEP, STL, OBJ or GLB, and drawings to PDF, DXF, DWG, SVG or PNG. The `.opad` format itself is plain
text: JSON for the history and OCCT ASCII BREP for the bodies.

**What is it not (yet)?**
There are no 3D annotations (PMI), no paper-space layouts as sheets and no installers yet. DXF and DWG open read-only
(edit them through Draw on drawing or Drawing to sketch), and macOS has not been built.

**How does it compare with other tools?**
FreeCAD is the established open-source modeller, with far more workbenches; OPAD is younger and narrower, built around
fast viewing and a git-friendly format. Fusion, SOLIDWORKS and Onshape are mature commercial modellers; Onshape has real
version control on its own servers, while OPAD does it locally with git. CadQuery and build123d are CAD as Python code;
OPAD exchanges BREP with them, but its document is a history of operations, not a program.

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
- The simulation engines are built from source with OPAD (cmake/sim_engines.cmake) and linked as shared libraries:
  Project Chrono (BSD-3-Clause; its collision detection is Bullet, Zlib) for dynamic studies and Netgen (LGPL 2.1) for
  the meshes of static and modal studies, which every package lists with their licence texts like the libraries above.
  Eigen (MPL 2.0) is compiled in from its headers, its MPL parts only. The single-file build leaves both engines out
  (`OPAD_CHRONO` and `OPAD_NETGEN` off in `windows-static`) until it can link them statically: Chrono's licence allows
  that with its notice; Netgen's LGPL would bring the same relinking offer as Qt and Open CASCADE above.
- CalculiX's `ccx` (GPL 2), which static and modal studies run, is a separate program found on the PATH, beside OPAD or
  through `OPAD_CCX`; OPAD never links or ships it.
- LibreDWG's `dwg2dxf` / `dxf2dwg` (GPL 3 or later) are separate programs that OPAD runs and never links; the portable
  package carries their licence, notice, source archive and build scripts in `licenses/LibreDWG/`.
- The ODA File Converter is third-party software that OPAD runs for DWG only when switched on (Preferences > Files);
  OPAD never bundles or downloads it.
- Drawings are lettered in Liberation Sans and Noto Sans Arabic (SIL Open Font License 1.1), compiled into the programs
  from `third_party/fonts`, where their licences are; the portable package carries them in `licenses/`.
- The models in the pictures on this page are the STEPcode AP214 AS1 test assembly (BSD), parts and mechanisms made in
  OPAD, and KiCad's pic_programmer demo board (from KiCad's source repository) with models from KiCad's library
  (CC-BY-SA 4.0 with KiCad's design exception).

Trademarks: Autodesk, AutoCAD, DWG, DWG TrueView, Fusion and ViewCube are trademarks of Autodesk, Inc.; SOLIDWORKS and
eDrawings of Dassault Systèmes; Onshape of PTC Inc.; Blender of the Blender Foundation; KiCad of the Linux Foundation;
Git of the Software Freedom Conservancy; Qt of The Qt Company; Open CASCADE of Open Cascade SAS; ODA and ODA File
Converter of the Open Design Alliance; Codex and ChatGPT of OpenAI. They are named only to describe compatibility;
OPAD is not affiliated with or endorsed by any of them.
