#!/bin/bash
# t18: a CS demoted by its owner (HA log flush) counts as the owner's hold:
# held samples, the verdict's daemon share, the owner fields (no server).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_sampler\.c$')
gcc -O1 -Wall -I"$HERE/../src" -o "$T/t18" "$HERE/t18_demote_holder.c" "${SRCS[@]}" -lm \
  || { echo "t18 FAIL: build"; exit 1; }
LANG=C "$T/t18"
