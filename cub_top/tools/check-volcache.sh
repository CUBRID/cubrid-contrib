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
# The volumes' page-cache residency: mincore over the server's open <db>, <db>_* files
# (one per inode), on its own period, without touching a page.  A stand-in holds two 32MB
# files open (one of them twice): one is left in the page cache after writing, the other
# is dropped with DONTNEED, so about half is resident.  The second frame is inside the
# period: no new measurement, only the age.  A server of another account whose volumes
# cannot be opened reads as not measured, with the reason.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-volcache.XXXXXX"); PIDS=""
trap 'for p in $PIDS; do kill -9 $p 2>/dev/null; done; rm -rf "$TMP"' EXIT
rc=0
mkdir -p "$TMP/db"; chmod 755 "$TMP" "$TMP/db"
cat > "$TMP/hold.c" <<'C'
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int c,char**v){   /* hold <db> <dir>: writes <db> (kept cached) and <db>_lgat (dropped), holds them open, <db> twice */
    char p[600]; static char buf[1<<20]; memset(buf,7,sizeof buf);
    snprintf(p,sizeof p,"%s/%s",v[2],v[1]); int a=open(p,O_RDWR|O_CREAT|O_TRUNC,0600);
    for(int i=0;i<32;i++) if(write(a,buf,sizeof buf)<0) return 1;
    fsync(a);
    snprintf(p,sizeof p,"%s/%s_lgat",v[2],v[1]); int b=open(p,O_RDWR|O_CREAT|O_TRUNC,0600);
    for(int i=0;i<32;i++) if(write(b,buf,sizeof buf)<0) return 1;
    fsync(b); posix_fadvise(b,0,0,POSIX_FADV_DONTNEED);
    snprintf(p,sizeof p,"%s/%s",v[2],v[1]); int a2=open(p,O_RDONLY); (void)a2;
    snprintf(p,sizeof p,"%s/%s_other",v[2],v[1]); int o=open(p,O_RDWR|O_CREAT|O_TRUNC,0600);   /* <db>_other: also a db file by name, 1MB cached */
    if(write(o,buf,sizeof buf)<0) return 1; fsync(o);
    for(;;) pause();
}
C
gcc -O1 -o "$TMP/hold" "$TMP/hold.c" || { echo "→ FAIL (stand-in)"; exit 1; }
"$TMP/hold" vcdb "$TMP/db" & H=$!; PIDS="$H"
sleep 1
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
int main(int c,char**v){
    int bad=0, ok; static proc_t P; P.pid=atoi(v[1]); snprintf(P.db,sizeof P.db,"vcdb");
    vol_cache_scan(&P,0.5);
    double pct=VC.total>0?100.0*VC.resident/VC.total:-1;
    ok = VC.nfiles==3 && VC.total==65.0*1048576 && pct>35 && pct<65 && VC.age_s==0;
    printf("  %s   3 files (one open twice counted once), %.0fMB, %.0f%% resident (one cached, one dropped), %.1fms%s\n",ok?"ok  ":"FAIL",VC.total/1048576,pct,VC.ms,VC.why[0]?VC.why:""); bad|=!ok;
    double m0=VC.ms, at0=VC.at_ms; usleep(200000);
    vol_cache_scan(&P,0.5);
    ok = VC.at_ms==at0 && VC.age_s>=0.2 && VC.ms==m0;
    printf("  %s   next frame inside the period: not measured again, age %.1fs\n",ok?"ok  ":"FAIL",VC.age_s); bad|=!ok;
    VC.at_ms=-1; vol_cache_scan(&P,0.5);
    ok = VC.age_s==0 && VC.nfiles==3;
    printf("  %s   due again: measured (%d files)\n",ok?"ok  ":"FAIL",VC.nfiles); bad|=!ok;
    static proc_t Q; Q.pid=-1; snprintf(Q.db,sizeof Q.db,"vcdb"); vol_cache_scan(&Q,0.5);
    ok = VC.total==0 && VC.why[0];
    printf("  %s   server down: not measured (%s)\n",ok?"ok  ":"FAIL",VC.why); bad|=!ok;
    static proc_t R; R.pid=1; snprintf(R.db,sizeof R.db,"nosuchdb"); unsetenv("CUBRID_DATABASES"); vol_cache_scan(&R,0.5);
    ok = VC.total==0 && strstr(VC.why,"databases.txt");
    printf("  %s   no file of that name anywhere: \"%s\"\n",ok?"ok  ":"FAIL",VC.why); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
"$TMP/t" $H || rc=1
# Another account's server: its fd table is not readable, so the files come from the
# databases.txt entry, and they cannot be opened (0600): not measured, with the reason
if [ "$(id -u)" = 0 ] && id nobody >/dev/null 2>&1 && command -v setpriv >/dev/null; then
    chmod 755 "$TMP/t"; printf 'vcdb\t%s\tlocalhost\t%s\n' "$TMP/db" "$TMP/db" > "$TMP/databases.txt"; chmod 644 "$TMP/databases.txt"
    out=$(setpriv --reuid=65534 --regid=65534 --clear-groups -- env CUBRID_DATABASES="$TMP" "$TMP/t" $H 2>&1 | head -1)
    case "$out" in *"읽기 불가(다른 계정)"*|*"another account"*) echo "  ok     another account: found through databases.txt, not readable, so not measured" ;;
                   *) echo "  FAIL   another account: $out"; rc=1 ;; esac
else echo "  SKIP   another account (needs root, nobody and setpriv)"; fi
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
