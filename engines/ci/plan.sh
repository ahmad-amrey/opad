#!/usr/bin/env bash
# engines.yml's plan: which packs (pack x system) this run builds, and which pack revisions it publishes and from where.
#
# A pull request's run (or a hand run without "publish") builds and checks every pack it is asked for, publishing nothing.
# On dev (a push touching engines/**, or a hand run with "publish"), a pack revision is published when its release
# engines-<id>-<version>-<rev> does not exist yet: one release holding every system's zip (engines/ci/publish.sh). Each
# system's part comes from the release a pack revision had per system before (engines-<id>-<version>-<rev>-<platform>, so
# the bytes OPAD pinned stay the same), else from the artifacts of an earlier engines.yml run that built exactly this code
# (the pull request's, when it was up to date with dev: same git tree), else it is built in this run. A published tag is
# never replaced: a change to a pack is a new revision (PACK_REV in its make-pack.sh).
#
# Environment: GH_TOKEN, GITHUB_REPOSITORY, GITHUB_REF, GITHUB_OUTPUT; PACKS (both, openfoam, calculix); PUBLISH (true or
# false). Runs in the checkout of the commit being built. Writes build=<json list> (pack x system) and publish=<json list>
# (one per pack revision, with its parts per system) to GITHUB_OUTPUT.
set -euo pipefail

tree=$(git rev-parse 'HEAD^{tree}')
commit=$(git rev-parse HEAD)
value() { sed -n "s/^$2=\(.*\)$/\1/p" "engines/$1/make-pack.sh" | head -1; }  # pack, variable
publishing=false
if [ "${PUBLISH:-false}" = true ]; then
  if [ "$GITHUB_REF" != refs/heads/dev ]; then
    echo "::error::Solver packs are published from dev only; this run is on $GITHUB_REF."
    exit 1
  fi
  publishing=true
fi
echo "Commit $commit, tree $tree; publishing: $publishing"

build='[]'
publish='[]'
for id in openfoam calculix; do
  [ "${PACKS:-both}" = both ] || [ "$PACKS" = "$id" ] || continue
  case "$id" in
    openfoam) version=$(value openfoam OF_VERSION) label="OpenFOAM $version" ;;
    calculix) version=$(value calculix CCX_VERSION) label="CalculiX $version" ;;
  esac
  rev=$(value "$id" PACK_REV)
  [ -n "$version" ] && [ -n "$rev" ] || { echo "::error::engines/$id/make-pack.sh names no version or PACK_REV"; exit 1; }
  release="engines-$id-$version-$rev"
  if [ "$publishing" = true ]; then
    if gh release view "$release" --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
      echo "$release: published already."
      continue
    fi
    if [ -n "$(git ls-remote --tags origin "refs/tags/$release")" ]; then
      echo "::error::The tag $release exists without a release: delete it (git push origin :refs/tags/$release) and run again."
      exit 1
    fi
  fi
  parts='[]'
  for platform in windows-x64 linux-x64 macos-arm64; do
    # Runners: the Windows OpenFOAM is cross-built on Linux (and run on Windows by its own job); the Linux packs on the
    # oldest Ubuntu runner, so that their glibc floor is low (OpenFOAM's libraries are not static; ccx is).
    case "$id/$platform" in
      openfoam/windows-x64) runner=ubuntu-24.04 ;;
      calculix/windows-x64) runner=windows-latest ;;
      */linux-x64) runner=ubuntu-22.04 ;;
      */macos-arm64) runner=macos-15 ;;
    esac
    case "$platform" in
      windows-x64) system="Windows x64" ;;
      linux-x64) system="Linux x64" ;;
      macos-arm64) system="macOS on Apple silicon" ;;
    esac
    unit=$(jq -nc --arg id "$id" --arg platform "$platform" --arg runner "$runner" --arg system "$system" \
      '{id: $id, platform: $platform, key: "\($id)-\($platform)", runner: $runner, artifact: "engine-\($id)-\($platform)", system: $system}')
    if [ "$publishing" = false ]; then
      build=$(jq -c --argjson u "$unit" '. + [$u]' <<<"$build")
      continue
    fi
    own="$release-$platform"  # this pack revision's release for this system alone, from before they were one release
    if gh release view "$own" --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
      echo "$release, $system: the pack published as $own (the bytes OPAD pinned)."
      parts=$(jq -c --argjson u "$unit" --arg own "$own" '. + [$u + {release: $own, run: ""}]' <<<"$parts")
      continue
    fi
    # An earlier run of this workflow, in this repository, that built this very code: its pack is the one to publish.
    run=
    while read -r rid head_sha; do
      [ -n "$rid" ] || continue
      [ "$(gh api "repos/$GITHUB_REPOSITORY/actions/runs/$rid" --jq .path)" = .github/workflows/engines.yml ] || continue
      if [ "$(gh api "repos/$GITHUB_REPOSITORY/git/commits/$head_sha" --jq .tree.sha)" != "$tree" ]; then
        echo "$release, $system: run $rid built other code ($head_sha)."
        continue
      fi
      run=$rid
      break
    done < <(gh api --paginate "repos/$GITHUB_REPOSITORY/actions/artifacts?name=engine-$id-$platform&per_page=100" \
      --jq '.artifacts[] | select((.expired | not) and .workflow_run.head_repository_id == .workflow_run.repository_id)
            | "\(.workflow_run.id) \(.workflow_run.head_sha)"' | sort -u -n -r)
    if [ -n "$run" ]; then
      echo "$release, $system: the pack of run $run (the same code)."
    else
      echo "$release, $system: no earlier build of this code; building it."
      build=$(jq -c --argjson u "$unit" '. + [$u]' <<<"$build")
    fi
    parts=$(jq -c --argjson u "$unit" --arg run "$run" '. + [$u + {release: "", run: $run}]' <<<"$parts")
  done
  if [ "$publishing" = true ]; then
    publish=$(jq -c --arg id "$id" --arg tag "$release" --arg title "$label for OPAD (pack revision $rev)" --arg target "$commit" \
      --argjson parts "$parts" '. + [{id: $id, tag: $tag, title: $title, target: $target, parts: $parts}]' <<<"$publish")
  fi
done
echo "build=$build" >>"$GITHUB_OUTPUT"
echo "publish=$publish" >>"$GITHUB_OUTPUT"
echo "Build: $(jq -r 'map(.key) | join(", ")' <<<"$build")"
echo "Publish: $(jq -r 'map(.tag) | join(", ")' <<<"$publish")"
