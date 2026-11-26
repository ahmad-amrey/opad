#!/usr/bin/env bash
# engines.yml's plan: which packs (pack x system) this run builds, and which it publishes and from where.
#
# A pull request's run (or a hand run without "publish") builds and checks every pack it is asked for, publishing nothing.
# On dev (a push touching engines/**, or a hand run with "publish"), a pack is published when its release tag
# engines-<id>-<version>-<rev>-<platform> does not exist yet: from the artifacts of an earlier engines.yml run that built
# exactly this code (the pull request's, when it was up to date with dev: same git tree), else built in this run. A
# published tag is never replaced: a change to a pack is a new revision (PACK_REV in its make-pack.sh).
#
# Environment: GH_TOKEN, GITHUB_REPOSITORY, GITHUB_REF, GITHUB_OUTPUT; PACKS (both, openfoam, calculix); PUBLISH (true or
# false). Runs in the checkout of the commit being built. Writes build=<json list> and publish=<json list> to GITHUB_OUTPUT.
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
    tag="engines-$id-$version-$rev-$platform"
    unit=$(jq -nc --arg id "$id" --arg platform "$platform" --arg runner "$runner" --arg tag "$tag" \
      --arg title "$label for OPAD, $system (pack revision $rev)" \
      '{id: $id, platform: $platform, key: "\($id)-\($platform)", runner: $runner, tag: $tag, artifact: "engine-\($id)-\($platform)", title: $title}')
    if [ "$publishing" = false ]; then
      build=$(jq -c --argjson u "$unit" '. + [$u]' <<<"$build")
      continue
    fi
    if gh release view "$tag" --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
      echo "$tag: published already."
      continue
    fi
    if [ -n "$(git ls-remote --tags origin "refs/tags/$tag")" ]; then
      echo "::error::The tag $tag exists without a release: delete it (git push origin :refs/tags/$tag) and run again."
      exit 1
    fi
    # An earlier run of this workflow, in this repository, that built this very code: its pack is the one to publish, its
    # commit the tag's target (the pull request's head, which dev's merge has as a parent).
    run=
    target=$commit
    while read -r rid head_sha; do
      [ -n "$rid" ] || continue
      [ "$(gh api "repos/$GITHUB_REPOSITORY/actions/runs/$rid" --jq .path)" = .github/workflows/engines.yml ] || continue
      if [ "$(gh api "repos/$GITHUB_REPOSITORY/git/commits/$head_sha" --jq .tree.sha)" != "$tree" ]; then
        echo "$tag: run $rid built other code ($head_sha)."
        continue
      fi
      run=$rid target=$head_sha
      break
    done < <(gh api --paginate "repos/$GITHUB_REPOSITORY/actions/artifacts?name=engine-$id-$platform&per_page=100" \
      --jq '.artifacts[] | select((.expired | not) and .workflow_run.head_repository_id == .workflow_run.repository_id)
            | "\(.workflow_run.id) \(.workflow_run.head_sha)"' | sort -u -n -r)
    if [ -n "$run" ]; then
      echo "$tag: publishing the pack of run $run (commit $target, the same code)."
    else
      echo "$tag: no earlier build of this code; building it."
      build=$(jq -c --argjson u "$unit" '. + [$u]' <<<"$build")
    fi
    publish=$(jq -c --argjson u "$unit" --arg run "$run" --arg target "$target" '. + [$u + {run: $run, target: $target}]' <<<"$publish")
  done
done
echo "build=$build" >>"$GITHUB_OUTPUT"
echo "publish=$publish" >>"$GITHUB_OUTPUT"
echo "Build: $(jq -r 'map(.key) | join(", ")' <<<"$build")"
echo "Publish: $(jq -r 'map(.tag) | join(", ")' <<<"$publish")"
