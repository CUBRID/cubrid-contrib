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
# Negative test: with an offset deliberately corrupted, do the gates actually fire?
# An L2 alignment violation is caught at runtime (method B disabled); an aligned but wrong value by L3.
# The C implementation only.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
# Source location: src/.
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-guard.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
fail=0
say(){ printf "  %-34s %s\n" "$1" "$2"; }
# The running server's build: the builtin table is made "this build" where a test needs it tried first
VER=$("$ROOT/cub_top" -d 2>/dev/null | sed -n 's/.*cubrid\.version="\([0-9.]*\)".*/\1/p' | head -1)
BUILTIN_TAG='{"table-11.5","11.5.0.2374",SYMN,0,'
BUILTIN_SSZ='{648,608,48,16,4,640,416,408,4,4,184,1360,56,16968}'
grep -qF "$BUILTIN_TAG" "$SRC" && grep -qF "$BUILTIN_SSZ" "$SRC" || { echo "  builtin table literal not found"; echo "→ FAIL"; exit 1; }

# ---- Negative 1: alignment violation (12->11); the builtin table must fail the gate.
#    A runtime DWARF fallback can rescue that slot, so what is tested is not
#    "B disabled" but "not adopted as table-11.5":
#      heapB.table=dwarf-runtime     gate fired, DWARF rescued it (PASS)
#      heapB.table=offsets-embedded  gate fired, an embedded table rescued it (PASS)
#      heapB.enabled=0               gate fired, no fallback available (PASS)
#      heapB.table=table-11.5        the corrupted table was adopted (FAIL)
# DWARF offsets are kept in memory only, so the test build gets a hook instead of a cache
# to corrupt: GUARD_DW_BAD=1 makes the DWARF candidate answer with every offset wrong.
# The builtin table claims this build and records no sizes, so the alignment gate is the one tried.
sed -e 's/#define OFF_LF_ALLOCCNT     12/#define OFF_LF_ALLOCCNT     11/' \
    -e "s|{\"table-11.5\",\"11.5.0.2374\",|{\"table-11.5\",\"${VER:-11.5.0.2374}\",|" \
    -e "s|$BUILTIN_SSZ|{0}|" \
    -e 's|static int dw_load_or_extract(const char\*so,long out\[\],char\*why,size_t wn){|&\n    if(getenv("GUARD_DW_BAD")){ for(int i_=0;i_<DW_N;i_++) out[i_]=-1; for(int i_=0;i_<NTG;i_++) out[i_]=4; (void)so; (void)why; (void)wn; return 1; }|' \
    "$SRC" > "$TMP/bad1.c"
