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
# A page the engine is flushing is not on its volume yet, and is not "clean".  A flush clears
# DIRTY and the page's oldest_unflush_lsa as it begins (pgbuf_bcb_mark_is_flushing); a page
# dirtied again mid-flush has both bits.  unflushed counts either bit once; the oldest dirty
# LSA says how many mid-flush pages it cannot see.  With a DWB a page counts as flushed once
# copied into it, so up to double_write_buffer_size / pagesize "clean" pages may still wait.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-flushing.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static uint64_t lsa(int64_t pg,int off){ return ((uint64_t)pg & 0xFFFFFFFFFFFFULL) | ((uint64_t)(uint16_t)off<<48); }
int main(void){
    int bad=0, ok;
    static pgrec_t rec[5]; PG.rec=rec; PG.nrec=5; PG.reccap=5;
    rec[0].flags=0;                                     rec[0].oldest_lsa=~(uint64_t)0;    /* clean */
    rec[1].flags=PGBUF_F_DIRTY;                         rec[1].oldest_lsa=lsa(500,10);    /* dirty */
    rec[2].flags=PGBUF_F_FLUSHING;                      rec[2].oldest_lsa=~(uint64_t)0;    /* mid-flush, LSA cleared */
    rec[3].flags=PGBUF_F_FLUSHING;                      rec[3].oldest_lsa=~(uint64_t)0;
    rec[4].flags=PGBUF_F_FLUSHING|PGBUF_F_DIRTY;        rec[4].oldest_lsa=lsa(700,0);     /* dirtied again mid-flush */
    pg_recount();
    ok = PG.dirty==2 && PG.flushing==3 && PG.unflushed==4 && PG.flush_only==2 && PG.have_oldest && PG.oldest_dirty.pageid==500;
    printf("  %s   dirty %d, flushing %d, not yet written %d (want 4: either bit once), oldest dirty %lld excludes %d mid-flush (want 2)\n",
           ok?"ok  ":"FAIL",PG.dirty,PG.flushing,PG.unflushed,(long long)PG.oldest_dirty.pageid,PG.flush_only); bad|=!ok;
    /* Panel D judges by the DB's own disk, never the host's busiest: a backup saturating
       another disk (host 97%) with the DB disk at 3% and 5MB/s of writes is not a write
       bottleneck; the same writes with the DB disk at 97% are; an unknown DB disk gives no
       device judgement at all. */
    { char vb[320]; const char*vc=VD_INFO;
      memset(HS,0,sizeof HS); HN=1;
      HS[0].pg_ok=1; HS[0].pg_dirty=50; HS[0].pg_span=10; HS[0].pg_lag=0; HS[0].wbps=5.0*1024*1024;
      HS[0].devutil=97; HS[0].dbdev_util=3;
      vd_flush(vb,sizeof vb,&vc); int ok1 = vc!=VD_WARN && !strstr(vb,"bottleneck") && !strstr(vb,"\xeb\xb3\x91\xeb\xaa\xa9");
      printf("  %s   host device 97%%, DB disk 3%%, writes 5MB/s: not a write bottleneck -> \"%s\"\n",ok1?"ok  ":"FAIL",vb); bad|=!ok1;
      HS[0].dbdev_util=97;
      vd_flush(vb,sizeof vb,&vc); int ok2 = vc==VD_WARN && (strstr(vb,"bottleneck")||strstr(vb,"\xeb\xb3\x91\xeb\xaa\xa9")) && strstr(vb,"97%");
      printf("  %s   DB disk 97%%: write bottleneck -> \"%s\"\n",ok2?"ok  ":"FAIL",vb); bad|=!ok2;
      HS[0].dbdev_util=-1;
      vd_flush(vb,sizeof vb,&vc); int ok3 = vc!=VD_WARN && (strstr(vb,"DB disk -")||strstr(vb,"\xeb\xaf\xb8\xed\x99\x95\xec\x9d\xb8"));   /* "-" or 미확인 */
      printf("  %s   DB disk unknown: no device judgement -> \"%s\"\n",ok3?"ok  ":"FAIL",vb); bad|=!ok3;
      HS[0].pg_ok=0; HS[0].dbdev_util=3;
      vd_flush(vb,sizeof vb,&vc); int ok4 = vc!=VD_WARN;
      printf("  %s   buffer-pool state unavailable, host 97%%, DB disk 3%%: not a write bottleneck -> \"%s\"\n",ok4?"ok  ":"FAIL",vb); bad|=!ok4;
      HN=0; }
    g_pagesize=16384;
    { char b[]="[S ] double_write_buffer_size=2M\n"; paramdump_apply(b); }
    int d1=dwb_pages();
    { char b[]="[S ] double_write_buffer_size=0\n"; paramdump_apply(b); }
    int d0=dwb_pages();
    g_nprm=0; int du=dwb_pages();
    ok = d1==128 && d0==0 && du==-1;
    printf("  %s   DWB pages that may hold \"clean\" pages: 2M -> %d, off -> %d, unknown -> %d\n",ok?"ok  ":"FAIL",d1,d0,du); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
rc=0; "$TMP/t" || rc=1
# A live server: the keys are there and consistent
BIN="$ROOT/cub_top"
if [ -x "$BIN" ] && "$BIN" -d 2>/dev/null | grep -q '^pgbuf.enabled=1'; then
    out=$("$BIN" -d 2>/dev/null)
    d=$(printf '%s\n' "$out" | sed -n 's/.* pgbuf.dirty=\([0-9]*\).*/\1/p'); f=$(printf '%s\n' "$out" | sed -n 's/.* pgbuf.flushing=\([0-9]*\).*/\1/p')
    u=$(printf '%s\n' "$out" | sed -n 's/^pgbuf.unflushed=\([0-9]*\).*/\1/p')
    if [ -n "$u" ] && [ "$u" -ge "$d" ] && [ "$u" -le $((d+f)) ] && printf '%s\n' "$out" | grep -q '^pgbuf.dwb_enabled='; then
        echo "  ok     live: dirty $d, flushing $f, unflushed $u, $(printf '%s\n' "$out" | grep '^pgbuf.dwb_enabled=')"
    else echo "  FAIL   live keys: dirty '$d' flushing '$f' unflushed '$u'"; rc=1; fi
else echo "  SKIP   live keys (no buffer pool readable)"; fi
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
