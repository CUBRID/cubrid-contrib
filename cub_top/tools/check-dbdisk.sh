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
# The capacity verdict judges the disk under the DB's own files, not the host's busiest
# disk.  Run as an unprivileged account (no root): the target's open <db>, <db>_* files, or
# its databases.txt entry when the fds are another account's; a file's block device through
# st_dev; partitions and dm/md/multipath down to the physical disks; a DB on no block device
# (tmpfs) gives no device verdict and says why.
# The block layer is a fake /sys tree built here (the test build reads it instead of /sys),
# and the directory holding the DB files is pointed at one fake device after another - so
# every mapping is checked the same way on every host, and nothing is written on a real mount.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-dbdisk.XXXXXX"); chmod 755 "$TMP"
SHM=$(mktemp -d /dev/shm/cub_top-dbdisk.XXXXXX 2>/dev/null)
trap 'rm -rf "$TMP" "$SHM"' EXIT
SB="$TMP/sys"; mkdir -p "$SB/class/block" "$SB/dev/block"; chmod 777 "$SB/dev/block"
blk(){ # blk <path under devices> [partition-number]: a device and its class/block link
    d="$SB/devices/$1"; mkdir -p "$d"; echo 1000 > "$d/size"; [ -n "$2" ] && echo "$2" > "$d/partition"
    ln -s "$d" "$SB/class/block/$(basename "$1")"; }
