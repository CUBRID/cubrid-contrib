#!/bin/bash
# t12: FIELD and SCHED rules (no server)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
gcc -O1 -Wall -I"$HERE/../src" -o "$T/t12" "$HERE/t12_field_sched.c" \
  "$HERE/../src/ctv_field.c" "$HERE/../src/ctv_sched.c" || { echo "t12 FAIL: build"; exit 1; }
"$T/t12"
