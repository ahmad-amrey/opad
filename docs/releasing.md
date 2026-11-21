# Checks and releases

Everything that reaches `main` has been built and tested on Windows, Linux and macOS, and the version in
`CMakeLists.txt` decides the releases: merging a pull request that raises it publishes that version. Both run in GitHub
Actions (`.github/workflows/ci.yml`, the "Build" workflow).

## One version, written once

The version lives in one place: `project(opad VERSION X.Y.Z ...)` in `CMakeLists.txt`. The program, the packages'
names, the installer, the release and its tag all take it from there. A version is *released* when it has a published
release; the workflow creates the release and its tag `vX.Y.Z` itself, on the commit whose push changed the version.
Nobody tags by hand in the normal course.

`CHANGELOG.md` moves with it: its newest version section must be the version in `CMakeLists.txt`. Work merged between
releases is written under `## [Unreleased]` at the top, which the checks ignore; a release turns it into `## [X.Y.Z] -
YYYY-MM-DD`.

## What runs when

| Event | Version checks | Builds, unit tests, install tests | Release |
|---|---|---|---|
| Pull request to `main` | yes; says "Merging this pull request publishes OPAD X.Y.Z" when the version is new | Windows, Linux, macOS | no |
| Push to `main` (a merged pull request) | yes | Windows, Linux, macOS, unless this exact code was already built (below) | yes, when this push changed the version to one not released yet |
| Tag `vX.Y.Z` pushed by hand | the tag must equal the version and be on `main` | as for `main` | yes |
| Actions > Build > Run workflow | yes | Windows, Linux, macOS ("rebuild" builds even code that was built before) | no |

The version checks, in seconds, before any build:

- the newest changelog section is the version in `CMakeLists.txt`;
- a version without a tag is above the latest release and has its section, dated (`## [X.Y.Z] - YYYY-MM-DD`) and not
  empty (`sh tools/changelog_section.sh X.Y.Z` prints it and exits 0);
- a tag pushed by hand equals the version.

Each platform builds, runs the unit tests and makes its packages: a Windows installer (`installer/opad.iss`) and
single-file exe, a Debian package and an AppImage, a macOS dmg. The Windows installer is then installed silently,
installed again over itself (an update) and uninstalled, and the `.deb` is installed, reinstalled and removed with
apt; both check that the user's settings and data come out exactly as they went in. A build failure is annotated
on the run's summary page (the first `error:` lines of the build log).

**Tested** is the one check that sums them up: it passes when the version checks passed and all three platforms built
and passed (or this code was already built and passed, below); it fails when anything failed or was cancelled.

## Building each change once

