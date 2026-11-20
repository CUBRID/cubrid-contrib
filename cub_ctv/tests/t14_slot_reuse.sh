#!/bin/bash
# t14: a transaction slot that changes hands within a window (no server).
# From the live fixture, frame 1 is changed so that LOG was held in the
# window by tran 29 of client pid 4000001 (identity kept in a J line), a
# queue hop waited as tran 19 of pid 4000002 (no identity kept), and both
# slots hold other clients at the window end.  The owner block, the wait
# chain and the summary screen's sessions must name the client seen, never
# the slot's current one.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail() { echo "t14 FAIL: $*"; exit 1; }

awk '
/^F /{f++}
f==1 && $1=="C" && $2=="1" {$3=0; $NF=4000001}
f==1 && $1=="S" && $3=="19" {$4=4000002}
{print}
f==1 && $1=="I" && $2=="29" {print "J 29 1 4000001 ~ DBA old_prog cubrid oldhost 10.0.0.1"}
f==1 && $1=="I" && $2=="29" {print "W 1 0 29 -1 40 4000001"}
' "$HERE/fixtures/demodb-live.rec" > "$T/reuse.rec"

OUT=$("$TOOL" --replay "$T/reuse.rec" --replay-speed 0 --narrow 2>&1 | sed -n '/^--- /,/^--- /p' | head -12)
echo "$OUT" | grep -q "| owner  DBA@oldhost old_prog pid=4000001 .* disconnected" \
  || fail "owner seen in the window not named as the client that left: $OUT"
echo "$OUT" | grep -q "| owner .*pid=3214311" && fail "owner given the slot's current client"
echo "$OUT" | grep -q "thr=4228(tran 19 pid 4000002, disconnected)" \
  || fail "queue hop not named by the pid seen: $OUT"

# summary screen: the session of the client that left
python3 - "$TOOL" "$T/reuse.rec" <<'EOF' || exit 1
import os, pty, sys, time, select, re, fcntl, termios, struct
tool, rec = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
    os.execv(tool, ["cub_ctv", "--replay", rec, "--plain"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 160, 0, 0))
buf, t = b"", time.time()
while time.time() - t < 1.0:
    r, _, _ = select.select([fd], [], [], 0.05)
    if r:
        try:
            buf += os.read(fd, 65536)
        except OSError:
            break
os.write(fd, b"q")
time.sleep(0.3)
try:
    os.kill(pid, 15)
except ProcessLookupError:
    pass
os.waitpid(pid, 0)
txt = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "\n", buf.decode("utf-8", "replace"))
rows = [l for l in txt.split("\n") if re.match(r"\s+29\s+\d+ ", l)]
if not any(re.match(r"\s+29\s+4000001 old_prog\s+\(disconnected\) ", l) for l in rows):
    print("t14 FAIL: session of the client that left:", rows[:3]); sys.exit(1)
EOF
echo "t14 PASS: a slot reused in the window keeps each client's identity (owner, queue hop, session)"
