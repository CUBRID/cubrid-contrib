#!/bin/sh
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
# collect-offsets.sh - collect heap detail B offset tables from the DWARF of CUBRID installations (10.0+).
#
# Usage: tools/collect-offsets.sh [-o outdir] <CUBRID root | libcubrid.so> ...
#   e.g. tools/collect-offsets.sh -o offsets /root/CUBRID-1020_9088 /root/CUBRID
#
# Output: <outdir>/cubrid-<version>.tbl   (default outdir: <this script>/../offsets)
#   Each file carries #meta version=<version> at its head; cub_top picks the table matching
#   the detected version from --offsets <dir> (or an offsets/ beside the executable).
#   Whether it really fits is decided by cub_top's three runtime gates (symbol, alignment,
#   paramdump), so this script only extracts, names and collects them.
#
# Requires: the target lib must have .debug_info (most official builds do).
#        A stripped build is skipped with a reason; that version runs through cub_top's probe path.
#
# Remote collection: with gcc on the target, copy this script and dwoff.c and run it there;
#            otherwise build a static dwoff here (cc -static tools/dwoff.c), send it, run it
#            and bring back the .tbl (the tables are text and move freely between machines).
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
OUT="$DIR/../offsets"
if [ "${1:-}" = "-o" ]; then OUT=$2; shift 2; fi
[ $# -ge 1 ] || { echo "usage: $0 [-o outdir] <CUBRID root | libcubrid.so> ..."; exit 1; }
mkdir -p "$OUT" || exit 1

# Prepare dwoff, building it here if absent (single file, no dependencies)
DWOFF="$DIR/dwoff"
if [ ! -x "$DWOFF" ] || [ "$DIR/dwoff.c" -nt "$DWOFF" ]; then
    ${CC:-cc} -O2 -std=gnu99 -o "$DWOFF" "$DIR/dwoff.c" || { echo "dwoff build failed"; exit 1; }
fi

ok=0; skip=0
for a in "$@"; do
    # An install root resolves to lib/libcubrid.so; a file is used as given
    if [ -d "$a" ]; then LIB="$a/lib/libcubrid.so"
    else LIB="$a"; fi
    [ -e "$LIB" ] || { echo "skip    $a - no libcubrid.so"; skip=$((skip+1)); continue; }
    LIB=$(readlink -f "$LIB")

    # Version detection from the "N.N.N.NNNN" release string in the lib rodata (cub_top uses the same source)
    VER=$(strings -a "$LIB" 2>/dev/null | grep -m1 -oE '^(9|1[0-9])\.[0-9]+\.[0-9]+\.[0-9]{3,6}$')
    [ -n "$VER" ] || { echo "skip    $LIB - no version string found"; skip=$((skip+1)); continue; }
    case "$VER" in 9.*) echo "skip    $LIB - $VER (below 10.0)"; skip=$((skip+1)); continue;; esac

    # Confirm DWARF is present
    if ! readelf -S "$LIB" 2>/dev/null | grep -q '\.debug_info'; then
        echo "skip    $LIB - $VER stripped build (no .debug_info); cub_top uses the probe path"
        skip=$((skip+1)); continue
    fi

    F="$OUT/cubrid-$VER.tbl"
    echo "collect $VER  <-  $LIB"
    if "$DWOFF" "$LIB" --emit-tbl > "$F.tmp" 2>/dev/null && [ -s "$F.tmp" ]; then
        # Insert the version meta at the head; cub_top's directory selection reads this line
        { head -1 "$F.tmp"; echo "#meta version=$VER"; tail -n +2 "$F.tmp"; } > "$F"
        rm -f "$F.tmp"
        N=$(grep -c '^OFF\|^SZ' "$F" 2>/dev/null || echo 0)
        echo "        -> $F  ($N entries)"
        ok=$((ok+1))
    else
        rm -f "$F.tmp"
        echo "        -> extraction failed ($VER) - DWARF present but the target symbols/types were not found"
        skip=$((skip+1))
    fi
done
echo "done: $ok tables generated, $skip skipped  ->  $OUT"
[ $ok -gt 0 ]