slave(){ mkdir -p "$SB/devices/$1/slaves"; ln -s "$SB/devices/$2" "$SB/devices/$1/slaves/$(basename "$2")"; }
blk sda; blk sda/sda1 1
blk nvme0n1; blk nvme0n1/nvme0n1p1 1; blk nvme0n1/nvme0n1p2 2
blk sdb; blk sdb/sdb1 1; blk sdb/sdb2 2
blk sdc; blk sdc/sdc1 1
blk sdd; blk sde
blk md0;  slave md0 sdb/sdb1; slave md0 sdc/sdc1               # RAID over two partitions
blk dm-0; slave dm-0 nvme0n1/nvme0n1p1                          # LVM on an NVMe partition
blk dm-2; slave dm-2 md0                                        # LVM on the RAID
blk dm-3; slave dm-3 sdd; slave dm-3 sde                        # multipath: two paths
blk dm-4; slave dm-4 sdb/sdb1; slave dm-4 sdb/sdb2              # two partitions of one disk
sed -e "s|\"/sys/dev/block/%u:%u\"|\"$SB/dev/block/%u:%u\"|" "$SRC" > "$TMP/src.c"
grep -q "$SB/dev/block/%u:%u" "$TMP/src.c" || { echo "→ FAIL (sysfs path not found)"; exit 1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static const char *SB;
static pid_t holder(const char *dir,const char *a,const char *b){   /* a server stand-in holding files open */
    pid_t c=fork(); if(c) { usleep(100000); return c; }
    char p[512]; if(a){ snprintf(p,sizeof p,"%s/%s",dir,a); open(p,O_RDWR|O_CREAT,0600); }
    if(b){ snprintf(p,sizeof p,"%s/%s",dir,b); open(p,O_RDWR|O_CREAT,0600); }
    pause(); _exit(0);
}
/* The directory's st_dev now resolves to fake device dev */
static void point(const char *dir,const char *dev){
    struct stat st; char l[1024], t[1024]; if(stat(dir,&st)) return;
    snprintf(l,sizeof l,"%s/dev/block/%u:%u",SB,major(st.st_dev),minor(st.st_dev)); unlink(l);
    snprintf(t,sizeof t,"%s/class/block/%s",SB,dev); if(symlink(t,l)){}
}
static void join(char *o,size_t on,char dk[][32],int n){ o[0]=0; for(int i=0;i<n;i++){ size_t l=strlen(o); snprintf(o+l,on-l,"%s%s",i?",":"",dk[i]); } }
int main(int c,char**v){
    const char *tmp=v[1], *shm=v[2]; int bad=0, ok; char dk[8][32], why[160], got[300]; pid_t h; int n;
    SB=v[3];
    /* Each mapping: the files' directory points at a device, a new holder (the result is cached per process) */
    static const struct { const char *dev,*want,*what; } M[]={
        {"sda1","sda","a partition: its disk"},
        {"sda","sda","a whole disk"},
        {"nvme0n1p2","nvme0n1","an NVMe partition: its namespace"},
        {"md0","sdb,sdc","md RAID over partitions: the member disks"},
        {"dm-0","nvme0n1","dm (LVM) on an NVMe partition"},
        {"dm-2","sdb,sdc","dm on md: through both layers"},
        {"dm-3","sdd,sde","multipath: every path's disk"},
        {"dm-4","sdb","dm over two partitions of one disk: listed once"} };
    for(unsigned i=0;i<sizeof M/sizeof M[0];i++){
        char db[16]; snprintf(db,sizeof db,"mdb%u",i);
        char lg[24]; snprintf(lg,sizeof lg,"%s_lgat",db);
        point(tmp,M[i].dev); h=holder(tmp,db,lg);
        n=db_disks(h,db,dk,8,why,sizeof why); join(got,sizeof got,dk,n);
        ok = !strcmp(got,M[i].want) && g_dk_via==1;
        printf("  %s   %-48s %s -> %s (want %s)%s%s\n",ok?"ok  ":"FAIL",M[i].what,M[i].dev,n?got:"-",M[i].want,n?"":" - ",n?"":why); bad|=!ok;
        kill(h,SIGKILL); waitpid(h,NULL,0);
    }
    point(tmp,"sda1");
    pid_t h0=holder(tmp,"tdb","tdb_lgat");
    if(shm[0]){ pid_t h1=holder(shm,"tdb2","tdb2_lgat");
        n=db_disks(h1,"tdb2",dk,8,why,sizeof why);
        ok = n==0 && why[0];
        printf("  %s   files on tmpfs: %d disk(s), reason \"%s\"\n",ok?"ok  ":"FAIL",n,why); bad|=!ok; kill(h1,SIGKILL); }
    else printf("  SKIP   files on tmpfs (no /dev/shm)\n");
    pid_t h2=holder(tmp,"tdb3x_lgat",NULL);              /* a name merely starting with the db name */
    unsetenv("CUBRID_DATABASES");
    n=db_disks(h2,"tdb3",dk,8,why,sizeof why);
    ok = n==0 && why[0];
    printf("  %s   another db's file (tdb3x_lgat) not taken for tdb3: %d disk(s), \"%s\"\n",ok?"ok  ":"FAIL",n,why); bad|=!ok;
    /* fds not readable (another account's server): databases.txt gives the volume path */
    { char p[512]; snprintf(p,sizeof p,"%s/databases.txt",tmp); FILE *f=fopen(p,"w");
      fprintf(f,"#db-name\tvol-path\tdb-host\tlog-path\tlob-base-path\ntdb4\t%s\tlocalhost\t%s\tfile:%s/lob\n",tmp,tmp,tmp); fclose(f);
      setenv("CUBRID_DATABASES",tmp,1); }
    n=db_disks(1,"tdb4",dk,8,why,sizeof why);
    char ref[8][32]; int nr=db_disks(h0,"tdb",ref,8,why,sizeof why);
    ok = n>0 && nr>0 && !strcmp(dk[0],"sda") && !strcmp(ref[0],"sda");
    printf("  %s   fds unreadable: databases.txt entry gives %s (open files: %s, want sda)\n",ok?"ok  ":"FAIL",n?dk[0]:"-",nr?ref[0]:"-"); bad|=!ok;
    /* An entry of 3 fields (no log path): the volume path alone, and no read of a log path
       that was never filled in */
    { char p[512]; snprintf(p,sizeof p,"%s/databases.txt",tmp); FILE *f=fopen(p,"w");
      fprintf(f,"tdb6 %s localhost\n",tmp); fclose(f); }
    n=db_disks(2,"tdb6",dk,8,why,sizeof why);
    ok = n>0 && !strcmp(dk[0],"sda");
    printf("  %s   a 3-field entry (no log path): %s from the volume path\n",ok?"ok  ":"FAIL",n?dk[0]:"-"); bad|=!ok;
    /* The verdict's device is the DB disk even when another disk is far busier */
    if(nr>0){
        static proc_t P; static iod_t IO; dstat_t d0[2], d1[2];
        P.pid=h0; snprintf(P.db,sizeof P.db,"tdb");
        memset(d0,0,sizeof d0); memset(d1,0,sizeof d1);
        snprintf(d0[0].name,32,"zzbusy"); snprintf(d1[0].name,32,"zzbusy"); d1[0].io_ms=990; d1[0].rios=5000; d1[0].tiq=9000;
        snprintf(d0[1].name,32,"%s",ref[0]); snprintf(d1[1].name,32,"%s",ref[0]); d1[1].io_ms=100; d1[1].rios=50; d1[1].rticks=100; d1[1].tiq=100;
        snprintf(IO.devnm,sizeof IO.devnm,"zzbusy");
        cap_collect(&P,&IO,d0,2,d1,2,1.0);
        ok = !strcmp(CAP2.dev,ref[0]) && CAP2.dev_iops==50 && CAP2.qdepth<1;
        printf("  %s   host's busiest is zzbusy (5000 IOPS, q9): judged %s at %.0f IOPS q%.1f\n",ok?"ok  ":"FAIL",CAP2.dev,CAP2.dev_iops,CAP2.qdepth); bad|=!ok;
    }
    /* No DB disk: no storage sentence built from some other disk */
    { const char *col; char o[512];
      memset(&CAP2,0,sizeof CAP2); CAP2.headroom_pct=-1; CAP2.await_ms=CAP2.qdepth=-1; CAP2.miss_pps=-1; CAP2.readmit_pct=-1; CAP2.hit_pct=-1;
      snprintf(CAP2.dev_why,sizeof CAP2.dev_why,"test reason");
      vd_capacity(o,sizeof o,&col);
      ok = strstr(o,"test reason") && !strstr(o,"IOPS");
      printf("  %s   no DB disk: \"%s\"\n",ok?"ok  ":"FAIL",o); bad|=!ok; }
    kill(h0,SIGKILL); kill(h2,SIGKILL);
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
chmod 755 "$TMP/t"; chmod 777 "$TMP"; [ -n "$SHM" ] && chmod 777 "$SHM"
# As root, drop to nobody (present on every system) so the open-file path is checked
# without privilege; as any other account, run as that account
RUN=""
if [ "$(id -u)" = 0 ]; then
    if command -v setpriv >/dev/null; then RUN="setpriv --reuid=65534 --regid=65534 --clear-groups --"
    elif command -v runuser >/dev/null && id nobody >/dev/null 2>&1; then RUN="runuser -u nobody --"
    else echo "  NOTE   running as root (no setpriv/runuser to drop privileges)"; fi
fi
if $RUN "$TMP/t" "$TMP" "${SHM:-}" "$SB"; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
