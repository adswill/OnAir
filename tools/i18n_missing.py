#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""List untranslated keys per language catalog (app/i18n/*.txt).

A key is untranslated when the catalog value equals the English key, the key
has a real word (3+ letters outside printf conversions) and it is not listed in
tools/i18n_keep_english.txt (texts that stay the same in every language).
Keys the sources use but a catalog lacks: dump them with
  build/.../test_i18n --dump > used.txt   and pass --used used.txt

  tools/i18n_missing.py                    counts per language
  tools/i18n_missing.py de                 the untranslated keys of one language
  tools/i18n_missing.py --common           keys untranslated in every language
  tools/i18n_missing.py --used used.txt    also count keys absent from the catalogs
"""
import glob, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', 'app', 'i18n')
KEEP = os.path.join(HERE, 'i18n_keep_english.txt')
WORD = re.compile(r'[A-Za-z]{3,}')


def load(path):
    d = {}
    for l in open(path, encoding='utf8'):
        l = l.rstrip('\n')
        if l.startswith('#') or '\t' not in l:
            continue
        k, v = l.split('\t', 1)
        d[k] = v
    return d


def keep_set():
    s = set()
    if os.path.exists(KEEP):
        for l in open(KEEP, encoding='utf8'):
            l = l.rstrip('\n')
            if l and not l.startswith('#'):
                s.add(l)
    return s


def untranslated(cat, keep):
    return [k for k, v in cat.items()
            if k == v and k not in keep and WORD.search(re.sub(r'%[-+ #0-9.*]*[a-zA-Z]', '', k))]


def main():
    a = sys.argv[1:]
    used = None
    if '--used' in a:
        i = a.index('--used')
        used = set(l.rstrip('\n') for l in open(a[i + 1], encoding='utf8'))
        del a[i:i + 2]
    keep = keep_set()
    cats = {os.path.basename(f)[:-4]: load(f) for f in sorted(glob.glob(os.path.join(ROOT, '*.txt')))}
    if '--common' in a:
        for k in sorted(set.union(*[set(untranslated(c, keep)) for c in cats.values()])):
            print(k)
        return
    if a:
        for k in untranslated(cats[a[0]], keep):
            print(k)
        return
    for code, c in cats.items():
        line = '%s: %d untranslated' % (code, len(untranslated(c, keep)))
        if used is not None:
            line += ', %d missing' % len([k for k in used if k not in c])
        print(line)


main()
