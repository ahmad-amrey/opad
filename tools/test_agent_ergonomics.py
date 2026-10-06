"""Acceptance: agent ergonomics found in a real agent's session (opad_resources/mcp-eval-smartknob.md), through the live
bridge of a hidden desktop window (opad-cli mcp --live).

usage: python tools/test_agent_ergonomics.py APP CLI [--only git,...]

git: a merge preview and a resolve listing need no request_id and change nothing, the live revision included (also after
a refused merge into a protected branch); a pull preview needs none either.
sizes: replies stay small on a linked board of many parts: import and model_batch with verbosity compact give counts, the
import's root and one line per linked file (a Move of the board), viewport_image counts the visible bodies unless asked
for their ids, tree pages.
open: from a window's start page an agent makes a new document (new_document) and opens others (open_document, a STEP in
viewer mode), bound to each in turn; unsaved changes refuse both (nothing discarded), an existing path refuses new_document.
"""
import json
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from test_live_agent import Desktop


def rid():
    return str(uuid.uuid4())


def git(repo, *args):
    return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True).stdout.strip()


def git_environment(root):
    return {"GIT_CONFIG_GLOBAL": str(root / "gitconfig"), "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "Agent Test",
            "GIT_AUTHOR_EMAIL": "agent@example.com", "GIT_COMMITTER_NAME": "Agent Test", "GIT_COMMITTER_EMAIL": "agent@example.com"}


def rebind(desktop, client, cli):
    """The document was reloaded by a git tool: bind its new target."""
    client.close()
    return desktop.bind(cli)


def check_git(app, cli, root):
    project = root / "project"
    project.mkdir(parents=True)
    document = project / "model.opad"
    subprocess.run([str(cli), "new", str(document)], check=True, capture_output=True)
    environment = git_environment(root)
    os.environ.update(environment)  # the test's own git calls (the user's part) use the same identity and config
    desktop = Desktop(app, cli, root / "desktop", document=document, environment=environment)
    client = None
    try:
        client = desktop.bind(cli)
        made = client.call("git_init", branch="develop", paths=["model.opad"], request_id=rid())
        assert made["result"]["state"] == "initialized", made
        client.write("param", name="w", expr="5 mm")
        client.write("save")
        client.call("git_commit", message="w 5", all=True, request_id=rid())
        client.call("git_branch_create", name="feature", request_id=rid())
        client.write("param", name="w", expr="7 mm")
        client.write("save")
        client.call("git_commit", message="w 7", all=True, request_id=rid())
        git(project, "branch", "main", "develop")  # the user's protected main
        client.call("git_switch", branch="main", request_id=rid())
        client = rebind(desktop, client, cli)
        revision = client.state()["revision"]
        refused = client.raw("git_merge", source="feature", request_id=rid())
        assert refused["isError"] and refused["structuredContent"]["error"]["code"] == "protected_branch", refused
        preview = client.raw("git_merge", source="feature", preview=True)  # no request_id: it changes nothing
        assert not preview.get("isError"), preview
        result = preview["structuredContent"]
        assert result["state"] == "read" and result["result"]["state"] == "preview" and result["result"]["fast_forward"], result
        assert client.state()["revision"] == revision, ("a merge preview changed the revision", revision, client.state()["revision"])
        assert git(project, "rev-parse", "main") == git(project, "rev-parse", "develop"), "the preview merged"
        missing = client.raw("git_merge", source="feature")
        assert missing["isError"] and missing["structuredContent"]["error"]["code"] == "invalid_arguments", missing
        # A pull preview only fetches: no request_id either.
        remote = root / "remote.git"
        git(root, "init", "-q", "--bare", "-b", "main", str(remote))
        git(project, "remote", "add", "origin", str(remote))
        assert client.call("git_push", request_id=rid())["result"]["upstream"] == "origin/main"
        pulled = client.call("git_pull", preview=True)
        assert pulled["state"] == "read" and pulled["result"]["state"] == "preview" and pulled["result"]["up_to_date"], pulled
        assert client.state()["revision"] == revision
        print("git: merge preview without request_id, revision unchanged after a refused merge and a preview: PASS", flush=True)
    finally:
        if client:
            client.close()
        desktop.close()


