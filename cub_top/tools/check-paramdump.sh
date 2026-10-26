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
# paramdump is the one request cub_top makes of a server, and each attempt is a full DBA
# login.  It must run from the install the server mapped (a client of another version
# waits on the handshake with no answer), never block a live frame, never open a database
# stand-alone (-C), and never be retried on its own (the user re-reads it by switching views).
# A stand-in "server" maps <root>/lib/libcubrid.so.11.5 and carries CUBRID_DATABASES in its
# environment; <root>/bin/cubrid is a script that answers, refuses or hangs.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=${TMPDIR:-/tmp}/cub_top-paramdump.$$; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
R="$TMP/root"; mkdir -p "$R/lib" "$R/bin"
head -c 8192 /dev/zero > "$R/lib/libcubrid.so.11.5"
cat > "$R/bin/cubrid" <<'SH'
#!/bin/sh
echo "$*" > "$PD_LOG.args"; echo "$CUBRID_DATABASES" > "$PD_LOG.dbs"; echo x >> "$PD_LOG.count"
id -u > "$PD_LOG.uid"; ls -l /proc/self/fd > "$PD_LOG.fd" 2>&1
case "$PD_MODE" in
  ok)     echo "[S ] data_buffer_size=512M"; echo "data_buffer_pages=32768" ;;
  refuse) echo "Failed to connect to database server, 'db', on the following host(s): localhost" >&2; exit 1 ;;
  hang)   sleep 30 ;;
esac
SH
chmod +x "$R/bin/cubrid"
cat > "$TMP/srv.c" <<'C'
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <grp.h>
#include <unistd.h>
int main(int c,char**v){
    const char *u=getenv("SRV_UID"), *g=getenv("SRV_GID");   /* a server owned by another account */
    if(u && g){ setgroups(0,0); if(setgid(atoi(g)) || setuid(atoi(u))) return 1; }
    int fd=open(v[1],O_RDONLY); void*p=mmap(0,8192,PROT_READ,MAP_PRIVATE,fd,0);
    (void)p; if(c>2) unlink(v[1]); pause(); return 0; }
