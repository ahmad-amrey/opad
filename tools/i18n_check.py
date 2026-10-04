#!/usr/bin/env python3
"""Lists the app's tr("...") strings that a translation does not cover yet.

  python tools/i18n_check.py            # every app/i18n/<code>.json with its fragments app/i18n/<code>/*.json,
                                        # the command help app/help/commands.<code>.json and the clip texts
  python tools/i18n_check.py --dump     # print all source strings as a JSON skeleton

Translations are plain JSON (source text -> translation, see app/I18n.hpp); keys starting with "@" are
metadata. A language is <code>.json plus the area fragments in <code>/, merged in that order as the app does. Words
handed straight to a label, tooltip, status or toast as QString("...") instead of tr("...") fail the check too.
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


SHOWN = re.compile(r'\b(setText|showMessage|setToolTip|setPlaceholderText|setWindowTitle|addItem|addAction|toast|setStatus|status|hoverChanged|'
                   r'setHeader|setContext|setSummary|setPrompt|QRadioButton|QCheckBox|QPushButton|QLabel)\s*\(\s*(?:QString(?:::fromUtf8)?|QStringLiteral)\s*\(\s*(' + LITERAL + ')')


def untranslated():
    """Words handed to something the user reads without tr(): setText(QString("Section %1")), emit status(QString(...)).
    File extensions (".step"), markup, %N placeholders and example addresses (a placeholder URL or e-mail) are not words;
    the benches' own texts are left alone."""
    found = []
    for path in sorted(glob.glob(os.path.join(ROOT, 'app', '*.cpp'))):
        if 'Bench' in os.path.basename(path):
            continue
        for number, line in enumerate(open(path, encoding='utf-8'), 1):
            for m in SHOWN.finditer(line):
                text = re.sub(r'<[^>]*>|%\d|(?<![A-Za-z])\.[A-Za-z0-9]+', '', m.group(2)[1:-1])
                if re.search(r'[A-Za-z]{3,}', text) and not re.search(r'://|@', text):  # a URL or an e-mail address is no word
                    found.append('%s:%d %s' % (os.path.basename(path), number, m.group(2)))
    print('untranslated literals shown: %d' % len(found))
    for f in found:
        print('  ' + f)
    return len(found)


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


def help_missing():
    """app/help/commands.json (English) against each commands.<code>.json: a record's text fields, translated by id."""
    english = json.load(open(os.path.join(ROOT, 'app', 'help', 'commands.json'), encoding='utf-8'))['commands']
    bad = 0
    for path in sorted(glob.glob(os.path.join(ROOT, 'app', 'help', 'commands.*.json'))):
        have = json.load(open(path, encoding='utf-8'))
        missing = [(c['id'], field) for c in english for field in ('title', 'summary', 'details', 'requires', 'keywords')
                   if c.get(field) and not have.get(c['id'], {}).get(field)]
        print('%s: %d commands, %d fields missing' % (os.path.basename(path), len(english), len(missing)))
        for command, field in missing:
            print('  %-28s %s' % (command, field))
        bad += len(missing)
    return bad


def clip_texts():
    """The translatable English texts of app/help/clips.json, templates expanded as app/HelpClip.cpp does: step
    captions, label and chip texts, card titles, buttons, row labels and word-only row values."""
    data = json.load(open(os.path.join(ROOT, 'app', 'help', 'clips.json'), encoding='utf-8'))
    templates = data.get('templates', {})
    name = re.compile(r'\$([A-Za-z_][A-Za-z0-9_]*)')

    def sub(v, args):
        if isinstance(v, str):
            if v.startswith('$') and v[1:] in args:
                return args[v[1:]]
            return name.sub(lambda m: str(args[m.group(1)]) if m.group(1) in args else m.group(0), v)
        if isinstance(v, list):
            return [sub(x, args) for x in v]
        if isinstance(v, dict):
            return {k: sub(x, args) for k, x in v.items()}
        return v

    def bound(template, given):
        args = {k: v for k, v in template.get('params', {}).items() if v is not None}
        args.update(given or {})
        return args

    def expand(items):
        for item in items:
            if 'use' in item:
                template = templates.get(item['use'], {})
                yield from expand(sub(template.get('items', []), bound(template, item.get('args'))))
            else:
                yield item

    def wordy(s):
        return isinstance(s, str) and not re.search(r'\d', s) and re.search(r'[A-Za-z]{2}', s)

    found = {}
    for clip in data.get('clips', []):
        if 'template' in clip:
            template = templates.get(clip['template'], {})
            base = sub(template, bound(template, clip.get('args')))
            clip = dict(base, **{k: v for k, v in clip.items() if k not in ('items', 'template', 'args')},
                        items=base.get('items', []) + clip.get('items', []))
        texts = [s.get('caption') for s in clip.get('steps', [])]
        for item in expand(clip.get('items', [])):
            for props in [item] + [k[1] for k in item.get('keys', []) if isinstance(k, list) and len(k) == 2 and isinstance(k[1], dict)]:
                if item.get('el') in ('label', 'chip'):
                    texts.append(props.get('text'))
                if item.get('el') == 'card':
                    texts += [props.get('title'), props.get('button')]
                    for row in props.get('rows', []):
                        text, value = (row + [None, None])[:2] if isinstance(row, list) else (row.get('text'), row.get('value'))
                        texts.append(text)
                        if wordy(value):
                            texts.append(value)
        for t in texts:
            if isinstance(t, str) and t:
                found.setdefault(t, clip['id'])
    return found


def clips_missing():
    """Every clip text in each language: app/i18n/<code>.json plus its area fragments app/i18n/<code>/*.json."""
    texts = clip_texts()
    bad = 0
    for path in sorted(glob.glob(os.path.join(ROOT, 'app', 'i18n', '*.json'))):
        code = os.path.splitext(os.path.basename(path))[0]
        have = language(path)[1]
        missing = [t for t in texts if not have.get(t)]
        print('clips (%s): %d texts, %d missing' % (code, len(texts), len(missing)))
        for t in missing:
            print('  %-22s %s' % (texts[t], json.dumps(t, ensure_ascii=False)))
        bad += len(missing)
    return bad


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
    bad += help_missing()
    bad += clips_missing()
    bad += untranslated()
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
