#!/usr/bin/env python3
"""Static check: every bilingual message pair in src/*.c must carry the
same printf conversion sequence in both languages.  M(en, ko) feeds printf
with a non-literal format, so -Wformat cannot see a mismatch; this does.
Reports every mismatch, then exits 1 if there was any."""
import re
import sys
import os

import glob
SRCDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
FILES = sorted(glob.glob(os.path.join(SRCDIR, "*.c")))
PARTS = [(f, open(f, encoding="utf-8").read()) for f in FILES]
text = "".join(t for _, t in PARTS)


def where(pos):
    """file:line of an offset in the concatenated text"""
    for f, t in PARTS:
        if pos < len(t):
            return "%s:%d" % (os.path.relpath(f, SRCDIR), t.count("\n", 0, pos) + 1)
        pos -= len(t)
    return "?"

# collapse adjacent string literals ("a" "b") into one; keep escapes raw
LIT = r'"((?:[^"\\]|\\.)*)"'
SEQ = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXfeEgGcsp])')


def convs(lit):
    return [c for c in SEQ.findall(lit) if c != "%%"]


def pairs_from(call):
    """yield (en, ko) literal pairs for a call body like M ("a" "b", "c")"""
    strs = re.findall(LIT, call)
    return strs


bad = 0
for m in re.finditer(r'\b(M|die|warn|warnf)\s*\(', text):
    # take the balanced-paren body
    i, depth = m.end() - 1, 0
    for j in range(i, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                body = text[m.end():j]
                break
    # split top-level args on commas not inside quotes
    args, cur, inq, esc = [], "", False, False
    for ch in body:
        if inq:
            cur += ch
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                inq = False
        elif ch == '"':
            inq = True
            cur += ch
        elif ch == ",":
            args.append(cur)
            cur = ""
        else:
            cur += ch
    args.append(cur)
    if len(args) < 2:
        continue
    en = "".join(re.findall(LIT, args[0]))
    ko = "".join(re.findall(LIT, args[1]))
    if not en or not ko:
        continue
    if convs(en) != convs(ko):
        print(f"mismatch at {where(m.start())}: {convs(en)} vs {convs(ko)}")
        bad += 1
print("msg pairs:", "FAIL" if bad else "PASS")
sys.exit(1 if bad else 0)
