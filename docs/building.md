# Building OPAD

OPAD builds natively on each OS against that OS's own packages: install the dependencies, then one command
configures, builds and tests. Clone with `git clone --recurse-submodules` (or run `git submodule update --init` in an
existing clone): `third_party/libredwg` is built along the way into the DWG converters beside the app (once, about a
minute and a half; `-DOPAD_DWG=OFF` skips it).

| Host and target | Install | Build |
|---|---|---|
| Windows | [MSYS2](https://www.msys2.org), then in its shell: `pacman -S mingw-w64-x86_64-{cmake,ninja,gcc,opencascade,qt6-base,nlohmann-json,pybind11,python}` | `cmake --workflow --preset windows` |
| Linux (Ubuntu 24.04) | `sudo apt install cmake ninja-build g++ pkg-config qt6-base-dev libqt6opengl6-dev nlohmann-json3-dev libfreetype-dev libfontconfig-dev libharfbuzz-dev libzstd-dev libgl-dev libglu1-mesa-dev libx11-dev libxext-dev libxi-dev rapidjson-dev pybind11-dev python3-dev libeigen3-dev calculix-ccx`, plus `xvfb` for the GUI benches; no OCCT package (see [Linux](#linux)) | `cmake --workflow --preset linux` |
| macOS | `xcode-select --install`, then `brew install cmake ninja opencascade qt nlohmann-json pybind11 python` | `cmake --workflow --preset macos` |

The macOS preset is written but has not been run yet; Windows and Ubuntu 24.04 are the platforms that are built and
tested.

The three steps can also be run one by one: `cmake --preset <os>`, `cmake --build --preset <os>`,
`ctest --preset <os>`. `ctest --preset windows-gui` adds the desktop safety net: in-app benches that drive
the real app in hidden windows (drag handles, note cards, 2D mode, drawing placement, picking, zoom
refinement...). It needs a desktop session with OpenGL, so the default presets leave it out. Outputs land in
`build/<os>/bin`: `opad` (app), `opad-cli`, `opad.pyd`/`opad.so`, the sample plugin and the test binaries.
`tests/fixtures.cpp` generates the STEP fixtures used by the tests.

Requirements: CMake 3.25+, a C++20 compiler, Open CASCADE Technology 7.8+ (on Linux the build compiles 7.9.2 itself
when the system has none or an older one), nlohmann-json, Eigen 3 (Ubuntu's libeigen3-dev; without one the build fetches 3.4.1, as on MSYS2, which ships only Eigen 5), pybind11 (optional), Qt 6.4+ Widgets (optional, app only).

## Windows

Run the commands with `C:\msys64\mingw64\bin` first on PATH. The preset expects MSYS2 in `C:\msys64`; for another
location override `CMAKE_PREFIX_PATH`, `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` with `-D` or in a
`CMakeUserPresets.json`. The build stages GCC's runtime DLLs next to the executables, because Git for Windows puts an
incompatible `libstdc++-6.dll` first on the PATH of its shells.

ccache (as the compiler launcher) and LLD (as MinGW's linker) are used when they are installed (`OPAD_CCACHE`,
`OPAD_LLD`, both ON by default); an explicit `CMAKE_<LANG>_COMPILER_LAUNCHER`, `-fuse-ld=` or `CMAKE_LINKER_TYPE` wins.

### Single-file executable

`cmake --workflow --preset windows-single` writes `build/windows-static/single/OPAD-<version>-windows-x64.exe` (and
`opad-cli-<version>-windows-x64.exe`): one exe with Qt, OCCT and everything else linked in, nothing to unpack, no MSYS2
on the target machine. It keeps its settings and cache in an `opad-data` folder beside itself. The build needs
`pacman -S mingw-w64-x86_64-{qt6-static,rapidjson,pkgconf}` on top of the packages above; the first configure
downloads the OCCT source and builds its toolkits statically into `build/windows-static/occt` (once, about 15
minutes; the OS packages ship OCCT as DLLs only). The target fails if the exe imports anything but Windows' own DLLs.
`THIRD-PARTY-NOTICES.txt` goes beside the exes (they also carry it compiled in). The exes link Qt, OCCT and other LGPL
libraries statically: read the [licence notes](../README.md#licence) before handing them out. The Python module,
plugins and the simulation engines (`OPAD_CHRONO`, `OPAD_NETGEN`) are off in this build: joints and motion studies
work, dynamic, static and modal studies say their engine is not in this build.

### Portable folder

`cmake --build --preset windows-portable` (or `cmake --workflow --preset windows-portable` for configure, build,
test and package) writes `build/windows/portable/OPAD-<version>-windows-x64` and the same folder zipped. It holds
`opad.exe`, `opad-cli.exe` and every DLL and Qt plugin they load, so it runs on a machine with no MSYS2, Qt or OCCT.
The `opad.portable` file beside the exe makes the app keep settings and cache in the folder's `data` directory instead
of the registry and `%LOCALAPPDATA%`. `THIRD-PARTY-NOTICES.txt` lists every DLL's package, version, licence and source,
with the licence files in `licenses/`. With MSYS2's own OCCT the target stops, because that OCCT pulls in GPL FFmpeg
(with the x264/x265/xvid encoders) and FreeImage; see the [licence notes](../README.md#licence)
(`-DOPAD_ALLOW_GPL_DLLS=ON` stages them anyway, for local use only).

The exe icon is the logo's cube mark; `python tools/make_icon.py <opad_logo.png>` (Pillow, numpy) regenerates
`app/res` when the logo changes.

## Linux

OPAD needs OCCT 7.8 or newer and Ubuntu 24.04 ships 7.6, so the first `cmake --preset linux` (the first step of the
workflow) downloads the OCCT 7.9.2 source and builds the toolkits OPAD uses as shared libraries into
`build/linux/occt/install` (once, about 15 minutes; later configures reuse it, delete that folder to build it again).
It is the same kernel the Windows build runs. The programs and tests find it through their build RPATH, and
`linux-single` bundles it into the AppImage. `-DOPAD_OCCT_BUILD=AUTO` (default) builds it only when the system OCCT is
missing or older than 7.8, `ON` always builds it, `OFF` requires a system OCCT 7.8+ (point `OpenCASCADE_DIR` at its
`lib/cmake/opencascade`) and stops otherwise. Other distros need the same packages under their own names.

Ubuntu 24.04 ships Qt 6.4; the code keeps version guards for what newer Qt versions changed, so it builds against
6.4 as well as the Qt 6.10 of the Windows build.

Tests: `QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux -LE gui` runs the unit tests. The GUI benches run in a
virtual display: `xvfb-run -a ctest --test-dir build/linux -L gui`, or one case at a time with
`xvfb-run -a -s "-screen 0 1920x1080x24" python3 tools/gui_benches.py build/linux/bin/opad build/linux/bin/opad-cli --only <case>`.
On Wayland the app runs through XWayland, since OCCT's viewer needs an X11 window.

Known limits of the Linux build: the OCCT it builds has no FreeImage, so headless renders draw image canvases without
their picture (the desktop decodes pictures with Qt); `tests/corpus/*.step` is not in git (`python3
tests/corpus/fetch.py` downloads it), and tests that need it skip without it.

### Single file on Linux and macOS

`cmake --workflow --preset linux-single` builds `build/linux/single/OPAD-<version>-linux-x86_64.AppImage` with
[linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) and its Qt plugin (both on PATH);
`cmake --workflow --preset macos-single` builds `build/macos/single/OPAD-<version>-macos.dmg` with macdeployqt. Both
run the OS-package build and bundle its libraries; `cmake --build --preset <os>-single` runs just the packaging step.
`cmake/bundle_stage.cmake` then writes `THIRD-PARTY-NOTICES.txt` from the bundled libraries' Debian packages or
Homebrew formulae (into the bundle, where Help > Third-party licences reads it, and beside it) and stops on GPL
FFmpeg, codec, FreeImage or OpenVR libraries as the portable target does. These two targets are written but have not
been run yet.

## Options

`OPAD_BUILD_APP`, `OPAD_BUILD_CLI`, `OPAD_BUILD_PYTHON`, `OPAD_BUILD_PLUGINS`, `OPAD_BUILD_TESTS` (all ON). Pass them
on the configure step, e.g. `cmake --preset linux -DOPAD_BUILD_APP=OFF` for core and CLI only. `OPAD_STATIC` (OFF; the
`windows-static` preset turns it on) links everything statically, see above. `OPAD_OCCT_BUILD` (AUTO, ON or OFF)
chooses between the system OCCT and 7.9.2 built from source, see [Linux](#linux). `OPAD_DWG` (ON) builds the LibreDWG
converters.

**Adding a target** (another toolchain, architecture or package source): add a configure preset in
`CMakePresets.json` that inherits `base` and sets what differs (compiler, `CMAKE_PREFIX_PATH`, toolchain file), plus
matching build, test and workflow entries. It gets its own `build/<preset>` tree automatically.

## Simulation engines

Dynamic studies use Project Chrono 9.0.1 and structural studies Netgen 6.2 meshes (`cmake/sim_engines.cmake`). Both are
cloned at a pinned commit and built into the build tree the first time CMake configures (about 10 minutes together),
against the same OCCT and Eigen; `OPAD_CHRONO_SOURCE_DIR` / `OPAD_NETGEN_SOURCE_DIR` point at a checkout instead for an
offline build, and `-DOPAD_CHRONO=OFF` / `-DOPAD_NETGEN=OFF` leave one out (its studies then say it is not in this
build; the `windows-static` preset leaves both out). Eigen 3.3+ is required for the kinematic solver. The Windows builds
(MSYS2's MinGW GCC) of both engines have not been tried yet: Chrono and Netgen are mostly built with MSVC upstream, so
the first Windows configure may need fixes, or `-DOPAD_CHRONO=OFF -DOPAD_NETGEN=OFF` meanwhile. CalculiX's `ccx` runs as
a separate program: from `OPAD_CCX`, beside the OPAD executables, or on the PATH (`calculix-ccx` on Ubuntu);
`OPAD_CCX_THREADS` sets its solver threads (default 1: the threaded SPOOLES solve was not repeatable). `mechanism`
reports which engines a build has.

## Python module

`pip install ./python` builds the `opad` module (scikit-build-core; needs OCCT on the build machine). The wheel
carries THIRD-PARTY-NOTICES.txt for the libraries the module links; before handing it to other machines, repair it
(delvewheel, auditwheel or delocate) so OCCT's shared libraries travel inside it, then check it with
`cmake -DWHEEL=<the .whl> -P cmake/wheel_guard.cmake`. Repairing against an OCCT built with FFmpeg, FreeImage or OpenVR
(MSYS2's, and often a distribution's) copies those GPL libraries into the wheel; the check refuses it, and such a wheel
must not be distributed (build OCCT without them, as for the portable package).

## The README's pictures

`docs/media` is made from real runs of the desktop app by `tools/make_readme_media.py`: it records the Linux build
under Xvfb (with a small built-in compositor and XTest input, so no window manager, xdotool or screen grabber is
needed) and assembles the GIFs with Pillow:

```sh
python3 tests/corpus/fetch.py                                   # the sample models
python3 tools/make_readme_media.py record --bin build/linux/bin # Linux: Xvfb, libX11, libXtst
python3 tools/make_readme_media.py assemble                     # anywhere with Pillow
```

`record --only <scene>` redoes one picture; `drawings` and `arabic` use the part the `design` scene saves, so record
`design` first. `kicad` downloads KiCad's pic_programmer demo board and its library models (network), `motion`, `gears`
and `thermal` build their mechanisms with `tools/sim_eval.py`, and `thermal` needs CalculiX (`OPAD_CCX` or `ccx` on
PATH).
