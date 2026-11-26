#!/usr/bin/env bash
# OPAD's CalculiX pack: ccx built from its pinned source with SPOOLES (built from its pinned source) as the equation solver
# and ARPACK-NG + OpenBLAS (the system's packaged static libraries) for eigenvalues. OPAD solves with one thread by
# default (OPAD_CCX_THREADS), so SPOOLES is built serial. The platform is the system this runs on:
#
#   engines/calculix/make-pack.sh <work dir> <out dir>
#
# windows-x64: MSYS2 with mingw64 first on PATH; needs mingw-w64-x86_64-{gcc,gcc-fortran,arpack,openblas}, cmake, ninja,
#   curl. One static ccx.exe that needs nothing but Windows.
# linux-x64: Ubuntu 22.04 or later; needs gcc gfortran libopenblas-dev libarpack2-dev cmake ninja-build curl. One fully
#   static ccx (no glibc version to match).
# macos-arm64: Homebrew's gcc (gfortran), arpack, openblas, cmake, ninja. ccx with the GCC runtime linked in where Homebrew
#   ships it static; any library of Homebrew's it still needs is carried beside it in bin/ (@loader_path), ad-hoc signed.
# Writes into <out dir>:
#   opad-calculix-<ver>-<rev>-<platform>.zip    the pack (bin/ccx, licenses/, NOTICE.txt, opad-pack.json)
#   ccx_<ver>.src.tar.bz2, spooles.2.2.tgz       the exact sources it was built from
#   opad-calculix-recipe.tar.gz                  this folder (the recipe and our patches)
#   SHA256SUMS
set -euo pipefail

CCX_VERSION=2.24
CCX_URL=https://www.dhondt.de/ccx_${CCX_VERSION}.src.tar.bz2
CCX_SHA256=99fb4f9b0ec19b100a914c3a097ea1e7f0c22f715b96338acc4a66b63306a2e2
SPOOLES_URL=https://www.netlib.org/linalg/spooles/spooles.2.2.tgz
SPOOLES_SHA256=a84559a0e987a1e423055ef4fdf3035d55b65bbe4bf915efaa1a35bef7f8c5dd
PACK_REV=1

case "$(uname -s)" in
  MINGW* | MSYS*) target=windows-x64 platform_name="Windows x64" exe=.exe ;;
  Linux) target=linux-x64 platform_name="Linux x64" exe= ;;
  Darwin) target=macos-arm64 platform_name="macOS on Apple silicon" exe= ;;
  *) echo "error: no CalculiX pack is built on $(uname -s)" >&2; exit 1 ;;
esac
PACK=opad-calculix-${CCX_VERSION}-${PACK_REV}-${target}

recipe=$(cd "$(dirname "$0")" && pwd)
work=$(mkdir -p "$1" && cd "$1" && pwd)
out=$(mkdir -p "$2" && cd "$2" && pwd)
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
fetch() {  # url sha256 file
  if [ ! -f "$work/$3" ]; then curl -sSfL --retry 3 -o "$work/$3.part" "$1" && mv "$work/$3.part" "$work/$3"; fi
  [ "$(sha256 "$work/$3" | cut -d' ' -f1)" = "$2" ] || { echo "error: $3 does not have the pinned SHA-256" >&2; exit 1; }
}
fetch "$CCX_URL" "$CCX_SHA256" "ccx_${CCX_VERSION}.src.tar.bz2"
fetch "$SPOOLES_URL" "$SPOOLES_SHA256" "spooles.2.2.tgz"

log="$work/build.log"
: >"$log"

