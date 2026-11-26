#!/usr/bin/env bash
# OPAD's OpenFOAM pack: OpenCFD's OpenFOAM, built from its pinned source tarball without MPI (Pstream's dummy library: no
# MS-MPI or Open MPI, no administrator install). Only the programs OPAD runs go in the pack, with the libraries they load.
#
#   engines/openfoam/make-pack.sh <work dir> <out dir> [windows-x64 | linux-x64 | macos-arm64]
#
# windows-x64 (the default on Linux): cross-compiled on Linux with mingw-w64, the way OpenCFD builds its own Windows
#   binaries. Needs (Ubuntu 24.04): mingw-w64 flex libfl-dev (FlexLexer.h: only a Recommends of flex) bison m4 make gcc
#   g++ curl cmake. Programs and DLLs together in bin/.
# linux-x64: native GCC. Needs (Ubuntu 22.04 or later): g++ flex libfl-dev bison m4 make curl cmake patchelf. bin/ and
#   lib/ (+ lib/dummy/libPstream.so) as OpenFOAM lays them out, run paths $ORIGIN-relative: the pack runs from any folder
#   with nothing set but WM_PROJECT_DIR (OPAD sets it). Only the C and C++ runtimes of the system are needed.
# macos-arm64 (the default on macOS): native Clang. Needs Homebrew's bash (OpenFOAM's scripts want bash 4+: run this with
#   it), make, flex, bison, and a case-sensitive work dir (OpenFOAM's sources need one; engines.yml mounts an APFS image).
#   bin/ and lib/, wmake's own @executable_path/@loader_path run paths; ad-hoc signed.
# Writes into <out dir>:
#   opad-openfoam-<ver>-<rev>-<platform>.zip   the pack (bin/, lib/, etc/, licenses/, NOTICE.txt, opad-pack.json)
#   OpenFOAM-<ver>.tgz, zlib-<ver>.tar.gz       the exact sources it was built from
#   opad-openfoam-recipe.tar.gz                 this folder (the recipe and our patches)
#   SHA256SUMS
# engines.yml publishes these on a release of their own; the pack's SHA-256 and size are then pinned in
# core/src/sim/engine_packs.cpp.
set -euo pipefail

OF_VERSION=v2606
OF_URL=https://dl.openfoam.com/source/${OF_VERSION}/OpenFOAM-${OF_VERSION}.tgz
OF_SHA256=2a1310e3ed192cc4c521e1d22dcc176f57bec61160c878dc4348f21d6672294d
ZLIB_VERSION=1.4
ZLIB_URL=https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz
ZLIB_SHA256=02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584
PACK_REV=1

target=${3:-}
if [ -z "$target" ]; then
  case "$(uname -s)" in Darwin) target=macos-arm64 ;; *) target=windows-x64 ;; esac
fi
case "$target" in
  windows-x64) platform_name="Windows x64" ;;
  linux-x64) platform_name="Linux x64" ;;
  macos-arm64) platform_name="macOS on Apple silicon" ;;
  *) echo "error: unknown platform $target (windows-x64, linux-x64, macos-arm64)" >&2; exit 1 ;;
esac
PACK=opad-openfoam-${OF_VERSION}-${PACK_REV}-${target}

