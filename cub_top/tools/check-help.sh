#!/bin/bash
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
# Regression: help.
#   -h  is a CLI help: the usage line and the option list, printed in place - no pager,
#       no explanation sections, no colour when it is not a terminal.
#   h   (live) carries the explanations.  hout()'s fixed buffer (HOUT_MAX) and the
#       collection array (HELPMAX) truncate silently, so both live help texts are
#       collected here without a terminal and checked to arrive whole.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d /tmp/cub_top-help.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
gcc -O2 -std=gnu99 -o "$TMP/bin" "$SRC" 2>/dev/null || { echo "-> FAIL (compile failed)"; exit 1; }
bad=0

# ---- 1) -h: the option page only ----
ERR="$("$TMP/bin" -h 2>&1 >/dev/null)"; OUT="$("$TMP/bin" -h 2>/dev/null)"; RC=$?
echo "-h: $(printf '%s\n' "$OUT" | wc -l) lines"
[ "$RC" -eq 0 ] && [ -z "$ERR" ] && echo "  ok  exit 0, nothing on stderr" || { echo "  FAIL exit $RC / stderr: $ERR"; bad=1; }
for opt in "-t, --tty" "-d, --dump" "--dump-json" "-b, -i, --live" "-p, --plot" "--replay" "-h, --help"; do
    printf '%s\n' "$OUT" | grep -qF -- "$opt" || { echo "  FAIL option missing from -h: $opt"; bad=1; }
done
[ $bad -eq 0 ] && echo "  ok  every mode option listed"
if printf '%s' "$OUT" | grep -q '──'; then echo "  FAIL an explanation section leaked into -h"; bad=1
else echo "  ok  no explanation section (those are in live h)"; fi
if printf '%s' "$OUT" | grep -q "$(printf '\033')"; then echo "  FAIL ANSI escapes in piped -h"; bad=1
else echo "  ok  no ANSI escapes when piped"; fi
# Without a mode the same page is printed; a DB name alone is a usage error (exit 2).
[ "$("$TMP/bin" 2>/dev/null)" = "$OUT" ] && echo "  ok  no arguments prints the same page" || { echo "  FAIL no-argument output differs from -h"; bad=1; }
"$TMP/bin" somedb >/dev/null 2>&1; [ $? -eq 2 ] && echo "  ok  a DB name without a mode exits 2" || { echo "  FAIL a DB name without a mode did not exit 2"; bad=1; }

# ---- 2) live h: both help texts collected without a terminal ----
cat >"$TMP/h.c" <<C
#define main cub_top_main
#include "$SRC"
#undef main
int main(int c,char**v){ (void)c;
    g_hn=0; g_hcap=1; usage_modal(v[1][0]=='p'?2:1); g_hcap=0;
    for(int i=0;i<g_hn;i++) puts(g_hln[i]?g_hln[i]:"");
    fprintf(stderr,"%d\n",g_hn); return 0; }
C
gcc -O2 -std=gnu99 -w -o "$TMP/h" "$TMP/h.c" 2>/dev/null || { echo "-> FAIL (help harness compile failed)"; exit 1; }
HM=$(grep -m1 '#define HELPMAX' "$SRC" | awk '{print $3}')
for mode in dash plot; do
    LIVE="$("$TMP/h" $mode 2>"$TMP/n")"; N=$(cat "$TMP/n")
    echo "live h ($mode): $N lines"
    printf '%s' "$LIVE" | grep -q 'HELPMAX 에서 잘렸다' && { echo "  FAIL HELPMAX exceeded"; bad=1; }
    [ "$N" -lt $((HM-20)) ] && echo "  ok  line count has margin ($N < HELPMAX $HM - 20)" || { echo "  FAIL $N lines is at or near HELPMAX $HM"; bad=1; }
    printf '%s' "$LIVE" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1 && echo "  ok  UTF-8 intact" || { echo "  FAIL UTF-8 broken"; bad=1; }
    if [ $mode = dash ]; then
        req="대시보드 · 키|파라미터 읽기 (설정 축|여러 데이터베이스|CPU 와 load|wa  iowait|D스레드 n/m|I/O 판정문|파라미터 귀속률|프로세스 박스 — top 과 같은 RSS|관측 등급|각 항목이 알려주는 것"
    else
        req="시계열 · 키|파라미터 읽기 (설정 축|여러 데이터베이스"
    fi
    miss=0; IFS='|'; for sec in $req; do printf '%s' "$LIVE" | grep -qF "$sec" || { echo "  FAIL missing: $sec"; miss=1; bad=1; }; done; unset IFS
    [ $miss -eq 0 ] && echo "  ok  required sections present"
done

[ $bad -eq 0 ] && { echo "→ PASS"; exit 0; }
echo "→ FAIL"; exit 1
