#!/usr/bin/env python3
"""Lists the app's tr("...") strings that a translation does not cover yet.

  python tools/i18n_check.py            # every app/i18n/<code>.json with its fragments app/i18n/<code>/*.json
  python tools/i18n_check.py --dump     # print all source strings as a JSON skeleton

Translations are plain JSON (source text -> translation, see app/I18n.hpp); keys starting with "@" are
metadata. A language is <code>.json plus the area fragments in <code>/, merged in that order as the app does.
Strings looked up at run time (property names, error messages) are not tr() literals, so this script does not
know them: keep them in a file by hand. Exit code 1 when something is missing, when a key appears twice (in one
file or across the files of a language) with different translations, or when a fragment folder has no language.
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
    app = os.path.join(ROOT, 'app')
    for path in sorted(glob.glob(os.path.join(app, '**', '*.cpp'), recursive=True) + glob.glob(os.path.join(app, '**', '*.hpp'), recursive=True)):
        if os.path.basename(path).startswith('I18n.'):
            continue  # the lookup helpers themselves
        text = open(path, encoding='utf-8').read()
        for m in TR.finditer(text):
            parts = re.findall(LITERAL, m.group(1))
            s = ''.join(json.loads('"' + re.sub(r'\\(?!["\\ntu])', r'\\\\', p[1:-1]) + '"') for p in parts)
            found.setdefault(s, os.path.basename(path))
    return found


def pairs(path):
    """The (key, value) pairs of one JSON object file, in order, duplicates kept."""
    return json.load(open(path, encoding='utf-8'), object_pairs_hook=list)


def language(path):
    """<code>.json then its fragments: the merged table and the clashes (same key, different translations)."""
    files = [path] + sorted(glob.glob(os.path.join(path[:-5], '*.json')))
    merged, where, clashes = {}, {}, []
    for file in files:
        for key, value in pairs(file):
            if key.startswith('@') or not isinstance(value, str):
                continue
            name = os.path.relpath(file, os.path.dirname(path)).replace(os.sep, '/')
            if key in merged and value and merged[key] and value != merged[key]:
                clashes.append((key, where[key], name))
            if value or key not in merged:
                merged[key], where[key] = value, name
    return files, merged, clashes


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    src = sources()
    if '--dump' in sys.argv:
        print(json.dumps({s: '' for s in src}, ensure_ascii=False, indent=1))
        return 0
    bad = 0
    root = os.path.join(ROOT, 'app', 'i18n')
    for code in sorted(name for name in os.listdir(root) if os.path.isdir(os.path.join(root, name))):
        if not os.path.exists(os.path.join(root, code + '.json')):
            print('%s/: fragments without %s.json (not a language)' % (code, code))
            bad += 1
    for path in sorted(glob.glob(os.path.join(root, '*.json'))):
        files, have, clashes = language(path)
        missing = [s for s in src if not have.get(s)]
        print('%s + %d fragments: %d strings, %d missing, %d clashes' % (os.path.basename(path), len(files) - 1, len(src), len(missing), len(clashes)))
        for s in missing:
            print('  %-16s %s' % (src[s], json.dumps(s, ensure_ascii=False)))
        for key, first, second in clashes:
            print('  clash: %s and %s translate %s differently' % (first, second, json.dumps(key, ensure_ascii=False)))
        bad += len(missing) + len(clashes)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
