#!/bin/bash
# t16: an activity's samples never exceed the window's, also for waiters of
# unknown tran (-1), and the live record replays (no server).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_sampler\.c$')
gcc -O1 -Wall -I"$HERE/../src" -o "$T/t16" "$HERE/t16_act_samples.c" "${SRCS[@]}" \
  || { echo "t16 FAIL: build"; exit 1; }
LANG=C "$T/t16" record "$T/r.rec" || { echo "t16 FAIL: record"; exit 1; }
LANG=C "$T/t16" replay "$T/r.rec" || { echo "t16 FAIL: the record does not replay"; exit 1; }
echo "t16 PASS: $(grep -c '^W ' "$T/r.rec") activities, none past the window's samples; the record replays"