def cli_document(cli, path, *commands):
    subprocess.run([str(cli), "new", str(path)], check=True, capture_output=True)
    for command in commands:
        subprocess.run([str(cli), command[0], str(path)] + list(command[1:]), check=True, capture_output=True)
    return path


def size(reply):
    return len(json.dumps(reply))


def check_sizes(app, cli, root):
    folder = root / "board"
    folder.mkdir(parents=True)
    cli_document(cli, root / "part.opad", ("feature", "--kind", "box", "--inputs", '{"length":"4 mm","width":"2 mm","height":"1.5 mm"}'),
                 ("export", "--format", "step", "--out", str(folder / "part.step")))
    parts = 40
    (folder / "board.kicad_pcb").write_text(
        '(kicad_pcb (version 20241229) (general (thickness 1.6))\n(gr_rect (start 100 100) (end 180 160) (layer "Edge.Cuts"))\n'
        + "".join(f'(footprint "Bench:Part" (layer "F.Cu") (uuid "bbbbbbbb-0000-0000-0000-{i:012d}") (at {104 + 8 * (i % 9)} {104 + 8 * (i // 9)}) '
                  f'(property "Reference" "R{i}")\n  (model "${{KIPRJMOD}}/part.step" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))\n'
                  for i in range(1, parts + 1)) + ")\n", encoding="utf-8")
    document = cli_document(cli, folder / "design.opad", ("feature", "--kind", "box", "--inputs", '{"x":"-60 mm","length":"10 mm","width":"10 mm","height":"10 mm"}'))
    desktop = Desktop(app, cli, root / "desktop", document=document, environment={"OPAD_CACHE_DIR": str(root / "cache")})
    client = None
    try:
        client = desktop.bind(cli)
        # An import, compact: counts and its root, no list per node.
        copied = client.write("import", file=str(folder / "board.kicad_pcb"), verbosity="compact")
        changes = copied["changes"]
        assert changes["counts"]["created"] > parts and "created" not in changes and len(changes["created_roots"]) == 1, changes
        assert "bodies" not in changes and changes["bodies_total"] >= parts and changes["invalid_bodies"] == [], changes
        assert size(copied) < 3000, (size(copied), copied)
        # Tree pages: the board's parts, a few at a time.
        board = changes["created_roots"][0]
        page = client.call("tree", node=board, limit=5, depth=1)["result"]
        assert len(page["roots"]) == 5 and page["total"] > parts and page["next_offset"] == 5, page
        # The same board linked: a Move of one of its parts moves the file as one; compact says so in one line.
        linked = client.write("import", file=str(folder / "board.kicad_pcb"), link=True, verbosity="compact")["changes"]
        assert len(linked["linked_files"]) == 1 and linked["linked_files"][0]["change"] == "created" and linked["counts"]["created"] > parts, linked
        assert linked["created"] == [], linked  # its parts are the file's line, not a list
        top = linked["linked_files"][0]["id"]
        stack, body = list(client.call("tree", node=top, limit=3, depth=6)["result"]["roots"]), None
        while stack and body is None:  # a part of the board: the Move takes the whole file
            item = stack.pop(0)
            body = item["id"] if item["type"] == "body" else None
            stack.extend(item.get("children", []))
        step = {"id": "lift", "command": "feature", "arguments": {"kind": "move", "name": "Lift", "inputs": {"bodies": [body], "dz": "5 mm"}}}
        full = client.write("model_batch", steps=[step], preview=True)
        client.call("preview_cancel", id=full["preview_id"])
        compact = client.write("model_batch", steps=[step], verbosity="compact")
        result = compact["result"]["steps"][0]["result"]
        assert len(result["placed"]) == 1 and result["placed"][0]["id"] == top and result["placed"][0]["bodies"] >= parts, result
        files = compact["changes"]["linked_files"]
        assert len(files) == 1 and files[0]["id"] == top and files[0]["bodies"] >= parts and files[0]["change"] == "modified", compact["changes"]
        assert "modified" not in compact["changes"] or len(compact["changes"]["modified"]) <= 20, compact["changes"]
        assert size(compact) < 2500 and size(compact) * 5 < size(full), (size(compact), size(full))
        # A later batch still refers to what a compact step's reply shortened (the connection keeps the full results).
        again = client.write("model_batch", steps=[{"id": "name", "command": "rename", "arguments": {"targets": ["@{lift#/placed_ids/0}"], "name": "Lifted board"}}], verbosity="compact")
        assert again["result"]["steps"][0]["state"] == "computed" and again["changes"]["counts"]["modified"] == 1, again
        # viewport_image: a count of the rendered bodies, their ids only when asked.
        image = client.call("viewport_image", fit=True, width=320, height=240)["result"]
        assert "visible_ids" not in image and image["visible_count"] > parts, image.keys()
        listed = client.call("viewport_image", fit=True, width=320, height=240, visible_ids=True)["result"]
        assert len(listed["visible_ids"]) == listed["visible_count"] == image["visible_count"], listed["visible_count"]
        print(f"sizes: compact import {size(copied)} bytes, compact Move of a {parts}-part linked board {size(compact)} bytes "
              f"(full {size(full)}), visible ids opt-in, tree pages: PASS", flush=True)
    finally:
        if client:
            client.close()
        desktop.close()


