#!/usr/bin/env bash
# Checks a built CalculiX pack: unzipped as OPAD's installer does, its ccx solves a one-element cantilever, and the
# results are written with two-digit exponents as on Linux (the .frd file's fixed columns need them; on Windows
# CMakeLists.txt's __USE_MINGW_ANSI_STDIO gives them).
#
#   engines/calculix/smoke.sh <out dir of make-pack.sh> <work dir>
set -euo pipefail

out=$(cd "$1" && pwd)
work=$(mkdir -p "$2" && cd "$2" && pwd)
exe=
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) exe=.exe ;; esac
rm -rf "$work/pack" "$work/case"
mkdir -p "$work/pack" "$work/case"
unzip -q "$(ls "$out"/opad-calculix-*.zip)" -d "$work/pack"
ccx="$work/pack/bin/ccx$exe"
[ -x "$ccx" ] || { echo "error: bin/ccx$exe is missing or not executable" >&2; exit 1; }
cd "$work/case"
printf '%s\n' '*NODE,NSET=N' '1,0,0,0' '2,1,0,0' '3,1,1,0' '4,0,1,0' '5,0,0,1' '6,1,0,1' '7,1,1,1' '8,0,1,1' \
  '*ELEMENT,TYPE=C3D8,ELSET=E' '1,1,2,3,4,5,6,7,8' '*MATERIAL,NAME=S' '*ELASTIC' '210000,0.3' '*SOLID SECTION,ELSET=E,MATERIAL=S' \
  '*BOUNDARY' '1,1,3' '4,1,3' '5,1,3' '8,1,3' '*STEP' '*STATIC' '*CLOAD' '2,3,1.' '3,3,1.' '6,3,1.' '7,3,1.' '*NODE PRINT,NSET=N' 'U' \
  '*NODE FILE' 'U' '*END STEP' >job.inp
unset LD_LIBRARY_PATH DYLD_LIBRARY_PATH || true
"$ccx" -i job >log.ccx 2>&1 || { tail -20 log.ccx >&2; echo "error: ccx failed" >&2; exit 1; }
tail -3 log.ccx
grep -q "displacements" job.dat || { echo "error: no displacements in job.dat" >&2; exit 1; }
# the loaded corner moved, written with two-digit exponents
grep -E "^ +2 +[0-9]\.[0-9]+E[+-][0-9]{2} " job.dat || { cat job.dat >&2; echo "error: node 2's displacement is not written as expected" >&2; exit 1; }
[ -s job.frd ] || { echo "error: no job.frd" >&2; exit 1; }
echo "ccx: the cantilever solves"
