#!/bin/bash
# t6: record -> replay round trip of the frame model (no server needed).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../src
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
W=-Wl,--wrap=thr_index_of_tid_mem,--wrap=thr_waiters_mem,--wrap=thr_waiters_mem2,--wrap=tran_identity_cached
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v '/cub_ctv\.c$')
gcc -O1 -Wall -I"$SRC" -o "$T/t6" "$HERE/t6_view_roundtrip.c" \
  "${SRCS[@]}" $W || { echo "t6 FAIL: build"; exit 1; }
"$T/t6" record "$T/r.rec" > "$T/a.out" || { echo "t6 FAIL: record"; exit 1; }
"$T/t6" replay "$T/r.rec" > "$T/b.out" || { echo "t6 FAIL: replay"; exit 1; }
cmp -s "$T/a.out" "$T/b.out" || { diff "$T/a.out" "$T/b.out" | head; echo "t6 FAIL: replay differs"; exit 1; }
grep -q "^Q " "$T/r.rec" && grep -q "%25" "$T/r.rec" && grep -q " ~" "$T/r.rec" \
  || { echo "t6 FAIL: record lacks chains or escapes"; exit 1; }
grep -q -- "-> thr=" "$T/a.out" || { echo "t6 FAIL: wait chain not rendered"; exit 1; }
echo "t6 PASS: record -> replay identical ($(grep -c . "$T/r.rec") record lines)"