C
gcc -O1 -o "$TMP/srv" "$TMP/srv.c" || { echo "→ FAIL (stand-in)"; exit 1; }
# Short deadline for the test
sed -e 's/^#define PD_WAIT_MS 10000 /#define PD_WAIT_MS 1500 /' "$SRC" > "$TMP/src.c"
grep -q '^#define PD_WAIT_MS 1500 ' "$TMP/src.c" || { echo "→ FAIL (constant not found)"; exit 1; }
# A library with DWARF, for an extraction child to crash in
printf 'struct session_state { int a; long b; char c[296]; }; struct session_state the_session;\n' > "$TMP/lib.c"
gcc -g -O0 -shared -fPIC -o "$TMP/lib.so" "$TMP/lib.c" || { echo "→ FAIL (lib.so)"; exit 1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src.c" > "$TMP/t.c"
cat >> "$TMP/t.c" <<'C'
static int run(const char *mode,int spid,int wait_ms,double *worst_ms,int *polls){
    setenv("PD_MODE",mode,1);
    FILE *pd=paramdump_start(spid,"db"); if(!pd) return g_pd_state;
    *worst_ms=0; *polls=0;
    while(pd){ double t=now_ms(); paramdump_poll(&pd,wait_ms); double d=now_ms()-t;
               if(d>*worst_ms) *worst_ms=d; (*polls)++; if(pd) usleep(50000); }
    return g_pd_state;
}
int main(int c,char**v){
    int spid=atoi(v[1]), dpid=atoi(v[2]), xpid=atoi(v[3]), bad=0; double w; int np; char b[256]; FILE*f;
    /* Hold the descriptors a live run keeps open: pagemap and /proc/<target>/mem */
    { char z; unsigned long a=0; char mp[64],ln[512]; snprintf(mp,sizeof mp,"/proc/%d/maps",spid);
      FILE*mf=fopen(mp,"r"); if(mf){ if(fgets(ln,sizeof ln,mf)) a=strtoul(ln,NULL,16); fclose(mf); }
      (void)pm_present(spid,a); CAP.vm_readv=0; CAP.mem_pread=1; (void)vmread(spid,a,&z,1); }
    const char *log=getenv("PD_LOG");
    int st=run("ok",spid,0,&w,&np);
    printf("  %s   answers: state %d (want %d), data_buffer_pages applied %lld, page %lld\n",
           st==PD_OK&&prm_get("data_buffer_pages")==32768&&g_pagesize==16384?"ok  ":"FAIL",st,PD_OK,prm_get("data_buffer_pages"),g_pagesize);
    bad|=!(st==PD_OK&&prm_get("data_buffer_pages")==32768);
    snprintf(b,sizeof b,"%s.args",log); f=fopen(b,"r"); char args[128]=""; if(f){ if(!fgets(args,sizeof args,f)) args[0]=0; fclose(f); }
    snprintf(b,sizeof b,"%s.dbs",log);  f=fopen(b,"r"); char dbs[128]=""; if(f){ if(!fgets(dbs,sizeof dbs,f)) dbs[0]=0; fclose(f); }
    int ok=strstr(args,"paramdump -C db")&&strstr(dbs,"/srv/databases");
    printf("  %s   run as the server: args \"%.*s\", CUBRID_DATABASES %.*s\n",ok?"ok  ":"FAIL",(int)strcspn(args,"\n"),args,(int)strcspn(dbs,"\n"),dbs);
    bad|=!ok;
    snprintf(b,sizeof b,"%s.uid",log); f=fopen(b,"r"); int uid=-1; if(f){ if(fscanf(f,"%d",&uid)!=1) uid=-1; fclose(f); }
    const char *wu=getenv("WANT_UID");
    if(wu && *wu){ ok = uid==atoi(wu);
        printf("  %s   run as the server's account: uid %d (want %s)\n",ok?"ok  ":"FAIL",uid,wu); bad|=!ok; }
    else printf("  SKIP   account switch (needs root and a second account)\n");
    snprintf(b,sizeof b,"%s.fd",log); f=fopen(b,"r"); int leak=0; char fl[512];
    while(f && fgets(fl,sizeof fl,f)) if(strstr(fl,"/mem")||strstr(fl,"pagemap")) leak++;
    if(f) fclose(f);
    printf("  %s   no descriptor of ours in the child (%d of /proc/<pid>/mem or pagemap; g_mem_fd %d open here)\n",leak==0&&g_mem_fd>=0?"ok  ":"FAIL",leak,g_mem_fd);
    bad|=!(leak==0&&g_mem_fd>=0);
    snprintf(b,sizeof b,"%s.count",log); unlink(b);
    { FILE *xp=paramdump_start(xpid,"db"); f=fopen(b,"r");
      ok = !xp && g_pd_state==PD_SKIPPED && !f;
      printf("  %s   install not found: not run with our own $CUBRID (state %d, \"%s\")\n",ok?"ok  ":"FAIL",g_pd_state,g_pd_why);
      if(f) fclose(f); bad|=!ok; }
    st=run("hang",spid,0,&w,&np);
    ok = st==PD_HUNG && w<50;
    printf("  %s   hangs: state %d (want %d), %d live polls, longest %.1fms (want < 50, never blocks)\n",ok?"ok  ":"FAIL",st,PD_HUNG,np,w);
    bad|=!ok;
    usleep(200000); pd_reap_zombies();
    printf("  %s   hung child killed and reaped (%d left)\n",g_pd_nz==0?"ok  ":"FAIL",g_pd_nz); bad|=g_pd_nz!=0;
    /* A refusal is reported with the server's own line and nothing is retried on a timer */
    snprintf(b,sizeof b,"%s.count",log); unlink(b);
    st=run("refuse",spid,0,&w,&np);
    usleep(500000); pd_reap_zombies();
    f=fopen(b,"r"); int n=0; char ln[8]; while(f && fgets(ln,sizeof ln,f)) n++; if(f) fclose(f);
    ok = st==PD_REFUSED && strstr(g_pd_why,"Failed to connect") && n==1 && !g_pd_applied;
    printf("  %s   refused: state %d (want %d), %d attempt(s), reason \"%s\"\n",ok?"ok  ":"FAIL",st,PD_REFUSED,n,g_pd_why);
    bad|=!ok;
    FILE *pd=paramdump_start(dpid,"db");
    printf("  %s   library replaced on disk: not run, state %d (want %d) \"%s\"\n",!pd&&g_pd_state==PD_SKIPPED?"ok  ":"FAIL",g_pd_state,PD_SKIPPED,g_pd_why);
    bad|=!(!pd&&g_pd_state==PD_SKIPPED);
    st=run("ok",spid,PD_WAIT_MS,&w,&np);
    printf("  %s   one-shot waits for the answer in one poll: %d poll(s), state %d\n",np==1&&st==PD_OK?"ok  ":"FAIL",np,st); bad|=!(np==1&&st==PD_OK);
    /* A server without CUBRID_DATABASES in its environment: the engine defaults to
       $CUBRID/databases, so the client gets that directory - not a skip, not this shell's */
    int npid=atoi(v[4]);
    setenv("CUBRID_DATABASES","/not/the/servers",1);
    snprintf(b,sizeof b,"%s.dbs",log); unlink(b);
    st=run("ok",npid,PD_WAIT_MS,&w,&np);
    f=fopen(b,"r"); dbs[0]=0; if(f){ if(!fgets(dbs,sizeof dbs,f)) dbs[0]=0; fclose(f); } dbs[strcspn(dbs,"\n")]=0;
    { char want[600]; snprintf(want,sizeof want,"%s/databases",getenv("CUBRID"));
      ok = st==PD_OK && !strcmp(dbs,want);
      printf("  %s   CUBRID_DATABASES unset in the server: run with $CUBRID/databases (%s), state %d\n",ok?"ok  ":"FAIL",dbs,st); bad|=!ok; }
    /* cubrid.conf is read before paramdump is asked, so a refused (or skipped, hung) attempt
       still leaves the configured values: the startup path, as after a restart */
    g_nprm=0; params_preload(npid,"db");
    long long c0=prm_get("data_buffer_size");
    st=run("refuse",npid,PD_WAIT_MS,&w,&np); usleep(300000); pd_reap_zombies();
    ok = c0==268435456 && st==PD_REFUSED && prm_get("data_buffer_size")==268435456 && !g_pd_applied;
    printf("  %s   preloaded cubrid.conf (data_buffer_size %lld) kept through a refusal (now %lld, state %d)\n",
           ok?"ok  ":"FAIL",c0,prm_get("data_buffer_size"),st); bad|=!ok;
    st=run("ok",npid,PD_WAIT_MS,&w,&np);
    ok = st==PD_OK && prm_get("data_buffer_size")==536870912 && g_pd_applied;
    printf("  %s   an answer replaces them (data_buffer_size %lld)\n",ok?"ok  ":"FAIL",prm_get("data_buffer_size")); bad|=!ok;
    /* A DWARF extraction child that crashes inherited on_fatal: it must not stop the
       parent's paramdump group (a sleeper standing in for it) with its copy of g_pd_pid */
    { struct rlimit rl={0,0}; setrlimit(RLIMIT_CORE,&rl);
      pid_t s=fork(); if(s==0){ setpgid(0,0); execlp("sleep","sleep","30",(char*)NULL); _exit(127); }
      setpgid(s,s); g_pd_pid=s; signal(SIGSEGV,on_fatal);
      const char *so=getenv("DW_SO");
      g_pd_async=1; long o[DW_N]; char w[160]=""; int st2=dw_load_or_extract(so,o,w,sizeof w);
      int stopped = st2==0 && g_dw_kid>0 && kill(g_dw_kid,SIGSTOP)==0, sig=0;
      if(stopped){ kill(g_dw_kid,SIGSEGV); kill(g_dw_kid,SIGCONT); int ws=0; waitpid(g_dw_kid,&ws,0); sig=WIFSIGNALED(ws)?WTERMSIG(ws):0; }
      usleep(200000); int sst=0, alive = waitpid(s,&sst,WNOHANG)==0;   /* a zombie still answers kill(s,0) */
      signal(SIGSEGV,SIG_DFL); g_dw_kid=0; if(g_dw_kfd>=0){ close(g_dw_kfd); g_dw_kfd=-1; }
      kill(-s,SIGKILL); if(alive) waitpid(s,NULL,0); g_pd_pid=-1;
      ok = stopped && sig==SIGSEGV && alive;
      printf("  %s   DWARF child crashing (SIGSEGV) leaves the paramdump group alone: child %s, group %s\n",ok?"ok  ":"FAIL",
             !stopped?"not caught running":sig==SIGSEGV?"died by SIGSEGV":"ended otherwise",alive?"alive":"killed"); bad|=!ok; }
    return bad;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/t" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile failed)"; exit 1; }
cp "$R/lib/libcubrid.so.11.5" "$TMP/gone.so.11.5"; mkdir -p "$TMP/root2/lib"; mv "$TMP/gone.so.11.5" "$TMP/root2/lib/libcubrid.so.11.5"
mkdir -p "$TMP/logs"; chmod 777 "$TMP/logs"; chmod 755 "$TMP" "$R" "$R/bin" "$R/lib"
SU=""; if [ "$(id -u)" = 0 ] && id hiclass >/dev/null 2>&1; then SU="SRV_UID=$(id -u hiclass) SRV_GID=$(id -g hiclass)"; fi
env CUBRID_DATABASES=/srv/databases $SU "$TMP/srv" "$R/lib/libcubrid.so.11.5" & s1=$!
sleep 600 & s3=$!
"$TMP/srv" "$TMP/root2/lib/libcubrid.so.11.5" unlink & s2=$!
# A server of this account with CUBRID set and no CUBRID_DATABASES, and a cubrid.conf to preload
mkdir -p "$R/conf"; printf '[common]\ndata_buffer_size=256M\n' > "$R/conf/cubrid.conf"
env -u CUBRID_DATABASES CUBRID="$R" "$TMP/srv" "$R/lib/libcubrid.so.11.5" & s4=$!
sleep 0.3
# fdopen failing after the fork: the child and the pipe must not be left behind
sed -e 's|FILE \*pf=fdopen(fd\[0\],"r");|FILE *pf=(errno=EMFILE,(FILE*)0);|' "$TMP/src.c" > "$TMP/src2.c"
grep -q 'errno=EMFILE' "$TMP/src2.c" || { echo "→ FAIL (fdopen not found in the source)"; exit 1; }
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$TMP/src2.c" > "$TMP/f.c"
cat >> "$TMP/f.c" <<'C'
static int nfd(void){ int n=0; DIR*d=opendir("/proc/self/fd"); struct dirent*e; while(d&&(e=readdir(d))) n++; if(d) closedir(d); return n; }
int main(int c,char**v){
    setenv("PD_MODE","hang",1);
    int f0=nfd(); FILE *pd=paramdump_start(atoi(v[1]),"db"); int f1=nfd();
    usleep(300000); pd_reap_zombies();
    int ok = !pd && g_pd_state==PD_SKIPPED && strstr(g_pd_why,"fdopen") && f1==f0 && g_pd_nz==0 && g_pd_pid<0;
    printf("  %s   fdopen failing: none returned, state %d, \"%s\", fds %d -> %d, child reaped (%d left)\n",
           ok?"ok  ":"FAIL",g_pd_state,g_pd_why,f0,f1,g_pd_nz);
    return !ok;
}
C
gcc -O1 -std=gnu99 -w -I "$(dirname "$SRC")" -o "$TMP/f" "$TMP/f.c" -lm -lpthread 2>"$TMP/cc2.err" || { cat "$TMP/cc2.err"; echo "→ FAIL (compile failed)"; exit 1; }
CUBRID="$R" PD_LOG="$TMP/logs/log" DW_SO="$TMP/lib.so" WANT_UID="$(echo $SU | sed -n 's/SRV_UID=\([0-9]*\).*/\1/p')" "$TMP/t" $s1 $s2 $s3 $s4; rc=$?
CUBRID="$R" PD_LOG="$TMP/logs/log" "$TMP/f" $s1 || rc=1
kill $s1 $s2 $s3 $s4 2>/dev/null; wait $s1 $s2 $s3 $s4 2>/dev/null
# Live: a paramdump that hangs is held to its deadline while paused or in the help, and
# nothing it started outlives the run - quit while it waits, a one-shot run interrupted by
# SIGINT or crashing (SIGSEGV), a child that closed its output and then hung.  The
# stand-in's cubrid execs a named script so its processes can be found and counted.
# Every run names the stand-in's database: unnamed, cub_top would take the first instance
# alphabetically, which on a host with real servers is not the stand-in (and the tree, -t,
# walks them all) - a real DBA paramdump login to a real database.
LT=$TMP/live; LR=$LT/root; mkdir -p $LR/lib $LR/bin
DB=cubtop_pd_$$
head -c 8192 /dev/zero > $LR/lib/libcubrid.so.11.5
printf '#!/bin/sh\nexec %s/${PD_HANG:-hang}\n' "$LT" > $LR/bin/cubrid; printf '#!/bin/sh\nsleep 60\n' > $LT/hang
printf '#!/bin/sh\nexec >/dev/null 2>&1\nsleep 60\n' > $LT/hang2; chmod +x $LR/bin/cubrid $LT/hang $LT/hang2
# The live scenarios run a private copy of the binary under this run's directory, so
# nothing is ever matched by a path another monitor or a parallel test could share.
TB=$LT/cub_top; /bin/cp -f "$ROOT/cub_top" "$TB" || { echo "→ FAIL (cannot copy the binary)"; exit 1; }
# Whatever a scenario left (its own cleanup not reached: timeout, exception): the hang
# groups, found by this run's private path, and the cub_top live.py forked, by its pid
sweep(){ for p in $(pgrep -f -- "$LT/hang"); do kill -9 -- -$(ps -o pgid= -p $p | tr -d ' ') 2>/dev/null; done
         [ -s $LT/live.pid ] && kill -9 $(cat $LT/live.pid) 2>/dev/null; rm -f $LT/live.pid; }
hangs(){ pgrep -f -- "$LT/hang" | wc -l; }
waithang(){ n=0; for i in $(seq 40); do n=$(hangs); [ "$n" -ge 1 ] && break; sleep 0.1; done; echo $n; }   # a loaded host starts slowly
cat > $LT/srv.c <<'C'
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int c,char**v){ int fd=open(v[2],O_RDONLY); void*p=mmap(0,8192,PROT_READ,MAP_PRIVATE,fd,0); (void)p; for(;;) pause(); }   /* cub_server <db> <lib> */
C
gcc -O1 -o $LT/cub_server $LT/srv.c
cat > $LT/live.py <<'PY2'
import os,sys,pty,time,select,signal,fcntl,termios,struct,subprocess
binp,mark,mode,db=sys.argv[1:5]
def kids():
    return [int(x) for x in subprocess.run(["pgrep","-f","--",mark+"/hang"],capture_output=True,text=True).stdout.split()]
