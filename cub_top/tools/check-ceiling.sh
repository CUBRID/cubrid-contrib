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
# Remembered ceilings are per device, in memory for the run, keyed by the
# device's identity (wwid/serial/dm uuid + size), not its kernel name.  Two devices alternate as
# the busiest; each must be judged against its own ceiling.  A ceiling is learned only where a
# queue built (depth >= 4): one outstanding I/O shows util ~100% at depth ~1 and measures the
# latency, which must be judged latency-bound.  Drives cap_collect() with synthetic diskstats;
# the frame's device stands in as the DB's disk (check-dbdisk covers finding it).
# The device identity is read from a fake /sys/class/block built here, so the keys are the
# same on every host: wwid, device/serial (spaces folded), dm/uuid first when present, a
# partition as its disk's identity plus ":p<n>", and the kernel name only with no identity.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-ceiling.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
SB="$TMP/sys"; mkdir -p "$SB/class/block"
dev(){ # dev <name> <size> [file=content ...]: a device directory and its class/block link
    d="$SB/devices/$1"; mkdir -p "$d"; echo "$2" > "$d/size"; n=$1; shift 2
    for kv in "$@"; do f=${kv%%=*}; mkdir -p "$d/$(dirname "$f")"; printf '%s\n' "${kv#*=}" > "$d/$f"; done
    ln -s "../../devices/$n" "$SB/class/block/$n"; }
dev nvme0n1 1000215216 "wwid=eui.0025388b91c1d5e1"
dev sda 1953525168 "device/wwid=naa.5000c500a1b2c3d4"
dev sdb 23437770752 "device/serial=ST12000NM000J  ZR701W4Z"
dev sdc 4096
dev dm-0 2048 "dm/uuid=LVM-abc123" "wwid=not.this.one"
mkdir -p "$SB/devices/sda/sda1"; echo 1024 > "$SB/devices/sda/sda1/size"; echo 1 > "$SB/devices/sda/sda1/partition"
ln -s "../../devices/sda/sda1" "$SB/class/block/sda1"
sed -e 's/nk = P->pid>=0 ? db_disks(/nk = 1 ? test_db_disks(/' \
    -e "s|\"/sys/class/block/%s\"|\"$SB/class/block/%s\"|" "$SRC" > "$TMP/src.c"
grep -q 'test_db_disks(' "$TMP/src.c" || { echo "→ FAIL (db_disks call not found)"; exit 1; }
grep -q "$SB/class/block/%s" "$TMP/src.c" || { echo "→ FAIL (sysfs path not found)"; exit 1; }
cat > "$TMP/t.c" <<C
#include <stddef.h>
static char g_test_dev[32];
static int test_db_disks(int pid,const char*db,char out[][32],int max,char*why,size_t wn){
    (void)pid; (void)db; (void)max; (void)why; (void)wn; memcpy(out[0],g_test_dev,32); return 1; }
#define main cub_top_main
#include "$TMP/src.c"
#undef main
/* One 1-second frame on device nm: iops of 16KB reads at utilization util, mean queue qd, wait aw ms */
static void frame4(const char*nm,double iops,double util,double qd,double aw){
    static proc_t P; static iod_t IO; dstat_t d0,d1;
    memset(&d0,0,sizeof d0); memset(&IO,0,sizeof IO);
    snprintf(d0.name,sizeof d0.name,"%s",nm); d1=d0;
    d1.rios=(unsigned long)iops; d1.rsect=(unsigned long)(iops*32); d1.rticks=(unsigned long)(iops*aw); d1.tiq=(unsigned long)(qd*1000);
    d1.io_ms=(unsigned long)(util*10);
    snprintf(IO.devnm,sizeof IO.devnm,"%s",nm); IO.devutil=util; snprintf(g_test_dev,sizeof g_test_dev,"%s",nm);
    cap_collect(&P,&IO,&d0,1,&d1,1,1.0);
}
static void frame(const char*nm,double iops,double util){ frame4(nm,iops,util,util>=90?8.0:0.5,0.5); }
static int expect(const char*what,double ceil,double used){
    double c=CAP2.ceil_valid?CAP2.ceil_iops:-1; int ok=fabs(c-ceil)<0.5 && fabs(CAP2.headroom_pct-used)<0.05;
    printf("  %-4s %-44s ceiling %6.0f (want %6.0f)  used %5.1f%% (want %5.1f)\n",ok?"ok":"FAIL",what,c,ceil,CAP2.headroom_pct,used);
    return ok;
}
int main(int argc,char**argv){
    int ok=1;
    frame("nvme0n1",10000,95); ok&=expect("nvme0n1 saturated at 10000 IOPS",10000,100.0);
    frame("sda",1000,95);      ok&=expect("sda saturated at 1000 IOPS",1000,100.0);
    frame("nvme0n1",5000,40);  ok&=expect("nvme0n1 at 5000 IOPS",10000,50.0);
    frame("sda",500,20);       ok&=expect("sda at 500 IOPS",1000,50.0);
    /* another disk (sdb) knows nothing of sda's ceiling */
    frame("sdb",500,20);       ok&=expect("sdb, never saturated: no ceiling",-1,-1.0);
    /* a serial stream: util 100%, depth 1, 25ms per I/O - not a ceiling, and latency-bound */
    frame4("sdb",40,100,1.0,25.0); ok&=expect("sdb serial 40 IOPS at depth 1: not learned",-1,-1.0);
    frame4("sdb",38,100,1.0,25.0);
    { char o[400]; const char*c=VD_INFO; vd_capacity(o,sizeof o,&c);
      int v=strstr(o,"지연 병목")!=NULL && strstr(o,"직렬")!=NULL && !strstr(o,"용량 한계");
      printf("  %-4s %-44s \"%s\"\n",v?"ok":"FAIL","serial stream verdict",o); ok&=v; }
    { int v=!strcmp(g_cap_devid,"ST12000NM000J_ZR701W4Z@23437770752");
      printf("  %-4s %-44s %s\n",v?"ok":"FAIL","the frame's device recorded by identity",g_cap_devid); ok&=v; }
    { static const struct { const char *nm,*want,*what; } K[]={
          {"nvme0n1","eui.0025388b91c1d5e1@1000215216","wwid"},
          {"sda","naa.5000c500a1b2c3d4@1953525168","device/wwid"},
          {"sdb","ST12000NM000J_ZR701W4Z@23437770752","device/serial, spaces folded"},
          {"dm-0","LVM-abc123@2048","dm/uuid before wwid"},
          {"sda1","naa.5000c500a1b2c3d4:p1@1024","partition: its disk's identity + :p1"},
          {"sdc","sdc@4096","no identity: the kernel name, sized"},
          {"nosuch","nosuch@0","no such device"} };
      for(unsigned i=0;i<sizeof K/sizeof K[0];i++){ const char *k=dev_key(K[i].nm); int v=!strcmp(k,K[i].want);
          char lbl[64]; snprintf(lbl,sizeof lbl,"key of %s (%s)",K[i].nm,K[i].what);
          printf("  %-4s %-44s %s%s%s\n",v?"ok":"FAIL",lbl,k,v?"":" want ",v?"":K[i].want); ok&=v; } }
    return ok?0:1;
}
C
gcc -O2 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
bad=0
"$TMP/t" || bad=1
[ $bad -eq 0 ] && { echo "→ PASS"; exit 0; }
echo "→ FAIL"; exit 1
