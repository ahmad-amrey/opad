"""Check hover suppression and context resets in all four selection modes.

Usage: python tools/test_hover_fade.py <opad.exe> <opad-cli.exe>
Requires a GUI/OpenGL session; uses isolated settings and in-app Qt events.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    app, cli = (Path(arg).resolve() for arg in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="opad-hover-") as directory:
        root = Path(directory)
        document, log = root / "empty.opad", root / "hover.log"
        subprocess.run([str(cli), "new", str(document)], capture_output=True, check=True)
        env = {key: value for key, value in os.environ.items() if not key.startswith("OPAD_BENCH_")}
        env.update(OPAD_LANG="en", OPAD_BENCH_SETTINGS=str(root / "settings"),
                   OPAD_TRACE=str(log), OPAD_BENCH_PICKING="1", OPAD_BENCH_HOVER_FADE="1")
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        result = subprocess.run([str(app), str(document), "--bench-select"], env=env,
                                startupinfo=startup, capture_output=True, timeout=120)
        trace = log.read_text(encoding="utf-8", errors="replace")
        assert result.returncode == 0 and "FAIL" not in trace, (result.returncode, trace[-10000:])
        for mode in range(4):
            assert f"bench: hover fade mode {mode}:" in trace, trace[-10000:]
        print("Hover fading: body, face, edge, vertex PASS")


if __name__ == "__main__":
    main()
