#!/usr/bin/env bash
# OPAD's OpenFOAM pack for Windows x64: OpenCFD's OpenFOAM, built from its pinned source tarball and cross-compiled on Linux
# with mingw-w64 (the way OpenCFD builds its own Windows binaries), without MPI (Pstream's dummy library: no MS-MPI, no
# administrator install). Only the programs OPAD runs go in the pack, with the DLLs they import.
#
#   engines/openfoam/make-pack.sh <work dir> <out dir>
#
# Needs (Ubuntu 24.04): mingw-w64 flex libfl-dev bison m4 make gcc g++ curl cmake. Writes into <out dir>:
#   opad-openfoam-<ver>-<rev>-windows-x64.zip   the pack (bin/, etc/, licenses/, NOTICE.txt, opad-pack.json)
#   OpenFOAM-<ver>.tgz, zlib-<ver>.tar.gz        the exact sources it was built from
#   opad-openfoam-recipe.tar.gz                  this folder (the recipe and our patches)
#   SHA256SUMS
# The pack's SHA-256 and size are then pinned in core/src/sim/engine_packs.cpp.
set -euo pipefail

OF_VERSION=v2512
OF_URL=https://dl.openfoam.com/source/${OF_VERSION}/OpenFOAM-${OF_VERSION}.tgz
OF_SHA256=ae9a0a133a2e996b88bd1d0f3cc229e3c49968c368feae61bd3ec63deaf337aa
ZLIB_VERSION=1.3.1
ZLIB_URL=https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
PACK_REV=1
PACK=opad-openfoam-${OF_VERSION}-${PACK_REV}-windows-x64

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
jobs=${JOBS:-$(nproc)}

fetch() {  # url sha256 file
  if [ ! -f "$work/$3" ]; then curl -sSfL --retry 3 -o "$work/$3.part" "$1" && mv "$work/$3.part" "$work/$3"; fi
  echo "$2  $work/$3" | sha256sum -c --quiet - || { echo "error: $3 does not have the pinned SHA-256" >&2; exit 1; }
}
fetch "$OF_URL" "$OF_SHA256" "OpenFOAM-${OF_VERSION}.tgz"
fetch "$ZLIB_URL" "$ZLIB_SHA256" "zlib-${ZLIB_VERSION}.tar.gz"

# The cross compilers with POSIX threads (std::thread, std::mutex): Ubuntu's default x86_64-w64-mingw32-g++ is the win32 one.
tools="$work/tools"
mkdir -p "$tools"
for t in gcc g++; do ln -sf "$(command -v x86_64-w64-mingw32-$t-posix)" "$tools/x86_64-w64-mingw32-$t"; done
export PATH="$tools:$PATH"
gcclib=$(dirname "$(x86_64-w64-mingw32-gcc -print-libgcc-file-name)")

# zlib, static: libOpenFOAM links -lz and the pack then needs no zlib DLL.
zlib="$work/zlib"
if [ ! -f "$zlib/lib/libz.a" ]; then
  rm -rf "$work/zlib-${ZLIB_VERSION}"
  tar -xzf "$work/zlib-${ZLIB_VERSION}.tar.gz" -C "$work"
  make -C "$work/zlib-${ZLIB_VERSION}" -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a -j"$jobs" >/dev/null
  mkdir -p "$zlib/include" "$zlib/lib"
  cp "$work/zlib-${ZLIB_VERSION}"/{zlib.h,zconf.h} "$zlib/include/"
  cp "$work/zlib-${ZLIB_VERSION}/libz.a" "$zlib/lib/"
fi

