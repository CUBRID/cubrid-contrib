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
# Heap detail B items mean what they say.  b_items() reads fake engine globals laid out in this
# process: the catalog's alloc_cnt comes from its own offset, not the sessions'; under the
# hidden new_lfhash=yes (lockfree_hashmap::m_type NEW) the old freelist's 0 is "not read", not
# a measured 0; object locks (resources and entries) are counted; the plan cache separates the
# engine's measured usage from its clone estimate; no "vacuum data" item stands for vacuum memory.
# The result cache's pages are temp-volume list files, a count outside the heap total; its heap
# is the entry pool and the (max_plan_cache_entries + 10) hash tables.  dynamic-heap's grade
# takes the larger of A and B, not their sum (A's walk already holds B's items).
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-bitems.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static unsigned char xc[1024], sess[1024], cat[1024], lk[1024], vac[256], ql[256], qp[256];
static btab_t bt;
static void put(unsigned char*b,int off,int32_t v){ memcpy(b+off,&v,4); }
static const bitem_t *item(const char*nm){ for(int i=0;i<B.n;i++) if(!strcmp(B.it[i].name,nm)) return &B.it[i]; return NULL; }
static void run(int newhash){
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    memset(&B,0,sizeof B); B.ok=1; g_bt=&bt;
    B.sym[S_XCACHE]=(unsigned long)(uintptr_t)xc; B.sym[S_SESS]=(unsigned long)(uintptr_t)sess;
    B.sym[S_CAT]=(unsigned long)(uintptr_t)cat;   B.sym[S_LKGL]=(unsigned long)(uintptr_t)lk;
    B.sym[S_VACUUM]=(unsigned long)(uintptr_t)vac;
    B.sym[S_QLIST]=(unsigned long)(uintptr_t)ql;  B.sym[S_QPOOL]=(unsigned long)(uintptr_t)qp;
    put(sess,bt.hx.sess_ht,newhash); put(cat,bt.hx.cat_ht,newhash); put(lk,bt.hx.lk_ht,newhash);
    b_items(&P);
}
int main(void){
    int bad=0;
    bt=BTABS[0];
    bt.hx.lf2=20;                                   /* the catalog's own offset, unlike the sessions' 12 */
    put(xc,bt.off_xc_entries,3); put(xc,bt.off_xc_usage,1000); put(xc,bt.off_xc_clone,9000);
    put(sess,bt.off_lf_alloccnt,5);
    put(cat,bt.off_lf_alloccnt,999); put(cat,20,7);
    put(lk,bt.off_lk_ntran,10); put(lk,bt.hx.lk_res,100); put(lk,bt.hx.lk_entfl+bt.hx.lffl,200);
    put(ql,bt.off_qlist_npages,640); put(qp,bt.off_qpool_nent,5); g_pagesize=16384;
    { char pb[]="[S ] max_query_cache_entries=100\n[S ] query_cache_size_in_pages=1000\n[S ] max_plan_cache_entries=1000\n"; paramdump_apply(pb); }
    run(0);
    const bitem_t *c=item("catalog cache"), *s=item("session table"), *pc=item("plan cache (xcache)"),
                  *pk=item("plan clones (est)"), *lr=item("lock resources (obj)"), *le=item("lock entries (obj)"),
                  *va=item("vacuum data");
    int ok = c && c->bytes==7.0*bt.sz_catalog;
    printf("  %s   catalog from its own offset: %.0f (want %d = 7 x %d, not 999 x)\n",ok?"ok  ":"FAIL",c?c->bytes:-1,7*bt.sz_catalog,bt.sz_catalog); bad|=!ok;
    ok = s && s->bytes==5.0*bt.sz_session && s->grade=='M';
    printf("  %s   session table, old hash: %.0f grade %c\n",ok?"ok  ":"FAIL",s?s->bytes:-1,s?s->grade:'-'); bad|=!ok;
    ok = pc && pk && pc->bytes==1000 && pc->grade=='M' && pk->bytes==9000 && pk->grade=='E';
    printf("  %s   plan cache measured %.0f (%c), clone estimate %.0f (%c)\n",ok?"ok  ":"FAIL",pc?pc->bytes:-1,pc?pc->grade:'-',pk?pk->bytes:-1,pk?pk->grade:'-'); bad|=!ok;
    ok = lr && le && lr->bytes==100.0*bt.hx.sz_lkr && le->bytes==200.0*bt.hx.sz_lke;
    printf("  %s   object locks: resources %.0f, entries %.0f\n",ok?"ok  ":"FAIL",lr?lr->bytes:-1,le?le->bytes:-1); bad|=!ok;
    printf("  %s   no vacuum data item\n",!va?"ok  ":"FAIL"); bad|=va!=NULL;
    { const bitem_t *rq=item("result cache (query)"), *rh=item("result cache hash (est)"), *rt=item("result temp pages");
      double tot=0; for(int i=0;i<B.n;i++) tot+=B.it[i].count_only?0:B.it[i].bytes;
      ok = rq && rq->bytes==5.0*4096 && rh && rh->grade=='E' && rh->bytes>1010.0*1010*8 && rt && rt->count_only && rt->count==640
           && B.total < 640.0*16384;
      printf("  %s   result cache: pool %.0f (5 x 4096), hash %.0f (%c, 1010 tables), temp pages %ld count-only; total %.0f excludes the %d pages\n",
             ok?"ok  ":"FAIL",rq?rq->bytes:-1,rh?rh->bytes:-1,rh?rh->grade:'-',rt?rt->count:-1,B.total,640); bad|=!ok; }
    { char pb[]="[S ] max_query_cache_entries=100\n[S ] query_cache_size_in_pages=0\n"; paramdump_apply(pb); run(0);
      const bitem_t *rq=item("result cache (query)"), *rh=item("result cache hash (est)");
      ok = rq && rq->bytes==0 && strstr(rq->note,"query_cache_size_in_pages") && !rh;
      printf("  %s   query_cache_size_in_pages=0: disabled, no hash item (\"%s\")\n",ok?"ok  ":"FAIL",rq?rq->note:"-"); bad|=!ok;
      char pb2[]="[S ] max_query_cache_entries=100\n[S ] query_cache_size_in_pages=1000\n[S ] max_plan_cache_entries=1000\n"; paramdump_apply(pb2); }
    /* dynamic-heap is graded E once B explains at least half of its RSS */
    { static const double cs[][2]={{30,0},{60,1}};   /* B, want E */
      for(int k=0;k<2;k++){
          NSR=1; memset(&SR[0],0,sizeof SR[0]); snprintf(SR[0].name,sizeof SR[0].name,"dynamic-heap");
          SR[0].rss=100; SR[0].grade='U'; double sb=B.total; B.total=cs[k][0];
          grade_dynamic_heap(); ok = (SR[0].grade=='E')==(int)cs[k][1];
          printf("  %s   dynamic-heap RSS 100, B %.0f: grade %c (want %c)\n",ok?"ok  ":"FAIL",cs[k][0],SR[0].grade,cs[k][1]?'E':'U');
          bad|=!ok; B.total=sb; } NSR=0; }
    run(1);                                         /* new_lfhash=yes */
    s=item("session table"); c=item("catalog cache"); lr=item("lock resources (obj)"); le=item("lock entries (obj)");
    ok = s && c && lr && s->grade=='U' && c->grade=='U' && lr->grade=='U' && le && le->grade=='M';
    printf("  %s   new lockfree hash: session %c, catalog %c, lock resources %c (want ?), lock entries %c (freelist, still M); total %.0f excludes them\n",
           ok?"ok  ":"FAIL",s?s->grade:'-',c?c->grade:'-',lr?lr->grade:'-',le?le->grade:'-',B.total); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
