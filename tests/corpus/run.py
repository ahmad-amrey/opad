#!/usr/bin/env python3
"""Import every STEP file in this directory through opad-cli and write corpus-report.json.

usage: run.py <path-to-opad-cli> [dir]
"""
import glob
import json
import os
import subprocess
import sys
import tempfile
import time

cli = os.path.abspath(sys.argv[1])
root = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
files = sorted(glob.glob(os.path.join(root, "*.st*p")))
report = []
tmp = tempfile.mkdtemp(prefix="opad-corpus-")
for f in files:
    doc = os.path.join(tmp, os.path.basename(f) + ".opad")
    subprocess.run([cli, "new", doc], capture_output=True)
    t0 = time.time()
    p = subprocess.run([cli, "import", doc, f, "--compact"], capture_output=True, text=True)
    entry = {"file": os.path.basename(f), "bytes": os.path.getsize(f), "seconds": round(time.time() - t0, 2), "ok": p.returncode == 0}
    if p.returncode == 0:
        r = json.loads(p.stdout)
        entry.update({k: r.get(k) for k in ("components", "bodies", "new_entries", "healed", "warnings")})
        entry["opad_bytes"] = os.path.getsize(doc)
    else:
        entry["error"] = p.stderr.strip()
    report.append(entry)
    print(f"{'ok  ' if entry['ok'] else 'FAIL'} {entry['file']:40s} {entry['seconds']:6.2f}s  {entry.get('bodies', '')}")
ok = sum(1 for e in report if e["ok"])
summary = {"files": len(report), "ok": ok, "rate": round(ok / len(report), 3) if report else 0, "results": report}
with open(os.path.join(root, "corpus-report.json"), "w") as out:
    json.dump(summary, out, indent=2)
print(f"{ok}/{len(report)} imported ({summary['rate']*100:.0f}%)")
