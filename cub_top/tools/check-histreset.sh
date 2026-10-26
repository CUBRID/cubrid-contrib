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
# History belongs to one process.  After a restart (or a switch, or the server going down) the
# old samples must not be compared with the new process: a dead server's PSS against the new
# one reads as "server -X GB - work memory / caches released", and the flush minimum and the
# plots mix two processes.  The rule is the pid the frame belongs to, so a replayed recording
# with a restart in it follows it too.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-histreset.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
static void push(int pid,double pss_gb,double t){ P.pid=pid; P.pss_kb=(unsigned long)(pss_gb*1024*1024); P.rss_kb=P.pss_kb; P.vsize_kb=P.pss_kb*2; hist_push(t,&P,&M,&T,&IO); }
int main(void){
    int bad=0, ok; char o[400]; const char*col;
    g_ascii=1;
    for(int i=0;i<10;i++) push(100,6.0,i);                 /* the old server at 6 GB */
    for(int i=0;i<6;i++)  push(200,1.0,10+i);              /* restarted under a new pid at 1 GB */
    vd_mem(o,sizeof o,&col);
    ok = HN==6 && !strstr(o,"released") && HS[0].srv==1.0*1024*1024*1024;
    printf("  %s   restart (new pid): history %d sample(s), verdict \"%s\"\n",ok?"ok  ":"FAIL",HN,o); bad|=!ok;
    push(200,1.0,20); unsigned g=g_inst_gen; reset_instance_state(); push(200,1.0,21);   /* same pid, new target generation */
    ok = HN==1 && g_inst_gen!=g;
    printf("  %s   a new target generation clears it even if the pid number repeats (%d)\n",ok?"ok  ":"FAIL",HN); bad|=!ok;
    push(-1,0,22);
    ok = HN==1; printf("  %s   server down: a fresh history (%d)\n",ok?"ok  ":"FAIL",HN); bad|=!ok;
    for(int i=0;i<5;i++) push(300,2.0,30+i);
    ok = HN==5; printf("  %s   up again: only the new process (%d)\n",ok?"ok  ":"FAIL",HN); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
rc=0; "$TMP/t" || rc=1
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
