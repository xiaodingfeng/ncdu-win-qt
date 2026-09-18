#!/usr/bin/env python3
"""Audit the locale files against the strings the code actually asks for.

Three things are checked, all of them ways an i18n table rots:

1. Symmetry   — zh.json and en.json define exactly the same keys. A key present
                in one language only silently falls back to the other.
2. Coverage   — every key string that appears in src/ exists in the locales.
                Catches typos and keys used but never added.
3. Dead keys  — keys nothing in src/ mentions. A key counts as used when its
                exact text appears anywhere in the sources, and also when it
                belongs to a family built at runtime (a literal prefix such as
                "theme." followed by a code), so renaming a whole family is not
                mistaken for a hundred dead keys.

Usage (from the project root):
    python scripts/check_i18n.py          # report
    python scripts/check_i18n.py --strict # non-zero exit when anything is off
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANGS = ("zh", "en")

# A literal shorter than this is too generic to prove a runtime family exists.
MIN_PREFIX = 6


def load(lang):
    path = os.path.join(ROOT, "locales", f"{lang}.json")
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def source_text():
    """All .cpp/.h text under src/, concatenated."""
    chunks = []
    for base, _dirs, files in os.walk(os.path.join(ROOT, "src")):
        for name in files:
            if name.endswith((".cpp", ".h")):
                with open(os.path.join(base, name), encoding="utf-8") as fh:
                    chunks.append(fh.read())
    return "\n".join(chunks)


def runtime_prefixes(text, keys):
    # C++ string literals, escapes included, so nothing shifts a literal into
    # the middle of the next one (which is how a naive '"([^"]+)"' scan reports
    # used keys as dead).
    lits = set(re.findall(r'"((?:[^"\\]|\\.)*)"', text))
    return sorted({lit for lit in lits
                   if len(lit) >= MIN_PREFIX
                   and any(k.startswith(lit) and k != lit for k in keys)})


def main():
    strict = "--strict" in sys.argv[1:]
    tables = {lang: load(lang) for lang in LANGS}
    keys = set().union(*(set(t) for t in tables.values()))
    text = source_text()
    prefixes = runtime_prefixes(text, keys)

    problems = 0
    missing = []

    print(f"locales: {', '.join(f'{l}={len(t)}' for l, t in tables.items())}"
          f" | runtime families: {len(prefixes)}")

    # 1. symmetry
    for a, b in (("zh", "en"), ("en", "zh")):
        only = sorted(set(tables[a]) - set(tables[b]))
        if only:
            problems += len(only)
            print(f"[FAIL] {len(only)} key(s) only in {a}.json:")
            for k in only:
                print(f"        {k}")

    # 2. coverage: keys asked for RIGHT AT A CALL SITE (I18n::tr("..."), the
    # local aliases some files use, and trIn(lang, "...")). Keys handed around
    # in variables are covered by the dead-key pass instead, which counts any
    # literal in the sources as usage.
    calls = set()
    for pattern in (r'(?:I18n::)?tr\d?(?:In)?\(\s*"((?:[^"\\]|\\.)*)"',
                    r'(?:I18n::)?tr\d?(?:In)?\(\s*QStringLiteral\("((?:[^"\\]|\\.)*)"\)',
                    r'(?:I18n::)?tr\d?(?:In)?\(\s*[^,()]+,\s*"((?:[^"\\]|\\.)*)"'):
        calls |= set(re.findall(pattern, text))
    for lit in sorted(calls):
        if re.fullmatch(r"[a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+", lit) and lit not in keys:
            missing.append(lit)
    if missing:
        problems += len(missing)
        print(f"[FAIL] {len(missing)} key(s) asked for but not defined:")
        for k in missing:
            print(f"        {k}")

    # 3. dead keys
    def used(key):
        return key in text or any(key.startswith(p) for p in prefixes)

    dead = sorted(k for k in keys if not used(k))
    if dead:
        problems += len(dead)
        print(f"[FAIL] {len(dead)} key(s) nothing in src/ ever asks for:")
        for k in dead:
            print(f"        {k:34s} = {tables['en'][k][:50]!r}")
    else:
        print("[ OK ] no dead keys")

    if not problems:
        print("[ OK ] locale tables are symmetric, complete and free of dead keys")
    if strict and problems:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
