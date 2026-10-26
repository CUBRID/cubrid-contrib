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
# Switching targets (< >, -t over several instances, a server that died) must not carry one
# target's view into the next.  A smaps another account owns is refused at open; the
# previous target's regions must not stand in for it, and perm.limited belongs to the
# target it was judged on.  Run as a non-root account (root re-runs it as hiclass): an own
# child is readable, pid 1 belongs to another account.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
if [ "$(id -u)" = 0 ]; then
    id hiclass >/dev/null 2>&1 || { echo "→ SKIP (root and no second account)"; exit 0; }
    RUN="runuser -u hiclass --"
else RUN=""; fi
TMP=$(mktemp -d /tmp/cub_top-permswitch.XXXXXX); chmod 755 "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static proc_t P;
static void look(int pid){ P.pid=pid; cap_probe(pid); parse_smaps(&P); parse_rollup(&P); }
int main(void){
    int bad=0; pid_t ch=fork(); if(ch==0){ pause(); _exit(0); } usleep(100000);
    look(ch);
    int n1=P.nreg, p1=g_perm_limited;
    printf("  %s   own child: %d regions, perm.limited %d\n",n1>0&&!p1?"ok  ":"FAIL",n1,p1); bad|=!(n1>0&&!p1);
    reset_instance_state(); look(1);            /* P keeps its arrays across a switch, as in the tool */
    int ok = P.nreg==0 && P.reg_dropped==0 && g_perm_limited==1 && P.rss_kb>0;
    printf("  %s   another account's process: %d regions (want 0), rss %lukB from status, perm.limited %d (want 1)\n",
           ok?"ok  ":"FAIL",P.nreg,P.rss_kb,g_perm_limited); bad|=!ok;
    reset_instance_state();                    /* the server died: no probe follows */
    printf("  %s   target gone: perm.limited %d (want 0)\n",g_perm_limited==0?"ok  ":"FAIL",g_perm_limited); bad|=g_perm_limited!=0;
    look(ch);
    ok = P.nreg>0 && g_perm_limited==0;
    printf("  %s   back to the own child: %d regions, perm.limited %d (want >0, 0)\n",ok?"ok  ":"FAIL",P.nreg,g_perm_limited); bad|=!ok;
    kill(ch,SIGKILL); waitpid(ch,NULL,0);
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
chmod 755 "$TMP/t"
if [ "$($RUN stat -c %U /proc/1 2>/dev/null)" = "$($RUN id -un)" ]; then echo "→ SKIP (pid 1 belongs to this account)"; exit 0; fi
if $RUN "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
