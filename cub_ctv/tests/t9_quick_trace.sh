#!/bin/bash
# t9_quick_trace.sh - is tracing possible on this server right now?
#
# Runs a short contention load (parallel INSERT + CREATE/DROP on its own
# cub_ctv_demo_* tables, dropped at the end), captures one live summary
# window on a pseudo-terminal while recording, and checks that cub_ctv
# (1) probed the server, (2) produced a verdict with waiting sessions
# identified, (3) wrote activity lines to the record, and (4) that [r] on
# the live screen starts and stops a record.
# Prints the captured screen so the result can be read by eye.
#
# usage: t9_quick_trace.sh <db> [duration_s] [workers] [--en|--ko]
set -u
DB=${1:?db name}
DUR=${2:-15}
W=${3:-4}
LANGOPT=${4:---ko}
HERE=$(cd "$(dirname "$0")/.." && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/cub_ctv}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail () { echo "t9 FAIL: $1"; exit 1; }
# DB may be a pid (several servers of one name); shared resolver
ENV=$(CUB_CTV_TOOL="$TOOL" python3 "$HERE/tests/server_env.py" "$DB") || fail "no cub_server for $DB"
eval "$ENV"

"$TOOL" "$PID" --en > "$T/snap.txt" 2>&1 || fail "cannot read $DB: $(tail -1 "$T/snap.txt")"
grep -q "^layout:" "$T/snap.txt" || fail "layout not probed"
echo "== target: $(grep '^target:' "$T/snap.txt")"
echo "== $(grep '^layout:' "$T/snap.txt")"

echo "== load: $W workers, ${DUR}s (INSERT + DDL churn on cub_ctv_demo_* tables)"
timeout $((DUR + 60)) bash "$HERE/tests/demo_contention.sh" "$PID" "$DUR" "$W" 1,2 > "$T/demo.log" 2>&1 &
LOAD=$!
sleep 5

python3 - "$TOOL" "$PID" "$T" "$LANGOPT" <<'PY' || { echo "t9: waiting for the load to finish (it drops its tables)"; wait $LOAD 2>/dev/null; exit 1; }
import os, pty, fcntl, termios, struct, time, select, re, sys
tool, db, T, langopt = sys.argv[1:]
lang = "ko_KR.UTF-8" if langopt == "--ko" else "en_US.UTF-8"
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(LANG=lang, TERM="xterm", COLUMNS="140")
    os.execv(tool, ["cub_ctv", db, "-i", "--record", T + "/quick.rec", langopt])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
buf = b""; t = time.time()
while time.time() - t < 4.5:
    r, _, _ = select.select([fd], [], [], 0.05)
    if r:
        try: d = os.read(fd, 1 << 16)
        except OSError: break
        if not d: break
        buf += d
os.kill(pid, 2); os.waitpid(pid, 0)
txt = buf.decode("utf-8", "replace")
tag = "[판정]" if langopt == "--ko" else "[verdict]"
frames = [f for f in txt.split("\x1b[H\x1b[2J") if tag in f]
if not frames:
    print("t9 FAIL: no summary frame on the terminal"); sys.exit(1)
best = max(frames, key=lambda f: f.count("\n"))
plain = re.sub(r"\x1b\[[\d;]*m", "", best)
open(T + "/screen.txt", "w").write(plain)
print("== live summary window (one frame, escapes stripped)")
print(plain)
sess = "[세션]" if langopt == "--ko" else "[sessions]"
waits = "대기" if langopt == "--ko" else "waits"
if sess not in plain:
    print("t9 FAIL: no session block"); sys.exit(1)
if waits not in plain.partition(sess)[2]:
    print("t9 FAIL: no waiting session identified during the load"); sys.exit(1)
PY

wait $LOAD
F=$(grep -c '^F' "$T/quick.rec" 2>/dev/null || echo 0)
Wl=$(grep -c '^W' "$T/quick.rec" 2>/dev/null || echo 0)
[ "$F" -ge 1 ] && [ "$Wl" -ge 1 ] || fail "record has $F frames, $Wl activity lines"
grep -E "^ +[0-9]+ [A-Z_]+ +enter" "$T/demo.log" | head -4

# [r] on the live screen: starts <pid>_<db>_<time>.ctv in the cwd with a
# top-left mark, a second [r] stops it
python3 - "$TOOL" "$PID" "$T" <<'PY' || exit 1
import os, pty, time, select, re, sys, glob, signal
tool, db, T = sys.argv[1:]
os.chdir(T)
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(LANG="en_US.UTF-8", TERM="xterm", COLUMNS="140")
    os.execv(tool, ["cub_ctv", db, "-i", "--en"])
buf = b""
def pump(sec):
    global buf
    end = time.time() + sec
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: buf += os.read(fd, 1 << 16)
            except OSError: return
pump(1.5); os.write(fd, b"r"); pump(2.5); os.write(fd, b"r"); pump(1.5)
os.kill(pid, signal.SIGINT); os.waitpid(pid, 0)
txt = re.sub(r"\x1b\[[\d;]*m", "", buf.decode("utf-8", "replace"))
files = glob.glob("*_*_*.ctv")  # <pid>_<dbname>_<time>.ctv (db may be given as a pid)
if len(files) != 1:
    print("t9 FAIL: [r] made %d record files" % len(files)); sys.exit(1)
frames = open(files[0]).read().count("\nE\n")
if frames < 1 or "[REC] cub_ctv" not in txt or "[r] stop recording" not in txt:
    print("t9 FAIL: [r] recording (frames=%d, mark=%s)" % (frames, "[REC] cub_ctv" in txt)); sys.exit(1)
tail = txt.split("\x1b[H\x1b[2J")[-1]
if "[REC]" in tail:
    print("t9 FAIL: second [r] did not stop the recording"); sys.exit(1)
print("== [r] recording: %s, %d windows, stopped" % (files[0], frames))
PY
echo "t9 PASS: probe ok, verdict with waiting sessions, record $F windows / $Wl activity lines, [r] record/stop"
