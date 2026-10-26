/*
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */
/* Compiled together with cub_top.c (check-verdict.sh): io_verdict() is the dashboard's own code */
struct T { const char*nm; double wa,dev; int hd; double mf; int d; double bp; int nc; double io; int want; };
int main(void){
    struct T t[]={
      /* 64 cores: wa_hi = 0.78 */
      {"idle",                 0.1, 5,1, 0,0, 0,  64, 0, 0},
      {"certain bottleneck (wa + device + ours)",1.5,95,1, 12,3, 40, 64, 0, 1},
      {"masked (device + ours, wa low)",0.2,95,1, 12,3, 40, 64, 0, 2},
      {"someone else (device only)",      0.2,95,1, 0,0, 0,  64, 0, 4},
      {"non-disk wait (wa only)",     3.0, 5,1, 0,0, 0,  64, 0, 3},
      {"no device + wa high",       3.0, 0,0, 0,0, 0,  64, 0, 3},
      /* 4 cores: wa_hi = 12.5, checking the threshold scales with the core count */
      {"4 cores, wa 5% is low",     5.0,95,1, 12,3, 40,  4, 0, 2},
      {"4 cores, wa 20% is high",   20.0,95,1, 12,3, 40,  4, 0, 1},
      /* Floor of 0.5: at 256 cores 100/256*0.5 = 0.195, so the floor applies */
      {"256 cores, wa 0.3 is low",  0.3,95,1, 12,3, 40,256, 0, 2},
      {"256 cores, wa 0.6 is high",  0.6,95,1, 12,3, 40,256, 0, 1},
      /* Each of vd_mine's three signals must hold on its own */
      {"ours = majflt only",         1.5,95,1, 5,0, 0,  64, 0, 1},
      {"ours = D threads only",         1.5,95,1, 0,2, 0,  64, 0, 1},
      {"ours = blkio only",          1.5,95,1, 0,0, 5,  64, 0, 1},
      /* Container (overlay) regression: the DB files' backing device cannot be found in
         diskstats, so devutil and the instance's I/O do not connect.  With the wait
         signals at 0 but the server genuinely reading, the verdict must not be
         "someone else's load" (4). */
      {"container: io only",    0.2,95,1, 0,0, 0,  64, 200.0*1024*1024, 2},
      {"genuinely someone else: io is 0 too",  0.2,95,1, 0,0, 0,  64, 0, 4},
      {"io below 1MB is ignored",     0.2,95,1, 0,0, 0,  64, 500.0*1024,      4},
    };
    int n=sizeof t/sizeof*t, bad=0;
    for(int i=0;i<n;i++){
        int g=io_verdict(t[i].wa,t[i].nc,t[i].hd,t[i].dev,t[i].mf,t[i].d,t[i].bp,t[i].io);
        printf("  %-26s want=%d got=%d %s\n",t[i].nm,t[i].want,g,g==t[i].want?"OK":"<<< FAIL");
        if(g!=t[i].want) bad++;
    }
    printf("%s (%d/%d)\n",bad?"FAIL":"PASS",n-bad,n);
    return bad?1:0;
}
