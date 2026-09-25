"""End-to-end crash recovery using isolated test instances (requires a GUI/OpenGL session)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from recovery_record import document_text


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
        for feature, legacy in ((False, False), (True, False), (False, True)):
            settings = root / ("legacy" if legacy else "feature" if feature else "sketch")
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
                    assert files
                    files.sort()
                    record = json.loads(files[-1].read_text(encoding="utf-8"))
                    assert record["format"] == 2
                    assert "document" not in record
                    assert (files[-1].parent / record["delta"]["base"]).exists()
                    assert record["edit"]["type"] == ("feature" if feature else "sketch")
                    if legacy:
                        for path in files:
                            delta = json.loads(path.read_text(encoding="utf-8"))
                            text = document_text(path)
                            edit = json.dumps(delta["edit"], separators=(",", ":"), ensure_ascii=False)
                            old = {key: delta[key] for key in ("title", "source", "time")}
                            old = dict(format=1, **old, document=text, edit=delta["edit"],
                                       sha256=hashlib.sha256((text + edit).encode()).hexdigest())
                            path.write_text(json.dumps(old, separators=(",", ":"), ensure_ascii=False), encoding="utf-8")
                            path.with_suffix(path.suffix + ".meta").unlink()
                    corrupt = dict(record, sha256="corrupted")
                    files[-1].with_name("corrupt.opad-recovery").write_text(json.dumps(corrupt), encoding="utf-8")
                print(f"{mode}{' (legacy)' if legacy else ''}: PASS")
        print("Crash recovery, active editors, checksums, source preservation and cleanup: PASS")


if __name__ == "__main__":
    main()
