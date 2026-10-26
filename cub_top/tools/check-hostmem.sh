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
# The memory facts beyond host RAM: the target's memory cgroup (v2 memory.max/current, v1
# limit/usage, "max" and the v1 no-limit value as none, the mount root when the path from
# /proc/<pid>/cgroup is not under the mount - a container), the host's memory PSI (absent
# on older kernels), and the share of the target's heap on transparent huge pages (smaps
# AnonHugePages).  The host verdict puts the cgroup limit before host RAM.  All through a
# fake /proc and /sys tree the test build reads.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-hostmem.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
sed -e "s|\"/proc/%d/cgroup\"|\"$TMP/proc/%d/cgroup\"|" -e "s|\"/sys/fs/cgroup/memory\"|\"$TMP/sys/fs/cgroup/memory\"|" \
    -e "s|\"/sys/fs/cgroup\"|\"$TMP/sys/fs/cgroup\"|" -e "s|\"/proc/pressure/memory\"|\"$TMP/pressure\"|" \
    -e "s|\"/proc/%d/smaps\"|\"$TMP/proc/%d/smaps\"|" "$SRC" > "$TMP/t.c"
for k in "$TMP/proc/%d/cgroup" "$TMP/sys/fs/cgroup/memory" "$TMP/sys/fs/cgroup\"" "$TMP/pressure" "$TMP/proc/%d/smaps\""; do
    grep -qF "$k" "$TMP/t.c" || { echo "→ FAIL (hook $k not inserted)"; exit 1; }; done
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/t.c" > "$TMP/h.c"
cat >> "$TMP/h.c" <<'C'
static const char *T;
static void put(const char *rel,const char *txt){ char p[600]; snprintf(p,sizeof p,"%s/%s",T,rel);
    char d[600]; snprintf(d,sizeof d,"%s",p); *strrchr(d,'/')=0; char cmd[700]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",d); if(system(cmd)){}
    FILE *f=fopen(p,"w"); if(f){ fputs(txt,f); fclose(f); } }
