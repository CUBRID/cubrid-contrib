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
# smaps is read on a period set by its own cost, not every frame: between reads the regions
# are kept (their age is shown and recorded) and the totals come from status, so resident
# memory still moves every frame; a change of the mapping set (maps, no page-table walk)
# brings the next read forward (within a 10% budget).  The /proc walk runs every 2 s: a server started between
# walks appears within one period, one that ended is gone the next frame.
# A stand-in cub_server (<db> <lib>) touches a pre-mapped 64MB on SIGUSR1 (resident grows,
# mappings unchanged) and maps another 64MB on SIGUSR2 (mappings change).  The test build
# sets SMAPS_BUDGET so low that the period is the 60 s cap: smaps only on the first frame
# and on a mapping change.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-adaptive.XXXXXX"); PIDS=""
trap 'for p in $PIDS; do kill -9 $p 2>/dev/null; done; rm -rf "$TMP"' EXIT
rc=0
sed 's/^#define SMAPS_BUDGET 0.01$/#define SMAPS_BUDGET 0.000001/' "$SRC" > "$TMP/t.c"
grep -q 'SMAPS_BUDGET 0.000001' "$TMP/t.c" || { echo "→ FAIL (SMAPS_BUDGET not found)"; exit 1; }
gcc -O2 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/ct" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (build)"; exit 1; }
mkdir -p "$TMP/root/lib"; head -c 8192 /dev/zero > "$TMP/root/lib/libcubrid.so.11.5"
cat > "$TMP/srv.c" <<'C'
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define MB (64UL<<20)
static char *pre; static volatile sig_atomic_t ev=0;
static void h(int s){ ev=s; }
int main(int c,char**v){   /* cub_server <db> <lib> */
    int fd=open(v[2],O_RDONLY); void*p=mmap(0,8192,PROT_READ,MAP_PRIVATE,fd,0); (void)p;
    pre=mmap(0,MB,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    signal(SIGUSR1,h); signal(SIGUSR2,h);
    for(;;){ pause();
        if(ev==SIGUSR1) memset(pre,1,MB);                                  /* resident +64MB, same mappings */
        if(ev==SIGUSR2){ char*q=mmap(0,MB,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); memset(q,1,MB); }   /* a new mapping */
        ev=0; }
}
C
gcc -O1 -o "$TMP/cub_server" "$TMP/srv.c" || { echo "→ FAIL (stand-in)"; exit 1; }
"$TMP/cub_server" adp_x "$TMP/root/lib/libcubrid.so.11.5" >/dev/null 2>&1 & S1=$!; PIDS="$S1"
sleep 0.3
"$TMP/ct" -b --interval 0.25 --record "$TMP/rec.jsonl" adp_x </dev/null >/dev/null 2>&1 & CT=$!; PIDS="$PIDS $CT"
sleep 1.5; kill -USR1 $S1; T1=$(date +%s.%N)
sleep 2.5; kill -USR2 $S1; T2=$(date +%s.%N)
sleep 1.5; "$TMP/cub_server" adp_y "$TMP/root/lib/libcubrid.so.11.5" >/dev/null 2>&1 & S2=$!; PIDS="$PIDS $S2"; T3=$(date +%s.%N)
sleep 3.0; kill -9 $S2; wait $S2 2>/dev/null; T4=$(date +%s.%N)
sleep 1.5; kill -TERM $CT; wait $CT 2>/dev/null
python3 - "$TMP/rec.jsonl" $T1 $T2 $T3 $T4 <<'PY' || rc=1
import json,sys
fr=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
t1,t2,t3,t4=map(float,sys.argv[2:6]); bad=0
def t(f): return f['ts_epoch_ms']/1000
def ok(c,msg): 
    global bad; print("  %s   %s"%("ok  " if c else "FAIL",msg)); bad|=not c
A=[f for f in fr if t(f)<t1-0.05]; B=[f for f in fr if t1+0.4<t(f)<t2-0.05]; C=[f for f in fr if t2+0.4<t(f)<t3-0.05]
ages=[f['server.smaps_age_s'] for f in A]
ok(len(A)>=3 and all(b>a for a,b in zip(ages,ages[1:])) and ages[-1]>0.5, "smaps not re-read while nothing changes: regions age %s"%ages)
rssB=max(f['server.rss_kb'] for f in B) if B else 0   # the 64MB touch may still be in progress on a loaded host
ok(B and rssB-A[-1]['server.rss_kb']>=30000 and B[-1]['server.smaps_age_s']>A[-1]['server.smaps_age_s'],
   "resident growth seen from status while the regions stay (rss %d -> %d kB, age %.1f s)"%(A[-1]['server.rss_kb'],rssB,B[-1]['server.smaps_age_s']))
ok(B and B[-1]['server.pss_kb']==A[-1]['server.pss_kb'], "PSS (smaps only) unchanged until smaps is read again (%d kB)"%B[-1]['server.pss_kb'])
ok(C and min(f['server.smaps_age_s'] for f in C)<=0.6 and C[-1]['server.pss_kb']-A[-1]['server.pss_kb']>=60000,
   "a new mapping brings smaps forward: age back to %.1f s, PSS %d -> %d kB"%(min(f['server.smaps_age_s'] for f in C),A[-1]['server.pss_kb'],C[-1]['server.pss_kb']))
seen=[t(f) for f in fr if 'instance.adp_y.pid' in f]
ok(seen and seen[0]-t3<=2.6, "a server started between walks appears within the 2 s walk period (%.1f s)"%((seen[0]-t3) if seen else -1))
ok(seen and seen[-1]-t4<=0.6, "a server that ended is gone the next frame (last seen %.1f s after the kill)"%((seen[-1]-t4) if seen else -1))
sys.exit(1 if bad else 0)
PY
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
