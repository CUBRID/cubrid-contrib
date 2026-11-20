#!/bin/bash
# t19: a holder is named only when it holds most of the occupancy (no
# server).  A CS read most of the window with a client writing once is
# "readers", with no session action; a CS a client writes most of the time
# still names that session; writers sharing a CS that nobody reads are not
# readers.  Checked in the verdict and on the summary screen of the replayed
# record.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL=${CUB_CTV_TOOL:-$HERE/../cub_ctv}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "t19 FAIL: $*"; exit 1; }
mapfile -t SRCS < <(sh "$HERE/../build.sh" --list-sources | grep -v -e '/cub_ctv\.c$' -e '/ctv_sampler\.c$')
gcc -O1 -Wall -I"$HERE/../src" -o "$T/t19" "$HERE/t19_readers_most.c" "${SRCS[@]}" -lm \
  || fail "build"
LANG=C "$T/t19" "$T/r.rec" || exit 1
# the summary screen (a terminal) of the replayed record, English, plain
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
txt = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", buf.decode("utf-8", "replace"))
print(txt.replace("\r", ""))
PY
)
echo "$SCREEN" | grep -q "who  *readers (no single owner)  *writer tran 3 held 1% of the window (occupied 94%)" \
  || fail "who row does not say readers with tran 3 as evidence: $SCREEN"
echo "$SCREEN" | grep -q "action  *check read volume and the writing session  *occupied 94%, writers held 1%; readers the rest" \
  || fail "action row: $SCREEN"
echo "$SCREEN" | grep -q "inspect the holding session\|a client holds it longest" \
  && fail "a session named as the holder of a CS read most of the window"
echo "$SCREEN" | grep -Eq "LOCATOR_CLASSNAME_TABLE .* readers *$" || fail "CS table names a holder for CS 0"
echo "$SCREEN" | grep -Eq "QPROC_QUERY_CACHE .*tran 5" || fail "CS table lost tran 5 on CS 1"
echo "$SCREEN" | grep -Eq "CT_OID_TABLE .*tran 11" || fail "writers sharing a CS shown as readers: $(echo "$SCREEN" | grep CT_OID_TABLE)"
echo "$SCREEN" | grep -Eq "HA_SERVER_STATE .*tran 21" || fail "a writer of half the occupancy shown as readers: $(echo "$SCREEN" | grep HA_SERVER_STATE)"
echo "t19 PASS: a writer seen in 1% of a CS read 94% is evidence only (readers, no session action); a writer of most is named; writers sharing a CS are not readers"
