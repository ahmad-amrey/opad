#!/bin/sh
# Prints one version's section of CHANGELOG.md (without its "## [X.Y.Z] - date" heading, blank lines trimmed at both
# ends): the release notes the Build workflow publishes for tag vX.Y.Z. Exits 1 when the section is missing or empty.
#
#   sh tools/changelog_section.sh 0.2.0 [CHANGELOG.md]
version="$1"
file="${2:-CHANGELOG.md}"
[ -n "$version" ] || { echo "usage: $0 <version> [CHANGELOG.md]" >&2; exit 2; }
[ -f "$file" ] || { echo "$0: no $file" >&2; exit 1; }
tr -d '\r' < "$file" | awk -v v="$version" '
  /^## / {
    if (found) exit
    h = $0
    gsub(/[][]/, "", h)          # "## [0.1.0] - date" -> "## 0.1.0 - date"
    split(h, part, " ")
    if (part[2] == v) { found = 1; next }
  }
  found {
    if ($0 ~ /^[[:space:]]*$/) { blank++; next }
    if (started) for (i = 0; i < blank; i++) print ""
    blank = 0; started = 1; print
  }
  END { if (!started) exit 1 }
'