def zombies(parent):                      # children of cub_top left unreaped (state Z)
    n=0
    for d in os.listdir("/proc"):
        if not d.isdigit(): continue
        try: st=open("/proc/%s/stat"%d).read()
        except OSError: continue
        f=st[st.rfind(")")+2:].split()
        if f[0]=="Z" and int(f[1])==parent: n+=1
    return n
def pump(fd,sec):
    end=time.time()+sec
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.1)
        if r:
            try: os.read(fd,65536)
            except OSError: return
pid,fd=pty.fork()
if pid==0:
    os.environ["TERM"]="xterm"; os.execv(binp,[binp,"-b","--interval","0.3",db])
open(mark+"/live.pid","w").write(str(pid))   # for the sweep: only the process forked here
k0=k1=k2=[]; z1=-1
try:
    fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',50,160,0,0))
    pump(fd,1.5); k0=kids()
    if mode=="pause":
        os.write(fd,b' '); pump(fd,13); k1=kids(); z1=zombies(pid)
    elif mode=="help":
        os.write(fd,b'h'); pump(fd,13); k1=kids(); z1=zombies(pid); os.write(fd,b'q'); pump(fd,0.5)   # q closes the help
    elif mode=="run":
        pump(fd,13); k1=kids(); z1=zombies(pid)   # frames go on with no request open after the kill
    os.write(fd,b'q'); pump(fd,1.5)
    try: os.waitpid(pid,0)
    except ChildProcessError: pass
    time.sleep(0.3); k2=kids()
