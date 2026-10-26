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
# Live --record renders each frame with fd 1 pointing at a memory file.  Ctrl-Z, fg and a
# crash restore the terminal from signal handlers; if one arrives while a frame is being
# rendered, the restore sequences must go to the terminal, not into the recorded frame.
# A test build raises SIGTSTP and SIGCONT inside the first recorded frame's rendering (the
# process runs in an orphaned group under the pty, so the stop itself is discarded and
# only the handlers run), and SIGSEGV inside a later one.
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$DIR")
SRC="$ROOT/src/cub_top.c"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cub_top-rectty.XXXXXX")
trap 'kill $SP 2>/dev/null; wait $SP 2>/dev/null; rm -rf "$TMP"' EXIT
rc=0
# Test hook: after fd 1 is switched to the frame buffer, frame 1 gets TSTP+CONT, frame 3 (if asked) SEGV
sed -e 's|    if(dup2(fileno(tf),1)<0){ int e=errno; close(save); fclose(tf); rec_fail("dup2",e); return; }|&\n    { static int n=0; n++; if(n==1){ raise(SIGTSTP); raise(SIGCONT); } if(n==3 \&\& getenv("RECTTY_SEGV")) raise(SIGSEGV); }|' \
    "$SRC" > "$TMP/t.c"
grep -q 'raise(SIGTSTP); raise(SIGCONT);' "$TMP/t.c" || { echo "→ FAIL (test hook not inserted)"; exit 1; }
gcc -O2 -std=gnu99 -w -I "$ROOT/src" -o "$TMP/ct" "$TMP/t.c" -lm -lpthread 2>"$TMP/cc.err" || { cat "$TMP/cc.err"; echo "→ FAIL (build)"; exit 1; }
# A stand-in server: a process named cub_server mapping <root>/lib/libcubrid.so; its
# paramdump answers at once
R="$TMP/root"; mkdir -p "$R/lib" "$R/bin"
head -c 8192 /dev/zero > "$R/lib/libcubrid.so.11.5"
printf '#!/bin/sh\necho data_buffer_pages=32768\n' > "$R/bin/cubrid"; chmod +x "$R/bin/cubrid"
cat > "$TMP/srv.c" <<'C'
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int c,char**v){ int fd=open(v[1],O_RDONLY); void*p=mmap(0,8192,PROT_READ,MAP_PRIVATE,fd,0); (void)p; for(;;) pause(); }
C
gcc -O1 -o "$TMP/cub_server" "$TMP/srv.c" || { echo "→ FAIL (stand-in)"; exit 1; }
env CUBRID="$R" CUBRID_DATABASES="$TMP" "$TMP/cub_server" "$R/lib/libcubrid.so.11.5" & SP=$!
sleep 0.3
cat > "$TMP/run.py" <<'PY'
import os,sys,pty,time,select,signal,fcntl,termios,struct
binp,rec,segv,outp=sys.argv[1:5]
pid,fd=pty.fork()
if pid==0:
    os.environ["TERM"]="xterm"
    if segv=="1": os.environ["RECTTY_SEGV"]="1"
    import resource; resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    os.execv(binp,[binp,"-b","--interval","0.3","--record",rec])
out=b""; st=None
def pump(sec):
    global out
    end=time.time()+sec
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.1)
        if r:
            try: out+=os.read(fd,65536)
            except OSError: return False
    return True
try:
    fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',50,160,0,0))
    pump(4.0)
    if segv!="1": os.write(fd,b'q'); pump(1.5)
    else: pump(0.5)
    for i in range(30):
        p,s=os.waitpid(pid,os.WNOHANG)
        if p: st=s; break
        time.sleep(0.1)
finally:
    if st is None:
        try: os.kill(pid,signal.SIGKILL); os.waitpid(pid,0)
        except OSError: pass
open(outp,"wb").write(out)
print(st if st is not None else -1)
PY
leave=$(printf '\033[?25h\033[0m\033(B\017\033[?2004h')     # tui_raw_restore / tui_leave
enter=$(printf '\033(B\017\033[2J\033[H\033[?25l\033[?2004l')   # tui_raw_reenter / tui_enter
cnt(){ python3 -c 'import sys; print(open(sys.argv[1],"rb").read().count(sys.argv[2].encode("latin1")))' "$1" "$2"; }
check(){ # check <label> <segv 0|1> <want leave on the tty> <want enter on the tty>
    REC="$TMP/rec$2.jsonl"; rm -f "$REC"
    st=$(timeout 30 python3 "$TMP/run.py" "$TMP/ct" "$REC" "$2" "$TMP/tty$2")
    nf=$(grep -c '^{' "$REC" 2>/dev/null); nf=${nf:-0}
    esc=$(grep -c -e '\\u001b' -e "$(printf '\033')" "$REC" 2>/dev/null); esc=${esc:-0}
    nl=$(cnt "$TMP/tty$2" "$leave"); ne=$(cnt "$TMP/tty$2" "$enter")
    if [ "$nf" -ge 1 ] && [ "$esc" = 0 ] && [ "$nl" -ge "$3" ] && [ "$ne" -ge "$4" ]; then
        echo "  ok     $1: $nf frame(s) recorded, $esc with a terminal sequence; on the tty restore x$nl, re-enter x$ne"
    else
        echo "  FAIL   $1: $nf frame(s) recorded, $esc with a terminal sequence (want 0); on the tty restore x$nl (want >= $3), re-enter x$ne (want >= $4); exit status $st"
        rc=1
    fi
}
# Ctrl-Z then fg during frame 1: restore by on_tstp and on quit, re-enter by tui_enter and on_cont
check "Ctrl-Z and fg while a recorded frame renders" 0 2 2
# A crash in frame 3: the terminal is restored by on_fatal (frames 1-2 recorded, the third never)
check "crash (SIGSEGV) while a recorded frame renders" 1 2 2
if [ $rc = 0 ]; then echo "→ PASS"; else echo "→ FAIL"; exit 1; fi