# OpenFOAM: unpacked once, our patches applied in order.
src="$work/OpenFOAM-${OF_VERSION}"
if [ ! -f "$src/.opad-unpacked" ]; then
  rm -rf "$src"
  tar -xzf "$work/OpenFOAM-${OF_VERSION}.tgz" -C "$work"
  for p in "$recipe"/patches/*.patch; do [ -f "$p" ] && patch -d "$src" -p1 < "$p"; done
  touch "$src/.opad-unpacked"
fi

# The build. bashrc reads unset variables, so -u is off while it runs.
# ccache in front of every compile when it is there (wmake's own "+ccache"): CI keeps its cache between runs.
control=
command -v ccache >/dev/null && control=+ccache
set +u
# shellcheck disable=SC1091
source "$src/etc/bashrc" WM_COMPILER=Mingw WM_MPLIB=dummy WM_COMPILE_CONTROL="$control" || true
set -u
[ "$WM_COMPILER" = Mingw ] || { echo "error: OpenFOAM's environment did not take WM_COMPILER=Mingw" >&2; exit 1; }
# A cross gcc ignores LIBRARY_PATH: zlib's folder goes in through the flags wmake adds to every compile and link line.
# -fno-jump-tables: GCC 13 for MinGW puts the switch tables of inline (COMDAT) functions in plain .rdata, and once the
# linker keeps another copy of such a function they point nowhere ("relocation truncated to fit" linking libOpenFOAM).
export CPATH="$zlib/include" FOAM_EXTRA_CXXFLAGS="-fno-jump-tables -L$zlib/lib" WM_NCOMPPROCS="$jobs"
log="$work/build.log"
echo "building OpenFOAM ${OF_VERSION} for $WM_OPTIONS in $src (log: $log)"
# set -e is off in a subshell whose status is tested (the || after it): each step that can fail ends it itself.
(
  fail() { echo "error: $*" >&2; exit 1; }
  cd "$src" || fail "no $src"
  wmake/src/Allmake || fail "wmake's own tools"
  cd src || fail "no src/"
  wmakeLnIncludeAll -j "$jobs" || fail "the libraries' headers"  # every library's headers, as src/Allwmake makes them first
  OpenFOAM/Alltouch -check 2>/dev/null || true
  OSspecific/MSwindows/Allwmake || fail "OSspecific"
  # MinGW: libOpenFOAM and libPstream need each other. Pstream's dummy goes into libOpenFOAM as an object first, then is
  # built as its own DLL against it, then libOpenFOAM is linked again against that DLL (src/Allwmake's Mingw case).
  WM_MPLIB=dummy Pstream/Allwmake libo || fail "Pstream (object)"
  FOAM_LINK_DUMMY_PSTREAM=libo wmake -j "$jobs" OpenFOAM || fail "OpenFOAM (first link)"
  Pstream/Allwmake || fail "Pstream"
  OpenFOAM/Alltouch 2>/dev/null || true
  wmake -j "$jobs" OpenFOAM || fail "OpenFOAM"
  for d in "${LIBRARIES[@]}"; do wmake -j "$jobs" "$d" || fail "src/$d"; done
  cd ..
  [ -f "platforms/$WM_OPTIONS/lib/libOpenFOAM.dll" ] || fail "no libOpenFOAM.dll was built"
  for p in "${PROGRAMS[@]}"; do wmake -j "$jobs" "applications/$p" || fail "applications/$p"; done
) >"$log" 2>&1 || { tail -60 "$log" >&2; exit 1; }

# The pack: the programs, every DLL they import that is not Windows' own (Pstream's dummy among them), etc/, licenses.
plat="$src/platforms/$WM_OPTIONS"
stage="$work/$PACK"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/licenses"
search=("$plat/lib/dummy" "$plat/lib" "$gcclib" "/usr/x86_64-w64-mingw32/lib")
declare -A seen=()
queue=()
for p in "${PROGRAMS[@]}"; do
  exe="$plat/bin/$(basename "$p").exe"
  [ -f "$exe" ] || { echo "error: $exe was not built (see $log)" >&2; exit 1; }
  cp "$exe" "$stage/bin/"
  queue+=("$exe")
done
while [ ${#queue[@]} -gt 0 ]; do
  f=${queue[0]}
  queue=("${queue[@]:1}")
  for dll in $(x86_64-w64-mingw32-objdump -p "$f" | sed -n 's/^\s*DLL Name: //p' | tr -d '\r'); do
    key=${dll,,}
    [ -n "${seen[$key]:-}" ] && continue
    seen[$key]=1
    found=
    for d in "${search[@]}"; do
      if [ -f "$d/$dll" ]; then found="$d/$dll"; break; fi
    done
    [ -n "$found" ] || continue  # a Windows DLL (KERNEL32, msvcrt, ...)
    cp "$found" "$stage/bin/"
    queue+=("$found")
  done
done
x86_64-w64-mingw32-strip --strip-unneeded "$stage/bin/"*.dll 2>/dev/null || true
cp -r "$src/etc" "$stage/etc"
cp "$src/COPYING" "$stage/licenses/OpenFOAM-COPYING.txt"
cp "$work/zlib-${ZLIB_VERSION}/LICENSE" "$stage/licenses/zlib-LICENSE.txt"
cp /usr/share/doc/gcc-mingw-w64-base/copyright "$stage/licenses/gcc-runtime-copyright.txt" 2>/dev/null || true
cp /usr/share/doc/mingw-w64-common/copyright "$stage/licenses/mingw-w64-copyright.txt" 2>/dev/null || true
sed -e "s/@OF_VERSION@/${OF_VERSION}/g" -e "s/@ZLIB_VERSION@/${ZLIB_VERSION}/g" -e "s/@PACK_REV@/${PACK_REV}/g" \
  "$recipe/NOTICE.txt" >"$stage/NOTICE.txt"
programs_json=$(printf '"%s",' "${PROGRAMS[@]##*/}")
cat >"$stage/opad-pack.json" <<EOF
{"id": "openfoam", "version": "${OF_VERSION}-${PACK_REV}", "platform": "windows-x64", "openfoam": "${OF_VERSION}",
 "mpi": "dummy", "programs": [${programs_json%,}],
 "sources": ["OpenFOAM-${OF_VERSION}.tgz", "zlib-${ZLIB_VERSION}.tar.gz", "opad-openfoam-recipe.tar.gz"]}
EOF

rm -f "$out/$PACK.zip"
(cd "$stage" && cmake -E tar cf "$out/$PACK.zip" --format=zip -- *)
cp "$work/OpenFOAM-${OF_VERSION}.tgz" "$work/zlib-${ZLIB_VERSION}.tar.gz" "$out/"
tar -czf "$out/opad-openfoam-recipe.tar.gz" -C "$(dirname "$recipe")" "$(basename "$recipe")"
(cd "$out" && sha256sum "$PACK.zip" "OpenFOAM-${OF_VERSION}.tgz" "zlib-${ZLIB_VERSION}.tar.gz" opad-openfoam-recipe.tar.gz >SHA256SUMS)
echo "pack: $out/$PACK.zip ($(stat -c %s "$out/$PACK.zip") bytes, $(ls "$stage/bin" | wc -l) files in bin)"
cat "$out/SHA256SUMS"
