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
# The long views (plot key w): every frame also feeds a ring of minute means - each double
# of the sample averaged over the frames of a wall-clock minute, a negative ("not measured")
# left out so a minute with no measurement stays -1, the ints and label from the minute's
# last frame - closed when the minute changes or the segment ends.  The view hands the
# plots the last hour or all minutes in place of the live ring.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-longwin.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
int main(void){
    int bad=0, ok; static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
    P.pid=1234; g_pagesize=16384;
    /* 3 minutes at 0.5 s from t=600: server RSS grows 1000 per frame, re-admit measured only on even frames */
    int f=0;
    for(double t=600;t<780;t+=0.5,f++){
        P.rss_kb=(unsigned long)(f+1); P.pss_kb=P.rss_kb;
        CAP2.readmit_pct = (f%2==0) ? 40.0 : -1; CAP2.miss_pps=100; CAP2.dev_iops=f;
        hist_push(t,&P,&M,&T,&IO);
    }
    ok = HMN==2 && HM_cnt==120;
    printf("  %s   180 s of frames: %d minutes closed, %d frames in the open minute\n",ok?"ok  ":"FAIL",HMN,HM_cnt); bad|=!ok;
    /* minute 1: frames 0..119, rss 1..120 kB -> mean 60.5 kB = 61952 B; re-admit 40 on even frames only -> 40, not 19.5 */
    ok = HMN>=1 && fabs(HMR[0].srv-60.5*1024)<1 && fabs(HMR[0].cap_readmit-40)<1e-9 && fabs(HMR[0].cap_iops-59.5)<1e-9 && fabs(HMR[0].t-629.75)<1e-9;
    printf("  %s   minute 1 means: srv %.0f B (want 61952), re-admit %.1f (want 40: the -1 frames left out), iops %.1f, t %.2f\n",ok?"ok  ":"FAIL",HMR[0].srv,HMR[0].cap_readmit,HMR[0].cap_iops,HMR[0].t); bad|=!ok;
    ok = HMN>=2 && fabs(HMR[1].srv-180.5*1024)<1 && !strcmp(HMR[1].hh,HS[HN-1].hh) == 0;
    ok = HMN>=2 && fabs(HMR[1].srv-180.5*1024)<1 && HMR[1].hh[0];
    printf("  %s   minute 2 means: srv %.0f B (want 184832), label %s from its last frame\n",ok?"ok  ":"FAIL",HMR[1].srv,HMR[1].hh); bad|=!ok;
    /* a field never measured in a minute stays -1 */
    { int n0=HMN; for(double t=780;t<840;t+=0.5){ CAP2.readmit_pct=-1; hist_push(t,&P,&M,&T,&IO); }
      hist_push(840,&P,&M,&T,&IO);
      ok = HMN==n0+2 && HMR[HMN-1].cap_readmit==-1;      /* the open minute closed at 780, the next at 840 */
      printf("  %s   a minute with no re-admit measurement: -1 (not 0) (%d minutes)\n",ok?"ok  ":"FAIL",HMN); bad|=!ok; }
    /* a segment change closes the open minute */
    { int n0=HMN, c0=HM_cnt; g_seg_gen++; hist_push(841,&P,&M,&T,&IO);
      ok = HMN==n0+1 && HM_cnt==1 && HN==1;
      printf("  %s   segment change: the open minute (%d frames) closed, a new one started\n",ok?"ok  ":"FAIL",c0); bad|=!ok; }
    /* views */
    { hsam_t *v; int n;
      ok = !hm_view(0,&v,&n) && hm_view(1,&v,&n) && n==HMN && v==HMR && hm_view(2,&v,&n) && n==HMN;
      printf("  %s   views: 0 = live, 1 = last %d minute(s) (all of them while under 60), 2 = all\n",ok?"ok  ":"FAIL",n); bad|=!ok;
      for(int k=HMN;k<70;k++){ HMR[k]=HMR[0]; HMR[k].t=HMR[0].t+60.0*k; } HMN=70;
      ok = hm_view(1,&v,&n) && n==60 && v==HMR+10 && hm_view(2,&v,&n) && n==70;
      printf("  %s   with 70 minutes: view 1 is the last 60, view 2 all 70\n",ok?"ok  ":"FAIL"); bad|=!ok; }
    /* the plots read the long view in place of the live ring and leave it as it was */
    { int hn=HN; hsam_t *hs=HS; g_plot_win=2; g_ascii=1;
      char o[400]; const char *c=VD_INFO;
      hsam_t *v; int n; hm_view(2,&v,&n); HS=v; HN=n; HCAP=n;
      double w=vd_win(); HS=hs; HN=hn; HCAP=HIST_MAX;
      ok = w>3000 && HS==HS_live && HN==hn;
      printf("  %s   window over the minute ring %.0f s (hours, not seconds); the live ring restored (%d samples)\n",ok?"ok  ":"FAIL",w,HN); bad|=!ok; (void)o; (void)c; }
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
if "$TMP/t"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
