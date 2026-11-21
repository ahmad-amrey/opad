# Checks and releases

OPAD has two long-lived branches. **`dev`** takes every change freely: merging into it runs no checks, so work flows
without waiting. A *development build* is made on demand: building, testing and install-checking every platform, and
published only when all of it passes. **`main`** holds the latest release: it only moves forward to such a fully tested
commit of `dev` (*Promote dev to main*, a fast-forward), and the version in `CMakeLists.txt` then decides whether that
publishes a release. Both run in GitHub Actions: `.github/workflows/ci.yml`
(the "Build" workflow) and `.github/workflows/promote.yml` ("Promote dev to main").

## One version, written once

The version lives in one place: `project(opad VERSION X.Y.Z ...)` in `CMakeLists.txt`. The program, the packages'
names, the installer, the release and its tag all take it from there. A version is *released* when it has a published
release; the workflow creates the release and its tag `vX.Y.Z` itself, on the commit whose push to `main` changed the
version. Nobody tags by hand in the normal course.

Every build also carries a **build number** (the Actions run number that built it) and the commit it was built from (a
pull request's head commit), shown in *About* and by `opad-cli version`. On `dev` every development build is newer than
the last: code that comes back exactly (a revert) is built again with a new number instead of reusing its old build. Development builds share their version with the release before them (or with the release
they will become), so the update check orders them by (version, build).

`CHANGELOG.md` moves with the version: its newest version section must be the version in `CMakeLists.txt`. Work merged
into `dev` between releases is written under `## [Unreleased]` at the top, which the checks ignore and which a
development build shows as its notes; a release turns it into `## [X.Y.Z] - YYYY-MM-DD`.

## What runs when

| Event | Version checks | Builds, unit tests, install tests | Publishes |
|---|---|---|---|
| Merge or push into `dev` | none | none | nothing |
| Actions > Build > Run workflow on `dev`, **Publish** ticked | yes | Windows, Linux, macOS (reused if this exact code was built already, below) | when everything passes: a **development build**, pre-release `vA.B.C-dev.<build>`, or the **release candidate** `vX.Y.Z-rc.<build>` when the version is new |
| *Promote dev to main* | `dev`'s commit passed that full run (`Tested`), its version is new, `main` is in `dev`'s history | none (it only moves `main`) | nothing by itself |
| Push to `main` (the promotion) | yes | none: the commit is `dev`'s, already built | the **release** `vX.Y.Z`, from the release candidate's very packages |
| Pull request into `main` (a hotfix only) | yes; "Merging this pull request publishes OPAD X.Y.Z" | Windows, Linux, macOS | nothing (the merge publishes) |
| Tag `vX.Y.Z` pushed by hand | the tag must equal the version and be on `main` | as for `main` | the release |
| Actions > Build > Run workflow, **Publish** not ticked (any branch) | yes | Windows, Linux, macOS ("rebuild" builds even code that was built before) | nothing |

The version checks, in seconds, before any build:

- the newest changelog section is the version in `CMakeLists.txt`;
- a version without a release is above the latest release and has its section, dated (`## [X.Y.Z] - YYYY-MM-DD`) and
  not empty (`sh tools/changelog_section.sh X.Y.Z` prints it and exits 0);
- a tag pushed by hand equals the version.

Each platform builds, runs the unit tests and makes its packages: a Windows installer (`installer/opad.iss`) and
single-file exe, a Debian package and an AppImage, a macOS dmg. The Windows installer is then installed silently,
installed again over itself (an update) and uninstalled, and the `.deb` is installed, reinstalled and removed with
apt; both check that the user's settings and data come out exactly as they went in. The dmg is mounted and checked for
its Applications link and for libraries from outside it. A build failure is annotated on the run's summary page (the
first `error:` lines of the build log).

**Tested** is the one check that sums them up: it passes when the version checks passed and all three platforms built
and passed (or this code was already built and passed, below); it fails when anything failed or was cancelled.

## Development builds and the release candidate