grep -q 'GUARD_DW_BAD' "$TMP/bad1.c" || { say "C misalignment build" "FAIL(DWARF hook not inserted)"; fail=1; }
if gcc -O2 -std=gnu99 -I "$(dirname "$SRC")" -o "$TMP/bad1" "$TMP/bad1.c" 2>/dev/null; then
  raw=$("$TMP/bad1" -d 2>/dev/null)
  tbl=$(printf '%s' "$raw" | grep -o 'heapB.table="[^"]*"')
  en=$(printf '%s' "$raw" | grep -o 'heapB.enabled=[01]')
  if printf '%s' "$raw" | grep -q '^server.up=0'; then
    say "C misalignment -> table rejected" "SKIP(cub_server not running)"
  elif [ "$tbl" = 'heapB.table="table-11.5"' ]; then
    say "C misalignment -> table rejected" "FAIL(corrupted table adopted)"; fail=1
  elif [ "$tbl" = 'heapB.table="dwarf-runtime"' ] \
    || [ "$tbl" = 'heapB.table="offsets-embedded"' ] \
    || [ "$en" = "heapB.enabled=0" ]; then
    say "C misalignment -> table rejected" "PASS($tbl)"
  else
    say "C misalignment -> table rejected" "FAIL(undecidable: $tbl $en)"; fail=1
  fi

  # ---- Negative 1b: with the DWARF candidate corrupted too, the fallback also fails -> B disabled ----
  if true; then
    raw=$(GUARD_DW_BAD=1 "$TMP/bad1" -d 2>/dev/null)
    en=$(printf '%s' "$raw" | grep -o 'heapB.enabled=[01]')
    tbl2=$(printf '%s' "$raw" | grep -o 'heapB.table="[^"]*"')
    # With the DWARF candidate corrupted too, the result must be
    #   an embedded table (a correct table, not the corrupted one),
    #   a downgrade to probe-runtime (count-only), or B off.
    # What is tested is that the CORRUPTED table invents no byte values - a rescue by a
    # table that is known good is the fallback working, not the gate leaking.  So when
    # an embedded table takes over, the items sized by the table (num x struct size)
    # are compared against the healthy build.  Not the total: it includes the plan
    # cache's live usage, which moves between the two runs on any active server.
    sized() { printf '%s' "$1" | grep -oE 'heapB\.(lock_table_tran|conn_entries_css)_bytes=[0-9]+' | tr '\n' ' '; }
    tot=$(printf '%s' "$raw" | grep -o 'heapB.total_bytes=[0-9]*')
    if [ "$tbl2" = 'heapB.table="probe-runtime"' ] && [ "$tot" = "heapB.total_bytes=0" ]; then
        say "corrupted DWARF -> probe downgrade" "PASS(count-only)"
    elif [ "$en" = "heapB.enabled=0" ]; then
        say "corrupted DWARF -> probe downgrade" "PASS(B OFF)"
    elif [ "$tbl2" = 'heapB.table="offsets-embedded"' ]; then
        good=$(sized "$("$ROOT/cub_top" -d 2>/dev/null)"); mine=$(sized "$raw")
        if [ -n "$good" ] && [ "$mine" = "$good" ]; then
            say "corrupted DWARF -> embedded rescue" "PASS(sized items match the healthy build)"
        else
            say "corrupted DWARF -> embedded rescue" "FAIL($mine vs healthy $good)"; fail=1
        fi
    else
        say "corrupted DWARF -> probe downgrade" "FAIL($tbl2 $tot)"; fail=1
    fi
  fi
else
    say "C misalignment build" "FAIL(compile failed - the gate was not exercised)"; fail=1
fi


# ---- Negative 2: aligned but wrong offset (12->16); the L3 comparison must catch it ----
sed 's/^#define OFF_LF_ALLOCCNT        12/#define OFF_LF_ALLOCCNT        16/' "$DIR/rules-11.5.h" > "$TMP/gold_bad.h"
if "$DIR/check-offsets.sh" "$TMP/gold_bad.h" >/dev/null 2>&1
then say "L3 detects a wrong table" "FAIL(let it through)"; fail=1
else say "L3 detects a wrong table" "PASS"; fi


# ---- Negative 3: a table missing a field (older tables lack the xcache usage fields) ----
#    The builtin table is made non-matching so the table file is used.  A missing offset
#    must not be read: xcache shows the count only, session/catalog are omitted.
TBL="$ROOT/offsets/cubrid-${VER:-none}.tbl"; [ -f "$TBL" ] || TBL="$ROOT/offsets/cubrid-11.5.0.2374.tbl"
sed 's/{"table-11.5","11.5.0.2374",SYMN,0,/{"table-11.5","99.9",SYMN,0,/' "$SRC" > "$TMP/miss.c"
grep -vE '^OFF_(XCACHE_USAGE|XCACHE_CLONE|LF_ALLOCCNT|LF_ALLOCCNT2) ' "$TBL" > "$TMP/miss.tbl"
if [ ! -f "$TBL" ]; then
    say "missing offset -> field not read" "SKIP(no 11.5 table)"