finally:                                  # whatever happened above, nothing is left running
    try: os.kill(pid,signal.SIGKILL)
    except OSError: pass
    for p in set(k0+k1+k2+kids()):
        try: os.killpg(os.getpgid(p),signal.SIGKILL)
        except OSError: pass
ok = len(k0)>=1 and len(k1)==0 and len(k2)==0 and z1<=0
if mode=="pause":  print("  %s   live, paused 13 s while paramdump hangs: %d waiting, %d after the deadline (zombies %d), %d after quit"%("ok  " if ok else "FAIL",len(k0),len(k1),z1,len(k2)))
elif mode=="help": print("  %s   live, help open 13 s while paramdump hangs: %d waiting, %d after the deadline (zombies %d), %d after quit"%("ok  " if ok else "FAIL",len(k0),len(k1),z1,len(k2)))
elif mode=="run":  print("  %s   live, running 13 s while paramdump hangs: %d waiting, %d after the deadline, zombies %d (want 0 - killed and reaped)"%("ok  " if ok else "FAIL",len(k0),len(k1),z1))
else:              print("  %s   live, quit while paramdump hangs: %d waiting, %d after quit"%("ok  " if ok else "FAIL",len(k0),len(k2)))
sys.exit(0 if ok else 1)
PY2
env CUBRID=$LR CUBRID_DATABASES=$LT $LT/cub_server $DB $LR/lib/libcubrid.so.11.5 & ls_=$!
sleep 0.3
live(){ timeout "$1" python3 $LT/live.py "$TB" "$LT" "$2" "$DB" || { echo "  FAIL   live scenario $2 did not finish cleanly"; rc=1; }; sweep; }
live 40 pause
live 40 help
live 40 run
live 20 quit
# One-shot cut by a signal: INT (the default action is kept), and a crash (SIGSEGV)
for sig in INT SEGV; do
    (ulimit -c 0; exec "$TB" -d $DB >/dev/null 2>&1) & op=$!
    n0=$(waithang); kill -$sig $op; wait $op 2>/dev/null; sleep 0.3; n1=$(hangs); sweep
    if [ "$n0" -ge 1 ] && [ "$n1" = 0 ]; then echo "  ok     one-shot cut by SIG$sig while paramdump hangs: $n0 waiting, $n1 after"
    else echo "  FAIL   one-shot cut by SIG$sig: $n0 waiting, $n1 after"; rc=1; fi