A development build is published by *Actions > Build > Run workflow* on `dev` with **Publish** ticked, only when every
platform builds and passes its tests and install checks. It is a GitHub **pre-release** tagged with a SemVer
pre-release version: `vA.B.C-dev.<build>`
for a development build (A.B.C is the next patch after the released version on `dev`) and `vX.Y.Z-rc.<build>` for the
release candidate of an unreleased X.Y.Z, so `0.10.0 < 0.10.1-dev.20 < 0.10.1-rc.25 < 0.10.1`. Only the tag carries the
label: the binaries say `X.Y.Z (build N)`, since a release candidate's binaries become the release unchanged. Each has
the same files (under the same names)
as a release, notes from `## [Unreleased]` and GitHub's list of the pull requests merged since the latest release, and a
marker line the update check reads (`<!-- opad-build: N; opad-version: X.Y.Z -->`). A pre-release is never the
"latest" release: the README's download links and copies on the default *Stable* channel never see it. Copies set to
*Help > Offer development builds* are told about each new one, like a release. The ten newest are kept.

When the version on `dev` is new (the "Release X.Y.Z" change was merged), the development build is titled
**OPAD X.Y.Z release candidate (build N)** and its notes are the version's changelog section. Promoting that commit
publishes those same packages as `vX.Y.Z`: what you tested is byte for byte what users get.

## Building each change once

Every run that passes everywhere records the git tree of the code it built (an artifact `tested-<tree>`). A later run
with exactly the same tree skips the builds and takes the packages from that earlier run: the push of a merged pull
request whose branch was up to date with its base, and the promotion of `dev` to `main`. Only runs of this workflow in
this repository count, and the tree includes `ci.yml`, so packages are never taken from a run whose workflow differed.
Packages are kept 30 days, so a release candidate can be tested for weeks before its promotion reuses it; after that the
code is built again.

Caches: the first run builds OCCT (Windows, Linux) and the simulation engines (macOS), about 15 to 40 minutes; the
Actions cache keeps them, and ccache keeps the compiled sources. A pull request reads the caches of its base branch,
and only a run on a branch can save that branch's caches. So the run that builds also uploads its dependencies and its
ccache as an artifact (`caches-<system>`, kept a week), and when a merge or a promotion reuses that build, the "Warm
cache" jobs on `dev` or `main` download them and save them under that branch: no build, about a minute. When there is no
hand-over (an older run, or the artifact expired), the jobs build just the dependencies the branch's cache lacks. If
builds get slow because a branch's ccache is old, run the workflow on that branch with "rebuild".

## Protecting `dev` and `main`

The rules live in the repository's settings, not in a file; set them once (owner or admin).

**`dev`**: *Settings > Rules > Rulesets > New branch ruleset*, name `dev`, *Enforcement status* Active, *Target
branches*: *Add target > Include by pattern* `dev` (no quotes). Leave the *Bypass list* empty. Tick only **Restrict
deletions** and **Block force pushes**: merges into `dev` need no pull request and no checks, and its history can
still never be rewritten. The checks bite where code reaches people: a published development build and `main`.

**`main`**: two rulesets, both targeting *Include default branch*, because a bypass lifts every rule of its ruleset:

- **`main-history`**: **Restrict deletions** and **Block force pushes**, with an empty *Bypass list*. Nobody, the deploy
  key included, can rewrite or delete `main`.
- **`main`**: **Require a pull request before merging** and **Require status checks to pass** (up to date, `Tested`),
  with **Deploy keys**, *Always allow*, on its *Bypass list*. That lets the promote workflow, which pushes with the
  deploy key below, move `main` forward without a pull request; everyone else, the owner included, still needs one.

Keep the promote key the only deploy key with write access in the repository.

### Promote setup (once)

The promotion pushes with a deploy key, because a push made with a workflow's own token would start no workflow, and
the release would never run. On your computer:

1. Make a key pair (no passphrase): `ssh-keygen -t ed25519 -N "" -C "opad promote" -f opad-promote` writes
   `opad-promote` (private) and `opad-promote.pub` (public).
2. *Settings > Deploy keys > Add deploy key*: title "Promote dev to main", paste `opad-promote.pub`, tick **Allow write
   access**.
3. *Settings > Environments > New environment*: name `promote`. Under *Deployment branches and tags* choose *Selected
   branches and tags* and add `main`. Then *Environment secrets > Add environment secret*: name `PROMOTE_DEPLOY_KEY`,
   value the whole of `opad-promote` (from `-----BEGIN` to `END ... KEY-----`). Only runs of `main`'s copy of the
   workflow can read it, so a changed copy on `dev` cannot.
4. Delete both files.
5. Add *Deploy keys* to the `main` ruleset's bypass list (above).

*Actions > Promote dev to main > Run workflow* with **Check only** ticked then tests everything but the push.

## Cutting a release