src="$work/src-${CCX_VERSION}"
if [ ! -f "$src/.opad-unpacked" ]; then
  rm -rf "$src"
  mkdir -p "$src/SPOOLES.2.2"
  tar -xjf "$work/ccx_${CCX_VERSION}.src.tar.bz2" -C "$src"
  tar -xzf "$work/spooles.2.2.tgz" -C "$src/SPOOLES.2.2"
  for p in "$recipe"/patches/*.patch; do [ -f "$p" ] && patch -d "$src" -p1 <"$p"; done
  touch "$src/.opad-unpacked"
fi

# The compilers, ARPACK-NG and OpenBLAS, how ccx is linked, and what goes in the notice about them.
licenses=()
case "$target" in
  windows-x64)
    cc=gcc fc=gfortran
    prefix=$(dirname "$(dirname "$(command -v gfortran)")")
    arpack="$prefix/lib/libarpack.a" blas="$prefix/lib/libopenblas.a"
    link="-static;-fopenmp;pthread;m"
    licenses=("$prefix/share/licenses/arpack/COPYING:ARPACK-NG-COPYING.txt" "$prefix/share/licenses/OpenBLAS/LICENSE:OpenBLAS-LICENSE.txt")
    for f in "$prefix"/share/licenses/gcc-libgfortran/* "$prefix"/share/licenses/gcc-libs/*; do
      [ -f "$f" ] && licenses+=("$f:gcc-runtime-$(basename "$f").txt")
    done
    pacman=$(command -v pacman || echo /c/msys64/usr/bin/pacman)  # MSYS2's usr/bin on PATH breaks gcc's TEMP under Git Bash
    packages="MSYS2 packages used for ARPACK-NG, OpenBLAS and the compilers: $("$pacman" -Q mingw-w64-x86_64-arpack mingw-w64-x86_64-openblas mingw-w64-x86_64-gcc-fortran | tr '\n' ';' | sed 's/;$//; s/;/; /g')."
    runtime="- The GCC runtime (libgfortran, libquadmath, libgomp, libgcc: GPL 3 with the GCC Runtime Library Exception) and"$'\n'"  mingw-w64's winpthreads, licenses/gcc-runtime-*."
    ;;
  linux-x64)
    cc=gcc fc=gfortran
    multiarch=$(gcc -print-multiarch)
    arpack=/usr/lib/$multiarch/libarpack.a
    blas=
    for b in /usr/lib/$multiarch/openblas-pthread/libopenblas.a /usr/lib/$multiarch/libopenblas.a; do
      [ -f "$b" ] && { blas=$b; break; }
    done
    link="-static;-fopenmp;pthread;m"
    for l in libarpack2:ARPACK-NG-COPYING.txt libopenblas0-pthread:OpenBLAS-LICENSE.txt libgfortran5:gcc-runtime-libgfortran5.txt \
      libgcc-s1:gcc-runtime-libgcc-s1.txt libquadmath0:gcc-runtime-libquadmath0.txt libc6:glibc-copyright.txt; do
      [ -f "/usr/share/doc/${l%%:*}/copyright" ] && licenses+=("/usr/share/doc/${l%%:*}/copyright:${l#*:}")
    done
    packages="Ubuntu packages used for ARPACK-NG, OpenBLAS and the compilers: $(dpkg-query -W -f='${Package} ${Version}; ' libarpack2-dev libopenblas-dev gfortran 2>/dev/null | sed 's/; $//')."
    runtime="- The GCC runtime (libgfortran, libquadmath, libgomp, libgcc: GPL 3 with the GCC Runtime Library Exception) and the GNU C"$'\n'"  Library (LGPL 2.1 or later), linked in: licenses/gcc-runtime-* and licenses/glibc-copyright.txt."
    ;;
  macos-arm64)
    gcc_prefix=$(brew --prefix gcc)
    fc=$(ls "$gcc_prefix"/bin/gfortran-[0-9]* | sort -V | tail -1)
    cc=$(ls "$gcc_prefix"/bin/gcc-[0-9]* | grep -E '/gcc-[0-9]+$' | sort -V | tail -1)
    # Homebrew's own libraries, carried beside ccx (below), not their static archives: its OpenBLAS uses LLVM's OpenMP
    # (libomp), which a static link with GCC's -fopenmp (libgomp, ccx's own OpenMP) would leave unresolved.
    arpack="$(brew --prefix arpack)/lib/libarpack.dylib"
    blas="$(brew --prefix openblas)/lib/libopenblas.dylib"
    link="-static-libgfortran;-static-libgcc;-fopenmp"
    # The licences of what is carried: each formula's own files at the top of its keg (ARPACK-NG's and OpenBLAS's under
    # the names the notice gives on every system).
    for l in arpack/COPYING:ARPACK-NG-COPYING.txt openblas/LICENSE:OpenBLAS-LICENSE.txt; do
      p="$(brew --prefix "${l%%/*}")/${l#*/}"
      p=${p%:*}
      [ -f "$p" ] && licenses+=("$p:${l##*:}")
    done
    for formula in libomp gcc; do
      for p in "$(brew --prefix "$formula")"/{LICENSE,LICENSE.txt,LICENSE.TXT,COPYING,COPYING.RUNTIME,COPYING3,COPYING.LIB}; do
        b=$(basename "$p")
        [ -f "$p" ] && licenses+=("$p:$formula-${b%.[tT][xX][tT]}.txt")
      done
    done
    packages="Homebrew formulae used for ARPACK-NG, OpenBLAS, its OpenMP and the compilers: $(brew list --versions arpack openblas libomp gcc | tr '\n' ';' | sed 's/;$//; s/;/; /g')."
    runtime="- LLVM's OpenMP runtime libomp (OpenBLAS's; Apache 2.0 with LLVM exceptions) and the GCC runtime (libgfortran,"$'\n'"  libgcc, libgomp: GPL 3 with the GCC Runtime Library Exception), as Homebrew builds them: licenses/libomp-*, gcc-*."
    ;;
