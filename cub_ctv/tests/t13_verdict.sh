#!/bin/bash
# t13: verdict figures of the live fixture's frames vs golden (no server).
# Regenerate the golden with --update.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
G=$HERE/fixtures/demodb-live.rec.verdict.golden
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v '/cub_ctv\.c$')
gcc -O1 -Wall -I"$HERE/../src" -o "$T/t13" "$HERE/t13_verdict.c" "${SRCS[@]}" \
  || { echo "t13 FAIL: build"; exit 1; }
LANG=C "$T/t13" "$HERE/fixtures/demodb-live.rec" > "$T/out" || { echo "t13 FAIL: run"; exit 1; }
if [ "${1:-}" = "--update" ]; then cp "$T/out" "$G"; echo "t13 golden updated"; exit 0; fi
cmp -s "$T/out" "$G" || { diff "$T/out" "$G" | head; echo "t13 FAIL: verdict figures differ"; exit 1; }
# window growth of the first LOG row's u32 counters, by case (awk sets the
# row; C tokens: $10 nenter, $11 nwait, $13 total_ms, $20/$21 window-start
# nenter/nwait):
#   reset    ;info csstat cleared the statistics (total_ms fell): growth is
#            the new value, even when the old one was past 2^31
#   wrap     nenter passed 2^32 while total_ms grew: growth modulo 2^32
#   torn     total_ms read a little low while nenter grew: plain growth
check () {
  awk "\$1 == \"C\" && \$2 == \"1\" && !d { $2; d = 1 } { print }" \
    "$HERE/fixtures/demodb-live.rec" > "$T/c.rec"
  "$HERE/../cub_ctv" --replay "$T/c.rec" --json 2>/dev/null | head -1 | python3 -c '
import json, sys
d = json.loads(sys.stdin.readline())
r = [c for c in d["cs"] if c["name"] == "LOG"][0]
de, dw = float(sys.argv[1]), float(sys.argv[2])
sys.exit(0 if abs(r["enter_s"] * d["dt"] / de - 1) < 0.01
         and abs(r["wait_s"] * d["dt"] / dw - 1) < 0.01 else 1)' "$3" "$4" \
    || { echo "t13 FAIL: counter growth ($1)"; exit 1; }
}
check "reset" '$10 = 1200; $11 = 300; $13 = 500.0' 1200 300
check "reset from past 2^31" '$20 = 3000000000; $21 = 3500000000; $10 = 1200; $11 = 300; $13 = 500.0' 1200 300
check "u32 wrap" '$20 = 4294967000; $21 = 4294967200; $10 = 500; $11 = 100; $13 = 999999.0' 796 196
check "torn total_ms" '$13 = 528854.0' 1243 362
echo "t13 PASS: verdict figures of $(wc -l < "$G") frames match the golden; counter reset (also past 2^31), wrap, torn time"
