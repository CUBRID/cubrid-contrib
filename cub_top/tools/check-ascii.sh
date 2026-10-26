#!/bin/bash
#
# Copyright 2016 CUBRID Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
# Regression: Hangul leaking into --ascii mode.
# A new label without its g_ascii branch (or L() table entry) leaks Hangul into
# English mode; this recurred across six plot panels and some 40 tree strings.
#
# Every --ascii output mode is checked, not just the tree.  The machine-readable
# modes matter most: -d and --dump-json are parsed by monitoring, and a Korean value
# there breaks the consumer rather than merely looking wrong.
#
# the modes are run as shipped because method B reason strings are produced only
# on that path.  The dashboard and plots are drawn from the example recording under a pty
# (script(1)); a plot legend ("load(1분)") once leaked that way.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/cub_top.c"
BIN="$(mktemp -u /tmp/cub_top-ascii.XXXXXX)"
trap 'rm -f "$BIN"' EXIT
gcc -O2 -std=gnu99 -o "$BIN" "$SRC" 2>/dev/null || { echo "-> FAIL (compile failed)"; exit 1; }

# A [Hangul] character class makes grep die with "Invalid collation character" under
# some locales, and that failure reads as zero matches - a false PASS.
# So the UTF-8 lead bytes 0xEA-0xED of Hangul syllables are matched as bytes.
HANGUL=$'[\xea-\xed][\x80-\xbf][\x80-\xbf]'

if ! "$BIN" --ascii -t 2>/dev/null | grep -q 'cub_server'; then
    echo "-> SKIP (cub_server not running - no tree body)"; exit 0
fi

# Self-test: the matcher must actually find Hangul, or every check below is vacuous.
# Without this, a broken pattern or grep would report a clean PASS forever.
if ! printf 'ED\xea\xb0\x80\n' | LC_ALL=C grep -q "$HANGUL"; then
    echo "-> FAIL (Hangul matcher is broken - the check would pass vacuously)"; exit 1
fi
# ...and it must not fire on pure ASCII.
if printf 'plain ascii only\n' | LC_ALL=C grep -q "$HANGUL"; then
    echo "-> FAIL (Hangul matcher matches ASCII - false positives)"; exit 1
fi

bad=0
check() {   # check <label> <args...>
    lbl=$1; shift
    out=$("$BIN" "$@" 2>/dev/null)
    n=$(printf '%s\n' "$out" | LC_ALL=C grep -c "$HANGUL"); rc=$?
    if [ "$rc" -gt 1 ]; then echo "  FAIL $lbl (grep failed rc=$rc)"; bad=1; return; fi
    if [ "${n:-0}" -ne 0 ]; then
        echo "  FAIL $lbl: $n lines with Hangul"
        printf '%s\n' "$out" | LC_ALL=C grep "$HANGUL" | head -3 | cut -c1-110 | sed 's/^/        /'
        bad=1
    else
        echo "  ok   $lbl"
    fi
}

echo "--ascii output modes (0 Hangul required)"
check "tree -t"            --ascii -t
check "tree -t"            --ascii -t
check "dump -d"            --ascii -d
check "dump -d"            --ascii -d
check "--dump-json"        --ascii --dump-json
check "--dump-json"        --ascii --dump-json

REC="$ROOT/examples/10-recording.jsonl"
if command -v script >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && [ -s "$REC" ]; then
    for m in -b -p; do
        cap="$BIN.$m"
        timeout -s INT 4 script -qc "stty cols 160 rows 50; '$BIN' --ascii --replay '$REC' $m --replay-speed 1" /dev/null >"$cap" 2>&1
        hits=$(python3 - "$cap" <<'PY'
import re,sys
t=open(sys.argv[1],'rb').read().decode('utf-8','replace')
t=re.sub(r'\x1b\[[0-9;?]*[a-zA-Z]','',t)
h=sorted(set(re.findall(r'.{0,20}[\uac00-\ud7a3]+.{0,10}',t)))
print(len(t)); [print(x) for x in h[:3]]
PY
)
        n=$(printf '%s\n' "$hits" | sed 1d | grep -c .)
        len=$(printf '%s\n' "$hits" | head -1)
        if [ "${len:-0}" -lt 2000 ]; then echo "  FAIL screen $m: nothing drawn ($len chars)"; bad=1
        elif [ "$n" -ne 0 ]; then echo "  FAIL screen $m: Hangul"; printf '%s\n' "$hits" | sed 1d | sed 's/^/        /'; bad=1
        else echo "  ok   screen $m (replay under a pty)"; fi
        rm -f "$cap"
    done
else echo "  SKIP screens -b/-p (needs script, python3 and examples/10-recording.jsonl)"; fi

# Item notes of the anchor-probing mode (B.partial) cannot be reached on this host: a
# stripped build of an unknown version is needed.  So the source is held to the contract
# instead: in b_items, every line that carries Hangul must branch on g_ascii.
bad_lines=$(sed -n '/^static void b_items(/,/^}/p' "$ROOT/src/cub_top.c" | LC_ALL=C grep "$HANGUL" | grep -v 'g_ascii?' | grep -v '^ *\(/\*\|\*\|:"\)')   # :" continues a ternary of the line before
if [ -n "$bad_lines" ]; then echo "  FAIL b_items: Hangul without a g_ascii branch"; printf '%s\n' "$bad_lines" | sed 's/^/        /'; bad=1
else echo "  ok   b_items: every Hangul note has its g_ascii branch"; fi

# Korean mode must keep its Korean - an over-eager fix that made everything English
# would otherwise pass every check above.
KO=$("$BIN" -d 2>/dev/null | LC_ALL=C grep -c "$HANGUL")
if [ "${KO:-0}" -eq 0 ]; then
    echo "  FAIL Korean mode lost its Korean (-d has none)"; bad=1
else
    echo "  ok   Korean mode still Korean ($KO lines)"
fi

[ $bad -eq 0 ] && { echo "-> PASS"; exit 0; } || { echo "-> FAIL"; exit 1; }
