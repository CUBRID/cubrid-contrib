#!/bin/bash
# t22: OS state is kept per (thread, CS), read once a sample per thread
# (20 Hz at most), and the verdict's disk/CPU row is the named holder's
# state under the top CS - or none (no server).  Checked in the sampler,
# the verdict, the record's O lines (also damaged ones) and the summary
# screen of the replayed record.  The cases are listed in the .c file.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "t22 FAIL: $*"; exit 1; }
W=-Wl,--wrap=mono_s,--wrap=thr_index_of_tid_mem,--wrap=thr_tid_of,--wrap=os_task_comm,--wrap=os_task_state
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_sampler\.c$')
gcc -O1 -Wall -I"$HERE/../src" -o "$T/t22" "$HERE/t22_os_per_cs.c" "${SRCS[@]}" -lm $W \
  || fail "build"
LANG=C "$T/t22" "$T/r.rec" || exit 1
for want in "O 10 1 1010 20 0 20 0 " "O 10 0 1010 79 79 0 0 " "O 11 1 1011 75 0 75 0 " "O 11 0 1011 1 0 1 0 "; do
  grep -q "^$want" "$T/r.rec" || fail "record lacks '$want': $(grep '^O ' "$T/r.rec" | tr '\n' ';')"
done
[ "$(grep -c '^O ' "$T/r.rec")" = 4 ] || fail "record has $(grep -c '^O ' "$T/r.rec") O lines, want 4"
# a damaged O line is refused with its line number
n=$(grep -n '^O 10 0 ' "$T/r.rec" | cut -d: -f1)
bad_o() { # name, sed expression
  sed "${n}$2" "$T/r.rec" > "$T/bad.rec"
  out=$(LANG=C timeout 10 "$TOOL" --replay "$T/bad.rec" --replay-speed 0 2>&1); rc=$?
  [ $rc -ne 0 ] && echo "$out" | grep -q "malformed record file at .*bad.rec:$n" \
    || fail "O line with $1 accepted (exit $rc): $(echo "$out" | tail -1)"
}
bad_o "a CS past the count" 's/^O 10 0 /O 10 2 /'
bad_o "CS -2" 's/^O 10 0 /O 10 -2 /'
bad_o "no CS token (the former 11-token line)" 's/^O 10 0 /O 10 /'
SCREEN=$(python3 - "$TOOL" "$T/r.rec" <<'PY'
import os, pty, fcntl, termios, struct, time, select, re, sys
tool, rec = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
    os.environ.pop("COLUMNS", None)
    os.execv(tool, ["cub_ctv", "--replay", rec, "--replay-speed", "0", "--plain", "--en"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
buf, t = b"", time.time()
while time.time() - t < 2.0:
    if select.select([fd], [], [], 0.05)[0]:
        try:
            d = os.read(fd, 1 << 16)
        except OSError:
            break
        if not d:
            break
        buf += d
try:
    os.kill(pid, 15)
except ProcessLookupError:
    pass
os.waitpid(pid, 0)
print(re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", buf.decode("utf-8", "replace")).replace("\r", ""))
PY
)
echo "$SCREEN" | grep -q "who  *client session tran 5 " || fail "who row is not tran 5: $SCREEN"
echo "$SCREEN" | grep -q "disk/cpu  *holder thread CPU-bound  *worker-10: D 0% R 100%" \
  || fail "disk/cpu row is not thread 10's state under CS 0: $(echo "$SCREEN" | grep -m1 'disk/cpu')"
echo "$SCREEN" | grep -q "worker-11: D" && fail "disk/cpu row shows the thread that held LOG"
echo "t22 PASS: OS state per (thread, CS); one procfs read a sample per thread, 20 Hz cap, failing reads too; disk/cpu = the named holder under the top CS, or none; damaged O lines refused"