# What OPAD runs (core/src/sim/flow.cpp, cfd.cpp): source folders under applications/.
PROGRAMS=(
  utilities/mesh/generation/blockMesh
  utilities/mesh/generation/snappyHexMesh
  solvers/incompressible/simpleFoam
  utilities/mesh/manipulation/splitMeshRegions
  utilities/mesh/manipulation/topoSet
  utilities/mesh/manipulation/createBaffles
  utilities/mesh/manipulation/createPatch
  solvers/basic/scalarTransportFoam
  solvers/heatTransfer/buoyantBoussinesqSimpleFoam
  solvers/heatTransfer/chtMultiRegionFoam/chtMultiRegionSimpleFoam
)
# Only the libraries they link, and those link against, in the order src/Allwmake builds them (all of src/ is about twice
# as much to compile: an hour and a half on 16 cores). The list is the closure of the programs' Make/options -l names
# through each library's own Make/options; most are only named on link lines (the linker wants them, no program imports
# them, so they stay out of the pack). libOpenFOAM and libPstream come first (see the build below).
LIBRARIES=(
  fileFormats surfMesh meshTools finiteArea finiteVolume mesh/blockMesh mesh/extrudeModel dynamicMesh genericPatchFields
  parallel/decompose/decompositionMethods
  dummyThirdParty/scotchDecomp dummyThirdParty/ptscotchDecomp dummyThirdParty/metisDecomp dummyThirdParty/kahipDecomp
  lagrangian/basic lagrangian/distributionModels parallel/decompose/decompose parallel/reconstruct/reconstruct
  parallel/distributed dynamicFvMesh sampling
  transportModels/twoPhaseMixture transportModels/interfaceProperties transportModels/twoPhaseProperties
  transportModels/incompressible transportModels/compressible transportModels/geometricVoF ODE
  thermophysicalModels/specie thermophysicalModels/solidSpecie thermophysicalModels/thermophysicalProperties
  thermophysicalModels/basic thermophysicalModels/reactionThermo thermophysicalModels/chemistryModel
  thermophysicalModels/SLGThermo thermophysicalModels/solidThermo thermophysicalModels/radiation
  TurbulenceModels/turbulenceModels TurbulenceModels/incompressible TurbulenceModels/compressible combustionModels
  overset fvOptions fvMotionSolver mesh/snappyHexMesh phaseSystemModels/reactingEuler/saturationModels
  phaseSystemModels/multiphaseInter/phasesSystem functionObjects/forces thermoTools regionModels/regionModel faOptions
  regionFaModels atmosphericModels
)

