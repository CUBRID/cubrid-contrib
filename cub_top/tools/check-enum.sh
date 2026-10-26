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
# Instance enumeration has no count limit: every cub_server is listed however many CUBRID
# processes there are, a PL met before its server joins it, a PL with no server is not
# listed, and every process's CPU is tracked.  enum_instances() walks a fake /proc built
# here (the source copy's /proc paths point at it): 600 CAS, 10 servers, PLs, a master.
# Their memory is RSS as top shows it, from /proc/PID/status and stat.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-enum.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
F="$TMP/proc"; mkdir -p "$F"
mk(){ # pid comm db rss_kb
    mkdir -p "$F/$1"; printf '%s\n' "$2" > "$F/$1/comm"
    printf '%s\000%s\000' "$2" "$3" > "$F/$1/cmdline"
    printf 'Name:\t%s\nVmRSS:\t%s kB\nRssFile:\t0 kB\nVmSwap:\t0 kB\n' "$2" "$4" > "$F/$1/status"
    # stat: utime 7, stime 3, rss (field 24) = rss_kb / page size in pages
    printf '%s (%s) S 1 1 1 0 -1 0 0 0 0 0 7 3 0 0 20 0 1 0 0 0 %s 0\n' "$1" "$2" $(( $4 * 1024 / $(getconf PAGESIZE) )) > "$F/$1/stat"
}
mk 50 cub_pl db03 1000            # a PL met before its server
mk 60 cub_pl ghost 1000           # a PL with no server
i=100; while [ $i -lt 700 ]; do mk $i cub_cas - 12; i=$((i+1)); done
s=0; while [ $s -lt 10 ]; do mk $((1000+s)) cub_server db0$s 2000; s=$((s+1)); done
mk 2000 cub_master - 8
sed "s|\"/proc|\"$F|g" "$SRC" > "$TMP/src.c"
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
int main(void){
    static tier_t T; int bad=0;
    snprintf(g_cur_db,sizeof g_cur_db,"db03");
    for(int round=0;round<2;round++){ memset(&T,0,sizeof T); g_enum_T=&T; g_enum_dt=1.0; enum_instances(); g_enum_T=NULL; }
    int ghost=0, d3=-1;
    for(int i=0;i<g_ninst;i++){ if(!strcmp(g_inst[i].db,"ghost")) ghost=1; if(!strcmp(g_inst[i].db,"db03")) d3=i; }
    printf("  servers listed %d (want 10), CAS counted %d (want 600), CPU-tracked processes %d (want 602)\n",g_ninst,T.ncas,g_tcpun);
    printf("  db03 PL attached: %d (rss %.0fK), PL without a server listed: %s\n",d3>=0?g_inst[d3].pl_n:0,d3>=0?g_inst[d3].pl_rss/1024:0,ghost?"yes":"no");
    printf("  tier RSS (as top): CAS %.0fK (want 7200K), master %.0fK (want 8K), PL %.0fK (want 1000K); db00 RSS %.0fK (want 2000K)\n",
           T.cas/1024,T.master/1024,T.pl/1024,g_ninst?g_inst[0].rss/1024:0);
    if(g_ninst!=10 || T.ncas!=600 || ghost || d3<0 || g_inst[d3].pl_n!=1 || g_tcpun!=602) bad=1;
    if(T.cas!=600*12*1024.0 || T.master!=8*1024.0 || T.pl!=1000*1024.0 || g_inst[0].rss!=2000*1024.0) bad=1;
    for(int i=1;i<g_ninst;i++) if(strcmp(g_inst[i-1].db,g_inst[i].db)>0) bad=1;   /* sorted by db */
    return bad;
}
C
gcc -O0 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