static int vd(mem_t *M,const char *want){
    static proc_t P; static tier_t Tr; static iod_t IO; char o[400]; const char *c=VD_INFO;
    M->total=16000000; M->avail=8000000; hist_push(1,&P,M,&Tr,&IO); vd_host(o,sizeof o,&c,8);
    int ok = want ? strstr(o,want)!=NULL : (!strstr(o,"cgroup") && !strstr(o,"PSI"));
    printf("  %s   verdict: \"%s\"\n",ok?"ok  ":"FAIL",o); return !ok;
}
int main(int c,char**v){
    T=v[1]; int bad=0, ok; static mem_t M; char rel[64];
    put("proc/1/cgroup","0::/user.slice/app\n");
    put("sys/fs/cgroup/user.slice/app/memory.max","max\n"); put("sys/fs/cgroup/user.slice/app/memory.current","123\n");
    parse_cgroup(&M,1); ok = M.cg_limit==-1 && !strcmp(M.cg_path,"/user.slice/app");
    printf("  %s   v2 memory.max=max: no limit (path %s)\n",ok?"ok  ":"FAIL",M.cg_path); bad|=!ok; bad|=vd(&M,NULL);
    put("sys/fs/cgroup/user.slice/app/memory.max","1073741824\n"); put("sys/fs/cgroup/user.slice/app/memory.current","996147200\n");
    parse_cgroup(&M,1); ok = M.cg_limit==1073741824.0 && M.cg_usage==996147200.0;
    printf("  %s   v2 limit 1GiB, usage 950MiB: %.0f / %.0f\n",ok?"ok  ":"FAIL",M.cg_usage,M.cg_limit); bad|=!ok; bad|=vd(&M,"cgroup");
    put("proc/2/cgroup","12:cpuset:/x\n9:memory:/machine.slice/libpod-abc.scope\n0::/\n");
    put("sys/fs/cgroup/memory/memory.limit_in_bytes","2147483648\n"); put("sys/fs/cgroup/memory/memory.usage_in_bytes","104857600\n");
    parse_cgroup(&M,2); ok = M.cg_limit==2147483648.0 && M.cg_usage==104857600.0 && !strcmp(M.cg_path,"/machine.slice/libpod-abc.scope");
    printf("  %s   v1, path not under the mount (container): the mount root's limit 2GiB, usage 100MiB (%.0f/%.0f)\n",ok?"ok  ":"FAIL",M.cg_usage,M.cg_limit); bad|=!ok; bad|=vd(&M,NULL);
    put("sys/fs/cgroup/memory/memory.limit_in_bytes","9223372036854771712\n");
    parse_cgroup(&M,2); ok = M.cg_limit==-1;
    printf("  %s   v1 no-limit value: none\n",ok?"ok  ":"FAIL"); bad|=!ok;
    parse_cgroup(&M,3); ok = M.cg_limit==-1 && !M.cg_path[0];
    printf("  %s   no /proc/<pid>/cgroup: none\n",ok?"ok  ":"FAIL"); bad|=!ok;
    put("pressure","some avg10=1.50 avg60=0.80 avg300=0.30 total=1234\nfull avg10=6.20 avg60=2.00 avg300=0.50 total=99\n");
    parse_psi(&M); ok = M.psi_some10==1.5 && M.psi_full10==6.2;
    printf("  %s   PSI memory: some %.2f full %.2f\n",ok?"ok  ":"FAIL",M.psi_some10,M.psi_full10); bad|=!ok; bad|=vd(&M,"PSI");
    snprintf(rel,sizeof rel,"%s/pressure",T); unlink(rel); parse_psi(&M); ok = M.psi_full10==-1 && M.psi_some10==-1;
    printf("  %s   no PSI on this kernel: -1\n",ok?"ok  ":"FAIL"); bad|=!ok;
    put("proc/4/smaps","00400000-00500000 rw-p 00000000 00:00 0 \nRss:        4096 kB\nPss:        4096 kB\nAnonHugePages:      2048 kB\nSwap:          0 kB\n"
                       "00600000-00800000 rw-p 00000000 00:00 0 \nRss:        8192 kB\nPss:        8192 kB\nAnonHugePages:      4096 kB\nSwap:          0 kB\n");
    static proc_t P; P.pid=4; parse_smaps(&P); parse_rollup(&P);
    ok = P.nreg==2 && P.s_thp_kb==6144 && P.thp_kb==6144 && P.rss_kb==12288;
    printf("  %s   smaps AnonHugePages summed: THP %lu of RSS %lu kB\n",ok?"ok  ":"FAIL",P.thp_kb,P.rss_kb); bad|=!ok;
    static proc_t RP; static mem_t RM; static tier_t RT; static iod_t RIO;
    rp_apply("{\"os.cgroup_limit_bytes\": 1073741824, \"os.cgroup_usage_bytes\": 5, \"os.cgroup_path\": \"/a\", \"os.psi_mem_some10\": 1.5, \"os.psi_mem_full10\": 6.2, \"server.thp_kb\": 6144}",&RP,&RM,&RT,&RIO);
    ok = RM.cg_limit==1073741824.0 && RM.cg_usage==5 && !strcmp(RM.cg_path,"/a") && RM.psi_full10==6.2 && RP.thp_kb==6144;
    printf("  %s   replay restores the cgroup, PSI and THP keys\n",ok?"ok  ":"FAIL"); bad|=!ok;
    rp_apply("{}",&RP,&RM,&RT,&RIO); ok = RM.cg_limit==-1 && RM.psi_full10==-1 && RP.thp_kb==0;
    printf("  %s   an older recording without them: none\n",ok?"ok  ":"FAIL"); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/h" "$TMP/h.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
if "$TMP/h" "$TMP"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
