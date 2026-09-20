#!/usr/bin/env python3
"""Lists the app's tr("...") strings that a translation file does not cover yet.

  python tools/i18n_check.py            # every app/i18n/*.json
  python tools/i18n_check.py --dump     # print all source strings as a JSON skeleton

Translations are plain JSON (source text -> translation, see app/I18n.hpp); keys starting with "@" are
metadata. Strings looked up at run time (property names, error messages) are not tr() literals, so this
script does not know them: keep them in the file by hand. Exit code 1 when something is missing.
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LITERAL = r'"(?:[^"\\]|\\.)*"'
TR = re.compile(r'\b(?:tr|translate|i18n::t)\(\s*(?:"[A-Za-z]*"\s*,\s*)?((?:' + LITERAL + r'\s*)+)')


def sources():
    found = {}
    for path in sorted(glob.glob(os.path.join(ROOT, 'app', '*.cpp')) + glob.glob(os.path.join(ROOT, 'app', '*.hpp'))):
        if os.path.basename(path).startswith('I18n.'):
            continue  # the lookup helpers themselves
        text = open(path, encoding='utf-8').read()
        for m in TR.finditer(text):
            parts = re.findall(LITERAL, m.group(1))
            s = ''.join(json.loads('"' + re.sub(r'\\(?!["\\ntu])', r'\\\\', p[1:-1]) + '"') for p in parts)
            found.setdefault(s, os.path.basename(path))
    return found


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    src = sources()
    if '--dump' in sys.argv:
        print(json.dumps({s: '' for s in src}, ensure_ascii=False, indent=1))
        return 0
    bad = 0
    for path in sorted(glob.glob(os.path.join(ROOT, 'app', 'i18n', '*.json'))):
        have = json.load(open(path, encoding='utf-8'))
        missing = [s for s in src if not have.get(s)]
        print('%s: %d strings, %d missing' % (os.path.basename(path), len(src), len(missing)))
        for s in missing:
            print('  %-16s %s' % (src[s], json.dumps(s, ensure_ascii=False)))
        bad += len(missing)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
