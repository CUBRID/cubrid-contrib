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
# A large buffer (above PG_LARGE) without a request: the whole-array pass runs once, then on
# a period set by its cost (at least PG_LARGE_MIN_S); between passes the last pass stands
# with its age and the turnover/re-admit it gave, while a slot sample (every 64th BCB) read
# each frame gives the admission rate - unbiased, from the share of sampled slots whose page
# changed.  A fake pool of 300,000 BCBs in this process (as check-readmit builds it).
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-largebuf.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
#define NB 300000
static unsigned char pool_[4096], *bcbs_, *iopg_;
static void put(int i,int pageid){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int16_t v=0; uint32_t f=0x30000u; uint64_t ol=~(uint64_t)0;
    memcpy(b+g_bt->pg.vpid,&pageid,4); memcpy(b+g_bt->pg.vpid+4,&v,2); memcpy(b+g_bt->pg.flags,&f,4); memcpy(b+g_bt->pg.olsa,&ol,8);
    unsigned char*io=iopg_+(size_t)i*64; uint64_t ip=(uint64_t)(uintptr_t)io; memcpy(b+g_bt->pg.iopg,&ip,8);
    unsigned char*h=io+g_bt->pg.iobuf_iopg; uint64_t l=5; memcpy(h,&l,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
int main(void){
    int bad=0, ok;
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1; CAP.mem_pread=1;
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb); iopg_=calloc(NB,64);
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++) put(i,1000000+i);
    g_pg_detail=0;
    pgbuf_scan(&P);
    ok = PG.ok && PG.large && !PG.from_last && PG.scan_seq==1 && PG.resident==NB;
    printf("  %s   first frame: a whole-array pass (ok %d, large %d, seq %ld, resident %d, %.0fms)\n",ok?"ok  ":"FAIL",PG.ok,PG.large,PG.scan_seq,PG.resident,PG.ms); bad|=!ok;
    cap_turnover(100.0); cap_miss_from_engine();
    pgbuf_scan(&P);
    ok = PG.ok && PG.from_last && PG.scan_seq==1 && SM.n==NB/PG_SAMPLE_STRIDE && SM.miss_pps<0;
    printf("  %s   next frame: not due, the last pass stands (from_last %d, seq %ld), sample of %d slots taken, no rate yet\n",ok?"ok  ":"FAIL",PG.from_last,PG.scan_seq,SM.n); bad|=!ok;
    for(int i=0;i<NB;i+=10) put(i,5000000+i);          /* 10% of the slots take a new page */
    usleep(200000);
    pgbuf_scan(&P); cap_turnover(100.5); cap_miss_from_engine();
    ok = PG.from_last && SM.frac>0.08 && SM.frac<0.12 && CAP2.miss_pps==SM.miss_pps && CAP2.miss_pps>0.08*NB/0.3 && !strcmp(CAP2.miss_src,"sample") && CAP2.readmit_pct<0;
    printf("  %s   10%% of slots changed: sample says %.1f%% (%.0f pages/s, source %s), re-admit not yet\n",ok?"ok  ":"FAIL",SM.frac*100,CAP2.miss_pps,CAP2.miss_src); bad|=!ok;
    { static mem_t M; static tier_t T; static iod_t IO; char o[400]; const char *c=VD_INFO;
      AG.ok=1; AG.nbuf=NB; AG.z1=NB; g_pagesize=16384; hist_push(1,&P,&M,&T,&IO); vd_buf(o,sizeof o,&c);
      ok = HS[HN-1].cap_mperf==2 && strstr(o,"슬롯 표본") && strstr(o,"다음 전수 스캔");
      printf("  %s   panel 3 between passes: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok; }
    PG.scan_pid=-1;                                      /* make the pass due now */
    pgbuf_scan(&P); cap_turnover(110.0); cap_miss_from_engine();
    ok = PG.ok && PG.large && !PG.from_last && PG.scan_seq==2 && CAP2.turnover_pct>9.9 && CAP2.turnover_pct<10.1 && CAP2.readmit_pct==0 && !strcmp(CAP2.miss_src,"scan") && CAP2.miss_pps>2999 && CAP2.miss_pps<3001;
    printf("  %s   the next pass: turnover %.1f%%, re-admit %.0f%%, %.0f pages/s over the 10 s between passes (source %s)\n",ok?"ok  ":"FAIL",CAP2.turnover_pct,CAP2.readmit_pct,CAP2.miss_pps,CAP2.miss_src); bad|=!ok;
    usleep(100000); pgbuf_scan(&P); cap_turnover(110.5); cap_miss_from_engine();
    ok = PG.from_last && CAP2.turnover_pct>9.9 && CAP2.turnover_pct<10.1 && CAP2.readmit_pct==0 && CAP2.miss_pps==0 && !strcmp(CAP2.miss_src,"sample");
    printf("  %s   between passes again: turnover and re-admit stand (%.1f%% / %.0f%%), the sample's rate is this frame's (%.0f pages/s)\n",ok?"ok  ":"FAIL",CAP2.turnover_pct,CAP2.readmit_pct,CAP2.miss_pps); bad|=!ok;
    for(int i=0;i<NB;i+=10) put(i,1000000+i);          /* the same 10% come back: re-admitted */
    PG.scan_pid=-1; pgbuf_scan(&P); cap_turnover(120.0);
    ok = CAP2.readmit_pct>99 && CAP2.turnover_pct>9.9;
    printf("  %s   a pass after the pages came back: re-admit %.0f%%\n",ok?"ok  ":"FAIL",CAP2.readmit_pct); bad|=!ok;
    g_pg_detail=1; pgbuf_scan(&P);
    ok = PG.ok && !PG.large && !PG.from_last && PG.scan_seq==4;
    printf("  %s   with d (--pgbuf-detail): every frame is a pass again (seq %ld)\n",ok?"ok  ":"FAIL",PG.scan_seq); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
