"""End-to-end crash recovery using isolated test instances (requires a GUI/OpenGL session)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("cli", type=Path)
    args = parser.parse_args()
    app, cli = args.app.resolve(), args.cli.resolve()
    with tempfile.TemporaryDirectory(prefix="opad-recovery-") as directory:
        root = Path(directory)
        document = root / "source.opad"
        subprocess.run([str(cli), "new", str(document)], check=True, capture_output=True)
        original = document.read_bytes()
        for feature in (False, True):
            settings = root / ("feature" if feature else "sketch")
            for stage in ("write", "read"):
                mode = stage + ("-feature" if feature else "")
                log = root / f"{mode}.log"
                env = dict(os.environ, OPAD_LANG="en", OPAD_BENCH_SETTINGS=str(settings),
                           OPAD_BENCH_RECOVERY=mode, OPAD_TRACE=str(log))
                startup = None
                if os.name == "nt":
                    startup = subprocess.STARTUPINFO()
                    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                    startup.wShowWindow = 0
                result = subprocess.run([str(app), str(document), "--bench-select"], env=env,
                                        startupinfo=startup, capture_output=True, timeout=45)
                trace = log.read_text(encoding="utf-8", errors="replace")
                assert result.returncode == 0, (mode, result.returncode, trace[-6000:])
                assert "PASS" in trace and "FAIL" not in trace and "CRASH:" not in trace, trace
                assert document.read_bytes() == original, "Autosave overwrote the source document"
                if stage == "write":
                    files = list(settings.rglob("*.opad-recovery"))
                    assert len(files) == 1
                    record = json.loads(files[0].read_text(encoding="utf-8"))
                    assert record["edit"]["type"] == ("feature" if feature else "sketch")
                    corrupt = dict(record, sha256="corrupted")
                    files[0].with_name("corrupt.opad-recovery").write_text(json.dumps(corrupt), encoding="utf-8")
                print(f"{mode}: PASS")
        print("Crash recovery, active editors, checksums, source preservation and cleanup: PASS")


if __name__ == "__main__":
    main()
