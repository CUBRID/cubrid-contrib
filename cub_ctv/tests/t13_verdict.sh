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
# engine counters reset by ;info csstat (first LOG row: counters below the
# window start) count the new value as the window's growth, not a u32 wrap
awk '$1 == "C" && $2 == "1" && !d { $10 = 1200; $11 = 300; $13 = 500.0; d = 1 } { print }' \
  "$HERE/fixtures/demodb-live.rec" > "$T/reset.rec"
"$HERE/../cub_ctv" --replay "$T/reset.rec" --json 2>/dev/null | head -1 | python3 -c '
import json, sys
d = json.loads(sys.stdin.readline())
r = [c for c in d["cs"] if c["name"] == "LOG"][0]
sys.exit(0 if abs(r["enter_s"] * d["dt"] / 1200 - 1) < 0.01 and abs(r["wait_s"] * d["dt"] / 300 - 1) < 0.01 else 1)' \
  || { echo "t13 FAIL: counter reset read as a wrap"; exit 1; }
echo "t13 PASS: verdict figures of $(wc -l < "$G") frames match the golden; counter reset"
