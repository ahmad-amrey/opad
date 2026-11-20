# Checks and releases

Everything that reaches `main` has been built and tested on Windows, Linux and macOS, and a release is published only
when a version tag is pushed by hand. Both run in GitHub Actions (`.github/workflows/ci.yml`).

## What runs when

| Event | Builds and unit tests | Packages | Release |
|---|---|---|---|
| Pull request to `main` | Windows, Linux, macOS | kept 7 days on the run's page | no |
| Push to `main` (a merged pull request) | Windows, Linux, macOS | kept 7 days | no |
| Tag `vX.Y.Z` | Windows, Linux, macOS | kept 30 days | yes, published |
| Actions > Build > Run workflow | Windows, Linux, macOS (tests optional) | kept 7 days | no |

The packages: a Windows installer (`installer/opad.iss`, per user, keeps the user's settings) and single-file exe, a
macOS dmg and a Linux AppImage. The first run builds OCCT on each machine (about 15 to 40 minutes); the Actions cache
keeps it and the compiled sources (ccache) for the runs after it. Caches saved by `main` runs are the ones pull requests
and tags can use, and one unused for 7 days is dropped.

## Protecting `main`

The rules live in the repository's settings, not in a file; set them once (owner or admin):

1. **Settings > Rules > Rulesets > New ruleset > New branch ruleset.** Name it `main`, *Enforcement status*: Active.
2. **Target branches:** *Add target > Include default branch*.
3. **Bypass list:** leave it empty, so the rules hold for the owner too.
4. Tick:
   - **Restrict deletions** and **Block force pushes**.
   - **Require a pull request before merging.** Required approvals can stay 0 when you work alone (GitHub does not let
     you approve your own pull request); the point is that nothing reaches `main` without one.
   - **Require status checks to pass**, with **Require branches to be up to date before merging** (the checks then ran
     on what the merge will be). *Add checks*: `Version`, `Windows`, `Linux`, `macOS`. A check is offered there once it
     has run at least once, so open a first pull request with the workflow before this step.
5. **Create.**

From then on a push straight to `main` is refused; changes go through a branch and a pull request whose four checks
pass. Tags are not affected: pushing `vX.Y.Z` stays possible.

## Cutting a release

1. On a branch, set the version in `CMakeLists.txt`: `project(opad VERSION X.Y.Z ...)`.
2. In `CHANGELOG.md`, add the section at the top (or turn the version's unreleased section into it):

   ```
   ## [X.Y.Z] - 2026-12-01

   One or two sentences on what this release is about.

   ### Added
   - ...
   ### Changed
   - ...
   ### Fixed
   - ...
   ### Notes
   - ...
   ```

   Write for the people using OPAD: what they can do now, what behaves differently, what to watch for. The headings
   under the version are free; only the `## [X.Y.Z] - date` line is read. Check it: `sh tools/changelog_section.sh
   X.Y.Z` prints the notes and exits 0.
3. Commit both in one pull request; merge it once its checks pass.
4. Tag the merged commit on `main` and push the tag:

   ```
   git switch main && git pull
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

5. The Build workflow checks that the tag matches the version and that the changelog has its section (else it stops in
   seconds, before any build), builds and tests the three platforms, and publishes the release: the changelog section, a
   downloads table with first-start notes (`tools/release_downloads.md`), GitHub's list of the merged pull requests, and
   the packages with `SHA256SUMS.txt` and the third-party notices.

The packages are published under names without the version (`OPAD-windows-x64-setup.exe`, `OPAD-windows-x64.exe`,
`opad-cli-windows-x64.exe`, `OPAD-macos.dmg`, `OPAD-linux-x86_64.AppImage`): the README's Download section links to
`https://github.com/ahmad-amrey/opad/releases/latest/download/<name>`, which always reaches the newest published
release. Renaming a package means changing those links in README.md and README.ar.md too.

A failed release run publishes nothing. To try again with the same version: delete the tag (`git push origin
:refs/tags/vX.Y.Z` and `git tag -d vX.Y.Z`), fix, tag again. Once a release has been downloaded, fix forward with a new
patch version instead of replacing it.
