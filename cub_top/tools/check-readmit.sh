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
# The re-admit rate is the one signal separating "buffer too small" from "scan without
# reuse".  When it was not measured (-1) it must stay -1 in the history and the verdict
# must not pick either conclusion; a 0 there read as a scan.  A large buffer scanned on
# request is scanned every frame, so every frame after the first has a measured rate; without
# a request the pass runs once and then on its own period (check-largebuf).
# A large buffer not scanned has no page set: admissions then come from the engine's ioread
# counter when a watcher keeps it running and two frames give a rate (live) - never from
# the cumulative one-shot figure - with re-admit still unmeasured, and the verdicts say so.
# Driven through cap_collect against a fake pstat_Global/pstat_Metadata in this process.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-readmit.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static int verdict(double readmit,double cold,const char *want_has,const char *want_not){
    static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
    AG.ok=1; AG.nbuf=1000; AG.z3=(long)(cold*10); AG.z1=1000-AG.z3; AG.z2=0; AG.dirty=0; PG.ok=0;
    IO.rb=20.0*1024*1024; CAP2.readmit_pct=readmit; CAP2.miss_pps=1280; g_pagesize=16384;   /* 20MB/s admitted */
    hist_push(1,&P,&M,&T,&IO);
    char o[400]; const char*c=VD_INFO; vd_buf(o,sizeof o,&c);
    int ok = HS[HN-1].cap_readmit==readmit && (!want_has || strstr(o,want_has)) && (!want_not || !strstr(o,want_not));
    printf("  %s   readmit %5.0f cold %.0f%% -> hist %.0f, \"%s\"\n",ok?"ok  ":"FAIL",readmit,cold,HS[HN-1].cap_readmit,o);
    return ok?0:1;
}
/* Exact LRU of B slots under uniform random access over W pages, with a second LRU of 2B fed
   the same accesses; the frame's resident set goes through cap_turnover. */
static int *pv1,*nx1,*in1,h1,t1,c1, *pv2,*nx2,*in2,h2,t2,c2;
static int lru(int p,int B,int*pv,int*nx,int*in,int*h,int*t,int*c){
    if(in[p]){ if(pv[p]>=0) nx[pv[p]]=nx[p]; else *h=nx[p]; if(nx[p]>=0) pv[nx[p]]=pv[p]; else *t=pv[p];
               pv[p]=-1; nx[p]=*h; if(*h>=0) pv[*h]=p; *h=p; if(*t<0) *t=p; return 1; }
    if(*c>=B){ int v=*t; if(pv[v]>=0) nx[pv[v]]=-1; else *h=-1; *t=pv[v]; in[v]=0; (*c)--; }
    pv[p]=-1; nx[p]=*h; if(*h>=0) pv[*h]=p; *h=p; if(*t<0) *t=p; in[p]=1; (*c)++; return 0;
}
static int lru_case(int B,double mult){
    int W=(int)(B*mult); srand(7);
    pv1=calloc(W,4); nx1=calloc(W,4); in1=calloc(W,4); pv2=calloc(W,4); nx2=calloc(W,4); in2=calloc(W,4);
    h1=t1=h2=t2=-1; c1=c2=0;
    static pgrec_t rec[1<<14]; reset_instance_state(); PG.rec=rec; PG.reccap=1<<14;
    long hit1=0,hit2=0,acc=0; double rs=0; int k=0;
    for(int f=0;f<400;f++){
        for(int a=0;a<B/4;a++){ int p=rand()%W;
            int g1=lru(p,B,pv1,nx1,in1,&h1,&t1,&c1), g2=lru(p,2*B,pv2,nx2,in2,&h2,&t2,&c2);
            if(f>=100){ hit1+=g1; hit2+=g2; acc++; } }
        PG.nrec=0; for(int p=h1;p>=0;p=nx1[p]){ pgrec_t*r=&rec[PG.nrec++]; memset(r,0,sizeof *r); r->pageid=p; }
        PG.ok=1; PG.nbuf=B; PG.degraded=0; cap_turnover(100.0+f*0.5);
        if(f>=100 && CAP2.readmit_pct>=0){ rs+=CAP2.readmit_pct; k++; }
    }
    double m1=1.0-(double)hit1/acc, m2=1.0-(double)hit2/acc, cut=100.0*(m1-m2)/m1, r=rs/k;
    int ok = r<=cut+1.0 && r>=cut-6.0;      /* exact LRU, uniform: below and close to the doubled buffer's cut */
    printf("  %s   W=%4.1fB: re-admit %5.1f%%, misses a doubled buffer removes %5.1f%%\n",ok?"ok  ":"FAIL",mult,r,cut);
    free(pv1); free(nx1); free(in1); free(pv2); free(nx2); free(in2);
    return ok?0:1;
}
/* Fake pool of NB BCBs (the review's 300,000 - above PG_LARGE).  The first 1000 BCBs
   swap between page sets A and B each frame, so from the third frame every admitted
   page was evicted two frames before: re-admit 100%. */
