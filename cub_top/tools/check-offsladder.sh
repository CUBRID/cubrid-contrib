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
# The offset-table ladder against a running server: exact build -> DWARF -> another build
# whose recorded symbol sizes all match -> probe.  DWARF and the embedded tables are turned
# off in a test build, so which table is adopted depends only on the --offsets directory:
#   1. this build's table present: adopted as exact;
#   2. only another 11.5 build's table whose sizes differ (the A-1 case: a 2543 server and
#      the 2374 table, pgbuf_Pool 592 vs 648, CSS_CONN_ENTRY 488 vs 480): never adopted;
#   3. several other builds: the one whose sizes match is adopted even when another has the
#      closer version string or none recorded sizes (11.5.0.2550 asks; 2374 is the builtin);
#   4. DWARF on again: preferred over another build's table.
# Needs a running cub_server of a release with a table in offsets/ (here 11.5.0.2543).
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-ladder.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
rc=0
VER=$("$ROOT/cub_top" -d 2>/dev/null | sed -n 's/.*cubrid\.version="\([0-9.]*\)".*/\1/p' | head -1)
[ -n "$VER" ] && [ "$VER" != unknown ] || { echo "→ SKIP (no running cub_server)"; exit 0; }
[ -f "$ROOT/offsets/cubrid-$VER.tbl" ] || { echo "→ SKIP (no table for $VER in offsets/)"; exit 0; }
# Test build: DWARF extraction answers "not available", no embedded tables
sed -e 's|static int dw_load_or_extract(const char\*so,long out\[\],char\*why,size_t wn){|&\n    if(getenv("LADDER_NO_DWARF")){ snprintf(why,wn,"DWARF off (test)"); return 2; }|' \
    -e 's|#include "offsets_embedded.h"|#include "offsets_embedded.h"\n#undef EMB_TBL_N\n#define EMB_TBL_N (getenv("LADDER_EMB")?(int)(sizeof EMB_TBL/sizeof EMB_TBL[0]):0)|' \
    "$SRC" > "$TMP/t.c"
grep -q 'LADDER_NO_DWARF' "$TMP/t.c" && grep -q 'LADDER_EMB' "$TMP/t.c" || { echo "→ FAIL (test hooks not inserted)"; exit 1; }
gcc -O2 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/ct" "$TMP/t.c" -lm 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (build)"; exit 1; }
run(){ # run <dir> [env...] -> "table version reason"
    d=$1; shift
    out=$(env "$@" "$TMP/ct" -d --offsets "$d" 2>/dev/null)
    t=$(printf '%s' "$out" | sed -n 's/.*heapB\.table="\([^"]*\)".*/\1/p' | head -1)
    v=$(printf '%s' "$out" | sed -n 's/^heapB\.table_version="\([^"]*\)".*/\1/p' | head -1)
    r=$(printf '%s' "$out" | sed -n 's/.*heapB\.reason="\([^"]*\)".*/\1/p' | head -1)
    echo "$t|$v|$r"
}
OTHER=$(ls "$ROOT"/offsets/cubrid-*.tbl | grep -v "cubrid-$VER.tbl" | grep "cubrid-${VER%.*}\." | head -1)
mkd(){ mkdir -p "$TMP/$1"; }
# 1. exact
mkd d1; cp "$ROOT"/offsets/cubrid-1*.tbl "$TMP/d1/"
s=$(run "$TMP/d1" LADDER_NO_DWARF=1)
case "$s" in "offsets-file|$VER|"*) echo "  ok     this build's table present: adopted exact ($s)" ;;
             *) echo "  FAIL   exact table not adopted: $s"; rc=1 ;; esac
# 2. only another build of the same release, sizes differ (A-1)
if [ -n "$OTHER" ]; then
  OV=$(sed -n 's/^#meta version=//p' "$OTHER" | head -1)
  mkd d2; cp "$OTHER" "$TMP/d2/"
  s=$(run "$TMP/d2" LADDER_NO_DWARF=1)
  case "$s" in *"|$OV|"*) echo "  FAIL   $OV adopted for a $VER server whose symbol sizes differ: $s"; rc=1 ;;
               *) echo "  ok     only $OV (sizes differ from $VER) and the builtin: not adopted (${s%%|*}: ${s##*|})" ;; esac
else echo "  SKIP   no other build of ${VER%.*} in offsets/"; fi
# 3. the running build's table relabelled as another patch (11.5.0.9999), next to a table whose
#    version string is closer ($VER.1, picked by the old prefix rule) but records no sizes, and the
#    size-mismatched one: the matching one must win
mkd d3; sed "s/^#meta version=.*/#meta version=${VER%.*}.9999/" "$ROOT/offsets/cubrid-$VER.tbl" > "$TMP/d3/a-match.tbl"
grep -v '^SYMSZ_' "$ROOT/offsets/cubrid-$VER.tbl" | sed "s/^#meta version=.*/#meta version=$VER.1/" > "$TMP/d3/b-nosizes.tbl"   # closest version string, no sizes
[ -n "$OTHER" ] && cp "$OTHER" "$TMP/d3/c-other.tbl"
s=$(run "$TMP/d3" LADDER_NO_DWARF=1)
case "$s" in "offsets-other|${VER%.*}.9999|"*) echo "  ok     several other builds: the one whose sizes match is adopted ($s)" ;;
             *) echo "  FAIL   other-build ranking: $s"; rc=1 ;; esac
# 3b. the same with the embedded tables on: still a size-matching table (embedded exact counts as exact)
s=$(run "$TMP/d3" LADDER_NO_DWARF=1 LADDER_EMB=1)
case "$s" in "offsets-embedded|$VER|"*) echo "  ok     embedded tables on as well: the embedded copy of this build wins as exact ($s)" ;;
             *) echo "  FAIL   with embedded tables: $s"; rc=1 ;; esac
# 4. DWARF available: it comes before another build's table
#    (the one-shot run extracts it first: several seconds)
s=$(run "$TMP/d3")
case "$s" in "dwarf-runtime|"*) echo "  ok     DWARF available: preferred over another build's table (${s%%|*})" ;;
             *"|DWARF"*|*"DWARF:"*) echo "  SKIP   this build's library has no usable DWARF: $s" ;;
             *) echo "  FAIL   DWARF not preferred: $s"; rc=1 ;; esac
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