done
# Live, signalled during its first frame (interval 3s plus the forced method-A walk, before
# the live handlers are installed): the request must not outlive it.  SIGTERM, because a
# command started with & from a non-interactive shell inherits SIGINT ignored; the wait is
# bounded so a binary that ignores the signal fails the check instead of hanging it.
"$TB" -b --interval 3 $DB >/dev/null 2>&1 & op=$!
n0=$(waithang); kill -TERM $op
for i in $(seq 30); do kill -0 $op 2>/dev/null || break; sleep 0.1; done
kill -0 $op 2>/dev/null && { kill -9 $op 2>/dev/null; echo "  FAIL   live did not end on SIGTERM in its first frame"; rc=1; }
wait $op 2>/dev/null; sleep 0.3; n1=$(hangs); sweep
if [ "$n0" -ge 1 ] && [ "$n1" = 0 ]; then echo "  ok     live cut by SIGTERM in its first frame while paramdump hangs: $n0 waiting, $n1 after"
else echo "  FAIL   live cut by SIGTERM in its first frame: $n0 waiting, $n1 after"; rc=1; fi
# The child closes its output, then hangs: EOF with the group still up is stopped at once
PD_HANG=hang2 "$TB" -d $DB >/dev/null 2>&1 & op=$!
n0=$(waithang); wait $op 2>/dev/null; sleep 0.3; n1=$(hangs); sweep
if [ "$n0" -ge 1 ] && [ "$n1" = 0 ]; then echo "  ok     paramdump closed its output and hung: $n0 while the run waited, $n1 after"
else echo "  FAIL   paramdump closed its output and hung: $n0 while the run waited, $n1 after"; rc=1; fi
kill $ls_ 2>/dev/null; wait $ls_ 2>/dev/null
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