elif gcc -O2 -std=gnu99 -I "$(dirname "$SRC")" -o "$TMP/miss" "$TMP/miss.c" 2>/dev/null; then
    raw=$("$TMP/miss" -d --offsets "$TMP/miss.tbl" 2>/dev/null)
    if printf '%s' "$raw" | grep -q '^server.up=0'; then
        say "missing offset -> field not read" "SKIP(cub_server not running)"
    elif ! printf '%s' "$raw" | grep -q 'heapB.table="offsets-file"'; then
        say "missing offset -> field not read" "SKIP(table file not adopted: $(printf '%s' "$raw" | grep -o 'heapB.table="[^"]*"'))"
    else
        bad=$(printf '%s' "$raw" | grep -oE 'heapB\.(plan_cache_xcache|session_table|catalog_cache)_bytes=[0-9]+' | tr '\n' ' ')
        if [ -n "$bad" ]; then
            say "missing offset -> field not read" "FAIL(read anyway: $bad)"; fail=1
        elif printf '%s' "$raw" | grep -q 'heapB.plan_cache_xcache_count='; then
            say "missing offset -> field not read" "PASS(xcache count only)"
        else
            say "missing offset -> field not read" "FAIL(xcache count missing)"; fail=1
        fi
    fi
else
    say "missing offset build" "FAIL(compile failed)"; fail=1
fi


# ---- Negatives 4-6: choosing a table for a build (num_buffers at offset 0 cannot tell builds apart) ----
# Test binaries live in $TMP, so no offsets/ beside them; the embedded tables are what they carry.
# DWARF, when one of them reaches it, is extracted for the run (a few seconds each, no file).
gate_build(){ # name sed-expr... : build $TMP/$1 from the source with the given edits
    n=$1; shift
    sed -e "s|^@@never@@||" "$@" "$SRC" > "$TMP/$n.c"
    gcc -O2 -std=gnu99 -w -I "$TMP/inc_$n" -I "$(dirname "$SRC")" -o "$TMP/$n" "$TMP/$n.c" 2>/dev/null
}
tblof(){ printf '%s' "$1" | grep -o 'heapB.table="[^"]*"' | sed 's/heapB.table=//;s/"//g'; }
if [ -z "$VER" ] || "$ROOT/cub_top" -d 2>/dev/null | grep -q '^server.up=0'; then
    say "table choice by build" "SKIP(cub_server not running)"
elif [ "$VER" = "11.5.0.2374" ]; then
    say "table choice by build" "SKIP(the running build is the builtin table's own)"
else
    # 4. The builtin table claims this build but carries 11.5.0.2374's symbol sizes: rejected on size.
    mkdir -p "$TMP/inc_g4"
    if gate_build g4 -e "s|{\"table-11.5\",\"11.5.0.2374\",|{\"table-11.5\",\"$VER\",|"; then
        raw=$("$TMP/g4" -d 2>/dev/null); t=$(tblof "$raw")
        if [ "$t" = "table-11.5" ]; then say "size gate: other build's sizes" "FAIL(adopted table-11.5)"; fail=1
        else say "size gate: other build's sizes" "PASS($t)"; fi
    else say "size gate build" "FAIL(compile failed)"; fail=1; fi
    # 5. No table of this build: its own DWARF comes before tables of another build.
    mkdir -p "$TMP/offs5" "$TMP/inc_g5"
    for f in "$ROOT"/offsets/cubrid-*.tbl; do case "$f" in *"cubrid-$VER.tbl") ;; *) cp "$f" "$TMP/offs5/";; esac; done
    sh "$ROOT/offsets/gen-embedded.sh" -o "$TMP/inc_g5/offsets_embedded.h" "$TMP/offs5" >/dev/null 2>&1
    if gate_build g5; then
        raw=$("$TMP/g5" -d 2>/dev/null); t=$(tblof "$raw")
        case "$t" in
          dwarf-runtime) say "no table of this build -> DWARF" "PASS($t)";;
          table-11.5|offsets-embedded) say "no table of this build -> DWARF" "FAIL(another build's table: $t)"; fail=1;;
          *) say "no table of this build -> DWARF" "SKIP(no DWARF here: $t)";;
        esac
    else say "DWARF order build" "FAIL(compile failed)"; fail=1; fi
    # 6. Neither a table of this build nor DWARF: another build's tables fail on size, never adopted.
    mkdir -p "$TMP/inc_g6"; cp "$TMP/inc_g5/offsets_embedded.h" "$TMP/inc_g6/" 2>/dev/null
    if gate_build g6 -e 's/if(dwst==0) dwst=dw_load_or_extract(so,dwv,dwwhy,sizeof dwwhy);/if(dwst==0) dwst=2;/'; then
        raw=$("$TMP/g6" -d 2>/dev/null); t=$(tblof "$raw")
        case "$t" in
          table-11.5|offsets-embedded|offsets-file|dwarf-runtime) say "no table, no DWARF -> not adopted" "FAIL($t)"; fail=1;;
          *) say "no table, no DWARF -> not adopted" "PASS(${t:-B off})";;
        esac
    else say "no-DWARF build" "FAIL(compile failed)"; fail=1; fi
fi


[ $fail -eq 0 ] && echo "→ PASS" || { echo "→ FAIL"; exit 1; }