Every run that passes everywhere records the git tree of the code it built (an artifact `tested-<tree>`). A later run
with exactly the same tree skips the builds and takes the packages from that earlier run: typically the push of a merged
pull request whose branch was up to date with `main`, which then publishes its release from the packages the pull
request's run made and tested. Only runs of this workflow in this repository count, and the tree includes `ci.yml`, so
packages are never taken from a run whose workflow differed. Packages are kept 7 days (30 for a tag's run); after that
the code is built again.

Caches: the first run builds OCCT (Windows, Linux) and the simulation engines (macOS), about 15 to 40 minutes; the
Actions cache keeps them, and ccache keeps the compiled sources. Caches saved by runs on `main` are the ones every pull
request can read, and only a run on `main` can save them. So a pull request's run also uploads its dependencies and
its ccache as an artifact (`caches-<system>`, kept as long as the packages), and when the merge reuses that run's build,
the "Warm cache" jobs on `main` download them and save them under `main`: no build, and the next pull request starts
with warm caches. The branch was up to date with `main` when it merged (the ruleset requires it), so the cache keys are
the ones `main` needs. When there is no hand-over (an older run, or the artifact expired), the jobs build just the
dependencies `main`'s cache lacks. If pull request builds get slow because `main`'s ccache is old, run the workflow on
`main` with "rebuild".

## Protecting `main` and the tags

The rules live in the repository's settings, not in a file; set them once (owner or admin):

1. **Settings > Rules > Rulesets > New ruleset > New branch ruleset.** Name it `main`, *Enforcement status*: Active.
2. **Target branches:** *Add target > Include default branch*.
3. **Bypass list:** leave it empty, so the rules hold for the owner too.
4. Tick:
   - **Restrict deletions** and **Block force pushes**.
   - **Require a pull request before merging.** Required approvals can stay 0 when you work alone (GitHub does not let
     you approve your own pull request); the point is that nothing reaches `main` without one.
   - **Require status checks to pass**, with **Require branches to be up to date before merging** (the checks then ran
     on what the merge will be, and the merge can reuse that build). *Add checks*: `Tested` only. It is offered there
     once it has run at least once.
5. **Create.**

Optionally, a tag ruleset so that only the workflow publishes: **New tag ruleset**, target `v*`, tick **Restrict
creations**, **Restrict updates** and **Restrict deletions**, and add **GitHub Actions** to the *bypass list*, or the
Release job cannot create the tag. Add the **Repository admin** role there too if you want to keep the hand-pushed tag
below as a way to recover a failed release.

## Cutting a release

1. On a branch, raise the version in `CMakeLists.txt`: `project(opad VERSION X.Y.Z ...)`.
2. In `CHANGELOG.md`, turn `## [Unreleased]` into the version's section, dated the day you will merge:

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

   Write for the people using OPAD: what they can do now, what behaves differently, what to watch for. The opening
   sentences are what the update notice in OPAD shows. The headings under the version are free; only the
   `## [X.Y.Z] - date` line is read.
3. Open the pull request. Its Version check says "Merging this pull request publishes OPAD X.Y.Z".
4. Merge it once **Tested** passes. The run on `main` creates the tag `vX.Y.Z` on the merge commit and publishes the
   release: the changelog section, a downloads table with first-start notes (`tools/release_downloads.md`), GitHub's
   list of the merged pull requests, and the packages with `SHA256SUMS.txt` and the third-party notices.

The packages are published under names without the version (`OPAD-windows-x64-setup.exe`, `OPAD-windows-x64.exe`,
`opad-cli-windows-x64.exe`, `OPAD-macos.dmg`, `OPAD-linux-amd64.deb`, `OPAD-linux-x86_64.AppImage`): the README's
Download section links to `https://github.com/ahmad-amrey/opad/releases/latest/download/<name>`, which always reaches
the newest release, and OPAD's update check downloads `releases/download/vX.Y.Z/<name>` for the user's system. Renaming
a package means changing those links in README.md and README.ar.md and the names in `app/UpdateCheck.cpp` too.

If a release run fails after the merge (a flaky step), nothing is published and the version stays without a release:
re-run the failed jobs of that run (a later push to `main` never publishes it, since it did not change the version), or
push the tag `vX.Y.Z` on that commit of `main` by hand (with a tag ruleset, only someone on its bypass list can). A tag
left without a release stops the next release's run with an error until it is deleted. Once a release has been
downloaded, fix forward with a new patch version instead of replacing it.

## Where OPAD keeps the user's data

Installed copies (the Windows installer, the `.deb`, the dmg) and the single-file exe keep settings and data in the
user's own folders, never beside the program (`app/AppData.hpp`): on Windows the registry (`HKCU\Software\opad\OPAD`)
and `%LOCALAPPDATA%\opad\OPAD` (templates in `%APPDATA%\opad\OPAD\templates`, cache `%LOCALAPPDATA%\opad\cache`), on Linux `~/.config/opad/OPAD.conf`,
`~/.local/share/opad/OPAD` and `~/.cache/opad`, on macOS `~/Library/Preferences/com.opad.OPAD.plist`,
`~/Library/Application Support/opad/OPAD` and `~/Library/Caches/opad`. Installing, updating and uninstalling never
touch them, and a new version keeps a copy of the settings before it changes any (`settings-backups` in the data
folder). On Windows, a file named `opad.portable` beside the exe makes a copy portable: everything then lives in a
`data` folder beside it. Single-file builds before 0.9.0 kept everything in an `opad-data` folder beside the exe; the first start of
a newer one copies it into the user's folders (once, when they have no settings yet) and leaves the old folder as it was.