esac
for f in "$arpack" "$blas"; do
  [ -n "$f" ] && [ -f "$f" ] || { echo "error: ARPACK-NG or OpenBLAS is missing (${f:-libopenblas.a})" >&2; exit 1; }
done

# SPOOLES and ccx through CMakeLists.txt beside this script (the upstream makefiles' source lists, no make needed).
echo "building SPOOLES 2.2 and ccx ${CCX_VERSION} for $target (log: $log)"
{
  cmake -S "$recipe" -B "$work/build-${CCX_VERSION}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$cc" -DCMAKE_Fortran_COMPILER="$fc" \
    -DSRC="$src" -DCCX_VERSION="$CCX_VERSION" -DARPACK="$arpack" -DBLAS="$blas" -DLINK="$link" &&
    cmake --build "$work/build-${CCX_VERSION}" -j "$jobs"
} >>"$log" 2>&1 || { tail -40 "$log" >&2; exit 1; }
built="$work/build-${CCX_VERSION}/ccx$exe"
[ -f "$built" ] || { echo "error: $built was not built" >&2; exit 1; }

stage="$work/$PACK"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/licenses"
cp "$built" "$stage/bin/ccx$exe"
case "$target" in
  windows-x64)
    strip "$stage/bin/ccx.exe"
    if objdump -p "$stage/bin/ccx.exe" | sed -n 's/^\s*DLL Name: //p' | grep -viE '^(kernel32|msvcrt|advapi32|user32|ws2_32|shell32|ole32|api-ms-win-.*)\.dll' >&2; then
      echo "error: ccx imports DLLs that Windows does not have (above)" >&2
      exit 1
    fi
    ;;
  linux-x64)
    strip "$stage/bin/ccx"
    if readelf -d "$stage/bin/ccx" | grep -q NEEDED; then
      readelf -d "$stage/bin/ccx" | grep NEEDED >&2
      echo "error: ccx is not static (above)" >&2
      exit 1
    fi
    ;;
  macos-arm64)
    # Every library outside macOS's own goes into lib/ (ccx names them @loader_path/../lib/<name>, they name each other
    # @loader_path/<name>), so a Mac without Homebrew runs it; their run paths into Homebrew are dropped, so a Mac with
    # Homebrew still loads the pack's copies. A library's own @rpath / @loader_path names are resolved where it came from.
    mkdir -p "$stage/lib"
    declare -A orig=()
    orig["$stage/bin/ccx"]=$built
    queue=("$stage/bin/ccx")
    while [ ${#queue[@]} -gt 0 ]; do
      f=${queue[0]}
      queue=("${queue[@]:1}")
      odir=$(dirname "${orig[$f]}")
      here=@loader_path
      [ "$(dirname "$f")" = "$stage/bin" ] && here=@loader_path/../lib
      rpaths=$(otool -l "$f" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }')
      own=$(otool -D "$f" | tail -n +2)
      for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
        [ "$dep" = "$own" ] && continue  # a library's own id
        case "$dep" in /usr/lib/* | /System/*) continue ;; esac
        name=${dep##*/}
        from=
        case "$dep" in
          @rpath/*)
            for r in $rpaths "$gcc_prefix/lib/gcc/current" "$(brew --prefix)/lib"; do
              r=${r//@loader_path/$odir}
              r=${r//@executable_path/$odir}
              [ -f "$r/$name" ] && { from="$r/$name"; break; }
            done
            ;;
          @loader_path/* | @executable_path/*) from="$odir/${dep#*/}" ;;
          *) from=$dep ;;
        esac
        [ -n "$from" ] && [ -f "$from" ] || { echo "error: $f needs $dep, which is not found" >&2; exit 1; }
        if [ ! -f "$stage/lib/$name" ]; then
          cp "$from" "$stage/lib/$name"
          chmod u+w "$stage/lib/$name"
          install_name_tool -id "@loader_path/$name" "$stage/lib/$name"
          orig["$stage/lib/$name"]=$(realpath "$from")
          queue+=("$stage/lib/$name")
        fi
        install_name_tool -change "$dep" "$here/$name" "$f"
      done
      for r in $rpaths; do install_name_tool -delete_rpath "$r" "$f"; done
    done
    shopt -s nullglob
    for f in "$stage"/bin/* "$stage"/lib/*; do
      strip -x "$f"
      codesign --force --sign - "$f"
    done
    if otool -L "$stage"/bin/* "$stage"/lib/* | awk '!/:$/ {print $1}' | grep -vE '^(/usr/lib/|/System/|@loader_path/)' >&2; then
      echo "error: ccx or its libraries still name a library outside the pack (above)" >&2
      exit 1
    fi
    shopt -u nullglob
    ;;
esac

cp "$src/CalculiX/ccx_${CCX_VERSION}/src/gpl-2.0.txt" "$stage/licenses/CalculiX-gpl-2.0.txt"
for l in "${licenses[@]}"; do cp "${l%:*}" "$stage/licenses/${l##*:}"; done
carried=$(ls "$stage/lib" 2>/dev/null | tr '\n' ' ' | sed 's/ $//' || true)
[ -n "$carried" ] && runtime+=$'\n'"- Carried in lib/: $carried."
notice=$(<"$recipe/NOTICE.txt")
notice=${notice//@CCX_VERSION@/$CCX_VERSION}
notice=${notice//@PACK_REV@/$PACK_REV}
notice=${notice//@PLATFORM@/$platform_name}
notice=${notice//@RUNTIME@/$runtime}
notice=${notice//@PACKAGES@/$packages}
printf '%s\n' "$notice" >"$stage/NOTICE.txt"
cat >"$stage/opad-pack.json" <<EOF
{"id": "calculix", "version": "${CCX_VERSION}-${PACK_REV}", "platform": "${target}", "ccx": "${CCX_VERSION}",
 "solvers": ["SPOOLES", "ARPACK"], "programs": ["ccx"],
 "sources": ["ccx_${CCX_VERSION}.src.tar.bz2", "spooles.2.2.tgz", "opad-calculix-recipe.tar.gz"]}
EOF

rm -f "$out/$PACK.zip"
(cd "$stage" && cmake -E tar cf "$out/$PACK.zip" --format=zip -- *)
cp "$work/ccx_${CCX_VERSION}.src.tar.bz2" "$work/spooles.2.2.tgz" "$out/"
tar -czf "$out/opad-calculix-recipe.tar.gz" -C "$(dirname "$recipe")" "$(basename "$recipe")"
(cd "$out" && sha256 "$PACK.zip" "ccx_${CCX_VERSION}.src.tar.bz2" spooles.2.2.tgz opad-calculix-recipe.tar.gz >SHA256SUMS)
echo "pack: $out/$PACK.zip ($(wc -c <"$out/$PACK.zip" | tr -d ' ') bytes)"
cat "$out/SHA256SUMS"
