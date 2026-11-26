#!/usr/bin/env bash
# Checks a built OpenFOAM pack as OPAD runs it: unzipped as OPAD's installer does (the programs' modes kept), each program
# starts with nothing set but WM_PROJECT_DIR (no MPI, no LD_LIBRARY_PATH / DYLD_LIBRARY_PATH, no OpenFOAM environment),
# and OpenCFD's pitzDaily tutorial (from the source tarball beside the pack) is meshed and solved for 20 iterations.
#
#   engines/openfoam/smoke.sh <out dir of make-pack.sh> <work dir>
#
# Runs on the pack's own system: Linux, macOS, or Windows under Git Bash / MSYS2.
set -euo pipefail

out=$(cd "$1" && pwd)
work=$(mkdir -p "$2" && cd "$2" && pwd)
zip=$(ls "$out"/opad-openfoam-*.zip)
tgz=$(ls "$out"/OpenFOAM-*.tgz)
version=$(basename "$tgz" .tgz)

rm -rf "$work/pack" "$work/pitzDaily"
mkdir -p "$work/pack"
unzip -q "$zip" -d "$work/pack"
pack="$work/pack"
exe=
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) exe=.exe ;; esac
project=$pack
[ -n "$exe" ] && project=$(cygpath -m "$pack")
unset LD_LIBRARY_PATH DYLD_LIBRARY_PATH FOAM_LIBBIN WM_PROJECT_DIR || true

run() {  # program, args...: from the case folder, as OPAD starts them (no shell environment of OpenFOAM's)
  local p=$1
  shift
  env WM_PROJECT_DIR="$project" "$pack/bin/$p$exe" "$@"
}

for p in $(sed -n 's/.*"programs": \[\(.*\)\].*/\1/p' "$pack/opad-pack.json" | tr -d '" ' | tr ',' ' '); do
  [ -x "$pack/bin/$p$exe" ] || { echo "error: bin/$p$exe is missing or not executable" >&2; exit 1; }
  if ! run "$p" -help >"$work/help.$p" 2>&1 || ! grep -q "^Usage:" "$work/help.$p"; then
    cat "$work/help.$p" >&2
    echo "error: $p does not start" >&2
    exit 1
  fi
  echo "starts: $p"
done

tar -xzf "$tgz" -C "$work" "$version/tutorials/incompressible/simpleFoam/pitzDaily"
mv "$work/$version/tutorials/incompressible/simpleFoam/pitzDaily" "$work/pitzDaily"
rm -rf "${work:?}/$version"
cd "$work/pitzDaily"
# 20 iterations, no function objects (the tutorial's streamlines live in a library the pack does not carry).
sed -e 's/^endTime .*/endTime         20;/' -e 's/^writeInterval .*/writeInterval   20;/' -e '/^functions/,/^}/d' \
  system/controlDict >system/controlDict.new && mv system/controlDict.new system/controlDict
run blockMesh >log.blockMesh 2>&1 || { tail -30 log.blockMesh >&2; echo "error: blockMesh failed" >&2; exit 1; }
run simpleFoam >log.simpleFoam 2>&1 || { tail -30 log.simpleFoam >&2; echo "error: simpleFoam failed" >&2; exit 1; }
grep -q "^End" log.simpleFoam || { tail -30 log.simpleFoam >&2; echo "error: simpleFoam did not end" >&2; exit 1; }
[ -d 20 ] && [ -f 20/U ] || { echo "error: simpleFoam wrote no fields at time 20" >&2; exit 1; }
grep -E "^(Time = |smoothSolver|GAMG)" log.simpleFoam | tail -3
echo "pitzDaily: $(grep -c '^Time = ' log.simpleFoam) iterations, fields written at 20"