#define NB 300000
static unsigned char pool_[4096], *bcbs_, *iopg_;
static void put(int i,int pageid){
    unsigned char*b=bcbs_+(size_t)i*g_bt->pg.sz_bcb; int16_t v=0; uint32_t f=0x30000u; uint64_t ol=~(uint64_t)0;
    memcpy(b+g_bt->pg.vpid,&pageid,4); memcpy(b+g_bt->pg.vpid+4,&v,2); memcpy(b+g_bt->pg.flags,&f,4); memcpy(b+g_bt->pg.olsa,&ol,8);
    unsigned char*io=iopg_+(size_t)i*64; uint64_t ip=(uint64_t)(uintptr_t)io; memcpy(b+g_bt->pg.iopg,&ip,8);
    unsigned char*h=io+g_bt->pg.iobuf_iopg; uint64_t l=5; memcpy(h,&l,8); memcpy(h+8,&pageid,4); memcpy(h+12,&v,2);
}
/* Midpoint-insertion cache of B pages (CUBRID-like): a hit moves the page to the head; a
   miss evicts the tail and inserts the new page with `mid` pages behind it, so a page
   used once survives about `mid` further misses.  Array-backed, O(B) per access: small B. */
typedef struct { int *a; int n, cap, mid; } mcache_t;
static int mc_find(mcache_t*c,int p){ for(int i=0;i<c->n;i++) if(c->a[i]==p) return i; return -1; }
static int mc_access(mcache_t*c,int p){
    int i=mc_find(c,p);
    if(i>=0){ memmove(c->a+1,c->a,(size_t)i*sizeof(int)); c->a[0]=p; return 1; }
    if(c->n>=c->cap) c->n--;                                  /* evict the tail */
    int at=c->n-c->mid; if(at<0) at=0;                        /* `mid` pages behind the new one */
    memmove(c->a+at+1,c->a+at,(size_t)(c->n-at)*sizeof(int)); c->a[at]=p; c->n++; return 0;
}
static pgrec_t mrec_[1<<14];
static void feed(const int*pages,int n,int B,double t){
        PG.rec=mrec_; PG.reccap=1<<14; PG.nrec=0;
    for(int i=0;i<n;i++){ pgrec_t*r=&mrec_[PG.nrec++]; memset(r,0,sizeof *r); r->pageid=pages[i]; }
    PG.ok=1; PG.nbuf=B; PG.degraded=0; cap_turnover(t);
}
/* The panel-3 verdict for given figures: admitted MB/s, re-admit, admissions per frame / nbuf */
static const char* vd_for(double readmit,double admfrac,const char**col){
    static proc_t P; static mem_t M; static tier_t T; static iod_t IO; static char o[400];
    AG.ok=1; AG.nbuf=1000; AG.z3=850; AG.z1=150; AG.z2=0; AG.dirty=0; PG.ok=0;
    IO.rb=20.0*1024*1024; CAP2.readmit_pct=readmit; CAP2.miss_pps=1280; CAP2.adm_frac=admfrac; g_pagesize=16384;
    hist_push(1,&P,&M,&T,&IO);
    *col=VD_INFO; vd_buf(o,sizeof o,col); return o;
}
int main(void){
    int bad=0;
    printf("1. verdict on panel 3\n");
    bad|=verdict(-1,85,"재적재율 미측정","재사용 없는");
    bad|=verdict(-1,85,NULL,"⚠ 버퍼 부족");
    bad|=verdict(50,85,"증설 효과 큼",NULL);
    bad|=verdict(50,85,"잠정, 창",NULL);          /* a window under a minute: provisional */
    bad|=verdict(5,85,"재사용 거리가 버퍼 밖","무효");
    bad|=verdict(20,85,"증설 효과 있음","무효");
    { /* the OS page cache absorbs a short buffer: no disk reads, the buffer still misses */
      static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
      IO.rb=0; CAP2.miss_pps=3840; CAP2.readmit_pct=85; g_pagesize=16384;
      hist_push(1,&P,&M,&T,&IO);
      char o[400]; const char*c=VD_INFO; vd_buf(o,sizeof o,&c);
      int ok = c==VD_WARN && strstr(o,"버퍼 부족") && strstr(o,"OS 페이지 캐시");
      printf("  %s   disk 0, 3840 pages/s admitted, re-admit 85%%: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
      CAP2.miss_pps=-1; hist_push(2,&P,&M,&T,&IO); c=VD_INFO; vd_buf(o,sizeof o,&c);
      ok = c!=VD_OK && strstr(o,"적재 미측정");
      printf("  %s   admissions not measured: not green, \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
      ok = HS[HN-1].cap_miss==-1;
      printf("  %s   history keeps admissions unmeasured (%.0f)\n",ok?"ok  ":"FAIL",HS[HN-1].cap_miss); bad|=!ok; }
    { /* capacity verdict: 300 pages/s admitted, re-admit 7% (a working set ~15x the buffer) */
      memset(&CAP2,0,sizeof CAP2); CAP2.await_ms=1; CAP2.dev_iops=100; CAP2.qdepth=-1; CAP2.headroom_pct=-1;
      CAP2.miss_pps=300; CAP2.readmit_pct=7; CAP2.turnover_pct=5; CAP2.hit_pct=-1; snprintf(CAP2.hit_src,sizeof CAP2.hit_src,"-");
      PG.ok=1; PG.nbuf=1000; PG.resident=1000;
      char o[400]; const char*c=VD_INFO; vd_capacity(o,sizeof o,&c);
      int ok=!strstr(o,"무효") && strstr(o,"재사용 거리가 버퍼 밖");
      printf("  %s   capacity, re-admit 7%%: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok; }
    printf("2. large buffer not scanned: admissions from the engine's ioread counter through cap_collect\n");
    { /* A fake pstat_Global/pstat_Metadata in this process, laid out by the offset table, read by
         cap_collect the way the server's is; PG.ok=0 + PG.large as pgbuf_scan leaves a large buffer */
      static proc_t P; static mem_t M; static tier_t T; static iod_t IO; char o[400]; const char*c; int ok;
      P.pid=getpid(); snprintf(P.db,sizeof P.db,"x"); CAP.vm_readv=1;
      B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0;
      if(g_bt->ps.nvals<0||g_bt->ps.sz_md<=0){ printf("  SKIP   no perfmon offsets in the builtin table\n"); }
      else {
        static unsigned char gbuf[256]; static uint64_t vals[64]; int maxp=0;
        for(int i=0;i<CPS_N;i++) if(CPS_PSID[i]>maxp) maxp=CPS_PSID[i];
        unsigned char *md=calloc((size_t)(maxp+1),(size_t)g_bt->ps.sz_md);
        int32_t nv=64; uint64_t gp=(uint64_t)(uintptr_t)vals; unsigned char one=1;
        memcpy(gbuf+g_bt->ps.nvals,&nv,4); memcpy(gbuf+g_bt->ps.gstats,&gp,8); memcpy(gbuf+g_bt->ps.init,&one,1);
        for(int i=0;i<CPS_N;i++){ unsigned char *e=md+(size_t)CPS_PSID[i]*(size_t)g_bt->ps.sz_md; int32_t id=CPS_PSID[i], st=i+1;
                                  memcpy(e+g_bt->ps.md_psid,&id,4); memcpy(e+g_bt->ps.md_start,&st,4); }
        B.sym[S_PSTATG]=(unsigned long)(uintptr_t)gbuf; B.sym[S_PSTATMD]=(unsigned long)(uintptr_t)md;
        PG.ok=0; PG.large=1; AG.ok=1; AG.nbuf=300000; AG.z1=250000; AG.z2=40000; AG.z3=5000; AG.dirty=0; g_pagesize=16384; IO.rb=0;
        memset(&CAP2,0,sizeof CAP2); cap_reset_values();
        #define FRAME(fe,io) do{ vals[1+CPS_PB_FETCH]=(fe); vals[1+CPS_PB_IOREAD]=(io); cap_collect(&P,&IO,NULL,0,NULL,0,0.5); }while(0)
        FRAME(100000,1000);
        ok = CAP2.ps_ok && !strcmp(CAP2.hit_src,"perfmon-cum") && CAP2.miss_pps<0 && !strcmp(CAP2.miss_src,"-");
        printf("  %s   first frame (cumulative only): admissions unmeasured (%.0f, source %s, hit source %s)\n",ok?"ok  ":"FAIL",CAP2.miss_pps,CAP2.miss_src,CAP2.hit_src); bad|=!ok;
        FRAME(110000,1640);
        ok = CAP2.miss_pps==1280 && !strcmp(CAP2.miss_src,"perfmon") && CAP2.pb_ioread_ps==1280 && !strcmp(CAP2.hit_src,"perfmon");
        printf("  %s   second frame: ioread +640 in 0.5s -> admissions %.0f/s (source %s)\n",ok?"ok  ":"FAIL",CAP2.miss_pps,CAP2.miss_src); bad|=!ok;
        hist_push(10,&P,&M,&T,&IO); c=VD_INFO; vd_buf(o,sizeof o,&c);
        ok = HS[HN-1].cap_miss==1280 && HS[HN-1].cap_mperf && c!=VD_OK && strstr(o,"엔진 카운터") && strstr(o,"pgbuf-detail") && !strstr(o,"적재 미측정");
        printf("  %s   panel 3 (20MB/s, re-admit unmeasured): \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
        c=VD_INFO; vd_capacity(o,sizeof o,&c);
        ok = strstr(o,"엔진 카운터") && strstr(o,"pgbuf-detail") && !strstr(o,"파일 기준선");
        printf("  %s   capacity verdict (PG.ok=0, engine aggregates): \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
        FRAME(110000,1640);
        ok = CAP2.miss_pps<0 && !strcmp(CAP2.miss_src,"-");
        printf("  %s   no fetch in the window (idle, or the watcher gone): unmeasured again (%.0f)\n",ok?"ok  ":"FAIL",CAP2.miss_pps); bad|=!ok;
        FRAME(120000,1640);
        hist_push(11,&P,&M,&T,&IO); c=VD_INFO; vd_buf(o,sizeof o,&c);
        ok = CAP2.miss_pps==0 && !strcmp(CAP2.miss_src,"perfmon") && c==VD_OK && strstr(o,"버퍼에서 처리 중");
        printf("  %s   fetches, no ioread: admissions 0 -> panel 3 \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
        AG.z1=100000; AG.z2=0; AG.z3=0;
        c=VD_INFO; vd_capacity(o,sizeof o,&c);
        ok = strstr(o,"과다 할당") && strstr(o,"33%");
        printf("  %s   capacity verdict, 33%% resident at 0 pages/s: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok;
        #undef FRAME
        CAP2.miss_pps=500; cap_miss_from_engine();
        ok = CAP2.miss_pps==500 && !strcmp(CAP2.miss_src,"scan");
        printf("  %s   a page scan this frame wins over the counter (%.0f, source %s)\n",ok?"ok  ":"FAIL",CAP2.miss_pps,CAP2.miss_src); bad|=!ok;
        static proc_t RP; static mem_t RM; static tier_t RT; static iod_t RIO;
        rp_apply("{\"cap.miss_pages_per_s\": 1280, \"cap.miss_source\": \"perfmon\"}",&RP,&RM,&RT,&RIO);
        ok = CAP2.miss_pps==1280 && !strcmp(CAP2.miss_src,"perfmon");
        rp_apply("{\"cap.miss_pages_per_s\": 300}",&RP,&RM,&RT,&RIO);
        ok &= CAP2.miss_pps==300 && !strcmp(CAP2.miss_src,"scan");
        rp_apply("{}",&RP,&RM,&RT,&RIO);
        ok &= CAP2.miss_pps<0 && !strcmp(CAP2.miss_src,"-");
        printf("  %s   replay: the source comes back; an older recording without it reads as scan; none as unmeasured\n",ok?"ok  ":"FAIL"); bad|=!ok;
        B.sym[S_PSTATG]=0; B.sym[S_PSTATMD]=0; free(md);
      }
    }
    printf("3. large buffer (%d BCBs) scanned on request: re-admit every frame\n",NB);
    static proc_t P; P.pid=getpid(); CAP.vm_readv=1;
    B.ok=1; B.partial=0; g_bt=&BTABS[0]; B.base=0; B.sym[S_PGBUF]=(unsigned long)(uintptr_t)pool_;
    bcbs_=calloc(NB,(size_t)g_bt->pg.sz_bcb); iopg_=calloc(NB,64);
    int32_t nb=NB; memcpy(pool_+g_bt->off_pgbuf_nbuf,&nb,4);
    uint64_t t=(uint64_t)(uintptr_t)bcbs_; memcpy(pool_+g_bt->pg.bcbtab,&t,8);
    for(int i=0;i<NB;i++) put(i,1000000+i);
    g_pg_detail=1;
    char seq[200]=""; int n=0, bad2=0;
    for(int f=0;f<6;f++){
        for(int i=0;i<1000;i++) put(i,(f%2?2000000:3000000)+i);
        pgbuf_scan(&P);
        cap_turnover(100.0+f*0.5);
        n+=snprintf(seq+n,sizeof seq-(size_t)n,"%s%.0f",f?",":"",CAP2.readmit_pct);
        if(!PG.ok) { printf("  FAIL frame %d not scanned: %s\n",f,PG.why); bad2=1; }
        if(f>=1 && CAP2.readmit_pct<0) bad2=1;
        if(f>=2 && CAP2.readmit_pct!=100) bad2=1;
    }
    printf("  %s   re-admit by frame: %s (want -1 then measured every frame, 100 from the third)\n",bad2?"FAIL":"ok  ",seq);
    bad|=bad2;
    g_pg_detail=0; pgbuf_scan(&P); cap_turnover(104.0); double rm=CAP2.readmit_pct;
    printf("  %s   not requested: the first frame still makes one pass (scanned=%d, large=%d, re-admit %.0f)\n",(PG.ok&&PG.large&&!PG.from_last)?"ok  ":"FAIL",PG.ok,PG.large,rm);
    if(!(PG.ok&&PG.large&&!PG.from_last)) bad=1;
    pgbuf_scan(&P); cap_turnover(104.5);
    printf("  %s   the next frame: no pass, the last one stands (from_last=%d, re-admit %.0f)\n",(PG.ok&&PG.from_last&&CAP2.readmit_pct==rm)?"ok  ":"FAIL",PG.from_last,CAP2.readmit_pct);
    if(!(PG.ok&&PG.from_last&&CAP2.readmit_pct==rm)) bad=1;
    printf("3. re-admit estimates what doubling the buffer saves (exact LRU, uniform random - where it holds)\n");
    { double m[]={4,6,10,15}; for(int i=0;i<4;i++) bad|=lru_case(4096,m[i]); }
    printf("4. where the estimate is not a bound (review A-3)\n");
    { /* (a) every page read twice D = 1.2B apart, B accesses per frame: pages admitted and
         evicted inside a frame are never seen, so re-admit is off and the frame must be
         flagged low-confidence instead of driving a shortage verdict */
      int B=1024, D=(int)(1.2*B); srand(3);
      pv1=calloc(1<<17,4); nx1=calloc(1<<17,4); in1=calloc(1<<17,4); pv2=calloc(1<<17,4); nx2=calloc(1<<17,4); in2=calloc(1<<17,4);
      h1=t1=h2=t2=-1; c1=c2=0; reset_instance_state();
      static pgrec_t rec[1<<14]; PG.rec=rec; PG.reccap=1<<14;
      long hit1=0,hit2=0,acc=0; double rs=0,fs=0; int k=0, next=0; int *pend=calloc(1<<17,4); int np=0;
      for(int f=0;f<60;f++){
          for(int a=0;a<B;a++){
              int p; if(np>0 && pend[0]<=next-D){ p=pend[0]; memmove(pend,pend+1,(size_t)(--np)*4); }
                     else { p=next++; pend[np++]=p; }
              p%=(1<<17);
              int g1=lru(p,B,pv1,nx1,in1,&h1,&t1,&c1), g2=lru(p,2*B,pv2,nx2,in2,&h2,&t2,&c2);
              if(f>=10){ hit1+=g1; hit2+=g2; acc++; } }
          PG.nrec=0; for(int q=h1;q>=0;q=nx1[q]){ pgrec_t*r=&rec[PG.nrec++]; memset(r,0,sizeof *r); r->pageid=q; }
          PG.ok=1; PG.nbuf=B; PG.degraded=0; cap_turnover(200.0+f*0.5);
          if(f>=10 && CAP2.readmit_pct>=0){ rs+=CAP2.readmit_pct; fs+=CAP2.adm_frac; k++; } }
      double m1=1.0-(double)hit1/acc, m2=1.0-(double)hit2/acc, cut=m1>0?100.0*(m1-m2)/m1:0, r=rs/k, af=fs/k;
      const char*c; const char*o=vd_for(r,af,&c);
      int ok = af>=0.5 && strstr(o,"신뢰도 낮음") && !strstr(o,"증설 효과");
      printf("  %s   two reads 1.2B apart, 1B per frame: re-admit %.0f%%, real cut %.0f%%, admitted %.0f%% of the buffer per frame -> \"%s\"\n",
             ok?"ok  ":"FAIL",r,cut,af*100,o); bad|=!ok;
      free(pv1); free(nx1); free(in1); free(pv2); free(nx2); free(in2); free(pend); }
    { /* (b) midpoint insertion, every page read twice 0.8B apart, B/10 per frame: the window
         counts reuse in (0.6B, 1.6B] while doubling helps only (0.6B, 1.2B] - the figure is
         an estimate under LRU, and the verdict must say so rather than call it a bound */
      int B=512, D=(int)(0.8*B); mcache_t c1m={calloc(B,4),0,B,(int)(0.6*B)}, c2m={calloc(2*B,4),0,2*B,(int)(1.2*B)};
      reset_instance_state();
      long hit1=0,hit2=0,acc=0; double rs=0; int k=0, next=0, *pend=calloc(1<<16,4), np=0;
      for(int f=0;f<300;f++){
          int miss_f=0;
          while(miss_f<B/10){
              int p; if(np>0 && pend[0]<=next-D){ p=pend[0]; memmove(pend,pend+1,(size_t)(--np)*4); }
                     else { p=next++; pend[np++]=p; }
              int g1=mc_access(&c1m,p), g2=mc_access(&c2m,p); miss_f+=!g1;
              if(f>=50){ hit1+=g1; hit2+=g2; acc++; } }
          feed(c1m.a,c1m.n,B,400.0+f*0.5);
          if(f>=50 && CAP2.readmit_pct>=0){ rs+=CAP2.readmit_pct; k++; } }
      double m1=1.0-(double)hit1/acc, m2=1.0-(double)hit2/acc, cut=m1>0?100.0*(m1-m2)/m1:0, r=rs/k;
      const char*c; const char*o=vd_for(r,0.1,&c);
      int ok = strstr(o,"LRU 가정")!=NULL && !strstr(o,"하한");
      printf("  %s   midpoint insertion, reuse 0.8B, B/10 per frame: re-admit %.0f%% vs real cut %.0f%% - labelled an LRU estimate -> \"%s\"\n",
             ok?"ok  ":"FAIL",r,cut,o); bad|=!ok;
      free(c1m.a); free(c2m.a); free(pend); }
    { /* (c) aliasing: a cycle over 1.5B pages, 1.5B accesses per frame - every access misses,
         yet the frames' resident sets are identical, so nothing is seen admitted.  Snapshots
         cannot detect this; the case is reproduced so the documented limit stays true */
      int B=1024, W=(int)(1.5*B);
      pv1=calloc(W,4); nx1=calloc(W,4); in1=calloc(W,4); h1=t1=-1; c1=0; reset_instance_state();
      static pgrec_t rec2[1<<14]; PG.rec=rec2; PG.reccap=1<<14;
      long miss=0,acc=0; double ms=0; int k=0, pos=0;
      for(int f=0;f<40;f++){
          for(int a=0;a<W;a++){ int g=lru(pos,B,pv1,nx1,in1,&h1,&t1,&c1); pos=(pos+1)%W; if(f>=5){ miss+=!g; acc++; } }
          PG.nrec=0; for(int q=h1;q>=0;q=nx1[q]){ pgrec_t*r=&rec2[PG.nrec++]; memset(r,0,sizeof *r); r->pageid=q; }
          PG.ok=1; PG.nbuf=B; PG.degraded=0; cap_turnover(600.0+f*0.5);
          if(f>=5 && CAP2.miss_pps>=0){ ms+=CAP2.miss_pps; k++; } }
      int ok = acc>0 && miss==acc && ms/k==0;
      printf("  %s   aliasing, 1.5B cycle at 1.5B per frame: real hit %.0f%%, seen admitted %.0f pages/s (documented limit, not detectable from snapshots)\n",
             ok?"ok  ":"FAIL",100.0*(acc-miss)/acc,ms/k); bad|=!ok;
      free(pv1); free(nx1); free(in1); }
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