recipe=$(cd "$(dirname "$0")" && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
out=$(mkdir -p "$2" && cd "$2" && pwd)
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
# Mirrors (SourceForge behind dl.openfoam.com) now and then answer with a web page instead of the file, quickly and with
# 200, so a download whose SHA-256 differs is tried again a few times before the recipe gives up.
fetch() {  # url sha256 file
  local try
  for try in 1 2 3 4; do
    if [ ! -f "$work/$3" ]; then curl -sSfL --retry 3 -o "$work/$3.part" "$1" && mv "$work/$3.part" "$work/$3" || true; fi
    if [ -f "$work/$3" ] && [ "$(sha256 "$work/$3" | cut -d' ' -f1)" = "$2" ]; then return 0; fi
    if [ -f "$work/$3" ]; then
      echo "warning: $3 (try $try) does not have the pinned SHA-256: $(wc -c <"$work/$3") bytes, $(head -c 60 "$work/$3" | tr -c '[:print:]' '.')" >&2
      rm -f "$work/$3"
    fi
    [ "$try" = 4 ] || sleep $((try * 20))
  done
  echo "error: $3 does not have the pinned SHA-256" >&2
  exit 1
}
fetch "$OF_URL" "$OF_SHA256" "OpenFOAM-${OF_VERSION}.tgz"
fetch "$ZLIB_URL" "$ZLIB_SHA256" "zlib-${ZLIB_VERSION}.tar.gz"

if [ "$target" = macos-arm64 ]; then
  [ "$(uname -s)" = Darwin ] || { echo "error: the macOS pack is built on macOS" >&2; exit 1; }
  [ "${BASH_VERSINFO[0]}" -ge 4 ] || { echo "error: run this with bash 4 or later (Homebrew's), not macOS's bash $BASH_VERSION" >&2; exit 1; }
  rm -f "$work/.case-check" "$work/.CASE-CHECK"
  touch "$work/.case-check"
  if [ -e "$work/.CASE-CHECK" ]; then
    rm -f "$work/.case-check"
    echo "error: $work is on a case-insensitive file system; OpenFOAM's sources need a case-sensitive one" >&2
    echo "  hdiutil create -size 30g -fs 'Case-sensitive APFS' -volname ofbuild -type SPARSEBUNDLE ofbuild && hdiutil attach ofbuild.sparsebundle" >&2
    exit 1
  fi
  rm -f "$work/.case-check"
  # GNU make, flex and bison from Homebrew before Xcode's older ones (both are keg-only).
  for f in make/libexec/gnubin flex/bin bison/bin; do
    d="$(brew --prefix "${f%%/*}")/${f#*/}"
    [ -d "$d" ] && export PATH="$d:$PATH"
  done
  export CPATH="$(brew --prefix flex)/include${CPATH:+:$CPATH}"
elif [ "$target" = linux-x64 ]; then
  [ "$(uname -s)" = Linux ] || { echo "error: the Linux pack is built on Linux" >&2; exit 1; }
  command -v patchelf >/dev/null || { echo "error: patchelf is needed (the pack's run paths)" >&2; exit 1; }
fi

if [ "$target" = windows-x64 ]; then
  # The cross compilers with POSIX threads (std::thread, std::mutex): Ubuntu's default x86_64-w64-mingw32-g++ is the win32 one.
  tools="$work/tools"
  mkdir -p "$tools"
  for t in gcc g++; do ln -sf "$(command -v x86_64-w64-mingw32-$t-posix)" "$tools/x86_64-w64-mingw32-$t"; done
  export PATH="$tools:$PATH"
  gcclib=$(dirname "$(x86_64-w64-mingw32-gcc -print-libgcc-file-name)")
fi

# zlib, static: libOpenFOAM links -lz and the pack then needs no zlib of the system.
zlib="$work/zlib-${ZLIB_VERSION}-$target"
if [ ! -f "$zlib/lib/libz.a" ]; then
  rm -rf "$work/zlib-${ZLIB_VERSION}"
  tar -xzf "$work/zlib-${ZLIB_VERSION}.tar.gz" -C "$work"
  if [ "$target" = windows-x64 ]; then
    make -C "$work/zlib-${ZLIB_VERSION}" -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a -j"$jobs" >/dev/null
    mkdir -p "$zlib/include" "$zlib/lib"
    cp "$work/zlib-${ZLIB_VERSION}"/{zlib.h,zconf.h} "$zlib/include/"
    cp "$work/zlib-${ZLIB_VERSION}/libz.a" "$zlib/lib/"
  else
    # -fPIC: it goes into a shared library.
    (cd "$work/zlib-${ZLIB_VERSION}" && CFLAGS="-O2 -fPIC" ./configure --static --prefix="$zlib" >/dev/null && make -j"$jobs" install >/dev/null)
  fi
fi

# OpenFOAM: unpacked once, our patches applied in order.
src="$work/OpenFOAM-${OF_VERSION}"
if [ ! -f "$src/.opad-unpacked" ]; then
  rm -rf "$src"
  tar -xzf "$work/OpenFOAM-${OF_VERSION}.tgz" -C "$work"
  for p in "$recipe"/patches/*.patch; do [ -f "$p" ] && patch -d "$src" -p1 < "$p"; done
  if [ "$target" = macos-arm64 ]; then
    # Apple Clang fuses multiply-adds by default, which changes results against every other build (OpenFOAM issue 2958; the
    # macOS OpenFOAM app does the same).
    perl -pi -e 's/-ftrapping-math(?! -ffp-contract)/-ftrapping-math -ffp-contract=off/' "$src/wmake/rules/darwin64Clang/c" "$src/wmake/rules/darwin64Clang/c++"
  fi
  touch "$src/.opad-unpacked"
fi
# One unpacked tree builds one platform (platforms/<options> differ, but wmake's own tools in wmake/platforms do not).
if [ -f "$src/.opad-target" ] && [ "$(cat "$src/.opad-target")" != "$target" ]; then
  echo "error: $src was built for $(cat "$src/.opad-target"); use another work dir for $target" >&2
  exit 1
fi
echo "$target" >"$src/.opad-target"

# The build. bashrc reads unset variables, so -u is off while it runs.
# ccache in front of every compile when it is there (wmake's own "+ccache"): CI keeps its cache between runs. No OpenMP in
# the native builds (~openmp): the programs OPAD runs do not use it, and libgomp / libomp would be one more library to carry.
control=
command -v ccache >/dev/null && control=+ccache
case "$target" in
  windows-x64) foam_env=(WM_COMPILER=Mingw WM_MPLIB=dummy "WM_COMPILE_CONTROL=$control"); want=Mingw; ext=dll ;;
  linux-x64) foam_env=(WM_COMPILER=Gcc WM_MPLIB=dummy "WM_COMPILE_CONTROL=${control}~openmp"); want=Gcc; ext=so ;;
  macos-arm64) foam_env=(WM_COMPILER=Clang WM_MPLIB=dummy "WM_COMPILE_CONTROL=${control}~openmp"); want=Clang; ext=dylib ;;
esac
set +u
# shellcheck disable=SC1091
source "$src/etc/bashrc" "${foam_env[@]}" || true
set -u
[ "$WM_COMPILER" = "$want" ] || { echo "error: OpenFOAM's environment did not take WM_COMPILER=$want ($WM_COMPILER)" >&2; exit 1; }
# bashrc exports its arguments word by word: "+ccache ~openmp" with a space lost ~openmp. wmake only looks for substrings.
if [ "$target" != windows-x64 ] && [[ "${WM_COMPILE_CONTROL:-}" != *"~openmp"* ]]; then
  echo "error: OpenFOAM's environment did not take WM_COMPILE_CONTROL=${control}~openmp (${WM_COMPILE_CONTROL:-})" >&2
  exit 1
fi
if [ "$target" = windows-x64 ]; then
  # A cross gcc ignores LIBRARY_PATH: zlib's folder goes in through the flags wmake adds to every compile and link line.
  # -fno-jump-tables: GCC 13 for MinGW puts the switch tables of inline (COMDAT) functions in plain .rdata, and once the
  # linker keeps another copy of such a function they point nowhere ("relocation truncated to fit" linking libOpenFOAM).
  export CPATH="$zlib/include" FOAM_EXTRA_CXXFLAGS="-fno-jump-tables -L$zlib/lib"
else
  export CPATH="$zlib/include${CPATH:+:$CPATH}" LIBRARY_PATH="$zlib/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
export WM_NCOMPPROCS="$jobs"
log="$work/build.log"
echo "building OpenFOAM ${OF_VERSION} for $WM_OPTIONS in $src (log: $log)"
# set -e is off in a subshell whose status is tested (the || after it): each step that can fail ends it itself.
(
  fail() { echo "error: $*" >&2; exit 1; }
  cd "$src" || fail "no $src"
  wmake/src/Allmake || fail "wmake's own tools"
  cd src || fail "no src/"
  # Every library's headers, as src/Allwmake makes them first. Serially: with -j its jobs start inside a pipeline's
  # subshell, so its final wait waits for none of them and the compiles began against half-linked lnInclude folders
  # (v2606: "scalarFwd.H: No such file" in OSspecific). -u: a folder an interrupted run left half-made is completed.
  wmakeLnIncludeAll -u || fail "the libraries' headers"
  wmakeLnInclude -u OpenFOAM && wmakeLnInclude -u "OSspecific/${WM_OSTYPE:-POSIX}" || fail "OpenFOAM's headers"
  OpenFOAM/Alltouch -check 2>/dev/null || true
  if [ "$target" = windows-x64 ]; then
    OSspecific/MSwindows/Allwmake || fail "OSspecific"
    # MinGW: libOpenFOAM and libPstream need each other. Pstream's dummy goes into libOpenFOAM as an object first, then is
    # built as its own DLL against it, then libOpenFOAM is linked again against that DLL (src/Allwmake's Mingw case).
    WM_MPLIB=dummy Pstream/Allwmake libo || fail "Pstream (object)"
    FOAM_LINK_DUMMY_PSTREAM=libo wmake -j "$jobs" OpenFOAM || fail "OpenFOAM (first link)"
    Pstream/Allwmake || fail "Pstream"
    OpenFOAM/Alltouch 2>/dev/null || true
  else
    OSspecific/POSIX/Allwmake || fail "OSspecific"
    Pstream/Allwmake || fail "Pstream"
  fi
  wmake -j "$jobs" OpenFOAM || fail "OpenFOAM"
  for d in "${LIBRARIES[@]}"; do wmake -j "$jobs" "$d" || fail "src/$d"; done
  cd ..
  [ -f "platforms/$WM_OPTIONS/lib/libOpenFOAM.$ext" ] || fail "no libOpenFOAM.$ext was built"
  for p in "${PROGRAMS[@]}"; do wmake -j "$jobs" "applications/$p" || fail "applications/$p"; done
) >"$log" 2>&1 || { tail -60 "$log" >&2; exit 1; }

# The pack: the programs, every library they load that is not the system's own (Pstream's dummy among them), etc/, licenses.
plat="$src/platforms/$WM_OPTIONS"
stage="$work/$PACK"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/licenses"
declare -A seen=()
queue=()
for p in "${PROGRAMS[@]}"; do
  exe="$plat/bin/$(basename "$p")"
  [ "$target" = windows-x64 ] && exe="$exe.exe"
  [ -f "$exe" ] || { echo "error: $exe was not built (see $log)" >&2; exit 1; }
  cp "$exe" "$stage/bin/"
  queue+=("$exe")
done
# needs <file>: the libraries it names (DLL imports, ELF NEEDED, Mach-O load commands).
needs() {
  case "$target" in
    windows-x64) x86_64-w64-mingw32-objdump -p "$1" | sed -n 's/^\s*DLL Name: //p' | tr -d '\r' ;;
    linux-x64) readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' ;;
    macos-arm64) otool -L "$1" | tail -n +2 | awk '{print $1}' ;;
  esac
}
case "$target" in
  windows-x64) search=("$plat/lib/dummy" "$plat/lib" "$gcclib" "/usr/x86_64-w64-mingw32/lib") ;;
  *) search=("$plat/lib" "$plat/lib/dummy") ;;
esac
while [ ${#queue[@]} -gt 0 ]; do
  f=${queue[0]}
  queue=("${queue[@]:1}")
  for lib in $(needs "$f"); do
    name=${lib##*/}
    key=${name,,}
    [ -n "${seen[$key]:-}" ] && continue
    seen[$key]=1
    found=
    for d in "${search[@]}"; do
      if [ -f "$d/$name" ]; then found="$d/$name"; break; fi
    done
    if [ -z "$found" ]; then
      case "$target" in
        windows-x64) continue ;;  # a Windows DLL (KERNEL32, msvcrt, ...)
        linux-x64)  # the C and C++ runtimes every Linux system has; anything else would be missing on some
          [[ "$name" =~ ^(libc|libm|libstdc\+\+|libgcc_s|libpthread|libdl|librt|ld-linux-x86-64)\.so ]] && continue ;;
        macos-arm64)
          [[ "$lib" == /usr/lib/* || "$lib" == /System/* ]] && continue ;;
      esac
      echo "error: $f needs $lib, which is neither built here nor the system's own" >&2
      exit 1
    fi
    if [ "$target" = windows-x64 ]; then
      cp "$found" "$stage/bin/"
    elif [ "$(dirname "$found")" = "$plat/lib/dummy" ]; then
      mkdir -p "$stage/lib/dummy" && cp "$found" "$stage/lib/dummy/"
    else
      mkdir -p "$stage/lib" && cp "$found" "$stage/lib/"
    fi
    queue+=("$found")
  done
done
shopt -s nullglob  # lib/dummy/ may hold nothing on some system
case "$target" in
  windows-x64) x86_64-w64-mingw32-strip --strip-unneeded "$stage/bin/"*.dll 2>/dev/null || true ;;
  linux-x64)
    # Where each finds the others, relative to itself: OpenFOAM's Linux build relies on LD_LIBRARY_PATH from its bashrc.
    strip --strip-unneeded "$stage"/bin/* "$stage"/lib/*.so "$stage"/lib/dummy/*.so
    for f in "$stage"/bin/*; do patchelf --set-rpath '$ORIGIN/../lib:$ORIGIN/../lib/dummy' "$f"; done
    for f in "$stage"/lib/*.so; do patchelf --set-rpath '$ORIGIN:$ORIGIN/dummy' "$f"; done
    for f in "$stage"/lib/dummy/*.so; do patchelf --set-rpath '$ORIGIN/..:$ORIGIN' "$f"; done
    ;;
  macos-arm64)
    # wmake already links with @executable_path/../lib and @loader_path run paths (install names @rpath/<lib>); run paths
    # that name a folder of this machine (ThirdParty's) are dropped. Stripping breaks the linker's signature: signed again.
    for f in "$stage"/bin/* "$stage"/lib/*.dylib "$stage"/lib/dummy/*.dylib; do
      for r in $(otool -l "$f" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }'); do
        [[ "$r" == @* ]] || install_name_tool -delete_rpath "$r" "$f"
      done
      strip -x "$f"
      codesign --force --sign - "$f"
    done
    ;;
esac
shopt -u nullglob
cp -RL "$src/etc" "$stage/etc"
for l in COPYING LICENSE.md; do  # the GPL text: COPYING up to v2512, LICENSE.md from v2606
  if [ -f "$src/$l" ]; then cp "$src/$l" "$stage/licenses/OpenFOAM-COPYING.txt"; break; fi
done
[ -f "$stage/licenses/OpenFOAM-COPYING.txt" ] || { echo "error: OpenFOAM's licence text is not where it was (COPYING, LICENSE.md)" >&2; exit 1; }
cp "$work/zlib-${ZLIB_VERSION}/LICENSE" "$stage/licenses/zlib-LICENSE.txt"
parts="- zlib ${ZLIB_VERSION} (zlib license, licenses/zlib-LICENSE.txt), linked into libOpenFOAM."
case "$target" in
  windows-x64)
    cp /usr/share/doc/gcc-mingw-w64-base/copyright "$stage/licenses/gcc-runtime-copyright.txt" 2>/dev/null || true
    cp /usr/share/doc/mingw-w64-common/copyright "$stage/licenses/mingw-w64-copyright.txt" 2>/dev/null || true
    parts+=$'\n'"- The GCC runtime (libstdc++, libgcc: GPL 3 with the GCC Runtime Library Exception) and mingw-w64's winpthreads"
    parts+=$'\n'"  (MIT-style), licenses/gcc-runtime-copyright.txt and licenses/mingw-w64-copyright.txt."
    ;;
  linux-x64) parts+=$'\n'"- Nothing else: the programs use the system's C and C++ runtimes (glibc, libstdc++), which are not in this folder." ;;
  macos-arm64) parts+=$'\n'"- Nothing else: the programs use macOS's own C and C++ runtimes (libSystem, libc++)." ;;
esac
if find "$stage" -type l | grep -q .; then
  echo "error: the pack holds symbolic links (a zip keeps them as text):" >&2
  find "$stage" -type l >&2
  exit 1
fi
notice=$(<"$recipe/NOTICE.txt")
notice=${notice//@OF_VERSION@/$OF_VERSION}
notice=${notice//@ZLIB_VERSION@/$ZLIB_VERSION}
notice=${notice//@PACK_REV@/$PACK_REV}
notice=${notice//@PLATFORM@/$platform_name}
notice=${notice//@PARTS@/$parts}
printf '%s\n' "$notice" >"$stage/NOTICE.txt"
programs_json=$(printf '"%s",' "${PROGRAMS[@]##*/}")
cat >"$stage/opad-pack.json" <<EOF
{"id": "openfoam", "version": "${OF_VERSION}-${PACK_REV}", "platform": "${target}", "openfoam": "${OF_VERSION}",
 "mpi": "dummy", "programs": [${programs_json%,}],
 "sources": ["OpenFOAM-${OF_VERSION}.tgz", "zlib-${ZLIB_VERSION}.tar.gz", "opad-openfoam-recipe.tar.gz"]}
EOF

rm -f "$out/$PACK.zip"
(cd "$stage" && cmake -E tar cf "$out/$PACK.zip" --format=zip -- *)
cp "$work/OpenFOAM-${OF_VERSION}.tgz" "$work/zlib-${ZLIB_VERSION}.tar.gz" "$out/"
tar -czf "$out/opad-openfoam-recipe.tar.gz" -C "$(dirname "$recipe")" "$(basename "$recipe")"
(cd "$out" && sha256 "$PACK.zip" "OpenFOAM-${OF_VERSION}.tgz" "zlib-${ZLIB_VERSION}.tar.gz" opad-openfoam-recipe.tar.gz >SHA256SUMS)
echo "pack: $out/$PACK.zip ($(wc -c <"$out/$PACK.zip" | tr -d ' ') bytes, $(ls "$stage/bin" | wc -l | tr -d ' ') files in bin)"
cat "$out/SHA256SUMS"
