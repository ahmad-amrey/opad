#!/usr/bin/env bash
# OPAD's CalculiX pack for Windows x64: ccx built from its pinned source with SPOOLES (built from its pinned source) as the
# equation solver and ARPACK-NG + OpenBLAS (MSYS2's static libraries) for eigenvalues, linked statically: one ccx.exe that
# needs nothing but Windows. OPAD solves with one thread by default (OPAD_CCX_THREADS), so SPOOLES is built serial.
#
#   engines/calculix/make-pack.sh <work dir> <out dir>
#
# Runs in MSYS2 with mingw64 first on PATH; needs mingw-w64-x86_64-{gcc,gcc-fortran,arpack,openblas}, cmake, ninja, curl.
# Writes into <out dir>:
#   opad-calculix-<ver>-<rev>-windows-x64.zip   the pack (bin/ccx.exe, licenses/, NOTICE.txt, opad-pack.json)
#   ccx_<ver>.src.tar.bz2, spooles.2.2.tgz       the exact sources it was built from
#   opad-calculix-recipe.tar.gz                  this folder (the recipe and our patches)
#   SHA256SUMS
set -euo pipefail

CCX_VERSION=2.22
CCX_URL=https://www.dhondt.de/ccx_${CCX_VERSION}.src.tar.bz2
CCX_SHA256=3a94dcc775a31f570229734b341d6b06301ebdc759863df901c8b9bf1854c0bc
SPOOLES_URL=https://www.netlib.org/linalg/spooles/spooles.2.2.tgz
SPOOLES_SHA256=a84559a0e987a1e423055ef4fdf3035d55b65bbe4bf915efaa1a35bef7f8c5dd
PACK_REV=1
PACK=opad-calculix-${CCX_VERSION}-${PACK_REV}-windows-x64

recipe=$(cd "$(dirname "$0")" && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
out=$(mkdir -p "$2" && cd "$2" && pwd)
jobs=${JOBS:-$(nproc)}

fetch() {  # url sha256 file
  if [ ! -f "$work/$3" ]; then curl -sSfL --retry 3 -o "$work/$3.part" "$1" && mv "$work/$3.part" "$work/$3"; fi
  echo "$2  $work/$3" | sha256sum -c --quiet - || { echo "error: $3 does not have the pinned SHA-256" >&2; exit 1; }
}
fetch "$CCX_URL" "$CCX_SHA256" "ccx_${CCX_VERSION}.src.tar.bz2"
fetch "$SPOOLES_URL" "$SPOOLES_SHA256" "spooles.2.2.tgz"

log="$work/build.log"
: >"$log"

src="$work/src"
if [ ! -f "$src/.opad-unpacked" ]; then
  rm -rf "$src"
  mkdir -p "$src/SPOOLES.2.2"
  tar -xjf "$work/ccx_${CCX_VERSION}.src.tar.bz2" -C "$src"
  tar -xzf "$work/spooles.2.2.tgz" -C "$src/SPOOLES.2.2"
  for p in "$recipe"/patches/*.patch; do [ -f "$p" ] && patch -d "$src" -p1 <"$p"; done
  touch "$src/.opad-unpacked"
fi

# SPOOLES and ccx through CMakeLists.txt beside this script (the upstream makefiles' source lists, no make needed).
echo "building SPOOLES 2.2 and ccx ${CCX_VERSION} (log: $log)"
prefix=$(dirname "$(dirname "$(command -v gfortran)")")
{
  cmake -S "$recipe" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_Fortran_COMPILER=gfortran \
    -DSRC="$src" -DCCX_VERSION="$CCX_VERSION" -DARPACK="$prefix/lib/libarpack.a" -DBLAS="$prefix/lib/libopenblas.a" &&
    cmake --build "$work/build" -j "$jobs"
} >>"$log" 2>&1 || { tail -40 "$log" >&2; exit 1; }
exe="$work/build/ccx.exe"
[ -f "$exe" ] || { echo "error: $exe was not built" >&2; exit 1; }
strip "$exe"
if objdump -p "$exe" | sed -n 's/^\s*DLL Name: //p' | grep -viE '^(kernel32|msvcrt|advapi32|user32|ws2_32|shell32|ole32|api-ms-win-.*)\.dll' >&2; then
  echo "error: ccx imports DLLs that Windows does not have (above)" >&2
  exit 1
fi

stage="$work/$PACK"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/licenses"
cp "$exe" "$stage/bin/ccx.exe"
cp "$src/CalculiX/ccx_${CCX_VERSION}/src/gpl-2.0.txt" "$stage/licenses/CalculiX-gpl-2.0.txt"
cp "$prefix/share/licenses/arpack/COPYING" "$stage/licenses/ARPACK-NG-COPYING.txt"
cp "$prefix/share/licenses/OpenBLAS/LICENSE" "$stage/licenses/OpenBLAS-LICENSE.txt"
for f in "$prefix"/share/licenses/gcc-libgfortran/* "$prefix"/share/licenses/gcc-libs/*; do
  [ -f "$f" ] && cp "$f" "$stage/licenses/gcc-runtime-$(basename "$f").txt"
done
pacman=$(command -v pacman || echo /c/msys64/usr/bin/pacman)  # MSYS2's usr/bin on PATH breaks gcc's TEMP under Git Bash
versions=$("$pacman" -Q mingw-w64-x86_64-arpack mingw-w64-x86_64-openblas mingw-w64-x86_64-gcc-fortran | tr '\n' ';')
sed -e "s/@CCX_VERSION@/${CCX_VERSION}/g" -e "s/@PACK_REV@/${PACK_REV}/g" -e "s|@MSYS2@|${versions%;}|g" \
  "$recipe/NOTICE.txt" >"$stage/NOTICE.txt"
cat >"$stage/opad-pack.json" <<EOF
{"id": "calculix", "version": "${CCX_VERSION}-${PACK_REV}", "platform": "windows-x64", "ccx": "${CCX_VERSION}",
 "solvers": ["SPOOLES", "ARPACK"], "programs": ["ccx"],
 "sources": ["ccx_${CCX_VERSION}.src.tar.bz2", "spooles.2.2.tgz", "opad-calculix-recipe.tar.gz"]}
EOF

rm -f "$out/$PACK.zip"
(cd "$stage" && cmake -E tar cf "$out/$PACK.zip" --format=zip -- *)
cp "$work/ccx_${CCX_VERSION}.src.tar.bz2" "$work/spooles.2.2.tgz" "$out/"
tar -czf "$out/opad-calculix-recipe.tar.gz" -C "$(dirname "$recipe")" "$(basename "$recipe")"
(cd "$out" && sha256sum "$PACK.zip" "ccx_${CCX_VERSION}.src.tar.bz2" spooles.2.2.tgz opad-calculix-recipe.tar.gz >SHA256SUMS)
echo "pack: $out/$PACK.zip ($(stat -c %s "$out/$PACK.zip") bytes)"
cat "$out/SHA256SUMS"
