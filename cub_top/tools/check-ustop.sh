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
# A user stop (Ctrl-Z, pause, help) ends the collection segment: data from both sides of
# it is not one data set.  A stand-in cub_server and cub_broker burn a core only while
# collection is stopped, so any frame - or frame-to-frame baseline - that spans a stop
# shows that CPU.  Checked on a live --record run:
#   - no frame shows the CPU burnt while stopped (server.cpu_pct, tier.broker_cores);
#   - nothing is recorded while stopped, and collect.segment goes up once per stop;
#   - a replay rebuilds the history per segment (it starts again at each one).
# And a pid reused while the watched server is gone (ns_last_pid): the new process is
# not taken for the old one - another database's server under the old pid is not shown
# under the old name, and the same database restarted under the old pid starts afresh.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-ustop.XXXXXX")
PIDS=""
trap 'for p in $PIDS; do kill -9 $p 2>/dev/null; done; rm -rf "$TMP"' EXIT
rc=0
[ -x "$ROOT/cub_top" ] || { echo "→ FAIL (build cub_top first)"; exit 1; }
cp "$ROOT/cub_top" "$TMP/ct"
cat > "$TMP/burn.c" <<'C'
#include <signal.h>
#include <unistd.h>
static volatile sig_atomic_t on=0;
static void t(int s){ (void)s; on=!on; }
int main(void){ signal(SIGUSR1,t); for(;;){ if(on){ while(on){} } else pause(); } }
C
gcc -O0 -o "$TMP/cub_server" "$TMP/burn.c" && cp "$TMP/cub_server" "$TMP/cub_broker" || { echo "→ FAIL (stand-in)"; exit 1; }
# Started from a subshell, so they are not this shell's jobs (no "Killed" line at the end)
SP=$( "$TMP/cub_server" aaa_ustop >/dev/null 2>&1 & echo $! ); BP=$( "$TMP/cub_broker" >/dev/null 2>&1 & echo $! ); PIDS="$SP $BP"
sleep 0.3
cat > "$TMP/live.py" <<'PY'
import os,sys,pty,time,select,signal,fcntl,termios,struct
binp,rec,sp,bp=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4])
pid,fd=pty.fork()
if pid==0:
    os.environ["TERM"]="xterm"; os.execv(binp,[binp,"-b","--ascii","--interval","0.5","--record",rec,"aaa_ustop"])
def pump(sec):
    end=time.time()+sec
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.1)
        if r:
            try: os.read(fd,65536)
            except OSError: return
def burn(sec):   # keep draining the pty meanwhile: paused/help screens still redraw, and a full pty blocks cub_top
    os.kill(sp,signal.SIGUSR1); os.kill(bp,signal.SIGUSR1); pump(sec)
    os.kill(sp,signal.SIGUSR1); os.kill(bp,signal.SIGUSR1); pump(0.3)
marks=[]
try:
    fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',50,160,0,0))
    pump(3.0)
    # Ctrl-Z: the handler runs (its stop is discarded in this orphaned group), SIGSTOP stops
    t=time.time(); os.kill(pid,signal.SIGTSTP); os.kill(pid,signal.SIGSTOP); burn(3); os.kill(pid,signal.SIGCONT)
    marks.append(("ctrl-z",t,time.time())); pump(3.0)
    t=time.time(); os.write(fd,b' '); pump(0.3); burn(3); os.write(fd,b' '); marks.append(("pause",t,time.time())); pump(3.0)
    t=time.time(); os.write(fd,b'h'); pump(0.3); burn(3); os.write(fd,b'q'); marks.append(("help",t,time.time())); pump(3.0)
    os.write(fd,b'q'); pump(1.5)
finally:
    try: os.kill(pid,signal.SIGKILL)
    except OSError: pass
    try: os.waitpid(pid,0)
    except OSError: pass
for m in marks: print("%s %.3f %.3f"%m)
PY
timeout 60 python3 "$TMP/live.py" "$TMP/ct" "$TMP/rec.jsonl" $SP $BP > "$TMP/marks" || { echo "  FAIL   live run did not finish"; rc=1; }
python3 - "$TMP/rec.jsonl" "$TMP/marks" <<'PY' || rc=1
import json,sys
fr=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
marks=[(l.split()[0],float(l.split()[1]),float(l.split()[2])) for l in open(sys.argv[2])]
bad=0
def t(f): return f.get("ts_epoch_ms",0)/1000.0
for name,a,b in marks:
    inside=[f for f in fr if a+0.05<t(f)<b-0.05]
    ok=not inside
    print("  %s   %s: %d frame(s) recorded while stopped"%("ok  " if ok else "FAIL",name,len(inside))); bad|=not ok
hot=[(round(t(f)%1000,2),f.get("server.cpu_pct"),f.get("tier.broker_cores")) for f in fr
     if (f.get("server.cpu_pct") or 0)>30 or (f.get("tier.broker_cores") or 0)>0.3]