1. On a branch from `dev`, raise the version in `CMakeLists.txt` (`project(opad VERSION X.Y.Z ...)`) and turn
   `## [Unreleased]` in `CHANGELOG.md` into the version's section, dated:

   ```
   ## [X.Y.Z] - 2026-12-01

   One or two sentences on what this release is about.

   ### Added
   - ...
   ### Changed
   - ...
   ### Fixed
   - ...
   ```

   Write for the people using OPAD: what they can do now, what behaves differently, what to watch for. The opening
   sentences are what the update notice in OPAD shows. The headings under the version are free; only the
   `## [X.Y.Z] - date` line is read.
2. Merge it into `dev` (a pull request or a direct merge: `dev` asks for neither checks nor reviews).
3. *Actions > Build > Run workflow* on `dev` with **Publish** ticked. When every platform passes, the run publishes
   **OPAD X.Y.Z release candidate (build N)**. Install it (or let a copy set to *Offer development builds* offer it) and
   test it on your machines.
4. When it is good: *Actions > Promote dev to main > Run workflow*. `main` moves to that commit, and the run on `main`
   publishes `vX.Y.Z` with the release candidate's packages, the changelog section, a downloads table with first-start
   notes (`tools/release_downloads.md`), GitHub's list of the pull requests merged since the previous release,
   `SHA256SUMS.txt` and the third-party notices.

A fix found while testing the release candidate is merged into `dev` like any change; publish again (step 3) for the
next release candidate. Promote refuses a `dev` whose newest commit has not passed such a run. While the version is not
released, its entries go into its own section, not under `[Unreleased]`: the checks refuse a non-empty `[Unreleased]`
then, since those entries would be missing from the release.

The packages are published under names without the version (`OPAD-windows-x64-setup.exe`, `OPAD-windows-x64.exe`,
`opad-cli-windows-x64.exe`, `OPAD-macos.dmg`, `OPAD-linux-amd64.deb`, `OPAD-linux-x86_64.AppImage`): the README's
Download section links to `https://github.com/ahmad-amrey/opad/releases/latest/download/<name>`, which always reaches
the newest release, and OPAD's update check downloads `releases/download/<tag>/<name>` for the user's system. Renaming
a package means changing those links in README.md and README.ar.md and the names in `app/UpdateCheck.cpp` too.

## Hotfixes and recovery

**A fix that cannot wait for `dev`** (it holds unfinished work): branch from `main`, fix, raise the patch version with
its dated section, and open the pull request into `main`; merging it publishes the release. Then bring it into `dev`,
since the next promotion requires `dev` to contain `main`: a branch from `main`, merge `dev` into it (resolving any
conflict there), and a pull request from that branch into `dev` (it is then up to date with `dev`, as its ruleset
asks).

**Several promotions of one version**: only the push that changes `main`'s version publishes, so promote refuses when
`main` already has dev's version (an earlier promotion whose release run failed). Re-run the failed jobs of that run
instead; anything `dev` gained since goes out as the next version.

**A release run that fails after the promotion** (a flaky step): nothing is published and the version stays without a
release. Re-run the failed jobs of that run (a later push to `main` never publishes it, since it did not change the
version), or push the tag `vX.Y.Z` on that commit of `main` by hand. A tag left without a release stops the next
release's run with an error until it is deleted. Once a release has been downloaded, fix forward with a new patch
version instead of replacing it.

## Where OPAD keeps the user's data

Installed copies (the Windows installer, the `.deb`, the dmg) and the single-file exe keep settings and data in the
user's own folders, never beside the program (`app/AppData.hpp`): on Windows the registry (`HKCU\Software\opad\OPAD`)
and `%LOCALAPPDATA%\opad\OPAD` (templates in `%APPDATA%\opad\OPAD\templates`, cache `%LOCALAPPDATA%\opad\cache`), on Linux `~/.config/opad/OPAD.conf`,
`~/.local/share/opad/OPAD` and `~/.cache/opad`, on macOS `~/Library/Preferences/com.opad.OPAD.plist`,
`~/Library/Application Support/opad/OPAD` and `~/Library/Caches/opad`. Installing, updating and uninstalling never
touch them, and a new version (or development build) keeps a copy of the settings before it changes any
(`settings-backups` in the data folder). On Windows, a file named `opad.portable` beside the exe makes a copy portable:
everything then lives in a `data` folder beside it. Single-file builds before 0.9.0 kept everything in an `opad-data`
folder beside the exe; the first start of a newer one copies it into the user's folders (once, when they have no
settings yet) and leaves the old folder as it was.
