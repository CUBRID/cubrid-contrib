#!/bin/bash
# t20: overlong, surrogate and past-U+10FFFF sequences from outside are
# broken UTF-8: sanitized to '?' and escaped in JSON, so --json/--dump stay
# valid UTF-8 (no server).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v '/cub_ctv\.c$')
gcc -O1 -Wall -I"$HERE/../src" -o "$T/t20" "$HERE/t20_utf8_ranges.c" "${SRCS[@]}" -lm \
  || { echo "t20 FAIL: build"; exit 1; }
LANG=C "$T/t20" "$T/out.json" || exit 1
python3 -c 'import json,sys; v=json.loads(open(sys.argv[1],"rb").read().decode("utf-8")); print(len(v))' "$T/out.json" >/dev/null \
  || { echo "t20 FAIL: the JSON output is not valid UTF-8 JSON"; exit 1; }
echo "t20 PASS: RFC 3629 second-byte ranges: overlong, surrogate, past U+10FFFF broken; edges kept; JSON valid"
