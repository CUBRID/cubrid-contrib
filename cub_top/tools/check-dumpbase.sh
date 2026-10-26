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
# A one-shot run's turnover baseline is the previous --bcb-dump file.  The interval between
# the two snapshots is taken to the nanosecond: runs 0.2-1s apart gave 0 (refused) or 1
# (a wrong rate) in whole seconds.  Live keeps its baseline in memory and does not re-read
# and sort the file every frame.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-dumpbase.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
/* A snapshot of n pages, the previous one written dt seconds before "now" */
static void snap(const char *path,int first,int n,int64_t s,int64_t ns){
    FILE *f=fopen(path,"wb");
    struct { char magic[8]; uint32_t version,reclen; int64_t ts_sec,ts_nsec; int32_t nbuf,nrec,dirty,pagesize;
             uint64_t la,ln,le,od; char db[32]; } h; memset(&h,0,sizeof h);
    memcpy(h.magic,"CBCBMAP1",8); h.version=1; h.reclen=32; h.ts_sec=s; h.ts_nsec=ns; h.nbuf=n; h.nrec=n; snprintf(h.db,32,"tdb");
    fwrite(&h,sizeof h,1,f);
    for(int i=0;i<n;i++){ pgrec_t r; memset(&r,0,sizeof r); r.volid=1; r.pageid=first+i; fwrite(&r,sizeof r,1,f); }
    fclose(f);
}
int main(void){
    int bad=0, ok; static pgrec_t rec[1000];
    snprintf(g_bcb_dump,sizeof g_bcb_dump,"%s/d.bin",getenv("TMPD")); snprintf(g_cur_db,sizeof g_cur_db,"tdb");
    PG.ok=1; PG.rec=rec; PG.reccap=1000; PG.nrec=1000; PG.nbuf=1000;
    for(int i=0;i<1000;i++){ rec[i].volid=1; rec[i].pageid=100+i; }   /* now: pages 100..1099 */
    PG.when.tv_sec=1000; PG.when.tv_nsec=300000000;                  /* 1000.3 s */
    snap(g_bcb_dump,0,1000,999,800000000);                          /* 999.8 s: 0.5 s before, pages 0..999 */
    g_pd_async=0; CAP2.miss_pps=-1;
    int r=cap_turnover_from_dump();
    ok = r==1 && fabs(CAP2.miss_pps-200.0)<0.01 && fabs(CAP2.turnover_pct-10.0)<0.01;
    printf("  %s   0.5 s apart across a second boundary: 100 pages admitted -> %.1f pages/s (want 200), turnover %.1f%%\n",
           ok?"ok  ":"FAIL",CAP2.miss_pps,CAP2.turnover_pct); bad|=!ok;
    snap(g_bcb_dump,0,1000,1000,100000000);                         /* 0.2 s before, same second */
    r=cap_turnover_from_dump();
    ok = r==1 && fabs(CAP2.miss_pps-500.0)<0.01;
    printf("  %s   0.2 s apart in the same second: %.1f pages/s (want 500; whole seconds refused it)\n",ok?"ok  ":"FAIL",CAP2.miss_pps); bad|=!ok;
    snap(g_bcb_dump,0,1000,999,0);                                  /* 1.3 s before: a baseline one-shot would use */
    g_pd_async=1; CAP2.miss_pps=-7;
    r=cap_turnover_from_dump();
    ok = r==0 && CAP2.miss_pps==-7;
    printf("  %s   live: the dump file is not read for a baseline\n",ok?"ok  ":"FAIL"); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if TMPD="$TMP" "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
