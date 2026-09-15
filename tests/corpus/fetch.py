#!/usr/bin/env python3
"""Download public STEP files for the robustness corpus (opt-in; see README.md).

Sources are (url, filename) pairs from repositories that publish sample STEP files under permissive terms.
Files land next to this script and are git-ignored.
"""
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

# Open CASCADE's own sample data and a few well-known open-hardware parts. Extend freely.
SOURCES = [
    # Open CASCADE sample data (LGPL with exception)
    ("https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/master/data/step/screw.step", "occt-screw.step"),
    ("https://raw.githubusercontent.com/Open-Cascade-SAS/OCCT/master/data/step/linkrods.step", "occt-linkrods.step"),
    # STEPcode test data (BSD): AP214 assemblies from the classic CAx-IF test cases
    ("https://raw.githubusercontent.com/stepcode/stepcode/develop/data/ap214e3/as1-oc-214.stp", "stepcode-as1-oc-214.stp"),
    ("https://raw.githubusercontent.com/stepcode/stepcode/develop/data/ap214e3/dm1-id-214.stp", "stepcode-dm1-id-214.stp"),
    ("https://raw.githubusercontent.com/stepcode/stepcode/develop/data/ap214e3/io1-cm-214.stp", "stepcode-io1-cm-214.stp"),
    ("https://raw.githubusercontent.com/stepcode/stepcode/develop/data/ap214e3/sg1-c5-214.stp", "stepcode-sg1-c5-214.stp"),
    ("https://raw.githubusercontent.com/stepcode/stepcode/develop/data/ap214e3/s1-c5-214/s1-c5-214.stp", "stepcode-s1-c5-214.stp"),
]


def main() -> int:
    ok = 0
    for url, name in SOURCES:
        dest = os.path.join(HERE, name)
        if os.path.exists(dest):
            ok += 1
            continue
        try:
            print(f"fetching {name} ...", end=" ", flush=True)
            urllib.request.urlretrieve(url, dest)
            print(f"{os.path.getsize(dest)} bytes")
            ok += 1
        except Exception as e:  # noqa: BLE001
            print(f"FAILED: {e}")
    print(f"{ok}/{len(SOURCES)} files available in {HERE}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
