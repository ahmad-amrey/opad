#!/usr/bin/env python3
"""Watch OPAD's pinned third-party code for new upstream releases and known vulnerabilities.

The dependencies and where each is pinned are listed in tools/deps.json; the versions themselves are read from the pin
sites (recipes, CMake files, the submodule), so there is one source. For each one the newest upstream release is looked
up (GitHub / GitLab release APIs, git tags, or the download page) and OSV.dev is asked for advisories against the pinned
commit (and the update's, to tell which advisories an update fixes). Only public upstream data is fetched; nothing about
the user or the machine is sent.

  python3 tools/deps_watch.py            dry run: print the report (Markdown) and touch nothing
  python3 tools/deps_watch.py --check    offline: every pin is found and parses (ctest deps-pins)
  python3 tools/deps_watch.py --issue    (workflow deps-watch.yml) open / update / close the issue "Dependency updates"
                                         in $GITHUB_REPOSITORY with $GITHUB_TOKEN
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tools" / "deps.json"
TITLE = "Dependency updates"
MARKER = "<!-- deps-watch"
AGENT = "opad-deps-watch"
TIMEOUT = 30
MAX_LISTED = 40  # advisories listed per dependency in the issue


class LookupError_(Exception):
    pass


# ---------------------------------------------------------------- versions

PRERELEASE = re.compile(r"(rc|beta|alpha|dev|pre|snapshot|nightly)", re.I)


def vkey(text):
    """'V7_9_2' -> (7, 9, 2); 'v2512' -> (2512,); 'V8_0_0_p1' -> (8, 0, 0, 1)."""
    return tuple(int(n) for n in re.findall(r"\d+", text))


def vlabel(tag):
    """A release's tag as a version: 'V8_0_2' -> '8.0.2', 'v1.4' -> '1.4' (OpenFOAM keeps its 'v2512')."""
    if re.fullmatch(r"v\d{4}", tag):
        return tag
    return re.sub(r"^[A-Za-z_-]+", "", tag).replace("_", ".")


def tag_for(template, version):
    return template.replace("{v_}", version.replace(".", "_")).replace("{v}", version)


# ---------------------------------------------------------------- network

