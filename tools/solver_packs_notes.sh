#!/bin/sh
# The "Solvers" section of an OPAD release's notes (ci.yml's Release notes step): the solver packs this version installs,
# read from the pins in core/src/sim/engine_packs.cpp (a pin with a SHA-256 is offered), each linked to its release
# (engines-<id>-<version>, every system's zip in it). Prints nothing when no pack is pinned.
#
#   sh tools/solver_packs_notes.sh [core/src/sim/engine_packs.cpp]
set -eu
src=${1:-core/src/sim/engine_packs.cpp}
# {"openfoam", "OpenFOAM v2606", "v2606-1", "windows-x64", 43636466, "70562bb0..."},
sed -n 's/^ *{"\([a-z]*\)", "\([^"]*\)", "\([^"]*\)", "\([a-z0-9-]*\)", [0-9]*, "\([0-9a-f]\{64\}\)"},.*$/\1|\2|\3|\4/p' "$src" |
  awk -F'|' '
    function os_name(p) { return p == "windows-x64" ? "Windows x64" : p == "linux-x64" ? "Linux x64" : p == "macos-arm64" ? "macOS on Apple silicon" : p }
    {
      key = $1 "-" $3
      if (!(key in name)) { order[++n] = key; name[key] = $2; rev[key] = $3; sub(/^.*-/, "", rev[key]) }
      systems[key] = (key in systems) ? systems[key] ", " os_name($4) : os_name($4)
    }
    END {
      if (n == 0) exit
      print ""
      print "### Solvers"
      print ""
      print "*Install* in Simulate downloads the solver for your system from these releases, checked against the size and"
      print "SHA-256 this version of OPAD carries:"
      print ""
      for (i = 1; i <= n; i++)
        printf "- [%s (pack revision %s)](https://github.com/ahmad-amrey/opad/releases/tag/engines-%s): %s\n", name[order[i]], rev[order[i]], order[i], systems[order[i]]
    }'
