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
# The live screen and its terminal.
#  - escape sequences for the arrow and page keys are read as one key each; an unknown
#    ESC prefix (Esc ] x) is returned as the keys typed
#  - Ctrl-Z gives the terminal back (cooked mode, cursor, colours) and fg takes it again
#    with a clean screen; SIGHUP/SIGQUIT leave the normal way, abort() restores then aborts,
#    and so do USR1, USR2, ALRM, XCPU and PIPE (their default action ends the run)
#  - a terminal under 60 columns or 10 rows is told so instead of drawn with wrapping
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"; REC="$ROOT/examples/10-recording.jsonl"
TMP=$(mktemp -d /tmp/cub_top-tui.XXXXXX); trap 'rm -rf "$TMP"' EXIT
bad=0; ok(){ echo "  ok   $1"; }; no(){ echo "  FAIL $1"; bad=1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static int keys(const char *in,int *out,int max){
    int p[2]; if(pipe(p)) return -1; if(write(p[1],in,strlen(in))<0){} close(p[1]); dup2(p[0],0); close(p[0]);
    g_kqn=0; int n=0;
    for(int i=0;i<40 && n<max;i++){ int k=tui_key(); if(k==-1 && g_kqn==0){ unsigned char c; if(read(0,&c,0)<0){} }
        if(k>=0) out[n++]=k; else if(k==-1){ struct pollfd pf={0,POLLIN,0}; if(poll(&pf,1,0)<=0 && g_kqn==0) break; } }
    return n;
}
int main(void){
    int bad=0, k[64], n;
    n=keys("\033[A\033[6~q\033]x",k,64);
    int ok = n==6 && k[0]==TUI_KEY_UP && k[1]==TUI_KEY_PGDN && k[2]=='q' && k[3]==0x1b && k[4]==']' && k[5]=='x';
    printf("  %s   keys: Up, PgDn, q, then Esc ] x as three keys (%d read)\n",ok?"ok  ":"FAIL",n); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
"$TMP/t" || bad=1
command -v script >/dev/null && [ -s "$REC" ] || { echo "  SKIP pty checks (no script or recording)"; [ $bad = 0 ] && echo "→ PASS" || { echo "→ FAIL"; exit 1; }; exit 0; }
cp "$ROOT/cub_top" "$TMP/cub_top_tui"; BIN="$TMP/cub_top_tui"
# Small terminals
for sz in "50 30" "120 6"; do set -- $sz
    timeout -s INT 3 script -qc "stty cols $1 rows $2; '$BIN' --ascii --replay '$REC' -b --replay-speed 1" /dev/null > "$TMP/small" 2>&1
    if grep -q "needs at least" "$TMP/small" && ! grep -q "OS context" "$TMP/small"; then ok "${1}x$2: told the terminal is too small, nothing drawn"
    else no "${1}x$2: drawn anyway"; fi
done
# Ctrl-Z / fg, SIGHUP, abort: run under a pty and look at the tty modes from outside
run(){ # sig...: start under a pty, send each signal in turn, report the tty modes after each
    script -qc "stty cols 140 rows 50; '$BIN' --ascii --replay '$REC' -b --replay-speed 0.05" "$TMP/typescript" >/dev/null 2>&1 &
    sp=$!; i=0; while [ $i -lt 30 ]; do pid=$(pgrep -n -f "^$BIN "); [ -n "$pid" ] && break; sleep 0.1; i=$((i+1)); done
    sleep 1; tty=$(readlink /proc/$pid/fd/0)
    for s in "$@"; do kill -$s $pid 2>/dev/null; sleep 0.6
        printf '%s:%s ' "$s" "$(stty -F "$tty" -a 2>/dev/null | grep -o -- '-\?icanon' | head -1)"; done
    kill -TERM $pid 2>/dev/null; sleep 0.3; kill $sp 2>/dev/null; wait $sp 2>/dev/null
}
r=$(run TSTP CONT)
case "$r" in "TSTP:icanon CONT:-icanon "*) ok "Ctrl-Z: cooked mode back while stopped, raw again after fg ($r)";; *) no "Ctrl-Z/fg tty modes: $r";; esac
n=$(grep -ao "$(printf '\033\[2J')" "$TMP/typescript" | wc -l); [ "$n" -ge 2 ] && ok "fg clears the screen for the next frame" || no "no clear after fg ($n)"
ends(){ # sig: the process leaves and restores the terminal (cursor shown again) after it
    script -qc "stty cols 140 rows 50; '$BIN' --ascii --replay '$REC' -b --replay-speed 0.05" "$TMP/ts2" >/dev/null 2>&1 &
    sp=$!; i=0; while [ $i -lt 30 ]; do pid=$(pgrep -n -f "^$BIN "); [ -n "$pid" ] && break; sleep 0.1; i=$((i+1)); done
    sleep 1; n0=$(grep -ao "$(printf '\033\[?25h')" "$TMP/ts2" | wc -l)
    kill -$1 $pid 2>/dev/null; i=0; while kill -0 $pid 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
    gone=1; kill -0 $pid 2>/dev/null && gone=0; sleep 0.2
    n1=$(grep -ao "$(printf '\033\[?25h')" "$TMP/ts2" | wc -l); kill $sp 2>/dev/null; wait $sp 2>/dev/null
    [ $gone = 1 ] && [ "$n1" -gt "$n0" ]
}
ends HUP && ok "SIGHUP: leaves the normal way, terminal restored" || no "SIGHUP: not left or not restored"
ends QUIT && ok "SIGQUIT: leaves the normal way, terminal restored" || no "SIGQUIT: not left or not restored"
ends ABRT && ok "abort: terminal restored, then the default action" || no "abort: not restored"
# Signals no handler used to take: their default action ended the run with the terminal raw
for s in USR1 USR2 ALRM XCPU PIPE; do
    ends $s && ok "SIG$s: terminal restored, then the default action" || no "SIG$s: not left or not restored"
done
if [ $bad = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
