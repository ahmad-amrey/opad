#!/usr/bin/env bash
# engines.yml's publish step: one release for one pack revision, every system's zip in it (engines/ci/plan.sh names the
# parts). Each part is downloaded from where the plan found it (a release of that system alone, an earlier run's artifact
# or this run's) and checked against its SHA256SUMS; the release gets the three zips, the sources once (the same file from
# every system, else it stops), each system's recipe as opad-<id>-recipe-<platform>.tar.gz, one SHA256SUMS over all of it,
# and the pack's NOTICE.txt as its notes (once when the systems' notices agree, else one section per system).
#
# Environment: GH_TOKEN, GH_REPO; TAG, TITLE, TARGET, PARTS (the plan's json list); GITHUB_RUN_ID (this run's artifacts);
# GITHUB_STEP_SUMMARY (optional). DRY_RUN=1 assembles and lists the release without creating it (try it by hand with a
# plan's parts).
set -euo pipefail

work=$(mktemp -d)
dist="$work/dist"
mkdir -p "$dist"
notes="$work/notes.md"
first_notice=
same_notices=true
summary="$work/summary.md"
: >"$summary"

count=$(jq length <<<"$PARTS")
for ((i = 0; i < count; i++)); do  # not a read loop: gh may read stdin
  part=$(jq -c ".[$i]" <<<"$PARTS")
  platform=$(jq -r .platform <<<"$part")
  system=$(jq -r .system <<<"$part")
  release=$(jq -r .release <<<"$part")
  run=$(jq -r .run <<<"$part")
  artifact=$(jq -r .artifact <<<"$part")
  dir="$work/$platform"
  mkdir -p "$dir"
  if [ -n "$release" ]; then
    echo "$system: from the release $release"
    gh release download "$release" --repo "$GH_REPO" --dir "$dir" </dev/null
  else
    echo "$system: from the artifact $artifact of run ${run:-$GITHUB_RUN_ID}"
    gh run download "${run:-$GITHUB_RUN_ID}" --repo "$GH_REPO" --name "$artifact" --dir "$dir" </dev/null
  fi
  (cd "$dir" && sha256sum -c --quiet SHA256SUMS)
  zips=$(cd "$dir" && ls opad-*-"$platform".zip)
  [ "$(printf '%s\n' "$zips" | wc -l)" = 1 ] || { echo "::error::$system: not one pack: $zips"; exit 1; }
  for f in "$dir"/*; do
    name=$(basename "$f")
    case "$name" in
      SHA256SUMS) ;;
      opad-*-recipe.tar.gz) cp "$f" "$dist/${name%.tar.gz}-$platform.tar.gz" ;;
      *)
        if [ -e "$dist/$name" ]; then
          cmp -s "$f" "$dist/$name" || { echo "::error::$name differs between the systems' packs"; exit 1; }
        else
          cp "$f" "$dist/$name"
        fi
        ;;
    esac
  done
  unzip -p "$dir/$zips" NOTICE.txt >"$work/notice-$platform.txt"
  if [ -z "$first_notice" ]; then first_notice="$work/notice-$platform.txt"
  elif ! cmp -s "$first_notice" "$work/notice-$platform.txt"; then same_notices=false; fi
  printf '%s: `%s`, size `%s`, SHA-256 `%s`\n\n' "$system" "$zips" "$(stat -c %s "$dir/$zips")" "$(sha256sum "$dir/$zips" | cut -d' ' -f1)" >>"$summary"
done

if [ "$same_notices" = true ]; then
  cp "$first_notice" "$notes"
else
  : >"$notes"
  for ((i = 0; i < count; i++)); do
    printf '## %s\n\n' "$(jq -r ".[$i].system" <<<"$PARTS")" >>"$notes"
    cat "$work/notice-$(jq -r ".[$i].platform" <<<"$PARTS").txt" >>"$notes"
    printf '\n' >>"$notes"
  done
fi
(cd "$dist" && sha256sum * >SHA256SUMS)
ls -l "$dist"
cat "$dist/SHA256SUMS"

if [ "${DRY_RUN:-}" = 1 ]; then
  echo "Dry run: $TAG not created. Notes:"
  cat "$notes"
  exit 0
fi
if gh release view "$TAG" --repo "$GH_REPO" >/dev/null 2>&1; then
  echo "::notice::$TAG is published already (another run was first); it is never replaced."
  exit 0
fi
gh release create "$TAG" "$dist"/* --repo "$GH_REPO" --latest=false --target "$TARGET" --title "$TITLE" --notes-file "$notes"
echo "::notice title=Published $TAG::$GITHUB_SERVER_URL/$GH_REPO/releases/tag/$TAG"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### $TITLE"
    echo "Published: $GITHUB_SERVER_URL/$GH_REPO/releases/tag/$TAG (commit $TARGET). To pin in core/src/sim/engine_packs.cpp:"
    echo
    cat "$summary"
  } >>"$GITHUB_STEP_SUMMARY"
fi
