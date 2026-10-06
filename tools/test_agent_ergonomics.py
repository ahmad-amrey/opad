"""Acceptance: agent ergonomics found in a real agent's session (opad_resources/mcp-eval-smartknob.md), through the live
bridge of a hidden desktop window (opad-cli mcp --live).

usage: python tools/test_agent_ergonomics.py APP CLI [--only git,...]

git: a merge preview and a resolve listing need no request_id and change nothing, the live revision included (also after
a refused merge into a protected branch); a pull preview needs none either.
"""
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
        for name, check in [("git", check_git)]:
            if not only or name in only:
                check(app, cli, root / name)
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