def check_open(app, cli, root):
    root.mkdir(parents=True)
    other = cli_document(cli, root / "other.opad", ("feature", "--kind", "box", "--inputs", '{"length":"10 mm","width":"10 mm","height":"10 mm"}'),
                         ("export", "--format", "step", "--out", str(root / "part.step")))
    desktop = Desktop(app, cli, root / "desktop", empty=True)
    client = None
    try:
        assert desktop.descriptor["target"].startswith("start:"), desktop.descriptor
        client = desktop.bind(cli)  # a window with no document is a target too
        none = client.raw("context")
        assert none["isError"] and none["structuredContent"]["error"]["code"] == "no_document", none
        made = root / "made" / "new.opad"
        created = client.call("new_document", path=str(made), request_id=rid())
        assert created["state"] == "committed" and created["result"]["created"] and made.exists(), created
        assert created["result"]["path"].replace("\\", "/").lower() == str(made).replace("\\", "/").lower(), created
        target = created["result"]["target"]
        assert client.state()["target"] == target  # bound to it: no live_bind needed
        client.write("param", name="w", expr="5 mm")
        exists = client.raw("new_document", path=str(other), request_id=rid())
        assert exists["isError"] and exists["structuredContent"]["error"]["code"] == "file_exists", exists
        unsaved = client.raw("open_document", path=str(other), request_id=rid())
        assert unsaved["isError"] and unsaved["structuredContent"]["error"]["code"] == "unsaved_changes", unsaved
        assert unsaved["structuredContent"]["error"]["next"] == ["save"], unsaved
        assert client.state()["target"] == target and client.state()["dirty"], "the unsaved document stays as it was"
        client.write("save")
        opened = client.call("open_document", path=str(other), request_id=rid())
        assert opened["result"]["target"] != target and not opened["result"]["viewer"] and opened["result"]["bodies"] == 1, opened
        assert client.call("context")["result"]["bodies"] == 1
        viewed = client.call("open_document", path=str(root / "part.step"), request_id=rid())
        assert viewed["result"]["viewer"] and viewed["result"]["bodies"] == 1, viewed
        missing = client.raw("open_document", path=str(root / "nowhere.opad"), request_id=rid())
        assert missing["isError"] and missing["structuredContent"]["error"]["code"] == "not_found", missing
        print("open: new_document from the start page, open_document of an .opad and a STEP (viewer), unsaved changes and existing paths refused: PASS", flush=True)
    finally:
        if client:
            client.close()
        desktop.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--only", default="")
    parser.add_argument("--keep", action="store_true", help="keep the temporary folder")
    args = parser.parse_args()
    app, cli = args.app.resolve(), args.cli.resolve()
    root = Path(tempfile.mkdtemp(prefix="opad-agent-ergonomics-"))  # outside any repository: the git checks make their own
    only = set(filter(None, args.only.split(",")))
    try:
        for name, check in [("git", check_git), ("sizes", check_sizes), ("open", check_open)]:
            if not only or name in only:
                check(app, cli, root / name)
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