ok=not hot and len(fr)>=10
print("  %s   no frame carries the CPU burnt while stopped (%d frames; over: %s)"%("ok  " if ok else "FAIL",len(fr),hot[:4])); bad|=not ok
seg=[f.get("collect.segment") for f in fr]
segs=sorted(set(s for s in seg if s is not None))
ok = None not in seg and len(segs)==len(marks)+1 and all(seg[i]<=seg[i+1] for i in range(len(seg)-1))
print("  %s   collect.segment goes up once per stop: %s"%("ok  " if ok else "FAIL",segs)); bad|=not ok
sys.exit(1 if bad else 0)
PY
# The replay's history: rebuilt per segment (HN back to 1 where the segment changes)
printf '#define main cub_top_main\n#include "%s"\n#undef main\n' "$SRC" > "$TMP/h.c"
cat >> "$TMP/h.c" <<'C'
int main(int c,char**v){
    FILE *f=fopen(v[1],"r"); if(!f) return 2;
    static proc_t P; static mem_t M; static tier_t T; static iod_t IO;
    static char ln[1<<20]; int resets=0, n=0, bad=0; unsigned seg=0;
    while(fgets(ln,sizeof ln,f)){
        rp_apply(ln,&P,&M,&T,&IO); hist_push(rp_time(ln),&P,&M,&T,&IO);
        if(n && g_seg_gen!=seg){ resets++; if(HN!=1) bad=1; }
        else if(n && HN==1) bad=1;              /* a reset with no segment change */
        seg=g_seg_gen; n++;
    }
    printf("%d %d %d\n",n,resets,bad); return 0;
}
C
gcc -O1 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/h" "$TMP/h.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (compile)"; exit 1; }
set -- $("$TMP/h" "$TMP/rec.jsonl")
if [ "$3" = 0 ] && [ "$2" = 3 ]; then echo "  ok     replay history starts again at each of the $2 segment changes ($1 frames)"
else echo "  FAIL   replay history: $1 frames, $2 segment changes (want 3), wrong reset $3"; rc=1; fi
# A reused pid
cat > "$TMP/reuse.c" <<'C'
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
static pid_t run(const char *bin,const char *db){ pid_t p=fork(); if(p==0){ execl(bin,bin,db,(char*)0); _exit(127); } return p; }
int main(int c,char**v){   /* bin dbA dbB: start A, on a line kill it and start B under A's pid */
    pid_t a=run(v[1],v[2]); printf("%d\n",a); fflush(stdout);
    char b[8]; if(!fgets(b,sizeof b,stdin)) return 1;
    kill(a,SIGKILL); waitpid(a,0,0);
    pid_t p=-1;
    for(int t=0;t<50;t++){
        FILE *f=fopen("/proc/sys/kernel/ns_last_pid","w"); if(!f) break;
        fprintf(f,"%d",(int)a-1); if(fclose(f)) break;
        p=run(v[1],v[3]); if(p==a) break;
        kill(p,SIGKILL); waitpid(p,0,0); p=-1;
    }
    printf("%d\n",(int)p); fflush(stdout);
    if(!fgets(b,sizeof b,stdin)){} if(p>0){ kill(p,SIGKILL); waitpid(p,0,0); }
    return 0;
}
C
gcc -O1 -o "$TMP/reuse" "$TMP/reuse.c" || { echo "→ FAIL (reuse helper)"; exit 1; }
cat > "$TMP/reuse.py" <<'PY'
import os,sys,pty,time,select,signal,subprocess,json
binp,helper,srv,rec,dba,dbb=sys.argv[1:7]
h=subprocess.Popen([helper,srv,dba,dbb],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
a=int(h.stdout.readline()); time.sleep(0.3)
pid,fd=pty.fork()
if pid==0:
    os.environ["TERM"]="xterm"; os.execv(binp,[binp,"-b","--ascii","--interval","0.5","--record",rec,dba])
def pump(sec):
    end=time.time()+sec
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.1)
        if r:
            try: os.read(fd,65536)
            except OSError: return
b=-1
try:
    pump(2.5); h.stdin.write("x\n"); h.stdin.flush(); b=int(h.stdout.readline()); pump(3.0)
    os.write(fd,b'q'); pump(1.0)
finally:
    try: os.kill(pid,signal.SIGKILL); os.waitpid(pid,0)
    except OSError: pass
    try: h.stdin.write("x\n"); h.stdin.flush(); h.wait(5)
    except Exception: h.kill()
print(a,b)
PY
reuse(){ # reuse <label> <dbA> <dbB>
    rm -f "$TMP/r.jsonl"
    out=$(timeout 40 python3 "$TMP/reuse.py" "$TMP/ct" "$TMP/reuse" "$TMP/cub_server" "$TMP/r.jsonl" "$2" "$3")
    set -- "$1" "$2" "$3" $out
    if [ "$5" != "$4" ]; then echo "  SKIP   $1: the pid could not be reused here (ns_last_pid)"; return; fi
    python3 - "$TMP/r.jsonl" "$4" "$2" "$3" "$1" <<'PY' || rc=1
import json,sys
fr=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
pid,dba,dbb,label=int(sys.argv[2]),sys.argv[3],sys.argv[4],sys.argv[5]
st=[f.get("server.start_ticks") for f in fr if f.get("server.pid")==pid]
before=[f for f in fr if f.get("server.pid")==pid and st and f.get("server.start_ticks")==st[0]]
after=fr[len(before):]
if dba!=dbb:   # another database under the old pid: never shown under the old name
    wrong=[f for f in after if f.get("server.pid")==pid]
    ok=before and after and not wrong
    print("  %s   %s: %d frame(s) of %s, then %d with pid %d still shown as %s (want 0)"%("ok  " if ok else "FAIL",label,len(before),dba,len(wrong),pid,dba))
else:          # the same database restarted under the old pid: a new process, a new segment
    back=[f for f in after if f.get("server.pid")==pid]     # frames in between may show it down
    s0=set(f.get("collect.segment") for f in before); s1=set(f.get("collect.segment") for f in back)
    t1=set(f.get("server.start_ticks") for f in back)
    ok=bool(before) and bool(back) and st[0] not in t1 and not (s0&s1)
    print("  %s   %s: start %s -> %s, segments %s -> %s"%("ok  " if ok else "FAIL",label,st[0] if st else None,sorted(t1),sorted(s0),sorted(s1)))
sys.exit(0 if ok else 1)
PY
}
reuse "pid reused by another database's server" aaa_reuse aaa_other
reuse "same database restarted under the same pid" aaa_reuse aaa_reuse
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