def request(url, data=None, method=None, accept="application/json", token=None):
    headers = {"User-Agent": AGENT, "Accept": accept}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    body = None
    if data is not None:
        body = json.dumps(data).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        raise LookupError_(f"{url}: HTTP {e.code}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise LookupError_(f"{url}: {getattr(e, 'reason', e)}") from None


def get_json(url):
    token = os.environ.get("GITHUB_TOKEN") if urllib.parse.urlsplit(url).hostname == "api.github.com" else None
    return json.loads(request(url, token=token)[1])


def get_text(url):
    return request(url, accept="text/html, */*")[1].decode("utf-8", "replace")


def exists(url):
    try:
        return request(url, method="HEAD", accept="*/*")[0] < 400
    except LookupError_:
        return False


_tags = {}


def remote_tags(url):
    """{tag: commit} of a git remote, annotated tags peeled."""
    if url not in _tags:
        try:
            out = subprocess.run(["git", "ls-remote", "--tags", url], capture_output=True, text=True, timeout=120,
                                 env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
        except (OSError, subprocess.TimeoutExpired) as e:
            raise LookupError_(f"git ls-remote {url}: {e}") from None
        if out.returncode != 0:
            raise LookupError_(f"git ls-remote {url}: {out.stderr.strip()[:200]}")
        tags = {}
        for line in out.stdout.splitlines():
            sha, ref = line.split("\t")
            name = ref.removeprefix("refs/tags/")
            if name.endswith("^{}"):
                tags[name[:-3]] = sha
            else:
                tags.setdefault(name, sha)
        _tags[url] = tags
    return _tags[url]


def osv(commit):
    """{id: summary} of the OSV advisories that affect a commit."""
    found, token = {}, None
    while True:
        query = {"commit": commit}
        if token:
            query["page_token"] = token
        d = json.loads(request("https://api.osv.dev/v1/query", data=query, method="POST")[1])
        for v in d.get("vulns", []):
            cves = [a for a in v.get("aliases", []) if a.startswith("CVE-")]
            summary = v.get("summary") or (v.get("details") or "").split("\n")[0]
            found[v["id"]] = (summary[:140] + (f" ({', '.join(cves)})" if cves and v["id"] not in cves else "")).strip()
        token = d.get("next_page_token")
        if not token:
            return found


# ---------------------------------------------------------------- pins

def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def read_pin(dep):
    """{'version', 'commit', 'file', 'line'} of the pinned version (version None for a submodule until tags are read)."""
    if "submodule" in dep:
        out = subprocess.run(["git", "-C", str(ROOT), "ls-tree", "HEAD", dep["submodule"]], capture_output=True, text=True)
        m = re.match(r"160000 commit ([0-9a-f]{40})\t", out.stdout)
        if not m:
            raise LookupError_(f"{dep['submodule']} is not a submodule in HEAD")
        return {"version": None, "commit": m.group(1), "file": ".gitmodules", "line": None, "path": dep["submodule"]}
    text = (ROOT / dep["file"]).read_text(encoding="utf-8")
    m = re.search(dep["version"], text, re.M)
    if not m:
        raise LookupError_(f"no match for {dep['version']!r} in {dep['file']}")
    pin = {"version": m.group(1), "commit": None, "file": dep["file"], "line": line_of(text, m.start())}
    if "commit" in dep:
        c = re.search(dep["commit"], text, re.M)
        if not c:
            raise LookupError_(f"no match for {dep['commit']!r} in {dep['file']}")
        pin["commit"] = c.group(1)
    if not vkey(pin["version"]):
        raise LookupError_(f"{dep['name']}: pinned version {pin['version']!r} has no number")
    return pin


# ---------------------------------------------------------------- upstream

def releases(dep):
    """(stable, snapshots): lists of (label, tag) found upstream, any order."""
    up = dep["upstream"]
    kind = up["kind"]
    stable, snaps = [], []
    if kind == "github-release":
        listed = get_json(f"https://api.github.com/repos/{up['repo']}/releases?per_page=100")
        try:  # the stable release can be older than the last 100 snapshots (LibreDWG's nightlies)
            latest = get_json(f"https://api.github.com/repos/{up['repo']}/releases/latest")
            if all(r["tag_name"] != latest["tag_name"] for r in listed):
                listed.append(latest)
        except LookupError_:
            pass  # no release marked latest
        for r in listed:
            if r.get("draft"):
                continue
            tag = r["tag_name"]
            pre = r.get("prerelease") or PRERELEASE.search(tag)
            if not pre and "tag_pattern" in dep and not re.match(dep["tag_pattern"], tag):
                pre = True
            (snaps if pre else stable).append((vlabel(tag), tag))
    elif kind == "gitlab-release":
        project = urllib.parse.quote(up["project"], safe="")
        for r in get_json(f"https://{up['host']}/api/v4/projects/{project}/releases?per_page=100"):
            tag = r["tag_name"]
            (snaps if r.get("upcoming_release") or PRERELEASE.search(tag) else stable).append((vlabel(tag), tag))
    elif kind == "git-tags":
        for tag in remote_tags(dep["git"]):
            m = re.match(up["pattern"], tag)
            if m:
                stable.append((m.group(1), tag))
    elif kind == "listing":
        found = {m for m in re.findall(up["pattern"], get_text(up["url"]))}
        ordered = sorted(found, key=vkey, reverse=True)
        if "exists" in up:  # a folder can show before its archive: take the newest whose archive is there
            for i, v in enumerate(ordered[:3]):
                if exists(up["exists"].replace("{v}", v)):
                    ordered = ordered[i:]
                    break
        stable = [(v, None) for v in ordered]
    elif kind != "frozen":
        raise LookupError_(f"unknown upstream kind {kind!r}")
    return stable, snaps


def check(dep, acknowledged):
    r = {"name": dep["name"], "note": dep.get("note", ""), "page": dep.get("page"), "errors": [],
         "advisories": {}, "covered": "git" in dep}
    try:
        pin = read_pin(dep)
    except LookupError_ as e:
        r["errors"].append(str(e))
        return r
    r["pin"] = pin
    tags = {}
    if "git" in dep:
        try:
            tags = remote_tags(dep["git"])
        except LookupError_ as e:
            r["errors"].append(str(e))
    if pin["version"] is None:  # a submodule: its version is the stable tag on its commit
        on = [t for t, c in tags.items() if c == pin["commit"]]
        named = [t for t in on if re.match(dep.get("tag_pattern", ".*"), t)] or on
        pin["version"] = vlabel(sorted(named, key=vkey)[0]) if named else pin["commit"][:10]
    if not pin["commit"] and tags:
        pin["commit"] = tags.get(tag_for(dep.get("tag", "{v}"), pin["version"]))
        if not pin["commit"]:
            r["errors"].append(f"no tag {tag_for(dep.get('tag', '{v}'), pin['version'])} upstream for the pinned version")

    up = dep["upstream"]
    if up["kind"] == "frozen":
        r["frozen"] = up.get("why", "no new releases")
    else:
        try:
            stable, snaps = releases(dep)
        except LookupError_ as e:
            r["errors"].append(str(e))
            stable, snaps = [], []
        if stable:
            stable.sort(key=lambda x: vkey(x[0]))
            newest = stable[-1]
            target = newest
            if dep.get("hold"):
                held = [s for s in stable if s[0].startswith(dep["hold"])]
                target = held[-1] if held else None
                if newest is not target:
                    r["held"] = (newest[0], dep.get("hold_why", ""))
            series = vkey(pin["version"])[:2]
            same = [s for s in stable if vkey(s[0])[:2] == series and len(series) > 1]
            if target and same and same[-1] is not target and vkey(same[-1][0]) > vkey(pin["version"]):
                r["series"] = same[-1][0]
                if same[-1][1] and tags:
                    r["series_commit"] = tags.get(same[-1][1])
            if target:
                r["target"] = target[0]
                r["update"] = vkey(target[0]) > vkey(pin["version"])
                if r["update"] and target[1] and tags:
                    r["target_commit"] = tags.get(target[1])
        elif not r["errors"]:
            r["errors"].append("no release found upstream")
        if up.get("snapshots") and snaps:
            snaps.sort(key=lambda x: vkey(x[0]))
            if vkey(snaps[-1][0]) > vkey(pin["version"]):
                r["snapshot"] = snaps[-1][0]
                r["snapshot_commit"] = tags.get(snaps[-1][1])

    if "git" in dep and pin.get("commit"):
        try:
            found = osv(pin["commit"])
            fixed_by = {}
            # the smallest step that fixes each: the pinned series' newest, the update, a snapshot
            for key, label in (("series_commit", r.get("series")), ("target_commit", r.get("target")),
                               ("snapshot_commit", r.get("snapshot"))):
                if found and r.get(key):
                    later = osv(r[key])
                    for a in found:
                        if a not in later:
                            fixed_by.setdefault(a, label)
            for a, summary in sorted(found.items()):
                r["advisories"][a] = {"summary": summary, "fixed_in": fixed_by.get(a),
                                      "acknowledged": acknowledged.get(a)}
        except LookupError_ as e:
            r["errors"].append(f"OSV: {e}")
    return r


# ---------------------------------------------------------------- report

def head_sha():
    """The commit whose pins were read (the workflow checks out dev, not the commit that started it)."""
    out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else os.environ.get("GITHUB_SHA")


def pin_link(pin):
    where = pin.get("path") or pin["file"]
    label = f"{where}:{pin['line']}" if pin.get("line") else where
    repo, sha = os.environ.get("GITHUB_REPOSITORY"), head_sha()
    if repo and sha:
        server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
        anchor = f"#L{pin['line']}" if pin.get("line") else ""
        return f"[`{label}`]({server}/{repo}/blob/{sha}/{where}{anchor})"
    return f"`{label}`"


def attention(r):
    """Keys of what wants a look: an update, an advisory not acknowledged."""
    keys = []
    if r.get("update"):
        keys.append(f"update:{r['name']}:{r['target']}")
    keys += [f"advisory:{r['name']}:{a}" for a, v in r["advisories"].items() if not v["acknowledged"]]
    return keys


def describe(key):
    _, name, what = key.split(":", 2)
    return f"{name} {what}" if key.startswith("update:") else f"{name}: advisory {what}"


def os_packages(manifest):
    lines = []
    for src in manifest.get("os_packages", {}).get("sources", []):
        try:
            text = (ROOT / src["file"]).read_text(encoding="utf-8")
        except OSError:
            continue
        names = []
        if src["key"] == "apt-get install":
            for m in re.finditer(r"apt-get install ([^\n&|;]+)", text):
                names += [w for w in m.group(1).split() if not w.startswith("-")]
        else:
            m = re.search(rf"^(\s*){re.escape(src['key'])}:[ \t]*(.*)$", text, re.M)
            if m:
                value = m.group(2).strip()
                if value in (">-", ">", "|", "|-", ""):
                    indent, value = len(m.group(1)), ""
                    for line in text[m.end() + 1:].splitlines():
                        if line.strip() and len(line) - len(line.lstrip()) <= indent:
                            break
                        value += " " + line.strip()
                names = value.split()
        names = [re.sub(r"^mingw-w64-(ucrt-)?x86_64-", "", n) for n in names if not n.startswith("$")]
        if names:
            lines.append(f"- **{src['label']}** (`{src['file']}` `{src['key']}`): {', '.join(dict.fromkeys(names))}")
    return lines


def report(results, manifest):
    keys = sorted(k for r in results for k in attention(r))
    when = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    out = [f"{MARKER} {json.dumps({'keys': keys})} -->",
           f"Checked {when} by `tools/deps_watch.py` (workflow `deps-watch.yml`, weekly). What to do with an entry: "
           "`docs/releasing.md`, *Dependency updates*. This issue closes itself when everything is current.", "",
           "| Dependency | Pinned | Newest | Advisories (OSV) | Pinned in |", "|---|---|---|---|---|"]
    for r in results:
        pin = r.get("pin")
        if not pin:
            out.append(f"| {r['name']} | ? | ? | ? | lookup failed: {r['errors'][0]} |")
            continue
        name = f"[{r['name']}]({r['page']})" if r.get("page") else r["name"]
        if r.get("update"):
            name = f"**{name}**"
        if "frozen" in r:
            newest = f"{pin['version']} ({r['frozen']})"
        elif "target" in r:
            newest = f"**{r['target']}**" if r.get("update") else f"{r['target']} (current)"
            if "series" in r:
                newest += f"; {r['series']} in the pinned series"
            if "held" in r:
                newest += f"; {r['held'][0]} held back: {r['held'][1]}"
            if "snapshot" in r:
                newest += f"; snapshot {r['snapshot']}"
        else:
            newest = "?"
        adv = r["advisories"]
        if not r["covered"]:
            advisories = "not in OSV"
        elif not adv:
            advisories = "none known" if not any("OSV" in e for e in r["errors"]) else "?"
        else:
            open_ = [a for a in adv.values() if not a["acknowledged"]]
            fixed = sum(1 for a in open_ if a["fixed_in"])
            advisories = f"**{len(open_)}**" if open_ else "0"
            parts = []
            if fixed:
                parts.append(f"{fixed} fixed upstream")
            if len(adv) > len(open_):
                parts.append(f"{len(adv) - len(open_)} acknowledged")
            if parts:
                advisories += f" ({', '.join(parts)})"
        errors = "".join(f"<br>⚠ {e}" for e in r["errors"])
        out.append(f"| {name} | {pin['version']} | {newest} | {advisories} | {pin_link(pin)}{errors} |")

    listed = [(r, a, v) for r in results for a, v in r["advisories"].items()]
    if listed:
        out += ["", "### Advisories against the pinned versions", ""]
        for r in results:
            items = sorted(r["advisories"].items(), key=lambda kv: (bool(kv[1]["acknowledged"]), kv[0]))
            if not items:
                continue
            out.append(f"**{r['name']} {r['pin']['version']}**")
            for a, v in items[:MAX_LISTED]:
                state = (f"acknowledged: {v['acknowledged']}" if v["acknowledged"] else
                         f"fixed in {v['fixed_in']}" if v["fixed_in"] else "not fixed in a newer release yet")
                out.append(f"- [{a}](https://osv.dev/vulnerability/{a}) {v['summary']} — {state}")
            if len(items) > MAX_LISTED:
                out.append(f"- ... and {len(items) - MAX_LISTED} more (osv.dev, by commit {r['pin']['commit']})")
            out.append("")
        out.append("An advisory looked at and accepted (not reachable in OPAD's use, or no fix yet) goes into "
                   "`acknowledged` in `tools/deps.json` with the reason; it then stops keeping this issue open.")

    pkgs = os_packages(manifest)
    if pkgs:
        out += ["", "<details><summary>OS packages (not pinned, so not in the table)</summary>", "",
                manifest["os_packages"].get("about", ""), ""] + pkgs + ["", "</details>"]
    return "\n".join(out) + "\n", keys


# ---------------------------------------------------------------- the issue

def github(method, path, data=None):
    repo = os.environ["GITHUB_REPOSITORY"]
    api = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    status, body = request(f"{api}/repos/{repo}{path}", data=data, method=method,
                           accept="application/vnd.github+json", token=os.environ["GITHUB_TOKEN"])
    return json.loads(body) if body else None


def find_issue():
    for page in range(1, 6):
        issues = github("GET", f"/issues?state=open&per_page=100&page={page}")
        for i in issues:
            if "pull_request" not in i and i["title"] == TITLE and (i.get("body") or "").startswith(MARKER):
                return i
        if len(issues) < 100:
            return None
    return None


def old_keys(body):
    m = re.match(re.escape(MARKER) + r" (\{.*?\}) -->", body or "")
    try:
        return set(json.loads(m.group(1))["keys"]) if m else set()
    except (ValueError, KeyError):
        return set()


def sync_issue(body, keys, clean):
    issue = find_issue()
    if not keys:
        if issue and clean:
            github("POST", f"/issues/{issue['number']}/comments",
                   {"body": "Every pinned dependency is current and no advisory is open: closing."})
            github("PATCH", f"/issues/{issue['number']}", {"body": body, "state": "closed", "state_reason": "completed"})
            print(f"closed #{issue['number']}")
        elif issue:
            github("PATCH", f"/issues/{issue['number']}", {"body": body})
            print(f"#{issue['number']} kept open: some lookups failed")
        else:
            print("nothing to report")
        return
    if not issue:
        created = github("POST", "/issues", {"title": TITLE, "body": body})
        print(f"opened #{created['number']}")
        return
    new = [k for k in keys if k not in old_keys(issue.get("body"))]
    github("PATCH", f"/issues/{issue['number']}", {"body": body})
    if new:  # a body edit notifies nobody: say what is new
        github("POST", f"/issues/{issue['number']}/comments",
               {"body": "New since the last check:\n" + "\n".join(f"- {describe(k)}" for k in new)})
    print(f"updated #{issue['number']}" + (f" ({len(new)} new)" if new else ""))


# ---------------------------------------------------------------- self-check (offline)

def self_check(manifest):
    assert vkey("V7_9_2") == (7, 9, 2) and vkey("v2512") == (2512,) and vkey("V8_0_0_p1") > vkey("8.0.0")
    assert vkey("10.0.0") > vkey("9.0.1") and vkey("1.4") > vkey("1.3.1") and vkey("2.24") > vkey("2.22")
    assert vlabel("V8_0_2") == "8.0.2" and vlabel("V8.0.1") == "8.0.1" and vlabel("v1.4") == "1.4"
    assert vlabel("v2606") == "v2606" and vlabel("v6.2.2608") == "6.2.2608" and vlabel("0.14") == "0.14"
    assert tag_for("V{v_}", "7.9.2") == "V7_9_2" and tag_for("v{v}", "1.3.1") == "v1.3.1"
    assert old_keys(f'{MARKER} {{"keys": ["update:a:1"]}} -->\nx') == {"update:a:1"}
    assert describe("advisory:LibreDWG:CVE-1") == "LibreDWG: advisory CVE-1"
    kinds = {"github-release", "gitlab-release", "git-tags", "listing", "frozen"}
    failed = 0
    for dep in manifest["pinned"]:
        try:
            if dep["upstream"]["kind"] not in kinds:
                raise LookupError_(f"unknown upstream kind {dep['upstream']['kind']!r}")
            if dep["upstream"]["kind"] in ("git-tags",) and "git" not in dep:
                raise LookupError_("git-tags needs 'git'")
            pin = read_pin(dep)
            print(f"ok   {dep['name']}: {pin['version'] or pin['commit']} ({pin.get('path') or pin['file']})")
        except (LookupError_, OSError, KeyError) as e:
            print(f"FAIL {dep['name']}: {e}")
            failed += 1
    if not os_packages(manifest):
        print("FAIL no OS package list found")
        failed += 1
    return failed


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="offline: the pins are found (no network)")
    ap.add_argument("--issue", action="store_true", help="open / update / close the GitHub issue")
    ap.add_argument("--json", action="store_true", help="print the raw results as JSON")
    args = ap.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if args.check:
        return 1 if self_check(manifest) else 0

    acknowledged = manifest.get("acknowledged", {})
    results = [check(dep, acknowledged) for dep in manifest["pinned"]]
    body, keys = report(results, manifest)
    clean = not any(r["errors"] for r in results)
    for r in results:
        for e in r["errors"]:
            print(f"::warning::{r['name']}: {e}", file=sys.stderr)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write(body)
    if args.json:
        print(json.dumps(results, indent=1, default=str))
    elif args.issue:
        if not os.environ.get("GITHUB_TOKEN") or not os.environ.get("GITHUB_REPOSITORY"):
            print("--issue needs GITHUB_TOKEN and GITHUB_REPOSITORY", file=sys.stderr)
            return 2
        sync_issue(body, keys, clean)
    else:
        print(body)
        print(f"(dry run: {len(keys)} item(s) want a look; the issue was not touched)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
