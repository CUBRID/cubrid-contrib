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
# The same database name under two accounts.  Instances are told apart by account as well as
# name: the PL attaches to its own account's server, a name given at start picks this
# account's server first, and when the watched server goes away the restart is followed
# only to the same account's database of that name - never quietly to another user's.
# The account is recorded when the target is chosen, so a switch followed by a stop before
# any frame still looks for the restart under the account switched to.
# Stand-in processes named cub_server / cub_pl carry the db name as their first argument.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
[ "$(id -u)" = 0 ] && id hiclass >/dev/null 2>&1 || { echo "→ SKIP (needs root and a second account)"; exit 0; }
TMP=$(mktemp -d /tmp/cub_top-samedb.XXXXXX); chmod 755 "$TMP"
a=; b=; c=; trap 'kill $a $b $c 2>/dev/null; rm -rf "$TMP"' EXIT
printf 'int main(){ for(;;) pause(); }\n' | sed 's/^/#include <unistd.h>\n/' > "$TMP/s.c"
gcc -O1 -o "$TMP/cub_server" "$TMP/s.c" && cp "$TMP/cub_server" "$TMP/cub_pl" && chmod 755 "$TMP/cub_server" "$TMP/cub_pl"
DB=samedb_$$
"$TMP/cub_server" $DB & a=$!
runuser -u hiclass -- "$TMP/cub_server" $DB 2>/dev/null &
runuser -u hiclass -- "$TMP/cub_pl" $DB 2>/dev/null &
sleep 0.5
# runuser stays as the parent: the stand-ins' own pids
b=$(pgrep -u hiclass -x cub_server -a | awk -v d=$DB '$3==d{print $1}')
c=$(pgrep -u hiclass -x cub_pl -a | awk -v d=$DB '$3==d{print $1}')
[ -n "$b" ] && [ -n "$c" ] || { echo "→ FAIL (stand-ins not started)"; exit 1; }
HU=$(id -u hiclass)
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
int main(int argc,char**v){
    const char *db=v[1]; int ra=atoi(v[2]), hb=atoi(v[3]), hu=atoi(v[4]), bad=0, ok;
    enum_instances();
    int n=0, ir=-1, ih=-1;
    for(int i=0;i<g_ninst;i++) if(!strcmp(g_inst[i].db,db)){ n++; if(g_inst[i].pid==ra) ir=i; if(g_inst[i].pid==hb) ih=i; }
    ok = n==2 && ir>=0 && ih>=0 && g_inst[ir].uid==0 && g_inst[ih].uid==hu;
    printf("  %s   two servers named %s: root's and uid %d's kept apart\n",ok?"ok  ":"FAIL",db,hu); bad|=!ok;
    ok = ih>=0 && ir>=0 && g_inst[ih].pl_n==1 && g_inst[ir].pl_n==0;
    printf("  %s   the PL attaches to its own account's server (%d / %d)\n",ok?"ok  ":"FAIL",ih>=0?g_inst[ih].pl_n:-1,ir>=0?g_inst[ir].pl_n:-1); bad|=!ok;
    int k=inst_find(db);
    ok = k>=0 && g_inst[k].pid==ra;
    printf("  %s   a name given at start: this account's server first (pid %d)\n",ok?"ok  ":"FAIL",k>=0?g_inst[k].pid:-1); bad|=!ok;
    ok = restart_pick(db,hu)==hb && restart_pick(db,0)==ra;
    printf("  %s   restart followed within the account\n",ok?"ok  ":"FAIL"); bad|=!ok;
    kill(hb,SIGKILL); usleep(200000); enum_instances();
    int np=restart_pick(db,hu);
    ok = np==-1;
    printf("  %s   watched server (uid %d) gone: not followed to root's %s (picked %d)\n",ok?"ok  ":"FAIL",hu,db,np); bad|=!ok;
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
rc=0
"$TMP/t" $DB $a $b $HU || rc=1
kill $a 2>/dev/null
# Switching to the other account's server of that name, which then stops before a frame has
# seen it: the restart is looked for under the account switched to (set at selection), so
# root's server of the same name is not taken, and that account's server is followed when
# it comes back.
"$TMP/cub_server" ${DB}w & a=$!
runuser -u hiclass -- "$TMP/cub_server" ${DB}w 2>/dev/null &
sleep 0.5
b=$(pgrep -u hiclass -x cub_server -a | awk -v d=${DB}w '$3==d{print $1}')
[ -n "$b" ] || { echo "→ FAIL (stand-ins not started)"; exit 1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/w.c"
cat >> "$TMP/w.c" <<'C'
int main(int argc,char**v){
    const char *db=v[1]; int ra=atoi(v[2]), hb=atoi(v[3]), hu=atoi(v[4]), bad=0, ok;
    static proc_t P,P0; static mem_t M; static tier_t T; static iod_t IO; FILE *pd=NULL;
    g_ascii=1; enum_instances();
    int ir=-1, ih=-1;
    for(int i=0;i<g_ninst;i++){ if(g_inst[i].pid==ra) ir=i; if(g_inst[i].pid==hb) ih=i; }
    target_select(&P,ir); sample_once(&P,&P0,&M,&T,&IO,0.05,&pd);      /* watching root's */
    target_select(&P,ih); reset_instance_state();                         /* < or > to uid hu's */
    kill(hb,SIGKILL); usleep(200000);                                     /* gone before a frame */
    sample_once(&P,&P0,&M,&T,&IO,0.05,&pd);
    ok = P.pid==-1;
    printf("  %s   switched to uid %d's server, it stopped before a frame: not running (pid %d), root's %d not taken\n",ok?"ok  ":"FAIL",hu,P.pid,ra); bad|=!ok;
    char cmd[600]; snprintf(cmd,sizeof cmd,"runuser -u hiclass -- %s %s >/dev/null 2>&1 &",v[5],db);
    if(system(cmd)){} usleep(500000);
    sample_once(&P,&P0,&M,&T,&IO,0.05,&pd);
    int nu=-1; for(int i=0;i<g_ninst;i++) if(g_inst[i].pid==P.pid) nu=g_inst[i].uid;
    ok = P.pid>0 && P.pid!=ra && nu==hu;
    printf("  %s   that account's server restarted: followed (pid %d, uid %d)\n",ok?"ok  ":"FAIL",P.pid,nu); bad|=!ok;
    if(P.pid>0 && P.pid!=ra) kill(P.pid,SIGKILL);
    if(pd) paramdump_abort(&pd);
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/w" "$TMP/w.c" -lm -lpthread 2>"$TMP/cc2.err" || { cat "$TMP/cc2.err"; echo "→ FAIL (compile)"; exit 1; }
"$TMP/w" ${DB}w $a $b $HU "$TMP/cub_server" || rc=1
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
