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
# Turnover and refill from synthetic BCB snapshots, through cap_turnover() itself.
# Refill (re-admit) counts pages read back within one buffer's worth of admissions, so it is
# the share of misses a buffer twice the size would have kept (LRU stack property, a lower
# bound) - not a verdict on enlarging in general.  A cyclic working set of 1.5x is all kept by
# 2x (100%); one of 2.5x or 3x is not (0%) - nor is an endless scan, and the verdict must say
# it cannot tell those apart instead of calling a larger buffer useless ([A-3]).
# A large buffer must not lose replacements: pages are compared by (volid, pageid), not a hash.
# An instance switch must drop the previous buffer pool snapshot.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-turnover.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/t.c" <<C
#define main cub_top_main
#include "$SRC"
#undef main
/* Slot i belongs to block i/q of nblk blocks; block f%nblk is refilled at frame f with
   the next pages of a sequence of length ws (0 = endless, never reread). */
static int nb_,nblk_; static long ws_;
static long last_fill(int f,int blk){ return f-((f%nblk_-blk+nblk_)%nblk_); }
static int page_of(int f,int i){ int q=nb_/nblk_; f+=nblk_;
    if(i>=q*nblk_) return i;
    long p=last_fill(f,i/q)*q + i%q; return (int)(ws_? p%ws_ : p); }
static int run(const char*name,int nb,int nblk,long ws,int frames,double want_t,double want_r){
    nb_=nb; nblk_=nblk; ws_=ws; g_inst_gen++;
    if(PG.reccap<nb){ PG.rec=realloc(PG.rec,sizeof(pgrec_t)*(size_t)nb); PG.reccap=nb; }
    double ts=0,rs=0; int k=0;
    for(int f=0;f<frames;f++){
        PG.ok=1; PG.nbuf=nb; PG.nrec=nb;
        for(int i=0;i<nb;i++){ memset(&PG.rec[i],0,sizeof PG.rec[i]); int pg=page_of(f,i); PG.rec[i].volid=(int16_t)(pg&3); PG.rec[i].pageid=pg; }   /* the volume follows the page, not the slot */
        cap_turnover(1000.0+f);
        if(f>=frames/2){ ts+=CAP2.turnover_pct; rs+=CAP2.readmit_pct; k++; }
    }
    double t=ts/k, r=rs/k; int ok=fabs(t-want_t)<0.5 && fabs(r-want_r)<0.5;
    printf("  %-4s %-40s turnover %6.2f%% (want %5.1f)  refill %6.2f%% (want %5.1f)\n",
           ok?"ok":"FAIL",name,t,want_t,r,want_r);
    return ok;
}
int main(void){
    int ok=1, nb=32768;
    ok&=run("endless scan: 2x keeps none",nb,4,0,40,25.0,0.0);
    ok&=run("cyclic 1.5x the buffer: 2x keeps all",nb,4,nb+nb/2,40,25.0,100.0);
    ok&=run("cyclic 1.9x the buffer: 2x still keeps all",nb,4,19L*nb/10,60,25.0,100.0);
    ok&=run("cyclic 2.5x the buffer: 2x keeps none",nb,4,5L*nb/2,60,25.0,0.0);
    ok&=run("cyclic 3x the buffer: 2x keeps none",nb,4,3L*nb,60,25.0,0.0);
    /* The verdict on that last state: the grade enlarging gets, and that a scan and a working set
       several times the buffer look the same here - never "a larger buffer does not help" */
    { const char *col; char o[512]; g_ascii=1;
      CAP2.headroom_pct=-1; CAP2.await_ms=-1; CAP2.qdepth=-1; CAP2.dev[0]=0; CAP2.over_budget=0;
      CAP2.miss_pps=8192; CAP2.hit_pct=-1;
      vd_capacity(o,sizeof o,&col);
      int v = strstr(o,"enlarging helps little")!=NULL && strstr(o,"re-read 0%")!=NULL && strstr(o,"a scan, or a working set many times the buffer")!=NULL
              && !strstr(o,"useless") && !strstr(o,"no use") && !strstr(o,"not help");
      printf("  %-4s %-40s \"%s\"\n",v?"ok":"FAIL","verdict at 0% re-admit",o);
      ok&=v; g_ascii=0; }
    ok&=run("262144 BCBs, 1% new pages per frame",262144,100,0,20,1.0,0.0);
    /* An instance switch must not keep the old snapshot, whatever the next scan does */
    { static proc_t P; P.pid=999999;
      B.ok=1; B.partial=0; g_bt=&BTABS[0];
      PG.ok=1; PG.nbuf=524288; PG.frame=5; PG.resident=500000; PG.dirty=1234; PG.have_log=1;
      reset_instance_state();
      B.ok=1; B.partial=0; g_bt=&BTABS[0];
      pgbuf_scan(&P);
      int sw=!PG.ok && PG.resident==0 && PG.dirty==0 && !PG.have_log;
      printf("  %-4s %-40s ok=%d resident=%d dirty=%d log=%d (want 0 0 0 0)\n",sw?"ok":"FAIL",
             "instance switch drops the old snapshot",PG.ok,PG.resident,PG.dirty,PG.have_log);
      ok&=sw; }
    return ok?0:1;
}
C
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
